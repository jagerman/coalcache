#include <CLI/CLI.hpp>
#include <CLI/Error.hpp>
#include <chrono>
#include <nlohmann/json.hpp>
#include <oxen/log.hpp>
#include <oxen/log/format.hpp>

#include "client.hpp"
#include "coalcache.hpp"
#include "reqcache.hpp"
#include "server.hpp"

using namespace std::literals;

int main(int argc, char** argv) {
    namespace log = oxen::log;
    using namespace log::literals;
    static auto cat = log::Cat("main");

    CLI::App app{"Solana request coalescer and cacher"};

    std::string upstream_url, listen_ip = "127.0.0.1";
    uint16_t listen_port = 5014;  // SOLA in 1337-speak
    app.add_option("-u,--upstream", upstream_url, "RPC provider URL")->required();
    app.add_option("-l,--listen-ip", listen_ip, "IP address to listen on")->check(CLI::ValidIPV4);
    app.add_option("-p,--port", listen_port, "Port to listen on");

    int sigstat_wait = 3000;
    size_t sigstat_coalesce = 256;
    app.add_option(
            "-s,--sigstat-coalesce",
            sigstat_coalesce,
            "Maximum number of getSignatureStatuses requests to coalesce; 0 to disable.");
    app.add_option(
            "-S,--sigstat-wait",
            sigstat_wait,
            "Maximum time (in milliseconds) to wait for more incoming getSignatureStatuses "
            "requests to coalesce before firing off the requests.");

    int cache_time = 15000, cache_negative = 5000;
    app.add_option("-c,--cache-time", cache_time, "How long, in milliseconds, we cache results.");
    app.add_option(
            "-C,--negative-cache-time",
            cache_time,
            "How long, in milliseconds, we cache not-found results for signature status lookups.  "
            "0 or negative to never cache them.");

    std::string loglevel = "info";
    app.add_option(
               "-L,--log-level",
               loglevel,
               "Log level; one of trace,debug,info,warn,error,critical,off")
            ->check(CLI::IsMember(
                    {"trace"sv, "debug"sv, "info"sv, "warn"sv, "error"sv, "critical"sv, "off"sv}));

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        return app.exit(e);
    }

    log::add_sink(log::Type::Print, "stderr");
    log::reset_level(log::level_from_string(loglevel));

    using namespace solcache;

    Client client{upstream_url};

    CoalCache coalcache{
            client, sigstat_wait * 1ms, sigstat_coalesce, cache_time * 1ms, cache_negative * 1ms};

    ReqCache reqcache{client, cache_time * 1ms};

    Server server{
            listen_ip,
            listen_port,
            [&coalcache, &reqcache, &server](std::shared_ptr<req_data> rd) {
                log::debug(cat, "request!");
                auto ct = rd->content_type();
                if (!ct || *ct != "application/json") {
                    log::warning(cat, "Invalid {} request", ct.value_or("no-content-type"));
                    server.send_response(
                            rd,
                            "Invalid request: this server only accepts application/json requests",
                            {},
                            "400 Bad Request");
                    return;
                }

                nlohmann::json jreq, id;
                std::string method;
                try {
                    jreq = nlohmann::json::parse(rd->body);

                    if (auto jsonrpc = jreq.at("jsonrpc").get<std::string_view>(); jsonrpc != "2.0")
                        throw std::invalid_argument{
                                "Invalid request jsonrpc version {}"_format(jsonrpc)};
                    id = jreq.at("id");
                    method = jreq.at("method").get<std::string>();
                } catch (const std::exception& e) {
                    log::warning(
                            cat,
                            "Malformed jsonrpc request: {}. Request was:\n{}",
                            e.what(),
                            rd->body);
                    server.send_response(
                            rd,
                            "Invalid request: json request body is malformed or is not a valid "
                            "jsonrpc request",
                            {},
                            "400 Bad Request");
                    return;
                }

                try {
                    log::info(cat, "jsonrpc request for {}", method);
                    if (method == "getSignatureStatuses") {
                        struct collected {
                            std::atomic<int> remaining;
                            // Set true by whichever callback dispatches the response, so that we
                            // dispatch exactly one response even if a later callback would also
                            // satisfy the "done" condition.
                            std::atomic<bool> sent = false;
                            std::vector<nlohmann::json> values;
                            nlohmann::json id;
                        };
                        auto keys = jreq.at("params").at(0).get<std::vector<std::string>>();
                        auto collection = std::make_shared<collected>();
                        collection->remaining = keys.size();
                        collection->values.resize(keys.size());
                        collection->id = std::move(id);
                        for (size_t i = 0; i < keys.size(); i++) {
                            coalcache.lookup(
                                    keys[i],
                                    [rd, i, &server, collection](
                                            const CoalCache::item* item) {
                                        if (collection->sent.load(std::memory_order_relaxed))
                                            return;  // already finalized
                                        if (!item) {
                                            bool expected = false;
                                            if (!collection->sent.compare_exchange_strong(
                                                        expected, true))
                                                return;
                                            server.send_response(
                                                    rd,
                                                    "Unable to process request: upstream RPC "
                                                    "provider returned an error",
                                                    {},
                                                    "502 Bad Gateway");
                                            return;
                                        }

                                        collection->values[i] = item->json();
                                        if (collection->remaining.fetch_sub(1) > 1)
                                            return;  // More results to go

                                        bool expected = false;
                                        if (!collection->sent.compare_exchange_strong(
                                                    expected, true))
                                            return;

                                        nlohmann::json resp{
                                                {"jsonrpc", "2.0"},
                                                {"id", collection->id},
                                                {"result",
                                                 {
                                                         {"context",
                                                          {{"slot", item->context_slot}}},
                                                 }}};
                                        auto& values = resp["result"]["value"];
                                        values = nlohmann::json::array();
                                        for (auto& v : collection->values)
                                            values.push_back(std::move(v));

                                        server.send_json_response(rd, std::move(resp));
                                    });
                        }
                        return;
                    } else {
                        reqcache.lookup(
                                std::move(jreq),
                                [rd, id = std::move(id), &server](const ReqCache::item* item) {
                                    if (!item) {
                                        server.send_response(
                                                rd,
                                                "Unable to process request: upstream RPC provider "
                                                "returned an error",
                                                {},
                                                "502 Bad Gateway");
                                        return;
                                    }
                                    auto result = item->result;
                                    result["id"] = id;
                                    server.send_json_response(rd, std::move(result));
                                });
                    }
                } catch (const std::exception& e) {
                    log::warning(
                            cat,
                            "Error while processing jsonrpc request: {}. Request was:\n{}",
                            e.what(),
                            rd->body);
                    server.send_response(
                            rd,
                            "An error occured while processing your request",
                            {},
                            "500 Internal Server Error");
                    return;
                }
            }};

    log::debug(cat, "Starting server");
    try {
        server.run();
    } catch (const std::exception& e) {
        log::critical(cat, "Unable to start server: {}", e.what());
        return 1;
    }
}
