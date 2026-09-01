#include "ws.h"
#include "ws_client.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#ifdef _WIN32
#include <winsock2.h>
#endif

// ============================================================
//  ws_client.cpp
//  命令行 WebSocket 客户端（二进制图传模拟版）。
//  用法：./ws_client [host] [port] [file]
//  示例：./ws_client 127.0.0.1 8080 0.jpg
//  读取指定文件（默认 0.jpg）作为二进制帧发送，接收回显并保存、字节对比。
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

    std::string host     = (argc > 1) ? argv[1] : "127.0.0.1";
    uint16_t port        = (argc > 2) ? (uint16_t)std::stoi(argv[2]) : 8080;
    std::string filePath = (argc > 3) ? argv[3] : "0.jpg";

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

    // 读取文件（二进制模式，jpg 作为不透明 blob，不解码）
    std::ifstream inFile(filePath, std::ios::binary);
    if (!inFile) {
        std::cerr << "无法打开文件: " << filePath << "\n";
        return 1;
    }
    std::vector<uint8_t> imgData((std::istreambuf_iterator<char>(inFile)),
                                   std::istreambuf_iterator<char>());
    inFile.close();
    std::cout << "读取文件: " << filePath << " (" << imgData.size() << " bytes)\n";

    if (!client.sendFrame(ws::Opcode::Bin, imgData.data(), imgData.size())) {
        std::cerr << "send failed\n";
        return 1;
    }
    std::cout << "已发送二进制帧 (" << imgData.size() << " bytes)\n";

    ws::Frame frame;
    if (!client.readFrame(frame)) {
        std::cerr << "读回显失败\n";
        return 1;
    }
    std::cout << "\n=== 回显结果 ===\n";
    std::cout << "opcode=" << (int)frame.opcode
              << " (" << (frame.opcode == ws::Opcode::Text ? "TEXT" : "BIN") << ")\n";
    std::cout << "回显大小: " << frame.payload.size() << " bytes\n";

    // 保存回显文件（原文件名 + .echo.jpg）
    std::string outPath = filePath + ".echo.jpg";
    std::ofstream outFile(outPath, std::ios::binary);
    if (!outFile) {
        std::cerr << "无法写入回显文件: " << outPath << "\n";
        return 1;
    }
    outFile.write(reinterpret_cast<const char*>(frame.payload.data()),
                  static_cast<std::streamsize>(frame.payload.size()));
    outFile.close();
    std::cout << "回显已保存: " << outPath << "\n";

    // 字节级对比（vector<uint8_t> 直接 == 比较）
    bool ok = (frame.payload == imgData);
    std::cout << (ok ? "\n[OK] 回显字节完全一致，图传模拟成功\n"
                     : "\n[FAIL] 回显与原文件不一致\n");

#ifdef _WIN32
    WSACleanup();
#endif
    return ok ? 0 : 1;
}
