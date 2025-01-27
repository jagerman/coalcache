#pragma once

#include <curl/curl.h>

#include <chrono>
#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <oxen/quic/loop.hpp>

using namespace std::literals;

namespace solcache {

class Client {
  public:
    Client(std::string upstream_url, std::chrono::milliseconds upstream_timeout = 10s);

    ~Client();

    oxen::quic::Loop loop{};

    using response_handler_t = std::function<void(
            int status, std::unordered_map<std::string, std::string> headers, std::string body)>;

    void request(const nlohmann::json& body, response_handler_t response_handler);

    // Same as above, but takes a pre-dumped json body (unchecked):
    void request_jsonrpc(std::string predumped_jsonrpc, response_handler_t response_handler);

  private:
    const std::string upstream_url;
    const std::chrono::milliseconds upstream_timeout;

    event* ev_timeout;
    std::shared_ptr<const bool> alive = std::make_shared<bool>(true);
    CURLM* curl_multi;

    std::unordered_set<CURL*> active_reqs;

    friend struct curl_context;

    static void curl_perform_c(int fd, short event, void* cctx);
    static void on_timeout_c(evutil_socket_t fd, short events, void* arg);
    static int start_timeout_c(CURLM* multi, long timeout_ms, void* userp);
    static int handle_socket_c(CURL* easy, curl_socket_t s, int action, void* self, void* socketp);
    void check_multi_info();
};

}  // namespace solcache
