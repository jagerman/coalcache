#include <CLI/CLI.hpp>
#include <CLI/Error.hpp>
#include <memory>
#include <nlohmann/json.hpp>
#include <oxen/log.hpp>
#include <oxen/log/format.hpp>
#include <unordered_map>

#include "client.hpp"
#include "coalcache.hpp"
#include "config.hpp"
#include "proxycache.hpp"
#include "reqcache.hpp"
#include "routes.hpp"
#include "server.hpp"

using namespace std::literals;
using namespace solcache;

namespace {

// NB: `log` is aliased function-locally (not at file scope) because a file-scope `namespace log`
// collides with the C math `::log`.
auto cat = oxen::log::Cat("main");

// Solana JSON-RPC handling (preserved verbatim from the original single-upstream server):
// getSignatureStatuses lookups are coalesced via CoalCache; all other methods are cached via
// ReqCache.  Both forward to the route's configured upstream.
void handle_solana(
        std::shared_ptr<req_data> rd, CoalCache& coalcache, ReqCache& reqcache, Server& server) {
    namespace log = oxen::log;
    using namespace oxen::log::literals;
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
            throw std::invalid_argument{"Invalid request jsonrpc version {}"_format(jsonrpc)};
        id = jreq.at("id");
        method = jreq.at("method").get<std::string>();
    } catch (const std::exception& e) {
        log::warning(cat, "Malformed jsonrpc request: {}. Request was:\n{}", e.what(), rd->body);
        server.send_response(
                rd,
                "Invalid request: json request body is malformed or is not a valid jsonrpc request",
                {},
                "400 Bad Request");
        return;
    }

