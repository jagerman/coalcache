#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

#include "proxycache.hpp"

namespace solcache {

// Selects the built-in request classifier for a route.  Add new chains here as needed.
enum class CacheKind {
    passthrough,   // forward only, never cache (safe default for any new path)
    tron_wallet,   // TRON native HTTP API (method is the URL path segment), raw-body key
    evm_jsonrpc,   // Ethereum-style JSON-RPC (TRON's /jsonrpc, or any EVM chain), id-normalized key
    solana,        // Solana JSON-RPC: getSignatureStatuses coalescing + generic cache (handled by
                   // dedicated CoalCache/ReqCache machinery, not the classify() TTL path)
};

constexpr std::string_view to_string(CacheKind k) {
    switch (k) {
        case CacheKind::passthrough: return "passthrough";
        case CacheKind::tron_wallet: return "tron_wallet";
        case CacheKind::evm_jsonrpc: return "evm_jsonrpc";
        case CacheKind::solana: return "solana";
    }
    return "?";
}

// Throws std::invalid_argument on an unknown name.
constexpr CacheKind cache_kind_from_string(std::string_view s) {
    if (s == to_string(CacheKind::passthrough))
        return CacheKind::passthrough;
    if (s == to_string(CacheKind::tron_wallet))
        return CacheKind::tron_wallet;
    if (s == to_string(CacheKind::evm_jsonrpc))
        return CacheKind::evm_jsonrpc;
    if (s == to_string(CacheKind::solana))
        return CacheKind::solana;
    throw std::invalid_argument{"unknown route kind: " + std::string{s}};
}

// The result of classifying a request.
struct Disposition {
    bool cacheable = false;   // false => pass through (no cache, no de-duplication)
    std::string key_body;     // normalized body used for the cache key (cacheable only)
    ProxyCache::ttl_fn ttl;   // decides cache lifetime from the eventual response (cacheable only)
};

// Decide how to handle a request: whether it is cacheable, how to key it, and (via the returned
// ttl function) how long to cache the response — including short TTLs for "current state" replies
// and "do not cache" for not-yet-mined transactions.  `path` is the full incoming request path.
Disposition classify(CacheKind kind, std::string_view path, const std::string& body);

}  // namespace solcache
