#pragma once

#include <chrono>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "client.hpp"

namespace solcache {

// A cached upstream HTTP response.  We cache the full response so the proxy can replay it
// faithfully (status line + content type + body).
struct CachedResponse {
    int status = 0;
    std::string content_type;
    std::string body;
};

// Case-insensitively pull the Content-Type out of a header map, defaulting to application/json
// (both TRON surfaces are JSON).  Shared by the cache and the pass-through path.
std::string content_type_of(const std::unordered_map<std::string, std::string>& headers);

// Map an HTTP status code to a "<code> <reason>" line suitable for uWS writeStatus().
std::string http_status_line(int code);

// Generic caching/de-duplicating reverse-proxy cache.
//
// This is the multi-chain generalization of ReqCache: it forwards a POST to an arbitrary upstream
// URL, caches the response keyed by a caller-supplied (domain, body) pair, de-duplicates concurrent
// identical requests (only one hits the upstream; the rest wait for it), and lets the caller decide
// the cache lifetime *per request* — including from the response itself, so "current state" replies
// can expire quickly while immutable/historical replies are kept for a long time.
//
// It is chain-agnostic: the per-method policy (what to cache, for how long, what to normalize for
// the key) lives in the caller (see routes.hpp), not here.
class ProxyCache {
  public:
    using resp_cb = std::function<void(const CachedResponse&)>;

    // Given the upstream (status, body), return how long to cache it.  A value <= 0, or a non-2xx
    // status, means "do not cache" (the response is still delivered to waiters).
    using ttl_fn = std::function<std::chrono::milliseconds(int status, const std::string& body)>;

    // `clean_interval` is how often expired entries are swept; `stale_grace` is how long an expired
    // entry is retained past its expiry so it can still be served if the upstream subsequently
    // fails (serve-stale-on-error — always correct for immutable/block-pinned data).  The grace
    // window also bounds memory: an entry lives at most ttl + stale_grace past its last refresh.
    explicit ProxyCache(
            Client& client,
            std::chrono::milliseconds clean_interval = std::chrono::seconds{1},
            std::chrono::milliseconds stale_grace = std::chrono::hours{1});

    // Serve `cb` from cache if a fresh entry exists; otherwise forward a POST of `send_body` (with
    // `headers`) to `url` and cache the result according to `ttl`.  Concurrent calls with the same
    // key coalesce onto a single upstream request.
    //
    // The cache key is blake2b(key_body, key_domain): use the request path as `key_domain` (so
    // different methods/paths never collide) and the *normalized* body as `key_body` (e.g. with the
    // JSON-RPC `id` zeroed), while `send_body` is what is actually forwarded upstream verbatim.
    void fetch(
            std::string url,
            std::string send_body,
            std::vector<std::string> headers,
            std::string key_domain,
            std::string key_body,
            ttl_fn ttl,
            resp_cb cb);

  private:
    Client& client;
    oxen::quic::Loop& loop{client.loop};
    const std::chrono::milliseconds stale_grace;

    struct Entry {
        CachedResponse response;  // last good (2xx, cacheable) response, if have_response
        bool have_response = false;
        std::chrono::steady_clock::time_point expiry{};
        bool in_flight = false;        // an upstream request is currently outstanding
        std::list<resp_cb> callbacks;  // waiters for the in-flight request
        ttl_fn ttl;
    };

    std::unordered_map<std::string, Entry> cache;
    std::shared_ptr<oxen::quic::Ticker> cleaner;

    void clean();
    void on_response(
            const std::string& key, int status, std::string content_type, std::string body);

    static std::string hash(std::string_view key_body, std::string_view key_domain);
};

}  // namespace solcache
