#include "core/http/http_server.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "common/log.h"
#include "common/util.h"

namespace eg {
namespace {

constexpr std::size_t kMaxRequestBytes = 16 * 1024;
constexpr std::size_t kMaxStaticFileBytes = 8 * 1024 * 1024;
constexpr const char* kStreamPath = "/api/v1/stream";

int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + (ch - 'a');
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + (ch - 'A');
    }
    return -1;
}

bool url_decode(std::string_view input, bool plus_as_space, std::string& output) {
    output.clear();
    output.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        const char ch = input[i];
        if (ch == '%') {
            if (i + 2 >= input.size()) {
                return false;
            }
            const int high = hex_value(input[i + 1]);
            const int low = hex_value(input[i + 2]);
            if (high < 0 || low < 0) {
                return false;
            }
            output.push_back(static_cast<char>((high << 4) | low));
            i += 2;
        } else if (ch == '+' && plus_as_space) {
            output.push_back(' ');
        } else {
            output.push_back(ch);
        }
    }
    return true;
}

std::string status_text(int status) {
    switch (status) {
        case 200:
            return "OK";
        case 204:
            return "No Content";
        case 304:
            return "Not Modified";
        case 400:
            return "Bad Request";
        case 403:
            return "Forbidden";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 413:
            return "Payload Too Large";
        case 500:
            return "Internal Server Error";
        case 503:
            return "Service Unavailable";
        default:
            return "Error";
    }
}

std::string extension_of(const std::filesystem::path& path) {
    const std::string extension = to_lower(path.extension().u8string());
    return extension;
}

std::string content_type_for(const std::filesystem::path& path) {
    const std::string extension = extension_of(path);
    if (extension == ".html" || extension == ".htm") {
        return "text/html; charset=utf-8";
    }
    if (extension == ".css") {
        return "text/css; charset=utf-8";
    }
    if (extension == ".js" || extension == ".mjs") {
        return "text/javascript; charset=utf-8";
    }
    if (extension == ".json") {
        return "application/json; charset=utf-8";
    }
    if (extension == ".svg") {
        return "image/svg+xml";
    }
    if (extension == ".png") {
        return "image/png";
    }
    if (extension == ".ico") {
        return "image/x-icon";
    }
    if (extension == ".woff2") {
        return "font/woff2";
    }
    if (extension == ".txt") {
        return "text/plain; charset=utf-8";
    }
    return "application/octet-stream";
}

}  // namespace

HttpServer::HttpServer(std::string host, std::uint16_t port, std::string web_root,
                       RequestHandler request_handler, StreamHandler stream_handler)
    : host_(std::move(host)),
      requested_port_(port),
      web_root_(std::move(web_root)),
      request_handler_(std::move(request_handler)),
      stream_handler_(std::move(stream_handler)) {}

HttpServer::~HttpServer() {
    stop();
}

bool HttpServer::start(std::string& error) {
    if (running_.load(std::memory_order_acquire)) {
        return true;
    }
    if (!listener_.listen_tcp(host_, requested_port_, 128, error)) {
        return false;
    }

    stop_requested_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    try {
        accept_thread_ = std::thread([this] { accept_loop(); });
    } catch (const std::exception& ex) {
        running_.store(false, std::memory_order_release);
        listener_.close();
        error = std::string("cannot start HTTP accept thread: ") + ex.what();
        return false;
    }

    EG_INFO("HTTP server listening on http://%s:%u/", host_.c_str(),
            static_cast<unsigned>(listener_.port()));
    return true;
}

void HttpServer::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    stop_requested_.store(true, std::memory_order_release);
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    listener_.close();

    // Connection threads observe either the socket timeout or the shared stop
    // flag (SSE handlers receive it directly).
    for (ConnectionThread& connection : connections_) {
        if (connection.thread.joinable()) {
            connection.thread.join();
        }
    }
    connections_.clear();
    EG_INFO("HTTP server stopped");
}

void HttpServer::reap_finished_connections() {
    for (auto it = connections_.begin(); it != connections_.end();) {
        if (it->done->load(std::memory_order_acquire)) {
            if (it->thread.joinable()) {
                it->thread.join();
            }
            it = connections_.erase(it);
        } else {
            ++it;
        }
    }
}

