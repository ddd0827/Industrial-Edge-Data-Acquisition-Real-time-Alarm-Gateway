#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common/socket.h"

namespace eg {

struct HttpRequest {
    std::string method;
    std::string path;
    std::string target;
    std::unordered_map<std::string, std::string> query;

    bool is_head() const { return method == "HEAD"; }
};

struct HttpResponse {
    int status = 200;
    std::string content_type = "text/plain; charset=utf-8";
    std::string body;
};

class HttpServer {
public:
    using RequestHandler = std::function<HttpResponse(const HttpRequest&)>;
    using StreamHandler = std::function<void(const HttpRequest&, TcpSocket&,
                                             const std::atomic<bool>&)>;

    HttpServer(std::string host, std::uint16_t port, std::string web_root,
               RequestHandler request_handler, StreamHandler stream_handler);
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    bool start(std::string& error);
    void stop();
    bool running() const { return running_.load(std::memory_order_acquire); }
    std::uint16_t port() const { return listener_.port(); }

private:
    struct ConnectionThread {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };

    void accept_loop();
    void reap_finished_connections();
    void handle_connection(TcpSocket socket);
    bool read_request(TcpSocket& socket, HttpRequest& request);
    void send_response(TcpSocket& socket, const HttpRequest& request,
                       const HttpResponse& response);
    HttpResponse serve_static(const HttpRequest& request) const;

    std::string host_;
    std::uint16_t requested_port_ = 0;
    std::string web_root_;
    RequestHandler request_handler_;
    StreamHandler stream_handler_;

    TcpListener listener_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::thread accept_thread_;
    std::vector<ConnectionThread> connections_;
};

}  // namespace eg
