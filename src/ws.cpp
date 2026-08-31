#include "ws.h"
#include "ws_utils.h"
#include <iostream>
#include <cstring>
#include <thread>
#include <random>
#include <chrono>

namespace ws {

static std::mutex g_log_mtx;

void log(const std::string& s) {
    std::lock_guard<std::mutex> lock(g_log_mtx);
    std::cout << s << "\n";
}

// ---------- 共享帧收发（服务端 / 客户端通用） ----------
bool readFrame(net::TcpSocket& sock, Frame& frame) {
    uint8_t hdr[2];
    if (!sock.recvAll(hdr, 2)) return false;
    frame.fin = (hdr[0] & 0x80) != 0;
    frame.opcode = static_cast<Opcode>(hdr[0] & 0x0F);
    bool masked = (hdr[1] & 0x80) != 0;
    uint64_t len = hdr[1] & 0x7F;
    if (len == 126) {
        uint8_t ext[2]; if (!sock.recvAll(ext, 2)) return false;
        len = ((uint64_t)ext[0] << 8) | ext[1];
    } else if (len == 127) {
        uint8_t ext[8]; if (!sock.recvAll(ext, 8)) return false;
        len = 0; for (int i = 0; i < 8; ++i) len = (len << 8) | ext[i];
    }
    uint8_t mask_key[4] = {0};
    if (masked && !sock.recvAll(mask_key, 4)) return false;
    frame.payload.resize((size_t)len);
    if (len > 0 && !sock.recvAll(frame.payload.data(), (size_t)len)) return false;
    if (masked) for (size_t i = 0; i < frame.payload.size(); ++i) frame.payload[i] ^= mask_key[i % 4];
    return true;
}

static void random_bytes(uint8_t* out, size_t n) {
    static std::mt19937_64 rng([]{
        std::random_device rd;
        uint64_t t = (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
        return (uint64_t(rd()) << 32) ^ uint64_t(rd()) ^ t;
    }());
    for (size_t i = 0; i < n; ++i) out[i] = (uint8_t)(rng() & 0xff);
}

bool sendFrameTo(net::TcpSocket& sock, Opcode op, const uint8_t* data, size_t len, bool mask) {
    std::vector<uint8_t> frame;
    frame.push_back(0x80 | static_cast<uint8_t>(op));
    uint8_t maskbit = mask ? 0x80 : 0x00;
    if (len < 126) frame.push_back(maskbit | (uint8_t)len);
    else if (len < 65536) {
        frame.push_back(maskbit | 126);
        frame.push_back((uint8_t)((len >> 8) & 0xff));
        frame.push_back((uint8_t)(len & 0xff));
    } else {
        frame.push_back(maskbit | 127);
        for (int i = 7; i >= 0; --i) frame.push_back((uint8_t)(len >> (8 * i)));
    }
    if (mask) {
        uint8_t mk[4];
        random_bytes(mk, 4);
        frame.insert(frame.end(), mk, mk + 4);
        for (size_t i = 0; i < len; ++i) frame.push_back((uint8_t)data[i] ^ mk[i % 4]);
    } else {
        frame.insert(frame.end(), data, data + len);
    }
    return sock.sendAll(frame.data(), frame.size());
}

Connection::Connection(std::unique_ptr<net::TcpSocket> sock) : sock_(std::move(sock)) {
    closing_ = false;
}

int Connection::fd() const { return sock_->fd(); }

bool Connection::handshake() {
    std::string raw;
    char c;
    while (raw.size() < 8192) {
        if (!sock_->recvAll((uint8_t*)&c, 1)) return false;
        raw.push_back(c);
        if (raw.size() >= 4 && raw.substr(raw.size() - 4) == "\r\n\r\n") break;
    }
    std::string lower_raw = raw;
    for (auto& ch : lower_raw) if (ch >= 'A' && ch <= 'Z') ch += 32;
    std::string key_marker = "sec-websocket-key: ";
    size_t pos = lower_raw.find(key_marker);
    if (pos == std::string::npos) return false;
    size_t start = pos + key_marker.size();
    while (start < raw.size() && (raw[start] == ' ' || raw[start] == '\t')) start++;
    size_t end = raw.find("\r\n", start);
    std::string key = raw.substr(start, end - start);
    std::string combined = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t hash[20];
    ws_utils::sha1((const uint8_t*)combined.data(), combined.size(), hash);
    std::string accept = ws_utils::base64_encode(hash, 20);
    std::string resp = "HTTP/1.1 101 Switching Protocols\r\n"
                       "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                       "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
    return sock_->sendAll((const uint8_t*)resp.data(), resp.size());
}

bool Connection::readFrame(Frame& frame) {
    return ws::readFrame(*sock_, frame);
}

bool Connection::sendFrame(Opcode op, const uint8_t* data, size_t len) {
    return ws::sendFrameTo(*sock_, op, data, len, false);
}

bool Connection::replyCloseFrame(uint16_t code, const std::string& reason){
    if (closing_) return false;  // 幂等性检查
        closing_ = true;
    // 构造 Close 帧
    // Opcode = 0x8 (Close), 前 2 字节为关闭码，后面为可选的 reason 文本
    std::vector<uint8_t> payload;
    payload.push_back((code >> 8) & 0xFF);
    payload.push_back(code & 0xFF);
    payload.insert(payload.end(), reason.begin(), reason.end());
    
    return sendFrame(Opcode::Close, payload.data(), payload.size());
}

bool Connection::sendText(const std::string& text) {
    return sendFrame(Opcode::Text, (const uint8_t*)text.data(), text.size());
}

void Server::log(const std::string& s) {
    ws::log(s); 
}

void Server::handleClient(std::unique_ptr<net::TcpSocket> client) {
    Connection conn(std::move(client));
    if (!conn.handshake()) {
        log("[fd=" + std::to_string(conn.fd()) + "] Handshake failed");
        return;
    }
    log("[fd=" + std::to_string(conn.fd()) + "] Handshake succeeded");
    Frame frame;
    while (conn.readFrame(frame)) {
        if (onMsg_) onMsg_(conn, frame);
        if (frame.opcode == Opcode::Close) {
            if (onClose_) onClose_(conn);
            break;
        }
    }
    log("[fd=" + std::to_string(conn.fd()) + "] Connection closed");
}

bool Server::start(const std::string& ip, uint16_t port) {
    bool ok = listenSock_.bindAndListen(ip, port);
    if (ok) log("[Server] Listening on " + ip + ":" + std::to_string(port));
    return ok;
}

void Server::run() {
    while (true) {
        auto client = listenSock_.accept();
        if (!client) break;
        log("[Server] New connection fd=" + std::to_string(client->fd()));
        std::thread t(&Server::handleClient, this, std::move(client));
        t.detach();
    }
}

void Server::setOnMessage(std::function<void(Connection&, const Frame&)> cb) { onMsg_ = std::move(cb); }

void Server::setOnClose(std::function<void(Connection&)> cb) { onClose_ = std::move(cb); }

}
