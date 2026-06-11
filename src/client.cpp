#include "client.hpp"

#include <chrono>
#include <oxen/log.hpp>

namespace solcache {

namespace log = oxen::log;
static auto cat = log::Cat("client");

namespace {

    extern "C" size_t data_accumulator(
            char* ptr, size_t size, size_t nmemb, std::string* userdata) {
        size_t sz = size * nmemb;
        userdata->append(ptr, sz);
        return sz;
    }

    struct req_data {
        Client::response_handler_t response_handler;
        std::string url;
        std::string req_body;
        std::string resp_body;
        curl_slist* headers = nullptr;

        ~req_data() {
            if (headers)
                curl_slist_free_all(headers);
        }
    };

}  // namespace

Client::Client(std::chrono::milliseconds upstream_timeout) :
        upstream_timeout{upstream_timeout},
        ev_timeout{evtimer_new(loop.get_event_base(), Client::on_timeout_c, this)} {

    curl_multi = curl_multi_init();
    curl_multi_setopt(curl_multi, CURLMOPT_SOCKETDATA, this);
    curl_multi_setopt(curl_multi, CURLMOPT_SOCKETFUNCTION, Client::handle_socket_c);
    curl_multi_setopt(curl_multi, CURLMOPT_TIMERDATA, this);
    curl_multi_setopt(curl_multi, CURLMOPT_TIMERFUNCTION, Client::start_timeout_c);
}

Client::~Client() {
    loop.call_get([this] {
        alive.reset();
        for (auto* handle : active_reqs) {
            req_data* rd = nullptr;
            curl_easy_getinfo(handle, CURLINFO_PRIVATE, &rd);
            if (rd)
                delete rd;
            curl_multi_remove_handle(curl_multi, handle);
        }
        active_reqs.clear();
        curl_multi_cleanup(curl_multi);
        event_free(ev_timeout);
    });
}

struct curl_context {
    Client& client;
    curl_socket_t sockfd;
    event* evt;

