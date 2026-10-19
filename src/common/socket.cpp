#include "common/socket.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace eg {
namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
using SocketLength = int;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
constexpr int kSendFlags = 0;
#else
using NativeSocket = int;
using SocketLength = socklen_t;
constexpr NativeSocket kInvalidSocket = -1;
#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif
#endif

NativeSocket to_native(std::intptr_t handle) {
    return static_cast<NativeSocket>(handle);
}

std::intptr_t to_handle(NativeSocket socket) {
    return static_cast<std::intptr_t>(socket);
}

int last_socket_error() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

void close_native(NativeSocket socket) {
#ifdef _WIN32
    closesocket(socket);
#else
    ::close(socket);
#endif
}

std::string error_text(const std::string& context, int code) {
#ifdef _WIN32
    return context + ": winsock error " + std::to_string(code);
#else
    return context + ": " + std::strerror(code);
#endif
}

bool is_connect_in_progress(int code) {
#ifdef _WIN32
    return code == WSAEWOULDBLOCK || code == WSAEINPROGRESS || code == WSAEINTR;
#else
    return code == EINPROGRESS || code == EWOULDBLOCK || code == EINTR;
#endif
}

bool set_nonblocking(NativeSocket socket, bool enabled) {
#ifdef _WIN32
    u_long mode = enabled ? 1UL : 0UL;
    return ioctlsocket(socket, FIONBIO, &mode) == 0;
#else
    const int flags = fcntl(socket, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    const int updated = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(socket, F_SETFL, updated) == 0;
#endif
}

void enable_tcp_options(NativeSocket socket) {
    int one = 1;
    setsockopt(socket, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&one), sizeof(one));
    setsockopt(socket, SOL_SOCKET, SO_KEEPALIVE,
               reinterpret_cast<const char*>(&one), sizeof(one));
#ifdef SO_NOSIGPIPE
    // macOS/BSD do not provide MSG_NOSIGNAL.
    setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE,
               reinterpret_cast<const char*>(&one), sizeof(one));
#endif
}

// Returns 1 when ready, 0 on timeout, -1 on error.
int wait_socket(NativeSocket socket, bool for_write, int timeout_ms) {
    if (timeout_ms < 0) {
        timeout_ms = 0;
    }
    fd_set set;
    FD_ZERO(&set);
    FD_SET(socket, &set);

    timeval timeout{};
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;

#ifdef _WIN32
    const int rc = select(0, for_write ? nullptr : &set, for_write ? &set : nullptr, nullptr,
                          &timeout);
#else
    const int rc = select(socket + 1, for_write ? nullptr : &set, for_write ? &set : nullptr,
                          nullptr, &timeout);
#endif
    return rc;
}

std::uint16_t address_port(const sockaddr* address) {
    if (address->sa_family == AF_INET) {
        return ntohs(reinterpret_cast<const sockaddr_in*>(address)->sin_port);
    }
    if (address->sa_family == AF_INET6) {
        return ntohs(reinterpret_cast<const sockaddr_in6*>(address)->sin6_port);
    }
    return 0;
}

std::string address_text(const sockaddr* address) {
    char host[128]{};
    char service[32]{};
    const SocketLength length = address->sa_family == AF_INET
                                 ? static_cast<SocketLength>(sizeof(sockaddr_in))
                                 : static_cast<SocketLength>(sizeof(sockaddr_in6));
    if (getnameinfo(address, length, host, sizeof(host), service, sizeof(service),
                    NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
        return std::string(host) + ":" + service;
    }
    return "unknown";
}

void ensure_runtime() {
#ifdef _WIN32
    static std::once_flag once;
    static int result = 0;
    std::call_once(once, [] {
        WSADATA data{};
        result = WSAStartup(MAKEWORD(2, 2), &data);
    });
    if (result != 0) {
        throw std::runtime_error("WSAStartup failed with error " + std::to_string(result));
    }
#endif
}

}  // namespace

void socket_runtime_init() {
    ensure_runtime();
}

TcpSocket::~TcpSocket() {
    close();
}

TcpSocket::TcpSocket(TcpSocket&& other) noexcept : handle_(other.handle_) {
    other.handle_ = -1;
}

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        other.handle_ = -1;
    }
    return *this;
}

bool TcpSocket::valid() const {
    return handle_ != -1;
}

void TcpSocket::close() {
    if (handle_ != -1) {
        close_native(to_native(handle_));
        handle_ = -1;
    }
}

void TcpSocket::shutdown_both() {
    if (handle_ != -1) {
#ifdef _WIN32
        shutdown(to_native(handle_), SD_BOTH);
#else
        shutdown(to_native(handle_), SHUT_RDWR);
#endif
    }
}

