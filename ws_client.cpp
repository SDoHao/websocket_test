#include "ws.h"
#include "ws_client.h"
#include <iostream>
#ifdef _WIN32
#include <winsock2.h>
#endif

// ============================================================
//  ws_client.cpp
//  命令行 WebSocket 客户端，用于测试 ws_echo_server。
//  用法：./ws_client [host] [port] [message]
//  示例：./ws_client 127.0.0.1 8080 "hello websocket"
// ============================================================
int main(int argc, char** argv) {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }
    SetConsoleOutputCP(CP_UTF8);
#endif

    std::string host = (argc > 1) ? argv[1] : "127.0.0.1";
    uint16_t port   = (argc > 2) ? (uint16_t)std::stoi(argv[2]) : 8080;
    std::string msg = (argc > 3) ? argv[3] : "hello websocket";

    ws::Client client;
    if (!client.connect(host, port)) {
        std::cerr << "connect failed\n";
        return 1;
    }
    if (!client.handshake(host, port, "/")) {
        std::cerr << "handshake failed\n";
        std::cerr << "期望 Accept: " << client.expectAccept() << "\n";
        std::cerr << "实际 Accept: " << client.accept() << "\n";
        return 1;
    }
    std::cout << "=== 握手成功 ===\n";
    std::cout << "期望 Accept: " << client.expectAccept() << "\n";
    std::cout << "实际 Accept: " << client.accept() << "\n";

    if (!client.sendText(msg)) {
        std::cerr << "send failed\n";
        return 1;
    }

    ws::Frame frame;
    if (!client.readFrame(frame)) {
        std::cerr << "读回显失败\n";
        return 1;
    }
    std::string echo(frame.payload.begin(), frame.payload.end());
    std::cout << "\n=== 回显结果 ===\n";
    std::cout << "opcode=" << (int)frame.opcode
              << " (" << (frame.opcode == ws::Opcode::Text ? "TEXT" : "BIN") << ")\n";
    std::cout << "内容: " << echo << "\n";
    bool ok = (echo == msg);
    std::cout << (ok ? "\n[OK] 回显一致，服务器工作正常\n"
                     : "\n[FAIL] 回显不一致\n");

#ifdef _WIN32
    WSACleanup();
#endif
    return ok ? 0 : 1;
}
