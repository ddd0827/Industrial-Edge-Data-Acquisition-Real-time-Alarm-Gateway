#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace eg {

void socket_runtime_init();

class TcpSocket {
public:
    TcpSocket() noexcept = default;
    ~TcpSocket();

    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;
    TcpSocket(TcpSocket&& other) noexcept;
    TcpSocket& operator=(TcpSocket&& other) noexcept;

    bool valid() const;
    void close();
    void shutdown_both();
    bool set_timeouts(int receive_ms, int send_ms);

    bool send_all(const void* data, std::size_t size, std::string& error);
    // Returns the number of bytes read, 0 on orderly shutdown, or -1 on error.
    int receive(void* data, std::size_t size, std::string& error);

    std::string peer_address() const;

    static TcpSocket connect_tcp(const std::string& host, std::uint16_t port,
                                 int timeout_ms, std::string& error);

private:
    friend class TcpListener;
    explicit TcpSocket(std::intptr_t handle) noexcept : handle_(handle) {}

    std::intptr_t handle_ = -1;
};

class TcpListener {
public:
    TcpListener() noexcept = default;
    ~TcpListener();

    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;
    TcpListener(TcpListener&& other) noexcept;
    TcpListener& operator=(TcpListener&& other) noexcept;

    bool listen_tcp(const std::string& host, std::uint16_t port, int backlog,
                    std::string& error);
    // A nullopt result with an empty error string means the timeout elapsed.
    std::optional<TcpSocket> accept_for(int timeout_ms, std::string& error);
    std::uint16_t port() const { return port_; }
    bool valid() const;
    void close();

private:
    std::intptr_t handle_ = -1;
    std::uint16_t port_ = 0;
};

}  // namespace eg
