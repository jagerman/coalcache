#include "server.hpp"

#include <fmt/core.h>

#include <nlohmann/json.hpp>
#include <oxen/log.hpp>
#include <oxen/log/format.hpp>
#include <thread>

using namespace oxen::log::literals;
using namespace std::literals;

namespace solcache {

namespace log = oxen::log;
static auto cat = log::Cat("server");

static std::string lc_string(std::string_view x) {
    std::string y;
    y.reserve(x.size());
    for (char c : x)
        y += (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
    return y;
}

req_data::req_data(uWS::HttpRequest& req, HttpResponse& response) :
        res{&response},
        method{req.getCaseSensitiveMethod()},
        full_url{req.getFullUrl()},
        url{req.getUrl()},
        remote_addr{res->getRemoteAddressAsText()} {
    for (const auto& [header, value] : req)
        headers.emplace(lc_string(header), value);

    // If a reverse proxy forwarded the real client address, prefer it over the socket peer (which
    // would otherwise just be the proxy, e.g. 127.0.0.1).  Safe to trust because we sit on
    // localhost behind nginx, which sets these headers itself, overwriting anything a client sent.
    if (auto it = headers.find("x-real-ip"); it != headers.end() && !it->second.empty())
        remote_addr = it->second;
    else if (auto fwd = headers.find("x-forwarded-for");
             fwd != headers.end() && !fwd->second.empty())
        remote_addr = fwd->second.substr(0, fwd->second.find(','));
}

std::optional<std::string_view> req_data::content_type() const {
    if (auto it = headers.find("content-type"); it != headers.end())
        return it->second;
    return std::nullopt;
}

void Server::run() {

    tid = std::this_thread::get_id();

    // Catch-all POST: the handler routes by path itself (legacy Solana mode ignores the path; the
    // proxy/config mode longest-prefix-matches it).  "/*" is a superset of the old "/".
    app.post("/*", [this](HttpResponse* res, uWS::HttpRequest* req) {
        auto rd = std::make_shared<req_data>(*req, *res);

        log::debug(cat, "Incoming request initiated for {} from {}", rd->full_url, rd->remote_addr);
        res->onAborted([rd] {
            rd->_aborted = true;
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - rd->start)
                              .count();
            log::debug(cat, "Request from {} aborted by client {}ms after arrival", rd->remote_addr, ms);
        });
        res->onData([this, rd](std::string_view chunk, bool fin) {
            log::debug(cat, "Incoming {} chunk of size {}", fin ? "final" : "non-final", chunk.size());
            if (rd->_aborted)
                return;
            if (!chunk.empty())
                rd->body += chunk;

            log::debug(
                    cat,
                    "Incoming {} request ({}B, {}) for {} from {}",
                    rd->method,
                    rd->body.size(),
                    rd->content_type().value_or("[no content type]"),
                    rd->full_url,
                    rd->remote_addr);

            if (fin)
                handler(rd);
        });
    });

    app.listen(ip, port, LIBUS_LISTEN_EXCLUSIVE_PORT, [this](auto* s) { socket = s; });

    if (!socket)
        throw std::runtime_error{"Failed to bind listening socket to {}:{}"_format(ip, port)};

    app.run();

    socket = nullptr;
}

void Server::send_json_response(
        std::shared_ptr<req_data> rd,
        nlohmann::json response,
        std::unordered_map<std::string, std::string> headers,
        std::string http_status,
        bool force_close) {
    if (!headers.count("Content-Type") && !headers.count("Content-type") &&
        !headers.count("content-type"))
        headers["Content-Type"] = "application/json";

    return send_response_impl(
            std::move(rd),
            response.dump(),
            std::move(headers),
            std::move(http_status),
            force_close);
}

void Server::send_response(
        std::shared_ptr<req_data> rd,
        std::string response,
        std::unordered_map<std::string, std::string> headers,
        std::string http_status,
        bool force_close) {
    send_response_impl(
            std::move(rd),
            std::move(response),
            std::move(headers),
            std::move(http_status),
            force_close);
}

void Server::send_response_impl(
        std::shared_ptr<req_data> rd,
        std::string response,
        std::unordered_map<std::string, std::string> headers,
        std::string http_status,
        bool force_close,
        bool _loop) {
    if (std::this_thread::get_id() == tid) {
        // We're on the uWS loop thread, which is also where onAborted runs, so this check and
        // the subsequent res-> calls are serialized with any abort that has happened or could
        // happen up to this point.  After abort the HttpResponse is freed by uWS, so we must
        // not touch `res`.
        if (rd->_aborted) {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - rd->start)
                              .count();
            log::debug(
                    cat,
                    "Dropping response for aborted request from {} ({}ms after arrival)",
                    rd->remote_addr,
                    ms);
            return;
        }
        auto* r = rd->res;
        r->cork([r,
                 response = std::move(response),
                 headers = std::move(headers),
                 http_status = std::move(http_status),
                 force_close]() mutable {
            r->writeStatus(http_status);
            if (!headers.count("Content-Type") && !headers.count("Content-type") &&
                !headers.count("content-type"))
                r->writeHeader("Content-Type", "text/plain");
            for (const auto& [k, v] : headers)
                r->writeHeader(k, v);

            r->end(std::move(response), force_close);
        });
        return;
    }

    // Otherwise we're not in the uWS thread, so we have to recall ourself through the thread-safe
    // loop defer() method.
    if (_loop) {
        // If we get here somehow then we *did* queue via defer but somehow ended up still not in
        // the proper uWS thread and something is seriously wrong.
        log::critical(cat, "response queue loop detected, dropping response!");
        return;
    }
    app.getLoop()->defer([this,
                          rd = std::move(rd),
                          response = std::move(response),
                          headers = std::move(headers),
                          http_status = std::move(http_status),
                          force_close]() mutable {
        send_response_impl(
                std::move(rd),
                std::move(response),
                std::move(headers),
                std::move(http_status),
                force_close,
                true);
    });
}

void Server::stop(bool wait) {
    if (socket)
        app.getLoop()->defer([this] { us_listen_socket_close(/*ssl=*/false, socket); });

    if (wait)
        while (socket)
            std::this_thread::sleep_for(25ms);
}

}  // namespace solcache