void HttpServer::accept_loop() {
    while (!stop_requested_.load(std::memory_order_acquire)) {
        std::string accept_error;
        auto client = listener_.accept_for(200, accept_error);
        reap_finished_connections();
        if (!client.has_value()) {
            if (!accept_error.empty() &&
                !stop_requested_.load(std::memory_order_acquire)) {
                EG_DEBUG("HTTP accept: %s", accept_error.c_str());
            }
            continue;
        }

        client->set_timeouts(2000, 5000);
        auto done = std::make_shared<std::atomic<bool>>(false);
        try {
            ConnectionThread connection;
            connection.done = done;
            connection.thread =
                std::thread([this, socket = std::move(*client), done]() mutable {
                    handle_connection(std::move(socket));
                    done->store(true, std::memory_order_release);
                });
            connections_.push_back(std::move(connection));
        } catch (const std::exception& ex) {
            EG_WARN("cannot create HTTP connection thread: %s", ex.what());
        }
    }
}

bool HttpServer::read_request(TcpSocket& socket, HttpRequest& request) {
    std::string buffer;
    buffer.reserve(2048);
    std::size_t header_end = std::string::npos;
    char chunk[2048];

    while (header_end == std::string::npos) {
        std::string error;
        const int count = socket.receive(chunk, sizeof(chunk), error);
        if (count <= 0) {
            return false;
        }
        buffer.append(chunk, static_cast<std::size_t>(count));
        if (buffer.size() > kMaxRequestBytes) {
            return false;
        }
        header_end = buffer.find("\r\n\r\n");
    }

    const std::string_view headers = std::string_view(buffer).substr(0, header_end);
    const std::size_t line_end = headers.find("\r\n");
    const std::string_view request_line =
        line_end == std::string_view::npos ? headers : headers.substr(0, line_end);
    if (request_line.empty() || request_line.size() > 4096) {
        return false;
    }

    std::istringstream line{std::string(request_line)};
    std::string version;
    line >> request.method >> request.target >> version;
    if (request.method.empty() || request.target.empty() || version.empty() ||
        !starts_with(version, "HTTP/1.")) {
        return false;
    }

    const std::size_t query_pos = request.target.find('?');
    const std::string_view raw_path = std::string_view(request.target).substr(0, query_pos);
    if (raw_path.empty() || raw_path.front() != '/') {
        return false;
    }
    if (!url_decode(raw_path, false, request.path) || request.path.empty() ||
        request.path.front() != '/') {
        return false;
    }
    if (request.path.find('\0') != std::string::npos) {
        return false;
    }

    if (query_pos != std::string::npos) {
        const std::string_view query =
            std::string_view(request.target).substr(query_pos + 1);
        std::size_t start = 0;
        while (start <= query.size()) {
            const std::size_t amp = query.find('&', start);
            const std::string_view pair =
                query.substr(start, amp == std::string_view::npos
                                        ? std::string_view::npos
                                        : amp - start);
            if (!pair.empty()) {
                const std::size_t equals = pair.find('=');
                const std::string_view raw_key = pair.substr(0, equals);
                const std::string_view raw_value =
                    equals == std::string_view::npos ? std::string_view{}
                                                     : pair.substr(equals + 1);
                std::string key;
                std::string value;
                if (!url_decode(raw_key, true, key) ||
                    !url_decode(raw_value, true, value)) {
                    return false;
                }
                if (!key.empty()) {
                    request.query[key] = value;
                }
            }
            if (amp == std::string_view::npos) {
                break;
            }
            start = amp + 1;
        }
    }
    return true;
}

void HttpServer::send_response(TcpSocket& socket, const HttpRequest& request,
                               const HttpResponse& response) {
    std::ostringstream headers;
    headers << "HTTP/1.1 " << response.status << " " << status_text(response.status)
            << "\r\n";
    headers << "Content-Type: " << response.content_type << "\r\n";
    headers << "Content-Length: " << response.body.size() << "\r\n";
    headers << "Connection: close\r\n";
    headers << "Cache-Control: no-store\r\n";
    headers << "X-Content-Type-Options: nosniff\r\n";
    headers << "\r\n";

    std::string wire = headers.str();
    if (!request.is_head()) {
        wire.append(response.body);
    }
    std::string error;
    if (!socket.send_all(wire.data(), wire.size(), error)) {
        EG_DEBUG("HTTP response send failed: %s", error.c_str());
    }
}

