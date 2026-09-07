#include "ws.h"
#include <iostream>
// Windows environment initialization
#ifdef _WIN32
#include <winsock2.h>
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }
    // 设置控制台输出使用UTF‑8
    SetConsoleOutputCP(CP_UTF8);
#endif

    uint16_t port = (argc > 1) ? std::stoi(argv[1]) : 8080;
    // 可选第三个参数：工作线程数（One-Loop-Per-Thread 的 loop 数量）
    // 不传 = 自动（CPU 核数）；传 0 也是自动；传 2 = 2 个工作线程
    int loopCount = (argc > 2) ? std::stoi(argv[2]) : 0;

    ws::Server server;
    
    // 注意：One-Loop-Per-Thread 下，onMsg/onClose 回调会分别在**不同的工作线程**里
    // 并行执行（不同连接的回调可能同时运行）。回调里操作"自己的 conn"是安全的
    // （每个连接只属于一个 loop 线程）；但如果回调要访问多个连接共享的数据
    // （比如全局统计、广播列表），必须自己加锁。
    server.setOnMessage([](ws::Connection& conn, const ws::Frame& frame) {
        if (frame.opcode == ws::Opcode::Text) {
            std::string text(frame.payload.begin(), frame.payload.end());
            ws::log("[Echo] Received text: " + text);
            conn.sendText(text);
        } else if (frame.opcode == ws::Opcode::Bin) {
            // 二进制帧原样回显（模拟机械臂图传：jpg 作为不透明二进制 blob 透传）
            ws::log("[fd=" + std::to_string(conn.fd()) + "] Received BIN, size="
                    + std::to_string(frame.payload.size()) + " bytes -> echo back");
            conn.sendFrame(ws::Opcode::Bin, frame.payload.data(), frame.payload.size());
        } else if (frame.opcode == ws::Opcode::Ping) {
            ws::log("[fd=" + std::to_string(conn.fd()) + "] Received PING -> reply PONG");
            conn.sendFrame(ws::Opcode::Pong, frame.payload.data(), frame.payload.size());
        }
    });
    server.setOnClose([](ws::Connection& conn) {
        conn.replyCloseFrame(1000, "OK"); 
        ws::log("[Echo] Client " + std::to_string(conn.fd()) + " requested close");
    });
    if (!server.start("0.0.0.0", port, loopCount)) {
        std::cerr << "Failed to start\n";
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }
    
    server.run();
    
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
