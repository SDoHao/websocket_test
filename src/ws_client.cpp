#include "ws_client.h"
#include "ws_utils.h"
#include <vector>
#include <random>
#include <chrono>

namespace ws {

// 生成 16 字节随机 key 并 Base64 编码，作为 Sec-WebSocket-Key
static std::string random_key() {
    std::vector<uint8_t> raw(16);
    static std::mt19937_64 rng([]{
        std::random_device rd;
        uint64_t t = (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
        return (uint64_t(rd()) << 32) ^ uint64_t(rd()) ^ t;
    }());
    for (size_t i = 0; i < raw.size(); ++i) raw[i] = (uint8_t)(rng() & 0xff);
    return ws_utils::base64_encode(raw.data(), raw.size());
}

bool Client::connect(const std::string& host, uint16_t port) {
    return sock_.connect(host, port);
}

bool Client::handshake(const std::string& host, uint16_t port, const std::string& path) {
    std::string key = random_key();
    std::string req =
        "GET " + (path.empty() ? "/" : path) + " HTTP/1.1\r\n"
        "Host: " + host + ":" + std::to_string(port) + "\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: " + key + "\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";
    if (!sock_.sendAll((const uint8_t*)req.data(), req.size())) return false;

    // 读握手响应，直到 \r\n\r\n
    std::string resp;
    char c;
    while (resp.size() < 8192) {
        if (!sock_.recvAll((uint8_t*)&c, 1)) return false;
        resp.push_back(c);
        if (resp.size() >= 4 && resp.substr(resp.size() - 4) == "\r\n\r\n") break;
    }

    // 计算期望 Accept 并校验
    std::string combined = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t hash[20];
    ws_utils::sha1((const uint8_t*)combined.data(), combined.size(), hash);
    expect_ = ws_utils::base64_encode(hash, 20);

    std::string marker = "Sec-WebSocket-Accept: ";
    size_t p = resp.find(marker);
    if (p == std::string::npos) return false;
    size_t start = p + marker.size();
    size_t end = resp.find("\r\n", start);
    accept_ = resp.substr(start, end - start);
    return accept_ == expect_;
}

bool Client::sendFrame(Opcode op, const uint8_t* data, size_t len) {
    return ws::sendFrameTo(sock_, op, data, len, true);
}

bool Client::sendText(const std::string& text) {
    return sendFrame(Opcode::Text, (const uint8_t*)text.data(), text.size());
}

bool Client::readFrame(Frame& frame) {
    return ws::readFrame(sock_, frame);
}

}
