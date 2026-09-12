#include "ws.h"
#include "ws_client.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <thread>
#include <chrono>
#include <poll.h>
#ifdef _WIN32
#include <winsock2.h>
#endif

// ============================================================
//  ws_client.cpp —— 命令行 WebSocket 测试客户端（阻塞模式）
//
//  用法：./ws_client [host] [port] <选项>
//    选项：
//      -f <文件>   发送二进制文件（jpg 图传，服务端原样回显）
//      -t <文本>   发送文本消息（服务端原样回显）
//      -p          测试心跳（发 Ping，等服务端回 Pong）
//      -m [秒]    指令超时测试：发 MOVE，等服务端下发 CMD，
//                  延迟 N 秒后回 ACK（默认 1 秒；填 0 = 不回 ACK，测超时）
//      -k [秒]    心跳保活测试：连接后保持 N 秒不发数据，
//                  观察服务端是否因超时主动断开（默认 15 秒）
//  示例：
//      ./ws_client -f 0.jpg
//      ./ws_client 127.0.0.1 8080 -t "hello websocket"
//      ./ws_client -p
//      ./ws_client -m 2        （2 秒后回 ACK，测正常流程）
//      ./ws_client -m 0        （不回 ACK，测指令超时重发）
//      ./ws_client -k 15       （15 秒不发数据，测心跳超时断开）
// ============================================================

// 命令行参数（解析结果存这里）
struct Args {
    std::string host = "127.0.0.1";   // 服务器地址（默认本机）
    uint16_t port = 8080;             // 服务器端口（默认 8080）
    enum Mode { None, File, Text, Ping, Move, Keepalive } mode = None;
    std::string file;                 // -f 的文件路径
    std::string text;                 // -t 的文本内容
    int moveAckDelay = 1;             // -m：收到 CMD 后多少秒回 ACK（0=不回）
    int keepaliveSecs = 15;           // -k：保持连接多少秒
};

// 打印用法说明
static void printUsage() {
    std::cout <<
        "用法: ./ws_client [host] [port] <选项>\n"
        "  选项:\n"
        "    -f <文件>   发送二进制文件（jpg 图传）\n"
        "    -t <文本>   发送文本消息\n"
        "    -p          测试心跳（Ping/Pong）\n"
        "    -m [秒]    指令超时测试（默认 1 秒后回 ACK；0=不回 ACK，测超时）\n"
        "    -k [秒]    心跳保活测试（默认 15 秒，测服务端超时断开）\n"
        "  示例:\n"
        "    ./ws_client -f 0.jpg\n"
        "    ./ws_client 127.0.0.1 8080 -t hello\n"
        "    ./ws_client -p\n"
        "    ./ws_client -m 2        （2 秒后回 ACK）\n"
        "    ./ws_client -m 0        （不回 ACK，测指令超时重发）\n"
        "    ./ws_client -k 15       （15 秒不发数据，测心跳超时）\n";
}

// 解析命令行参数
// 规则：-f / -t / -p 是选项；前两个"非选项参数"依次当作 host、port。
// 返回 false 表示解析失败（用法错误），main 直接退出。
static bool parseArgs(int argc, char** argv, Args& args) {
    bool hasHost = false;   // 是否已收到 host 位置参数
    bool hasPort = false;   // 是否已收到 port 位置参数
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-f" && i + 1 < argc) {
            args.mode = Args::File;
            args.file = argv[++i];        // 选项带一个参数：文件路径
        } else if (a == "-t" && i + 1 < argc) {
            args.mode = Args::Text;
            args.text = argv[++i];        // 选项带一个参数：文本内容
        } else if (a == "-p") {
            args.mode = Args::Ping;       // 无参数选项：测心跳
        } else if (a == "-m") {
            args.mode = Args::Move;
            // -m 可带一个可选参数：延迟秒数（默认 1）
            if (i + 1 < argc) {
                std::string next = argv[i + 1];
                if (!next.empty() && (next[0] >= '0' && next[0] <= '9')) {
                    args.moveAckDelay = std::stoi(argv[++i]);
                }
            }
        } else if (a == "-k") {
            args.mode = Args::Keepalive;
            // -k 可带一个可选参数：持续秒数（默认 15）
            if (i + 1 < argc) {
                std::string next = argv[i + 1];
                if (!next.empty() && (next[0] >= '0' && next[0] <= '9')) {
                    args.keepaliveSecs = std::stoi(argv[++i]);
                }
            }
        } else if (a == "-h" || a == "--help") {
            printUsage();
            return false;
        } else if (!hasHost) {
            args.host = a;                // 第一个普通参数 = host
            hasHost = true;
        } else if (!hasPort) {
            args.port = (uint16_t)std::stoi(a);  // 第二个普通参数 = port
            hasPort = true;
        } else {
            std::cerr << "未知参数: " << a << "\n";
            printUsage();
            return false;
        }
    }
    if (args.mode == Args::None) {   // 一个选项都没给，打印用法
        printUsage();
        return false;
    }
    return true;
}

