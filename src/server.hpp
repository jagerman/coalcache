#pragma once

#include <uWebSockets/App.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <nlohmann/json_fwd.hpp>

namespace solcache {

using HttpResponse = uWS::HttpResponse<false>;

struct req_data {
    req_data(uWS::HttpRequest& req, HttpResponse& response);

    // The uWS response.  Only safe to dereference on the uWS loop thread, and only while
    // `_aborted` is false.  After abort, uWS frees the underlying object — never touch it.
    HttpResponse* res;

    // Set true when uWS notifies us that the client connection was aborted.  Read it on the
    // uWS loop thread immediately before touching `res`.  Atomic so other threads can do a
    // best-effort short-circuit check, but the authoritative check must be on the uWS thread.
    std::atomic<bool> _aborted = false;

    // When this request arrived, for measuring how long until it aborts or gets a response.
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

    std::string method;
    std::string full_url, url;
    std::string remote_addr;
    std::string body;
    std::unordered_map<std::string, std::string> headers;  // Keys always lower case

    std::optional<std::string_view> content_type() const;
};

using ReqHandler = std::function<void(std::shared_ptr<req_data> req)>;

class Server {
    std::string ip;
    uint16_t port;
    uWS::App app;
    std::atomic<us_listen_socket_t*> socket = nullptr;
    ReqHandler handler;
    std::thread::id tid;

    void send_response_impl(
            std::shared_ptr<req_data> rd,
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
    // main uWS thread.  Silently drops the response if the connection was already aborted.
    void send_response(
            std::shared_ptr<req_data> rd,
            std::string response,
            std::unordered_map<std::string, std::string> headers = {},
            std::string http_status = "200 OK",
            bool force_close = false);
    void send_json_response(
            std::shared_ptr<req_data> rd,
            nlohmann::json response,
            std::unordered_map<std::string, std::string> headers = {},
            std::string http_status = "200 OK",
            bool force_close = false);


    void stop(bool wait = true);

    ~Server() { stop(true); }
};

}  // namespace solcache
