#pragma once
#include "net.h"
#include <functional>
#include <vector>
#include <mutex>

namespace ws {
void log(const std::string& s);

enum class Opcode : uint8_t {
    Cont = 0x0, Text = 0x1, Bin = 0x2,
    Close = 0x8, Ping = 0x9, Pong = 0xA
};

struct Frame {
    Opcode opcode;
    bool fin;
    std::vector<uint8_t> payload;
};

// 共享的帧收发（服务端、客户端通用）。
// readFrame 兼容带/不带 mask 的帧；sendFrameTo 的 mask=true 时发送带 mask 的帧（客户端→服务端必需）。
bool readFrame(net::TcpSocket& sock, Frame& frame);
bool sendFrameTo(net::TcpSocket& sock, Opcode op, const uint8_t* data, size_t len, bool mask);

class Connection {
    bool closing_;
    std::unique_ptr<net::TcpSocket> sock_;
public:
    explicit Connection(std::unique_ptr<net::TcpSocket> sock);
    int fd() const;
    bool handshake();
    bool readFrame(Frame& frame);
    bool sendFrame(Opcode op, const uint8_t* data, size_t len);
    bool sendText(const std::string& text);
    bool replyCloseFrame(uint16_t code = 1000, const std::string& reason = "");
};

class Server {
    net::TcpSocket listenSock_;
    std::function<void(Connection&, const Frame&)> onMsg_;
    std::function<void(Connection&)> onClose_;
    std::mutex logMtx_;
    void log(const std::string& s);
    void handleClient(std::unique_ptr<net::TcpSocket> client);
public:
    bool start(const std::string& ip, uint16_t port);
    void run();
    void setOnMessage(std::function<void(Connection&, const Frame&)> cb);
    void setOnClose(std::function<void(Connection&)> cb);
};
}
