#pragma once
#include <sodium/crypto_generichash_blake2b.h>

#include <array>
#include <chrono>
#include <mutex>
#include <nlohmann/json.hpp>

#include "client.hpp"

using namespace std::literals;

namespace solcache {

enum class sig_conf_status { unknown, null, processed, confirmed, finalized };

constexpr std::string_view to_string(sig_conf_status s) {
    switch (s) {
        case sig_conf_status::null: return "null"sv;
        case sig_conf_status::processed: return "processed"sv;
        case sig_conf_status::confirmed: return "confirmed"sv;
        case sig_conf_status::finalized: return "finalized"sv;
        default: return "unknown"sv;
    }
}

/// Cache and coalescer of getSignatureStatuses requests.
class CoalCache {
  public:
    struct item {
        sig_conf_status conf_status;
        int64_t context_slot;
        std::optional<int64_t> confirmations;
        std::optional<int64_t> err_code;
        std::optional<int64_t> slot;
        std::optional<nlohmann::json> err_status;
        std::chrono::steady_clock::time_point expiry;

        bool is_null() const { return conf_status == sig_conf_status::null; }
        explicit operator bool() const { return !is_null(); };

        void load(const nlohmann::json& value);

        template <typename T>
        static nlohmann::json nullable(const std::optional<T>& val) {
            if (val)
                return *val;
            return nullptr;
        }
        nlohmann::json json() const {
            if (conf_status == sig_conf_status::null)
                return nullptr;

            return nlohmann::json{
                    {"confirmationStatus", to_string(conf_status)},
                    {"confirmations", nullable(confirmations)},
                    {"err_code", nullable(err_code)},
                    {"slot", nullable(slot)},
                    {"status",
                     err_status ? nlohmann::json{{"Err", *err_status}}
                                : nlohmann::json{{"Ok", nullptr}}}};
        }
    };

    // Callback used when the result is available.  Returns a nullptr if the upstream request failed
    // completely (note that this is different from a not-found result, which will be an item with
    // status
    using item_callback = std::function<void(const item*)>;

  private:
    Client& client;
    oxen::quic::Loop& loop{client.loop};

    const std::chrono::milliseconds coalesce_time;
    const std::chrono::milliseconds positive_cache_time;
    const std::chrono::milliseconds negative_cache_time;
    const size_t max_coalesce;
    std::unordered_map<std::string, item> cache;
    mutable std::mutex cache_mut;

    void clean_cache();

    std::shared_ptr<oxen::quic::Ticker> cache_clean_timer;

    // These are not explicitly mutex protected, but may only be touched inside the loop thread:
    std::unordered_map<std::string, std::list<item_callback>> q_coalescing;
    std::unordered_map<std::string, std::list<item_callback>> q_sent;
    int q_num = 0;

  public:
    // Initializes the coalescing cache client wrapper.
    //
    // This works by starting a timer when the first request is received and waiting until the timer
    // expires to forward the request to the upstream RPC.  During this timer, if more lookups are
    // requested, they are queued together with earlier ones.  If we get max_coalesce unsent queued
    // requests then we fire the request for those items without waiting for the timer to fire,
    // otherwise when the timer fires, we fire off any requests during that duration.  When the
    // responses arrive, we fire a callback (given when the request is made) to deliver the values.
    //
    // Additionally we cache all these values for cache_time, so that we can respond immediately
    // without queuing anything if a cached signature status is requested.
    //
    // `client` is used to make upstream requests for values not in the cache, and must be kept
    // alive for the lifetime of this object.
    //
    // `coalesce_time` is the maximum time we will wait for more lookups to coalesce before actually
    // firing the request, and so is the max delay for initiate a request to the upstream RPC
    // server.
    //
    // `max_coalesce` is the maximum number of requests to coalesce into one upstream request.
    // Default Solana RPC has a limit of 256, but RPC providers might have higher or lower limits
    // depending on the plan in use.
    //
    // `positive_cache_time` is how long we hold onto positive status values, while
    // `negative_cache_time` is how long we hold onto negative (i.e. null) results.  (Completely
    // failed requests are never cached).
    CoalCache(
            Client& client,
            std::chrono::milliseconds coalesce_time,
            size_t max_coalesce,
            std::chrono::milliseconds positive_cache_time,
            std::chrono::milliseconds negative_cache_time);

    // Looks up a key, calls the callback when the result is available.  callback *could* be called
    // immediately (before returning) if the key is in the cache, otherwise it queues the request
    // and will be invoked when the result comes back from the next coalesced call.  Results will be
    // nullopt if we got a `null` from the server for that item (which generally indicates a "not
    // found").
    void lookup(const std::string& k, item_callback cb);

    // Called automatically to send off the current queue according to the construction parameters,
    // but can also be called manually if desired.
    void send_coalesced();
};

}  // namespace solcache