// ------------------------------------------------------------
// 功能 1：发送文件（二进制图传）
// 读文件 → 发 Bin 帧 → 收回显 → 保存 .echo.jpg → 字节级对比
// ------------------------------------------------------------
static bool sendFile(ws::Client& client, const std::string& path) {
    // 二进制模式读文件（jpg 是不透明 blob，不解码）
    std::ifstream inFile(path, std::ios::binary);
    if (!inFile) {
        std::cerr << "无法打开文件: " << path << "\n";
        return false;
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(inFile)),
                              std::istreambuf_iterator<char>());
    inFile.close();
    std::cout << "读取文件: " << path << " (" << data.size() << " bytes)\n";

    // 发二进制帧（客户端自动带 mask，符合 RFC 6455）
    if (!client.sendFrame(ws::Opcode::Bin, data.data(), data.size())) {
        std::cerr << "send failed\n";
        return false;
    }
    std::cout << "已发送二进制帧 (" << data.size() << " bytes)\n";

    // 收回显
    ws::Frame frame;
    if (!client.readFrame(frame)) {
        std::cerr << "读回显失败\n";
        return false;
    }
    std::cout << "=== 回显结果 ===\n";
    std::cout << "opcode=" << (int)frame.opcode
              << " (" << (frame.opcode == ws::Opcode::Text ? "TEXT" : "BIN") << ")\n";
    std::cout << "回显大小: " << frame.payload.size() << " bytes\n";

    // 保存回显文件
    std::string outPath = path + ".echo.jpg";
    std::ofstream outFile(outPath, std::ios::binary);
    if (!outFile) {
        std::cerr << "无法写入回显文件: " << outPath << "\n";
        return false;
    }
    outFile.write(reinterpret_cast<const char*>(frame.payload.data()),
                  static_cast<std::streamsize>(frame.payload.size()));
    outFile.close();
    std::cout << "回显已保存: " << outPath << "\n";

    // 字节级对比
    bool ok = (frame.payload == data);
    std::cout << (ok ? "\n[OK] 回显字节完全一致，图传成功\n"
                     : "\n[FAIL] 回显与原文件不一致\n");
    return ok;
}

// ------------------------------------------------------------
// 功能 2：发送文本（服务端原样回显）
// ------------------------------------------------------------
static bool sendText(ws::Client& client, const std::string& msg) {
    if (!client.sendText(msg)) {
        std::cerr << "send failed\n";
        return false;
    }
    std::cout << "已发送文本帧: " << msg << "\n";

    // 收回显
    ws::Frame frame;
    if (!client.readFrame(frame)) {
        std::cerr << "读回显失败\n";
        return false;
    }
    std::string echo(frame.payload.begin(), frame.payload.end());
    std::cout << "=== 回显结果 ===\n";
    std::cout << "opcode=" << (int)frame.opcode
              << " (" << (frame.opcode == ws::Opcode::Text ? "TEXT" : "BIN") << ")\n";
    std::cout << "内容: " << echo << "\n";

    bool ok = (echo == msg);
    std::cout << (ok ? "\n[OK] 回显一致，文本发送正常\n"
                     : "\n[FAIL] 回显不一致\n");
    return ok;
}

// ------------------------------------------------------------
// 功能 3：测试心跳（发 Ping 帧，等服务端回 Pong 帧）
// ------------------------------------------------------------
static bool testPing(ws::Client& client) {
    std::string payload = "ping-test";
    if (!client.sendFrame(ws::Opcode::Ping, (const uint8_t*)payload.data(), payload.size())) {
        std::cerr << "Ping 发送失败\n";
        return false;
    }
    std::cout << "已发送 Ping 帧 (payload=\"" << payload << "\")，等待 Pong...\n";
    std::cout << "(若长时间无响应，说明心跳失败或服务端未回 Pong)\n";

    // 阻塞等一帧（正常情况服务端会立刻回 Pong；简单版不做超时）
    ws::Frame frame;
    if (!client.readFrame(frame)) {
        std::cerr << "读帧失败\n";
        return false;
    }
    if (frame.opcode == ws::Opcode::Pong) {
        std::string pong(frame.payload.begin(), frame.payload.end());
        std::cout << "[OK] 收到 Pong，payload=\"" << pong << "\"，心跳正常\n";
        return true;
    }
    std::cout << "[FAIL] 收到非 Pong 帧，opcode=" << (int)frame.opcode << "\n";
    return false;
}

