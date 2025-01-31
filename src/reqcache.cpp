#include "reqcache.hpp"

#include <sodium/crypto_generichash_blake2b.h>

#include <chrono>
#include <oxen/log.hpp>

namespace solcache {

namespace log = oxen::log;
auto cat = log::Cat("reqcache");

ReqCache::ReqCache(Client& c, std::chrono::milliseconds cache_expiry) :
        client{c},
        cache_expiry{cache_expiry},

        cache_clean_timer{loop.call_every(1s, [this] { clean_cache(); })} {}

void ReqCache::clean_cache() {
    assert(loop.in_event_loop());

    auto before = cache.size();
    log::debug(cat, "Cleaning ReqCache cache");
    auto now = std::chrono::steady_clock::now();
    for (auto it = cache.begin(); it != cache.end();) {
        auto& item = it->second;
        if (!item._pending && item.expiry < now)
            it = cache.erase(it);
        else
            ++it;
    }
    log::debug(cat, "Cleaned {} ReqCache cache items", before - cache.size());
}

void ReqCache::lookup(nlohmann::json jsonrpc, item_callback cb, std::string_view domain) {
    de_id(jsonrpc);
    std::string body = jsonrpc.dump();
    auto cache_key = hash(body, domain);

    loop.call([this,
               body = std::move(body),
               cache_key = std::move(cache_key),
               cb = std::move(cb)]() mutable {
        auto& item = cache[cache_key];
        if (!item._pending) {
            cb(&item);
            return;
        }

        item._callbacks.push_back(std::move(cb));

        if (item._sent)
            return;

        client.request_jsonrpc(
                std::move(body),
                [this, cache_key = std::move(cache_key)](
                        int status,
                        std::unordered_map<std::string, std::string> headers,
                        std::string body) mutable {
                    loop.call([this,
                               cache_key = std::move(cache_key),
                               status,
                               headers = std::move(headers),
                               body = std::move(body)] {
                        bool failed = status < 200 || status >= 300;
                        nlohmann::json resp;
                        if (!failed) {
                            try {
                                resp = nlohmann::json::parse(body);
                            } catch (const std::exception& e) {
                                log::error(
                                        cat,
                                        "Unable to parse upstream {} response: {}.  Body:\n{}",
                                        status,
                                        e.what(),
                                        body);
                                failed = true;
                            }
                        }

                        auto now = std::chrono::steady_clock::now();
                        auto it = cache.find(cache_key);
                        if (it == cache.end()) {
                            log::warning(cat, "Internal error: didn't find cache entry!");
                            return;
                        }
                        auto& item = it->second;
                        item._pending = false;
                        auto callbacks = std::move(item._callbacks);
                        item._callbacks.clear();

                        if (failed) {
                            for (auto& cb : callbacks)
                                cb(nullptr);
                            cache.erase(it);
                        } else {
                            item.result = std::move(resp);
                            item.expiry = now + cache_expiry;
                            for (auto& cb : callbacks)
                                cb(&item);
                        }
                    });
                });
    });
}

void ReqCache::de_id(nlohmann::json& json_rpc) {
    json_rpc["id"] = 0;
}

std::string ReqCache::hash(std::string_view body, std::string_view key) {
    std::string out;
    out.resize(32);
    crypto_generichash_blake2b(
            reinterpret_cast<unsigned char*>(out.data()),
            out.size(),
            reinterpret_cast<const unsigned char*>(body.data()),
            body.size(),
            reinterpret_cast<const unsigned char*>(key.data()),
            key.size());
    return out;
}

}  // namespace solcache