    try {
        log::info(cat, "jsonrpc request for {}", method);
        if (method == "getSignatureStatuses") {
            struct collected {
                std::atomic<int> remaining;
                // Set true by whichever callback dispatches the response, so that we dispatch
                // exactly one response even if a later callback would also satisfy "done".
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
                coalcache.lookup(keys[i], [rd, i, &server, collection](const CoalCache::item* item) {
                    if (collection->sent.load(std::memory_order_relaxed))
                        return;  // already finalized
                    if (!item) {
                        bool expected = false;
                        if (!collection->sent.compare_exchange_strong(expected, true))
                            return;
                        server.send_response(
                                rd,
                                "Unable to process request: upstream RPC provider returned an error",
                                {},
                                "502 Bad Gateway");
                        return;
                    }

                    collection->values[i] = item->json();
                    if (collection->remaining.fetch_sub(1) > 1)
                        return;  // More results to go

                    bool expected = false;
                    if (!collection->sent.compare_exchange_strong(expected, true))
                        return;

                    nlohmann::json resp{
                            {"jsonrpc", "2.0"},
                            {"id", collection->id},
                            {"result", {{"context", {{"slot", item->context_slot}}}}}};
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
                                    "Unable to process request: upstream RPC provider returned an "
                                    "error",
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
                rd, "An error occured while processing your request", {}, "500 Internal Server Error");
        return;
    }
}

}  // namespace

int main(int argc, char** argv) {
    namespace log = oxen::log;

    CLI::App app{"Coalescing and caching RPC proxy (Solana, TRON, ...)"};

    std::string listen_ip = "127.0.0.1";
    std::vector<std::string> route_specs;
    uint16_t listen_port = 5014;
    app.add_option(
            "-r,--route",
            route_specs,
            "Proxy/cache route (repeatable; at least one required).  Format: 'prefix=/tron/wallet; "
            "kind=tron_wallet; upstream=URL; [backup=URL;] [header=K: V; ...]'.  kind is one of "
            "passthrough/tron_wallet/evm_jsonrpc/solana.  Longest-prefix match wins.");
    app.add_option("-l,--listen-ip", listen_ip, "IP address to listen on")->check(CLI::ValidIPV4);
    app.add_option("-p,--port", listen_port, "Port to listen on");
    // Any option above may also be supplied from a config file (CLI11 format, supports '#' comments
    // and `route = [...]` arrays) via --config <file>.
    app.set_config("--config", "", "Read options from a config file");

    int sigstat_wait = 3000;
    size_t sigstat_coalesce = 256;
    app.add_option(
            "-s,--sigstat-coalesce",
            sigstat_coalesce,
            "(solana routes) Maximum number of getSignatureStatuses requests to coalesce; 0 to "
            "disable.");
    app.add_option(
            "-S,--sigstat-wait",
            sigstat_wait,
            "(solana routes) Maximum time (ms) to wait for more getSignatureStatuses requests to "
            "coalesce before firing.");

    int cache_time = 15000, cache_negative = 5000;
    app.add_option(
            "-c,--cache-time", cache_time, "(solana routes) How long, in ms, we cache results.");
    app.add_option(
            "-C,--negative-cache-time",
            cache_negative,
            "(solana routes) How long, in ms, we cache not-found signature-status results; 0 or "
            "negative to never cache them.");

    int upstream_timeout = 3000;
    app.add_option(
            "-t,--upstream-timeout",
            upstream_timeout,
            "How long, in ms, to wait for an upstream provider to respond before giving up "
            "(all routes).  Independent of cache TTLs: a successful response is still cached for "
            "the full cache time.  Set this below the clients' own RPC timeout so that a hung "
            "upstream yields a prompt error the client can act on rather than a dropped request.");

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

    if (route_specs.empty()) {
        log::critical(cat, "At least one --route is required (see --help)");
        return 1;
    }

    Config config;
    config.listen_ip = listen_ip;
    config.port = listen_port;
    try {
        for (const auto& spec : route_specs)
            config.routes.push_back(parse_route(spec));
    } catch (const std::exception& e) {
        log::critical(cat, "Invalid --route: {}", e.what());
        return 1;
    }
    for (const auto& r : config.routes)
        log::info(cat, "route {} -> {} ({})", r.prefix, r.upstream, to_string(r.kind));

    Client client{upstream_timeout * 1ms};
    ProxyCache proxy{client};  // shared by all proxy-kind routes (URL is supplied per fetch)

    // Solana routes each get their own coalescer + request cache, bound to the route's upstream.
    std::unordered_map<const Route*, std::unique_ptr<CoalCache>> coals;
    std::unordered_map<const Route*, std::unique_ptr<ReqCache>> reqs;
    for (const auto& r : config.routes) {
        if (r.kind == CacheKind::solana) {
            coals[&r] = std::make_unique<CoalCache>(
                    client,
                    r.upstream,
                    sigstat_wait * 1ms,
                    sigstat_coalesce,
                    cache_time * 1ms,
                    cache_negative * 1ms);
            reqs[&r] = std::make_unique<ReqCache>(client, r.upstream, cache_time * 1ms);
        }
    }

    Server server{
            config.listen_ip,
            config.port,
            [&config, &client, &proxy, &coals, &reqs, &server](std::shared_ptr<req_data> rd) {
                const Route* route = config.match(rd->url);
                if (!route) {
                    log::warning(cat, "No route configured for {}", rd->url);
                    server.send_response(
                            rd, "no route configured for this path", {}, "404 Not Found");
                    return;
                }

                if (route->kind == CacheKind::solana) {
                    handle_solana(rd, *coals.at(route), *reqs.at(route), server);
                    return;
                }

                // Generic proxy/cache kinds: forward to upstream + (incoming path minus the prefix).
                std::string fwd_url = route->upstream + rd->url.substr(route->prefix.size());

                // Replays an upstream/cached response to the client; captures rd by value
                // (shared_ptr) so the request stays alive across the async hop.
                auto respond = [&server, rd](const CachedResponse& r) {
                    server.send_response(
                            rd,
                            r.body,
                            {{"Content-Type", r.content_type}},
                            http_status_line(r.status));
                };

                auto disp = classify(route->kind, rd->url, rd->body);
                if (!disp.cacheable) {
                    client.post(
                            fwd_url,
                            rd->body,
                            route->headers,
                            [respond](
                                    int status,
                                    std::unordered_map<std::string, std::string> headers,
                                    std::string body) {
                                respond(CachedResponse{
                                        status, content_type_of(headers), std::move(body)});
                            });
                    return;
                }

                proxy.fetch(
                        std::move(fwd_url),
                        rd->body,
                        route->headers,
                        rd->url,  // cache-key domain: the full path
                        std::move(disp.key_body),
                        std::move(disp.ttl),
                        std::move(respond));
            }};

    log::debug(cat, "Starting coalcache on {}:{}", config.listen_ip, config.port);
    try {
        server.run();
    } catch (const std::exception& e) {
        log::critical(cat, "Unable to start server: {}", e.what());
        return 1;
    }
    return 0;
}