bool TcpSocket::set_timeouts(int receive_ms, int send_ms) {
    if (handle_ == -1) {
        return false;
    }
#ifdef _WIN32
    DWORD recv_timeout = static_cast<DWORD>(std::max(0, receive_ms));
    DWORD send_timeout = static_cast<DWORD>(std::max(0, send_ms));
    const int recv_ok =
        setsockopt(to_native(handle_), SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&recv_timeout), sizeof(recv_timeout));
    const int send_ok =
        setsockopt(to_native(handle_), SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char*>(&send_timeout), sizeof(send_timeout));
    return recv_ok == 0 && send_ok == 0;
#else
    timeval recv_timeout{};
    recv_timeout.tv_sec = std::max(0, receive_ms) / 1000;
    recv_timeout.tv_usec = (std::max(0, receive_ms) % 1000) * 1000;
    timeval send_timeout{};
    send_timeout.tv_sec = std::max(0, send_ms) / 1000;
    send_timeout.tv_usec = (std::max(0, send_ms) % 1000) * 1000;
    const int recv_ok =
        setsockopt(to_native(handle_), SOL_SOCKET, SO_RCVTIMEO, &recv_timeout,
                   sizeof(recv_timeout));
    const int send_ok =
        setsockopt(to_native(handle_), SOL_SOCKET, SO_SNDTIMEO, &send_timeout,
                   sizeof(send_timeout));
    return recv_ok == 0 && send_ok == 0;
#endif
}