HttpResponse HttpServer::serve_static(const HttpRequest& request) const {
    HttpResponse response;
    if (web_root_.empty()) {
        response.status = 404;
        response.body = "web root is not configured\n";
        return response;
    }

    std::string relative = request.path;
    if (relative == "/") {
        relative = "/index.html";
    }
    if (relative.find('\\') != std::string::npos) {
        response.status = 403;
        response.body = "forbidden\n";
        return response;
    }

    std::vector<std::string> segments = split(
        std::string_view(relative).substr(1), '/', false);
    std::filesystem::path root = std::filesystem::u8path(web_root_);
    std::filesystem::path candidate = root;
    for (const std::string& segment : segments) {
        if (segment == "." || segment == "..") {
            response.status = 403;
            response.body = "forbidden\n";
            return response;
        }
        candidate /= std::filesystem::u8path(segment);
    }

    std::error_code ec;
    const std::filesystem::path canonical_root =
        std::filesystem::weakly_canonical(std::filesystem::absolute(root), ec);
    if (ec) {
        response.status = 404;
        response.body = "not found\n";
        return response;
    }
    const std::filesystem::path canonical_file =
        std::filesystem::weakly_canonical(candidate, ec);
    if (ec) {
        response.status = 404;
        response.body = "not found\n";
        return response;
    }

    const std::string root_string = canonical_root.generic_u8string();
    const std::string file_string = canonical_file.generic_u8string();
    const std::string prefix =
        root_string.back() == '/' ? root_string : root_string + "/";
    if (!starts_with(file_string, prefix)) {
        response.status = 403;
        response.body = "forbidden\n";
        return response;
    }

    if (!std::filesystem::is_regular_file(canonical_file, ec)) {
        response.status = 404;
        response.body = "not found\n";
        return response;
    }
    const std::uintmax_t size = std::filesystem::file_size(canonical_file, ec);
    if (ec || size > kMaxStaticFileBytes) {
        response.status = 404;
        response.body = "not found\n";
        return response;
    }

    std::vector<std::uint8_t> data;
    std::string read_error;
    if (!read_binary_file(canonical_file.u8string(), data, read_error)) {
        response.status = 404;
        response.body = "not found\n";
        return response;
    }
    response.status = 200;
    response.content_type = content_type_for(canonical_file);
    response.body.assign(reinterpret_cast<const char*>(data.data()), data.size());
    return response;
}

void HttpServer::handle_connection(TcpSocket socket) {
    HttpRequest request;
    if (!read_request(socket, request)) {
        HttpResponse response;
        response.status = 400;
        response.body = "bad request\n";
        send_response(socket, request, response);
        return;
    }

    if (request.method != "GET" && request.method != "HEAD") {
        HttpResponse response;
        response.status = 405;
        response.body = "method not allowed\n";
        response.content_type = "text/plain; charset=utf-8";
        send_response(socket, request, response);
        return;
    }

    if (request.method == "GET" && request.path == kStreamPath && stream_handler_) {
        try {
            stream_handler_(request, socket, stop_requested_);
        } catch (const std::exception& ex) {
            EG_DEBUG("SSE handler failed: %s", ex.what());
        }
        socket.close();
        return;
    }

    HttpResponse response;
    try {
        if (starts_with(request.path, "/api/")) {
            if (request_handler_) {
                response = request_handler_(request);
            } else {
                response.status = 404;
                response.content_type = "application/json; charset=utf-8";
                response.body = "{\"error\":\"not found\"}";
            }
        } else {
            response = serve_static(request);
        }
    } catch (const std::exception& ex) {
        EG_ERROR("HTTP handler failed for %s: %s", request.path.c_str(), ex.what());
        response = HttpResponse{};
        response.status = 500;
        response.content_type = "application/json; charset=utf-8";
        response.body = "{\"error\":\"internal server error\"}";
    }

    send_response(socket, request, response);
}

}  // namespace eg
