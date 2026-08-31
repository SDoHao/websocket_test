#pragma once
#include "net.h"
#include "ws.h"
#include <string>

namespace ws {

// 极简 WebSocket 客户端：connect -> handshake -> 收发帧（发送自动带 mask，符合 RFC 6455）。
// 复用 ws_utils::sha1/base64、net::TcpSocket 与 ws 的共享帧收发函数，无重复实现。
class Client {
    net::TcpSocket sock_;
    std::string accept_;   // 服务器返回的 Sec-WebSocket-Accept
    std::string expect_;   // 本地按 RFC 6455 计算的期望 Accept
public:
    bool connect(const std::string& host, uint16_t port);
    // 客户端握手：生成随机 Sec-WebSocket-Key，校验服务器 Accept 是否匹配
    bool handshake(const std::string& host, uint16_t port,
                   const std::string& path = "/");
    // 发送文本帧（自动 mask）
    bool sendText(const std::string& text);
    bool sendFrame(Opcode op, const uint8_t* data, size_t len);
    bool readFrame(Frame& frame);

    int fd() const { return sock_.fd(); }
    const std::string& accept() const { return accept_; }
    const std::string& expectAccept() const { return expect_; }
};

}
