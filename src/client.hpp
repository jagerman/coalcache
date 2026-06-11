#pragma once

#include <curl/curl.h>
#include <event2/event.h>

#include <chrono>
#include <memory>
#include <oxen/quic/loop.hpp>
#include <unordered_set>
#include <vector>

using namespace std::literals;

namespace solcache {

class Client {
  public:
    explicit Client(std::chrono::milliseconds upstream_timeout = 10s);

    ~Client();

    oxen::quic::Loop loop{};

    using response_handler_t = std::function<void(
            int status, std::unordered_map<std::string, std::string> headers, std::string body)>;

    // POST `body` to an arbitrary upstream URL with optional extra headers (e.g. provider auth).
    // The body is sent verbatim with a JSON content type.  This is the only forwarding primitive;
    // callers (ProxyCache, CoalCache, ReqCache) supply the per-request upstream URL.
    void post(std::string url,
              std::string body,
              std::vector<std::string> extra_headers,
              response_handler_t response_handler);

  private:
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
