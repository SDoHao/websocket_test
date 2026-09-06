#pragma once
#include <string>
#include <memory>
#include <cstdint>

// 👇 跨平台头文件引入
#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <sys/types.h>
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <sys/types.h>
    #include <fcntl.h>      // fcntl() 函数，用于设置非阻塞（Linux）
#endif

namespace net {
class TcpSocket {
    int fd_ = -1;
public:
    TcpSocket() = default;
    explicit TcpSocket(int fd) : fd_(fd) {}
    ~TcpSocket();

    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;
    TcpSocket(TcpSocket&& o) noexcept;
    TcpSocket& operator=(TcpSocket&& o) noexcept;

    int fd() const { return fd_; }
    bool isValid() const { return fd_ >= 0; }
    // 设置为非阻塞模式（reactor/epoll 模式必需）。
    // 非阻塞含义：recv/send 数据没齐时不等待，立即返回，稍后重试。
    bool setNonBlocking();
    bool bindAndListen(const std::string& ip, uint16_t port);
    bool connect(const std::string& host, uint16_t port);
    std::unique_ptr<TcpSocket> accept();
    bool recvAll(uint8_t* buf, size_t n);
    bool sendAll(const uint8_t* buf, size_t n);
    void close();
};
}
