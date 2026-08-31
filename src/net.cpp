#include "net.h"
#include <cstring>

// 👇 跨平台关闭 Socket 宏
#ifdef _WIN32
    #define CLOSE_SOCKET(fd) ::closesocket(fd)
#else
    #include <unistd.h>
    #define CLOSE_SOCKET(fd) ::close(fd)
#endif

namespace net {
TcpSocket::~TcpSocket() { close(); }
TcpSocket::TcpSocket(TcpSocket&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
TcpSocket& TcpSocket::operator=(TcpSocket&& o) noexcept {
    if (this != &o) { close(); fd_ = o.fd_; o.fd_ = -1; }
    return *this;
}

bool TcpSocket::bindAndListen(const std::string& ip, uint16_t port) {
    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) return false;
    int opt = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
    
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(ip.c_str());
    addr.sin_port = htons(port);
    
    if (::bind(fd_, (sockaddr*)&addr, sizeof(addr)) < 0) return false;
    return ::listen(fd_, 128) == 0;
}

bool TcpSocket::connect(const std::string& host, uint16_t port) {
    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        close();
        return false;
    }
    if (::connect(fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        close();
        return false;
    }
    return true;
}

std::unique_ptr<TcpSocket> TcpSocket::accept() {
    sockaddr_in peer{};
    socklen_t len = sizeof(peer);
    int client_fd = ::accept(fd_, (sockaddr*)&peer, &len);
    if (client_fd < 0) return nullptr;
    return std::make_unique<TcpSocket>(client_fd);
}

bool TcpSocket::recvAll(uint8_t* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = ::recv(fd_, (char*)buf + got, n - got, 0);
        if (r <= 0) return false;
        got += r;
    }
    return true;
}

bool TcpSocket::sendAll(const uint8_t* buf, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        ssize_t r = ::send(fd_, (const char*)buf + sent, n - sent, 0);
        if (r <= 0) return false;
        sent += r;
    }
    return true;
}

void TcpSocket::close() {
    if (fd_ >= 0) { 
        CLOSE_SOCKET(fd_); // 使用跨平台宏
        fd_ = -1; 
    }
}
}