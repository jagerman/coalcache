#include "server.hpp"

#include <fmt/core.h>

#include <exception>
#include <future>
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

req_data::req_data(uWS::HttpRequest& req, HttpResponse& res) :
        method{req.getCaseSensitiveMethod()},
        full_url{req.getFullUrl()},
        url{req.getUrl()},
        remote_addr{res.getRemoteAddressAsText()} {
    for (const auto& [header, value] : req)
        headers.emplace(lc_string(header), value);
}

std::optional<std::string_view> req_data::content_type() const {
    if (auto it = headers.find("content-type"); it != headers.end())
        return it->second;
    return std::nullopt;
}

void Server::run() {

    tid = std::this_thread::get_id();

    app.post("/", [this](HttpResponse* res, uWS::HttpRequest* req) {
        auto rd = std::make_shared<req_data>(*req, *res);

        log::debug(cat, "Incoming request initiated for {} from {}", rd->full_url, rd->remote_addr);
        res->onAborted([rd] { rd->_aborted = true; });
        res->onData([this, res, rd = std::move(rd)](std::string_view chunk, bool fin) mutable {
            log::debug(cat, "Incoming {} chunk of size {}, rd {}", fin ? "final" : "non-final", chunk.size(), rd ? "good" : "EMPTY");
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

            if (fin && !rd->_aborted) {
                handler(std::move(*rd), res);
            }
        });
    });

    app.listen(ip, port, LIBUS_LISTEN_EXCLUSIVE_PORT, [this](auto* s) { socket = s; });

    if (!socket)
        throw std::runtime_error{"Failed to bind listening socket to {}:{}"_format(ip, port)};

    app.run();

    socket = nullptr;
}

void Server::send_json_response(
        HttpResponse* r,
        nlohmann::json response,
        std::unordered_map<std::string, std::string> headers,
        std::string http_status,
        bool force_close) {
    if (!headers.count("Content-Type") && !headers.count("Content-type") &&
        !headers.count("content-type"))
        headers["Content-Type"] = "application/json";

    return send_response_impl(
            r, response.dump(), std::move(headers), std::move(http_status), force_close);
}

void Server::send_response(
        HttpResponse* r,
        std::string response,
        std::unordered_map<std::string, std::string> headers,
        std::string http_status,
        bool force_close) {
    app.getLoop()->defer([this,
                          r,
                          response = std::move(response),
                          headers = std::move(headers),
                          http_status = std::move(http_status),
                          force_close]() mutable {
        send_response_impl(
                r, std::move(response), std::move(headers), std::move(http_status), force_close);
    });
}

void Server::send_response_impl(
        HttpResponse* r,
        std::string response,
        std::unordered_map<std::string, std::string> headers,
        std::string http_status,
        bool force_close,
        bool _loop) {
    if (std::this_thread::get_id() == tid) {
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
        // If we get here somehow then we *did* queue via defer but someone end up still not in the
        // proper uWS thread and something is seriously wrong.
        log::critical(cat, "response queue loop detected, dropping response!");
        return;
    }
    app.getLoop()->defer([this,
                          r,
                          response = std::move(response),
                          headers = std::move(headers),
                          http_status = std::move(http_status),
                          force_close]() mutable {
        send_response_impl(
                r,
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
