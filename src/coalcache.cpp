#include "coalcache.hpp"

#include <chrono>
#include <iterator>
#include <mutex>
#include <oxen/log.hpp>
#include <oxen/log/format.hpp>

using namespace std::literals;
using namespace oxen::log::literals;

namespace solcache {

namespace log = oxen::log;
static auto cat = log::Cat("coalcache");

void CoalCache::item::load(const nlohmann::json& value) {
    if (value.is_null()) {
        conf_status = sig_conf_status::null;
        confirmations.reset();
        err_code.reset();
        slot.reset();
        err_status.reset();
        return;
    }

    {
        auto conf = value["confirmationStatus"].get<std::string_view>();
        conf_status = conf == "confirmed" ? sig_conf_status::confirmed
                    : conf == "processed" ? sig_conf_status::processed
                    : conf == "finalized" ? sig_conf_status::finalized
                                          : sig_conf_status::unknown;
    }

    if (auto confs = value["confirmations"]; confs.is_null())
        confirmations = std::nullopt;
    else
        confirmations = confs.get<int64_t>();

    if (auto ec = value["err"]; ec.is_null())
        err_code = std::nullopt;
    else
        err_code = ec.get<int64_t>();

    if (auto sl = value["slot"]; !sl.is_null())
        slot = sl.get<int64_t>();
    else
        slot = std::nullopt;

    if (auto& status = value["status"]; status.is_null() || status.count("Ok"))
        err_status = std::nullopt;
    else if (auto& err = status["Err"]; err.is_string())
        err_status = err.get<std::string>();
    else
        err_status = err.dump();
}

CoalCache::CoalCache(
        Client& client,
        std::chrono::milliseconds coalesce_time,
        size_t max_coalesce,
        std::chrono::milliseconds positive_cache_time,
        std::chrono::milliseconds negative_cache_time) :
        client{client},
        coalesce_time{coalesce_time},
        positive_cache_time{positive_cache_time},
        negative_cache_time{negative_cache_time},
        max_coalesce{max_coalesce},
        cache_clean_timer{loop.call_every(1s, [this] { clean_cache(); })} {}

void CoalCache::clean_cache() {
    std::lock_guard lock{cache_mut};
    log::debug(cat, "Cleaning cache");
    size_t before = cache.size();
    auto now = std::chrono::steady_clock::now();
    for (auto it = cache.begin(); it != cache.end();) {
        if (it->second.expiry <= now)
            it = cache.erase(it);
        else
            ++it;
    }
    log::debug(cat, "Cleaned {} cache items", before - cache.size());
}

void CoalCache::lookup(const std::string& k, item_callback cb) {
    {
        std::lock_guard lock{cache_mut};
        if (auto it = cache.find(k); it != cache.end()) {
            cb(&it->second);
            return;
        }
    }

    loop.call([this, k, cb = std::move(cb)]() mutable {
        log::info(cat, "initiating CoalCache lookup for {}", k);
        if (auto it = q_sent.find(k); it != q_sent.end()) {
            it->second.push_back(std::move(cb));
            return;
        }

        if (q_coalescing.empty())
            loop.call_later(coalesce_time, [this, q_num = q_num] {
                // If q_num has advanced then something already triggered the coalesced send,
                // and so the timer is stale.
                if (q_num == this->q_num)
                    send_coalesced();
            });

        q_coalescing[k].push_back(std::move(cb));

        if (q_coalescing.size() >= max_coalesce)
            send_coalesced();
    });
}

void CoalCache::send_coalesced() {
    if (!loop.inside()) {
        loop.call_soon([this] { send_coalesced(); });
        return;
    }

    if (q_coalescing.empty())
        return;

    // Drain q_coalescing: copy all the callbacks into q_sent, and make a local vector of all the
    // keys so that we can match up response items to the ones we requested.
    std::vector<std::string> keys;
    const size_t q_size = std::min(q_coalescing.size(), max_coalesce);
    keys.reserve(q_size);
    for (auto it = q_coalescing.begin(); it != q_coalescing.end() && keys.size() < q_size;) {
        auto& [key, callbacks] = *it;
        if (!callbacks.empty()) {
            keys.push_back(key);
            auto& s = q_sent[key];
            for (auto& cb : callbacks)
                s.push_back(std::move(cb));
        }
        it = q_coalescing.erase(it);
    }

    nlohmann::json req{{"id", 0}, {"jsonrpc", "2.0"}, {"method", "getSignatureStatuses"}};
    auto& params = (req["params"] = nlohmann::json::array());
    auto& sigs = params.emplace_back(nlohmann::json::array());
    for (const auto& k : keys)
        sigs.emplace_back(k);

    params.push_back(nlohmann::json{{"searchTransactionHistory", true}});

    client.request(
            req,
            [this, keys = std::move(keys)](
                    int status,
                    std::unordered_map<std::string, std::string> headers,
                    std::string body) {
                loop.call([this,
                           status,
                           headers = std::move(headers),
                           body = std::move(body),
                           keys = std::move(keys)] {
                    bool failed = status < 200 || status >= 300;
                    int64_t ctx_slot;
                    nlohmann::json resp;
                    if (!failed) {
                        try {
                            resp = nlohmann::json::parse(body);
                            if (resp.at("jsonrpc") != "2.0"sv)
                                throw std::invalid_argument{"missing or invalid jsonrpc value"};
                            if (auto it = resp.find("error"); it != resp.end())
                                throw std::runtime_error{
                                        "jsonrpc request returned an error: {}"_format(it->dump())};
                            resp = resp.at("result");
                            auto& ctx = resp.at("context");
                            ctx_slot = ctx.at("slot").get<int64_t>();
                            resp = resp.at("value");
                            if (!resp.is_array())
                                throw std::runtime_error{
                                        "jsonrpc request did not return a value array"};
                            if (resp.size() != keys.size())
                                throw std::runtime_error{
                                        "jsonrpc response contains a wrong number of values: "
                                        "{}, expected {}"_format(resp.size(), keys.size())};
                        } catch (const std::exception& e) {
                            log::error(
                                    cat,
                                    "Unable to parse getSignatureStatuses {} response: {}.  "
                                    "Body:\n{}",
                                    status,
                                    e.what(),
                                    body);
                            failed = true;
                        }
                    }
                    std::unique_lock lock{cache_mut, std::defer_lock};
                    if (!failed)
                        lock.lock();

                    size_t calls = 0;
                    auto now = std::chrono::steady_clock::now();
                    for (size_t i = 0; i < keys.size(); i++) {
                        const auto& value = resp[i];
                        const auto& key = keys[i];
                        auto it = q_sent.find(key);
                        if (it == q_sent.end()) {
                            log::warning(cat, "Did not find key in q_sent, bug?");
                            continue;
                        }

                        auto& callbacks = it->second;

                        item x{};
                        item* loaded = nullptr;
                        if (!failed) {
                            try {
                                x.load(value);
                                x.context_slot = ctx_slot;
                                x.expiry = now + (x ? negative_cache_time : positive_cache_time);
                                if (x.expiry > now)
                                    loaded = &(cache[key] = std::move(x));
                                else
                                    loaded = &x;
                            } catch (const std::exception& e) {
                                log::error(
                                        cat,
                                        "Failed to load signature status item from json: "
                                        "{}.  Failing JSON element:\n{}",
                                        e.what(),
                                        value.dump());
                            }
                        }

                        calls += callbacks.size();
                        for (auto& cb : callbacks)
                            cb(loaded);

                        q_sent.erase(it);
                    }

                    log::info(cat, "Coalesced {} getSignatureStatuses calls", calls);
                });
            });

    // If we hit the request max but there are more here still in the queue then keep going.
    if (!q_coalescing.empty())
        send_coalesced();
}

}  // namespace solcache
