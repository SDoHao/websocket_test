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
    ws::Server server;
    
    server.setOnMessage([](ws::Connection& conn, const ws::Frame& frame) {
        if (frame.opcode == ws::Opcode::Text) {
            std::string text(frame.payload.begin(), frame.payload.end());
            ws::log("[Echo] Received text: " + text);
            conn.sendText(text);
        } else if (frame.opcode == ws::Opcode::Bin) {
            ws::log("[fd=" + std::to_string(conn.fd()) + "] Received BIN");
        } else if (frame.opcode == ws::Opcode::Ping) {
            ws::log("[fd=" + std::to_string(conn.fd()) + "] Received PING -> reply PONG");
            conn.sendFrame(ws::Opcode::Pong, frame.payload.data(), frame.payload.size());
        }
    });
    server.setOnClose([](ws::Connection& conn) {
        conn.replyCloseFrame(1000, "OK"); 
        ws::log("[Echo] Client " + std::to_string(conn.fd()) + " requested close");
    });
    if (!server.start("0.0.0.0", port)) {
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