// ------------------------------------------------------------
// 功能 4：指令超时测试（-m）
// 发 MOVE → 服务端下发 CMD:MOVE → 延迟 N 秒后回 ACK（或不回，测超时）
// 持续读帧，观察服务端的指令下发 / 重发 / 超时报错
// ------------------------------------------------------------
static bool testMoveTimeout(ws::Client& client, int ackDelay) {
    // ① 发送 MOVE 指令，触发服务端启动指令超时定时器
    if (!client.sendText("MOVE")) {
        std::cerr << "发送 MOVE 失败\n";
        return false;
    }
    std::cout << "已发送 MOVE，等待服务端下发 CMD...\n";

    bool ackSent = false;
    bool gotError = false;
    int round = 0;

    // ② 循环读帧，观察服务端行为
    while (true) {
        ws::Frame frame;
        if (!client.readFrame(frame)) {
            std::cerr << "连接已断开（可能是服务端超时关闭）\n";
            break;
        }
        if (frame.opcode == ws::Opcode::Close) {
            std::cout << "服务端发送 Close 帧\n";
            break;
        }
        if (frame.opcode != ws::Opcode::Text) continue;

        std::string text(frame.payload.begin(), frame.payload.end());
        round++;
        std::cout << "[recv #" << round << "] " << text << "\n";

        // 收到 CMD:MOVE → 按 ackDelay 决定是否回 ACK
        if (text.rfind("CMD:MOVE", 0) == 0 && !ackSent) {
            if (ackDelay > 0) {
                std::cout << "收到指令，等待 " << ackDelay
                          << " 秒后回 ACK...\n";
                std::this_thread::sleep_for(
                    std::chrono::seconds(ackDelay));
                client.sendText("ACK");
                std::cout << "已发送 ACK\n";
                ackSent = true;
            } else {
                std::cout << "收到指令，故意不回 ACK（测试超时重发）...\n";
                // ackDelay=0 → 不回 ACK，让服务端超时重发
            }
        }

        // 收到 OK:ACK_RECEIVED → 流程完成
        if (text == "OK:ACK_RECEIVED") {
            std::cout << "\n[OK] 指令流程完成（ACK 已被服务端确认）\n";
            break;
        }

        // 收到 ERROR → 超时报错
        if (text.rfind("ERROR", 0) == 0) {
            std::cout << "\n[ERROR] 服务端报错: " << text << "\n";
            gotError = true;
            break;
        }
    }
    return !gotError;
}

// ------------------------------------------------------------
// 功能 5：心跳保活测试（-k）
// 连接后保持 N 秒不发任何数据，观察服务端心跳超时是否主动断开
// ------------------------------------------------------------
static bool testKeepalive(ws::Client& client, int secs) {
    std::cout << "连接已建立，保持 " << secs << " 秒不发数据...\n";
    std::cout << "（服务端每 5 秒检查一次，超过 10 秒无活跃将断开）\n\n";

    int fd = client.fd();
    auto start = std::chrono::steady_clock::now();
    while (true) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed >= secs) {
            std::cout << "\n[OK] 存活 " << secs << " 秒，测试结束\n";
            return true;
        }

        // 用 poll 检查服务端是否断开（1 秒超时）
        // 如果 socket 可读但 readFrame 返回 false → 服务端关闭了连接
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        int ret = ::poll(&pfd, 1, 1000);  // 1 秒超时

        if (ret > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR))) {
            // socket 有事件 → 尝试读一帧
            ws::Frame frame;
            if (!client.readFrame(frame)) {
                // 读失败 → 服务端断开了连接
                std::cout << "\n[服务端断开] 连接已被服务端心跳超时关闭 (elapsed="
                          << elapsed << "s)\n";
                return false;
            }
            // 收到数据 → 忽略（心跳测试不关心内容）
        }
        std::cout << "  [" << elapsed + 1 << "s] 仍连接...\n" << std::flush;
    }
}

// ============================================================
//  main：解析参数 → 连接 → 握手 → 按模式分发给功能函数
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

    // ① 解析命令行参数
    Args args;
    if (!parseArgs(argc, argv, args)) return 1;

    // ② TCP 连接
    ws::Client client;
    if (!client.connect(args.host, args.port)) {
        std::cerr << "connect failed: " << args.host << ":" << args.port << "\n";
        return 1;
    }

    // ③ WebSocket 握手（校验 Accept）
    if (!client.handshake(args.host, args.port, "/")) {
        std::cerr << "handshake failed\n";
        std::cerr << "期望 Accept: " << client.expectAccept() << "\n";
        std::cerr << "实际 Accept: " << client.accept() << "\n";
        return 1;
    }
    std::cout << "=== 握手成功 ===\n";
    std::cout << "期望 Accept: " << client.expectAccept() << "\n";
    std::cout << "实际 Accept: " << client.accept() << "\n\n";

    // ④ 按模式分发
    bool ok = false;
    switch (args.mode) {
        case Args::File:      ok = sendFile(client, args.file);         break;
        case Args::Text:      ok = sendText(client, args.text);         break;
        case Args::Ping:      ok = testPing(client);                    break;
        case Args::Move:      ok = testMoveTimeout(client, args.moveAckDelay); break;
        case Args::Keepalive: ok = testKeepalive(client, args.keepaliveSecs); break;
        default:              ok = false;                               break;
    }

#ifdef _WIN32
    WSACleanup();
#endif
    return ok ? 0 : 1;
}
