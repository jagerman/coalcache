#pragma once

#include <uWebSockets/App.h>

#include <functional>
#include <nlohmann/json_fwd.hpp>

namespace solcache {

using HttpResponse = uWS::HttpResponse<false>;

struct req_data {
    req_data(uWS::HttpRequest& req, HttpResponse& res);
    bool _aborted = false;
    std::string method;
    std::string full_url, url;
    std::string remote_addr;
    std::string body;
    std::unordered_map<std::string, std::string> headers;  // Keys always lower case

    std::optional<std::string_view> content_type() const;
};

using ReqHandler = std::function<void(req_data&& req, HttpResponse* resp)>;

class Server {
    std::string ip;
    uint16_t port;
    uWS::App app;
    std::atomic<us_listen_socket_t*> socket = nullptr;
    ReqHandler handler;
    std::thread::id tid;

    void send_response_impl(
            HttpResponse* r,
            std::string response,
            std::unordered_map<std::string, std::string> headers,
            std::string http_status,
            bool force_close,
            bool _loop = false);

  public:
    Server(std::string ip, uint16_t port, ReqHandler handler) :
            ip{std::move(ip)}, port{port}, handler{std::move(handler)} {}

    void run();

    // This sends or queues a response, based on whether it is called from within or outside the
    // main uWS thread.
    void send_response(
            HttpResponse* r,
            std::string response,
            std::unordered_map<std::string, std::string> headers = {},
            std::string http_status = "200 OK",
            bool force_close = false);
    void send_json_response(
            HttpResponse* r,
            nlohmann::json response,
            std::unordered_map<std::string, std::string> headers = {},
            std::string http_status = "200 OK",
            bool force_close = false);


    void stop(bool wait = true);

    ~Server() { stop(true); }
};

}  // namespace solcache
