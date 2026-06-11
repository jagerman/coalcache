#include "routes.hpp"

#include <chrono>
#include <nlohmann/json.hpp>

using namespace std::chrono_literals;

namespace solcache {

namespace {

    // Default cache lifetimes.  Block-pinned / immutable data never changes, so its TTL is bounded
    // only by memory (see ProxyCache stale-grace) — kept long to maximise the hit rate, which for
    // the historical wallet calls is what keeps us under a provider's free tier.
    constexpr std::chrono::milliseconds TTL_IMMUTABLE = 2h;
    constexpr std::chrono::milliseconds TTL_CHAINID = 24h;
    // Chain head (eth_blockNumber): a few seconds stale is harmless — witnessing runs ~19 blocks
    // (~57s) behind the tip, and broadcasts get their ref-block fresh from triggersmartcontract.
    // 10s collapses many validators' head polls to ~one upstream call per interval.
    constexpr std::chrono::milliseconds TTL_HEAD = 10s;

    ProxyCache::ttl_fn const_ttl(std::chrono::milliseconds t) {
        return [t](int, const std::string&) { return t; };
    }

    // Cache only when the JSON-RPC `result` is present (non-null): a not-yet-mined tx returns null
    // and must not be cached long, or we'd keep serving "missing" after it appears.
    ProxyCache::ttl_fn ttl_if_result_present(std::chrono::milliseconds t) {
        return [t](int, const std::string& body) -> std::chrono::milliseconds {
            try {
                auto j = nlohmann::json::parse(body);
                if (j.contains("result") && !j.at("result").is_null())
                    return t;
            } catch (...) {
            }
            return 0ms;
        };
    }

    // Same idea for the native wallet API, which returns an empty object {} for a missing tx.
    ProxyCache::ttl_fn ttl_if_nonempty(std::chrono::milliseconds t) {
        return [t](int, const std::string& body) -> std::chrono::milliseconds {
            try {
                auto j = nlohmann::json::parse(body);
                if (j.is_object() && !j.empty())
                    return t;
            } catch (...) {
            }
            return 0ms;
        };
    }

    // Zero the JSON-RPC `id` so requests differing only by id share a cache entry.
    std::string deid(const std::string& body) {
        try {
            auto j = nlohmann::json::parse(body);
            j["id"] = 0;
            return j.dump();
        } catch (...) {
            return body;  // unparseable: key on the raw bytes
        }
    }

    Disposition classify_jsonrpc(const std::string& body) {
        Disposition d;
        std::string method;
        nlohmann::json params;
        try {
            auto j = nlohmann::json::parse(body);
            method = j.value("method", "");
            if (j.contains("params"))
                params = j.at("params");
        } catch (...) {
            return d;  // unparseable => pass through
        }

        auto cache_with = [&](ProxyCache::ttl_fn fn) {
            d.cacheable = true;
            d.key_body = deid(body);
            d.ttl = std::move(fn);
        };

        if (method == "eth_chainId")
            cache_with(const_ttl(TTL_CHAINID));
        else if (method == "eth_blockNumber")
            cache_with(const_ttl(TTL_HEAD));
        else if (method == "eth_getLogs" || method == "eth_getBlockByHash" ||
                 method == "eth_getTransactionByHash")
            cache_with(const_ttl(TTL_IMMUTABLE));
        else if (method == "eth_getBlockByNumber") {
            // An explicit block number is immutable; a "latest"/"pending" tag is current state.
            bool head = false;
            if (params.is_array() && !params.empty() && params[0].is_string()) {
                auto tag = params[0].get<std::string>();
                head = tag == "latest" || tag == "pending" || tag == "earliest";
            }
            cache_with(const_ttl(head ? TTL_HEAD : TTL_IMMUTABLE));
        } else if (method == "eth_getTransactionReceipt")
            cache_with(ttl_if_result_present(TTL_IMMUTABLE));
        // else: eth_estimateGas / eth_feeHistory / unknown -> pass through
        return d;
    }

    Disposition classify_wallet(std::string_view path, const std::string& body) {
        Disposition d;
        // The wallet method is the final path segment, e.g. /wallet/getblockbalance.
        auto method = path.substr(path.find_last_of('/') + 1);
        if (auto q = method.find('?'); q != std::string_view::npos)
            method = method.substr(0, q);

        auto cache_with = [&](ProxyCache::ttl_fn fn) {
            d.cacheable = true;
            d.key_body = body;  // wallet bodies have no id to normalize
            d.ttl = std::move(fn);
        };

        if (method == "getblockbalance")
            cache_with(const_ttl(TTL_IMMUTABLE));  // the historical/archive call
        else if (method == "gettransactionbyid" || method == "gettransactioninfobyid")
            cache_with(ttl_if_nonempty(TTL_IMMUTABLE));
        // triggerconstantcontract / estimateenergy (current-state sims) and triggersmartcontract /
        // broadcasttransaction (writes) and anything unknown -> pass through
        return d;
    }

}  // namespace

Disposition classify(CacheKind kind, std::string_view path, const std::string& body) {
    switch (kind) {
        case CacheKind::evm_jsonrpc: return classify_jsonrpc(body);
        case CacheKind::tron_wallet: return classify_wallet(path, body);
        case CacheKind::solana:       // handled by dedicated CoalCache/ReqCache, not here
        case CacheKind::passthrough: break;
    }
    return {};  // passthrough
}

}  // namespace solcache
