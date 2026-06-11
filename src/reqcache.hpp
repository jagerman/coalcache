#pragma once

#include <chrono>
#include <list>
#include <nlohmann/json.hpp>
#include <unordered_map>

#include "client.hpp"

namespace solcache {

/// Cache of upstream requests.
class ReqCache {
  public:

    // Callback invoked when results become available.  nullptr if the request failed.  Note that
    // the ->result element will have the "id" value set to 0, and needs to be copied and updated if
    // non-0 in the originating request.
    struct item;
    using item_callback = std::function<void(const item*)>;

    struct item {
        nlohmann::json result;
        std::chrono::steady_clock::time_point expiry;

        bool _pending = true; // True if this request is in progress; in progress requests don't yet
                              // have an expiry, and don't get cleaned from the cache.
        bool _sent = false; // True if this request has been sent to the upstream
        std::list<item_callback> _callbacks;
    };

  private:
    Client& client;
    oxen::quic::Loop& loop{client.loop};
    const std::string upstream;  // upstream RPC URL we forward cache misses to

    std::unordered_map<std::string, item> cache;

    const std::chrono::milliseconds cache_expiry;

    std::shared_ptr<oxen::quic::Ticker> cache_clean_timer;
    void clean_cache();

  public:
    ReqCache(Client& c, std::string upstream, std::chrono::milliseconds cache_expiry);

    // Initiates a cached upstream request for the given json rpc request.  If found in the cache
    // the callback is invoked immediately, otherwise the request is initiated and the callback
    // invoked when it returns.  If multiple identical requests arrive while a proxied request is
    // ongoing, all requests and triggered when the (original) request response comes back.
    //
    // If the upstream request failed then the callback is invoked with nullptr.
    void lookup(nlohmann::json jsonrpc, item_callback cb, std::string_view domain = "");

    // Replaces the id value with integer 0, so that it is suitable for hashing.
    static void de_id(nlohmann::json& json_rpc);

    static std::string hash(std::string_view body, std::string_view key = "");
};

}  // namespace solcache
