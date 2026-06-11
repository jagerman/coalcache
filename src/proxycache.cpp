#include "proxycache.hpp"

#include <sodium/crypto_generichash_blake2b.h>

#include <oxen/log.hpp>
#include <oxen/log/format.hpp>

using namespace std::literals;
using namespace oxen::log::literals;

namespace solcache {

namespace log = oxen::log;
static auto cat = log::Cat("proxycache");

ProxyCache::ProxyCache(
        Client& client,
        std::chrono::milliseconds clean_interval,
        std::chrono::milliseconds stale_grace) :
        client{client},
        stale_grace{stale_grace},
        cleaner{loop.call_every(clean_interval, [this] { clean(); })} {}

std::string ProxyCache::hash(std::string_view key_body, std::string_view key_domain) {
    std::string out;
    out.resize(crypto_generichash_blake2b_BYTES);
    crypto_generichash_blake2b(
            reinterpret_cast<unsigned char*>(out.data()),
            out.size(),
            reinterpret_cast<const unsigned char*>(key_body.data()),
            key_body.size(),
            reinterpret_cast<const unsigned char*>(key_domain.data()),
            key_domain.size());
    return out;
}

std::string content_type_of(const std::unordered_map<std::string, std::string>& headers) {
    constexpr std::string_view want = "content-type";
    for (const auto& [k, v] : headers) {
        if (k.size() != want.size())
            continue;
        bool match = true;
        for (size_t i = 0; i < k.size(); i++) {
            char c = k[i] >= 'A' && k[i] <= 'Z' ? k[i] + ('a' - 'A') : k[i];
            if (c != want[i]) {
                match = false;
                break;
            }
        }
        if (match)
            return v;
    }
    return "application/json";
}

std::string http_status_line(int code) {
    switch (code) {
        case 200: return "200 OK";
        case 201: return "201 Created";
        case 202: return "202 Accepted";
        case 204: return "204 No Content";
        case 400: return "400 Bad Request";
        case 401: return "401 Unauthorized";
        case 403: return "403 Forbidden";
        case 404: return "404 Not Found";
        case 429: return "429 Too Many Requests";
        case 500: return "500 Internal Server Error";
        case 502: return "502 Bad Gateway";
        case 503: return "503 Service Unavailable";
        case 504: return "504 Gateway Timeout";
        default: break;
    }
    if (code >= 200 && code < 600)
        return "{} Status"_format(code);
    // status 0 (transport failure / no response) -> surface as a gateway error
    return "502 Bad Gateway";
}

void ProxyCache::clean() {
    auto now = std::chrono::steady_clock::now();
    size_t before = cache.size();
    for (auto it = cache.begin(); it != cache.end();) {
        auto& e = it->second;
        // Keep in-flight entries and entries still within their expiry + stale-grace window (the
        // grace lets us serve-stale-on-error and bounds memory at the same time).
        if (!e.in_flight && now > e.expiry + stale_grace)
            it = cache.erase(it);
        else
            ++it;
    }
    if (before != cache.size())
        log::debug(cat, "Cleaned {} proxy cache entries ({} remain)", before - cache.size(), cache.size());
}

void ProxyCache::fetch(
        std::string url,
        std::string send_body,
        std::vector<std::string> headers,
        std::string key_domain,
        std::string key_body,
        ttl_fn ttl,
        resp_cb cb) {
    auto key = hash(key_body, key_domain);

    loop.call([this,
               key = std::move(key),
               url = std::move(url),
               send_body = std::move(send_body),
               headers = std::move(headers),
               ttl = std::move(ttl),
               cb = std::move(cb)]() mutable {
        auto now = std::chrono::steady_clock::now();
        auto& e = cache[key];

        // Fresh cache hit: serve immediately.
        if (e.have_response && now < e.expiry) {
            log::debug(cat, "cache hit");
            cb(e.response);
            return;
        }
        log::debug(cat, "cache miss ({})", e.in_flight ? "joining in-flight request" : "forwarding");

        // Otherwise we need to (re)fetch.  Join an in-flight request if there is one; this is the
        // de-duplication that collapses concurrent identical requests into a single upstream call.
        e.callbacks.push_back(std::move(cb));
        if (e.in_flight)
            return;
        e.in_flight = true;
        e.ttl = std::move(ttl);

        client.post(
                std::move(url),
                std::move(send_body),
                std::move(headers),
                [this, key](int status,
                            std::unordered_map<std::string, std::string> resp_headers,
                            std::string body) {
                    auto ct = content_type_of(resp_headers);
                    // The libcurl completion already runs on the loop thread, but bounce through
                    // the loop explicitly to be safe against any future transport change.
                    loop.call([this, key, status, ct = std::move(ct), body = std::move(body)]() mutable {
                        on_response(key, status, std::move(ct), std::move(body));
                    });
                });
    });
}

void ProxyCache::on_response(
        const std::string& key, int status, std::string content_type, std::string body) {
    auto it = cache.find(key);
    if (it == cache.end()) {
        log::warning(cat, "proxy cache entry vanished before response, bug?");
        return;
    }
    auto& e = it->second;
    e.in_flight = false;
    auto callbacks = std::move(e.callbacks);
    e.callbacks.clear();

    CachedResponse resp{status, std::move(content_type), std::move(body)};
    bool ok = status >= 200 && status < 300;
    auto ttl_ms = e.ttl ? e.ttl(status, resp.body) : 0ms;

    if (ok && ttl_ms > 0ms) {
        // Cacheable success: store and serve.
        e.response = std::move(resp);
        e.have_response = true;
        e.expiry = std::chrono::steady_clock::now() + ttl_ms;
        for (auto& c : callbacks)
            c(e.response);
        return;
    }

    if (ok) {
        // 2xx but not to be cached (e.g. a "current state" reply, or a not-yet-mined tx): deliver
        // it fresh without storing.
        for (auto& c : callbacks)
            c(resp);
        if (!e.have_response)
            cache.erase(it);
        return;
    }

    // Upstream failure.  If we still hold a previously-cached response, serve it: for the data we
    // cache (block-pinned / immutable), a stale value is still correct, and this rides out a
    // provider outage or rate-limit instead of failing the validator.
    if (e.have_response) {
        log::warning(cat, "upstream returned {}, serving stale cached response", status);
        for (auto& c : callbacks)
            c(e.response);
        return;
    }

    log::warning(cat, "upstream request failed with status {} and no cached value", status);
    for (auto& c : callbacks)
        c(resp);
    cache.erase(it);
}

}  // namespace solcache