bool TcpSocket::send_all(const void* data, std::size_t size, std::string& error) {
    if (handle_ == -1) {
        error = "send on closed socket";
        return false;
    }
    const auto* bytes = static_cast<const char*>(data);
    std::size_t sent_total = 0;
    while (sent_total < size) {
        const std::size_t remaining = size - sent_total;
        const int chunk = static_cast<int>(
            std::min<std::size_t>(remaining, static_cast<std::size_t>(INT_MAX)));
#ifdef _WIN32
        const int sent = ::send(to_native(handle_), bytes + sent_total, chunk, 0);
#else
        const int sent = static_cast<int>(
            ::send(to_native(handle_), bytes + sent_total, chunk, kSendFlags));
#endif
        if (sent > 0) {
            sent_total += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent == 0) {
            error = "socket closed while sending";
            return false;
        }
        const int code = last_socket_error();
#ifdef _WIN32
        if (code == WSAEINTR) {
#else
        if (code == EINTR) {
#endif
            continue;
        }
        error = error_text("send failed", code);
        return false;
    }
    return true;
}

int TcpSocket::receive(void* data, std::size_t size, std::string& error) {
    if (handle_ == -1) {
        error = "receive on closed socket";
        return -1;
    }
    const int chunk = static_cast<int>(
        std::min<std::size_t>(size, static_cast<std::size_t>(INT_MAX)));
    for (;;) {
#ifdef _WIN32
        const int received = ::recv(to_native(handle_), static_cast<char*>(data), chunk, 0);
#else
        const int received = static_cast<int>(::recv(to_native(handle_), data, chunk, 0));
#endif
        if (received >= 0) {
            return received;
        }
        const int code = last_socket_error();
#ifdef _WIN32
        if (code == WSAEINTR) {
            continue;
        }
        if (code == WSAETIMEDOUT || code == WSAEWOULDBLOCK) {
            error = "receive timeout";
        } else {
            error = error_text("receive failed", code);
        }
#else
        if (code == EINTR) {
            continue;
        }
        if (code == EAGAIN || code == EWOULDBLOCK) {
            error = "receive timeout";
        } else {
            error = error_text("receive failed", code);
        }
#endif
        return -1;
    }
}

std::string TcpSocket::peer_address() const {
    if (handle_ == -1) {
        return "closed";
    }
    sockaddr_storage address{};
    SocketLength length = sizeof(address);
    if (getpeername(to_native(handle_), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        return "unknown";
    }
    return address_text(reinterpret_cast<const sockaddr*>(&address));
}

TcpSocket TcpSocket::connect_tcp(const std::string& host, std::uint16_t port,
                                 int timeout_ms, std::string& error) {
    ensure_runtime();
    if (timeout_ms <= 0) {
        timeout_ms = 3000;
    }
    const std::string node = host.empty() ? "127.0.0.1" : host;
    const std::string service = std::to_string(port);

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* results = nullptr;
    const int gai = getaddrinfo(node.c_str(), service.c_str(), &hints, &results);
    if (gai != 0) {
        error = "cannot resolve " + host + ": " + gai_strerror(gai);
        return {};
    }

    std::string last_error = "connection refused";
    for (addrinfo* it = results; it != nullptr; it = it->ai_next) {
        const NativeSocket raw = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (raw == kInvalidSocket) {
            last_error = error_text("socket creation failed", last_socket_error());
            continue;
        }

        if (!set_nonblocking(raw, true)) {
            last_error = error_text("failed to enter non-blocking mode", last_socket_error());
            close_native(raw);
            continue;
        }

        int rc = ::connect(raw, it->ai_addr, static_cast<SocketLength>(it->ai_addrlen));
        if (rc != 0) {
            const int code = last_socket_error();
            if (!is_connect_in_progress(code)) {
                last_error = error_text("connect failed", code);
                close_native(raw);
                continue;
            }

            const int ready = wait_socket(raw, true, timeout_ms);
            if (ready == 0) {
                last_error = "connect timed out";
                close_native(raw);
                continue;
            }
            if (ready < 0) {
                last_error = error_text("connect select failed", last_socket_error());
                close_native(raw);
                continue;
            }

            int socket_error = 0;
            SocketLength socket_error_size = sizeof(socket_error);
            if (getsockopt(raw, SOL_SOCKET, SO_ERROR,
                           reinterpret_cast<char*>(&socket_error),
                           &socket_error_size) != 0 ||
                socket_error != 0) {
                last_error = error_text("connect failed",
                                        socket_error != 0 ? socket_error
                                                          : last_socket_error());
                close_native(raw);
                continue;
            }
        }

        if (!set_nonblocking(raw, false)) {
            last_error = "failed to restore blocking mode";
            close_native(raw);
            continue;
        }

        enable_tcp_options(raw);
        freeaddrinfo(results);
        return TcpSocket(to_handle(raw));
    }

    freeaddrinfo(results);
    error = "cannot connect to " + host + ":" + service + ": " + last_error;
    return {};
}

TcpListener::~TcpListener() {
    close();
}

TcpListener::TcpListener(TcpListener&& other) noexcept
    : handle_(other.handle_), port_(other.port_) {
    other.handle_ = -1;
    other.port_ = 0;
}

TcpListener& TcpListener::operator=(TcpListener&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        port_ = other.port_;
        other.handle_ = -1;
        other.port_ = 0;
    }
    return *this;
}

bool TcpListener::valid() const {
    return handle_ != -1;
}

void TcpListener::close() {
    if (handle_ != -1) {
        close_native(to_native(handle_));
        handle_ = -1;
    }
}

bool TcpListener::listen_tcp(const std::string& host, std::uint16_t port, int backlog,
                             std::string& error) {
    ensure_runtime();
    close();

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    const char* node = host.empty() ? nullptr : host.c_str();
    const std::string service = std::to_string(port);
    addrinfo* results = nullptr;
    const int gai = getaddrinfo(node, service.c_str(), &hints, &results);
    if (gai != 0) {
        error = "cannot resolve listen address '" + host + "': " + gai_strerror(gai);
        return false;
    }

    std::string last_error = "bind failed";
    for (addrinfo* it = results; it != nullptr; it = it->ai_next) {
        const NativeSocket raw = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (raw == kInvalidSocket) {
            last_error = error_text("socket creation failed", last_socket_error());
            continue;
        }

        int one = 1;
        setsockopt(raw, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&one), sizeof(one));

        if (bind(raw, it->ai_addr, static_cast<SocketLength>(it->ai_addrlen)) != 0) {
            last_error = error_text("bind failed", last_socket_error());
            close_native(raw);
            continue;
        }
        if (listen(raw, std::max(1, backlog)) != 0) {
            last_error = error_text("listen failed", last_socket_error());
            close_native(raw);
            continue;
        }

        sockaddr_storage bound{};
        SocketLength bound_length = sizeof(bound);
        if (getsockname(raw, reinterpret_cast<sockaddr*>(&bound), &bound_length) == 0) {
            port_ = address_port(reinterpret_cast<const sockaddr*>(&bound));
        } else {
            port_ = port;
        }
        handle_ = to_handle(raw);
        freeaddrinfo(results);
        return true;
    }

    freeaddrinfo(results);
    error = "cannot listen on " + host + ":" + service + ": " + last_error;
    return false;
}

std::optional<TcpSocket> TcpListener::accept_for(int timeout_ms, std::string& error) {
    if (handle_ == -1) {
        error = "accept on closed listener";
        return std::nullopt;
    }
    const int ready = wait_socket(to_native(handle_), false, timeout_ms);
    if (ready == 0) {
        return std::nullopt;
    }
    if (ready < 0) {
        const int code = last_socket_error();
#ifdef _WIN32
        if (code == WSAEINTR) {
            return std::nullopt;
        }
#else
        if (code == EINTR) {
            return std::nullopt;
        }
#endif
        error = error_text("accept select failed", code);
        return std::nullopt;
    }

    sockaddr_storage address{};
    SocketLength length = sizeof(address);
    const NativeSocket client =
        accept(to_native(handle_), reinterpret_cast<sockaddr*>(&address), &length);
    if (client == kInvalidSocket) {
        error = error_text("accept failed", last_socket_error());
        return std::nullopt;
    }

    enable_tcp_options(client);
    return TcpSocket(to_handle(client));
}

}  // namespace eg