    curl_context(Client& client, curl_socket_t fd) :
            client{client},
            sockfd{fd},
            evt{event_new(client.loop.get_event_base(), sockfd, 0, Client::curl_perform_c, this)} {}
    ~curl_context() {
        event_del(evt);
        event_free(evt);
    }
};

void Client::curl_perform_c(int /*fd*/, short event, void* cctx) {
    log::trace(cat, "{}, {}", __func__, cctx);
    int running_handles;
    int flags = 0;
    auto* ctx = static_cast<curl_context*>(cctx);
    auto& client = ctx->client;
    log::trace(cat, "{}, {}", __func__, cctx);

    if (event & EV_READ)
        flags |= CURL_CSELECT_IN;
    if (event & EV_WRITE)
        flags |= CURL_CSELECT_OUT;

    log::trace(
            cat, "{}, {}, {}, {}", __func__, cctx, (const void*)(client.curl_multi), ctx->sockfd);
    curl_multi_socket_action(client.curl_multi, ctx->sockfd, flags, &running_handles);
    // Can't use `ctx` anymore because it might have been destroyed during the above call (typically
    // because the socket is no longer being polled).

    log::trace(cat, "{}, {}", __func__, cctx);
    client.check_multi_info();
}
void Client::on_timeout_c(evutil_socket_t /*fd*/, short /*events*/, void* arg) {
    auto& client = *static_cast<Client*>(arg);
    int running_handles;
    curl_multi_socket_action(client.curl_multi, CURL_SOCKET_TIMEOUT, 0, &running_handles);
    client.check_multi_info();
}
int Client::start_timeout_c(CURLM* /*multi*/, long timeout_ms, void* userp) {
    auto& client = *static_cast<Client*>(userp);
    evtimer_del(client.ev_timeout);
    if (timeout_ms >= 0) {
        timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        if (timeout_ms == 0)
            tv.tv_usec = 1; /* 0 means call socket_action asap */
        evtimer_add(client.ev_timeout, &tv);
    }
    return 0;
}
int Client::handle_socket_c(
        CURL* /*easy*/, curl_socket_t s, int action, void* self, void* socketp) {
    auto& client = *static_cast<Client*>(self);
    auto* curl_ctx = static_cast<curl_context*>(socketp);
    int events = 0;

    switch (action) {
        case CURL_POLL_IN:
        case CURL_POLL_OUT:
        case CURL_POLL_INOUT:
            if (!curl_ctx) {
                curl_ctx = new curl_context{client, s};
                curl_multi_assign(client.curl_multi, s, curl_ctx);
            }

            if (action != CURL_POLL_IN)
                events |= EV_WRITE;
            if (action != CURL_POLL_OUT)
                events |= EV_READ;

            events |= EV_PERSIST;

            event_del(curl_ctx->evt);
            event_assign(
                    curl_ctx->evt,
                    client.loop.get_event_base(),
                    curl_ctx->sockfd,
                    events,
                    Client::curl_perform_c,
                    curl_ctx);
            event_add(curl_ctx->evt, NULL);

            break;
        case CURL_POLL_REMOVE:
            if (curl_ctx) {
                curl_multi_assign(client.curl_multi, s, nullptr);
                delete curl_ctx;
            }
            break;
        default: log::error(cat, "Unexpected socket action {} from libcurl\n", action);
    }

    return 0;
}

void Client::check_multi_info() {
    int pending;
    while (CURLMsg* message = curl_multi_info_read(curl_multi, &pending)) {
        if (message->msg == CURLMSG_DONE) {
            CURL* e = message->easy_handle;

            req_data* rd = nullptr;
            curl_easy_getinfo(e, CURLINFO_PRIVATE, &rd);
            if (rd) {
                if (rd->response_handler) {
                    try {
                        std::unordered_map<std::string, std::string> headers;
                        for (curl_header* h = nullptr;
                             (h = curl_easy_nextheader(e, CURLH_HEADER, -1, h));)
                            headers.emplace(h->name, h->value);
                        long code = 0;
                        curl_easy_getinfo(e, CURLINFO_RESPONSE_CODE, &code);
                        rd->response_handler(code, std::move(headers), std::move(rd->resp_body));
                    } catch (const std::exception& e) {
                        log::warning(cat, "Response handler raised an exception: {}\n", e.what());
                    }
                }
                delete rd;
            }

            active_reqs.erase(e);
            curl_multi_remove_handle(curl_multi, e);
            curl_easy_cleanup(e);
        } else {
            log::error(
                    cat,
                    "Unexpected/unhandled curl-multi message type: {}",
                    static_cast<int>(message->msg));
        }
    }
}

void Client::post(
        std::string url,
        std::string body,
        std::vector<std::string> extra_headers,
        response_handler_t response_handler) {

    log::debug(cat, "request called");
    auto* rd = new req_data{};
    rd->response_handler = std::move(response_handler);
    rd->url = std::move(url);
    rd->req_body = std::move(body);

    // Build the header list now: this is just a linked-list of strings and touches neither the
    // curl-multi handle nor the loop, so there's no need to defer it to the loop thread.
    for (const char* header : {"Content-Type: application/json", "User-Agent: coalcache/0"})
        rd->headers = curl_slist_append(rd->headers, header);
    for (const auto& header : extra_headers)
        rd->headers = curl_slist_append(rd->headers, header.c_str());

    loop.call([this, rd]() mutable {
        log::debug(cat, "initiating request to {}", rd->url);
        CURL* handle = curl_easy_init();
        curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 1);
        curl_easy_setopt(handle, CURLOPT_TCP_KEEPALIVE, 1);
        curl_easy_setopt(handle, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(handle, CURLOPT_POST, 1);
        curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, upstream_timeout.count());
        curl_easy_setopt(handle, CURLOPT_URL, rd->url.c_str());
        curl_easy_setopt(handle, CURLOPT_PRIVATE, rd);

        curl_easy_setopt(handle, CURLOPT_HTTPHEADER, rd->headers);

        curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, rd->req_body.size());
        curl_easy_setopt(handle, CURLOPT_POSTFIELDS, rd->req_body.data());

        curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, &data_accumulator);
        curl_easy_setopt(handle, CURLOPT_WRITEDATA, &rd->resp_body);

        curl_multi_add_handle(curl_multi, handle);
    });
}

}  // namespace solcache
