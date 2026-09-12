#include "ws.h"
#include <iostream>
#include <chrono>
#include <functional>
#include <memory>
// Windows environment initialization
#ifdef _WIN32
#include <winsock2.h>
#endif

// ============================================================
// 定时器示例服务端
// ============================================================
// 本示例在 onConnect / onMessage 回调里演示两种定时器用法：
//
//   1. 心跳超时（Heartbeat Timeout）
//      每个连接建立时启动一个周期定时器（5 秒间隔），每次到期检查
//      "最后一次活跃时间"；若超过阈值（10 秒）则认为连接僵死，主动关闭。
//      每次收到客户端任何数据都刷新 lastActive。
//      实现：一次性 timer + 回调末尾重新 addTimer（自重启）。
//
//   2. 指令超时（Command Ack Timeout）
//      下发运动指令后启动一次性定时器（3 秒），等待客户端回 ack。
//      收到 ack → 取消定时器；超时 → 重发（最多 3 次），超过后报错。
//      用法：客户端发 "MOVE" → 服务端下发指令 → 客户端回 "ACK" → 取消。
//      若客户端不回 ack，服务端 3 秒后重发，最多 3 次后报错。
// ============================================================

// ---- per-连接上下文（挂载到 conn.userData）----
struct ConnCtx {
    std::atomic<int64_t> lastActiveMs{0};   // 最后一次活跃时间（steady_clock ms）
    uint64_t heartbeatTimerId{0};          // 心跳定时器 ID
    uint64_t cmdTimerId{0};                // 当前指令超时定时器 ID
    int cmdRetryCount{0};                   // 指令已重发次数

    static constexpr int MAX_RETRY = 3;         // 最大重发次数
    static constexpr int CMD_TIMEOUT_MS = 3000; // 指令超时 3 秒
    static constexpr int HB_INTERVAL_MS = 5000; // 心跳检查间隔 5 秒
    static constexpr int HB_TIMEOUT_MS = 10000; // 心跳超时阈值 10 秒
};

static int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

int main(int argc, char** argv) {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }
    SetConsoleOutputCP(CP_UTF8);
#endif

    uint16_t port = (argc > 1) ? std::stoi(argv[1]) : 8080;
    int loopCount = (argc > 2) ? std::stoi(argv[2]) : 0;

    ws::Server server;

    // ================================================================
    // onConnect：握手成功后启动心跳定时器
    // ================================================================
    server.setOnConnect([](ws::Connection& conn) {
        auto ctx = std::make_shared<ConnCtx>();
        ctx->lastActiveMs = nowMs();
        conn.userData = ctx;

        // 心跳定时器：自重启模式（一次性 timer + 回调末尾重新 addTimer）
        // 用 shared_ptr<function> 让回调引用自身，实现周期效果
        ws::Connection* p = &conn;
        auto hbCheck = std::make_shared<std::function<void()>>();
        *hbCheck = [p, hbCheck]() {
            auto ctx = std::static_pointer_cast<ConnCtx>(p->userData);
            if (!ctx) return;

            int64_t elapsed = nowMs() - ctx->lastActiveMs.load();
            if (elapsed > ConnCtx::HB_TIMEOUT_MS) {
                ws::log("[heartbeat] fd=" + std::to_string(p->fd())
                        + " stale " + std::to_string(elapsed)
                        + "ms > " + std::to_string(ConnCtx::HB_TIMEOUT_MS)
                        + "ms, closing");
                p->close();
                return;
            }
            ws::log("[heartbeat] fd=" + std::to_string(p->fd())
                    + " alive, last active " + std::to_string(elapsed) + "ms ago");
            // 自重启：重新 addTimer
            ctx->heartbeatTimerId = p->loop()->addTimer(
                ConnCtx::HB_INTERVAL_MS, *hbCheck);
        };
        ctx->heartbeatTimerId = conn.loop()->addTimer(
            ConnCtx::HB_INTERVAL_MS, *hbCheck);

        ws::log("[onConnect] fd=" + std::to_string(conn.fd())
                + " heartbeat timer started");
    });

    // ================================================================
    // onMessage：刷新活跃时间 + 处理 MOVE / ACK
    // ================================================================
    server.setOnMessage([](ws::Connection& conn, const ws::Frame& frame) {
        auto ctx = std::static_pointer_cast<ConnCtx>(conn.userData);
        if (ctx) ctx->lastActiveMs = nowMs();   // 任何数据都刷新活跃时间

        if (frame.opcode == ws::Opcode::Ping) {
            // WebSocket 协议层 Ping 帧 → 回 Pong（必须用相同 payload）
            conn.sendFrame(ws::Opcode::Pong, frame.payload.data(), frame.payload.size());
            return;
        }

        if (frame.opcode != ws::Opcode::Text) {
            ws::log("[fd=" + std::to_string(conn.fd()) + "] BIN size="
                    + std::to_string(frame.payload.size()) + " -> echo");
            conn.sendFrame(ws::Opcode::Bin, frame.payload.data(), frame.payload.size());
            return;
        }

        std::string text(frame.payload.begin(), frame.payload.end());
        ws::log("[onMsg] fd=" + std::to_string(conn.fd()) + " text: " + text);

        if (text == "MOVE") {
            // ---- 指令超时示例：下发运动指令，启动超时定时器等 ack ----
            ctx->cmdRetryCount = 0;
            ws::Connection* p = &conn;

            // 超时回调：检查重试次数，重发或报错，然后重新 addTimer
            auto onTimeout = std::make_shared<std::function<void()>>();
            *onTimeout = [p, onTimeout]() {
                auto ctx = std::static_pointer_cast<ConnCtx>(p->userData);
                if (!ctx || ctx->cmdTimerId == 0) return; // 已被 ack 取消

                ctx->cmdRetryCount++;
                if (ctx->cmdRetryCount >= ConnCtx::MAX_RETRY) {
                    ws::log("[cmd] fd=" + std::to_string(p->fd())
                            + " ACK timeout after "
                            + std::to_string(ConnCtx::MAX_RETRY)
                            + " retries, giving up");
                    p->sendText("ERROR:MOVE_TIMEOUT");
                    ctx->cmdTimerId = 0;
                    return;
                }
                // 重发指令
                p->sendText("CMD:MOVE arm_to(100,200,150)");
                ws::log("[cmd] fd=" + std::to_string(p->fd())
                        + " ACK timeout, resending (retry "
                        + std::to_string(ctx->cmdRetryCount) + "/"
                        + std::to_string(ConnCtx::MAX_RETRY) + ")");
                // 重新启动超时定时器
                ctx->cmdTimerId = p->loop()->addTimer(
                    ConnCtx::CMD_TIMEOUT_MS, *onTimeout);
            };

            // 第一次下发指令
            conn.sendText("CMD:MOVE arm_to(100,200,150)");
            ws::log("[cmd] fd=" + std::to_string(conn.fd())
                    + " sent MOVE command, waiting for ACK (timeout "
                    + std::to_string(ConnCtx::CMD_TIMEOUT_MS) + "ms)");

            // 启动超时定时器
            ctx->cmdTimerId = conn.loop()->addTimer(
                ConnCtx::CMD_TIMEOUT_MS, *onTimeout);

        } else if (text == "ACK") {
            // ---- 客户端回复 ack：取消指令超时定时器 ----
            if (ctx && ctx->cmdTimerId) {
                conn.loop()->cancelTimer(ctx->cmdTimerId);
                ws::log("[cmd] fd=" + std::to_string(conn.fd())
                        + " ACK received, timer cancelled");
                ctx->cmdTimerId = 0;
                ctx->cmdRetryCount = 0;
            }
            conn.sendText("OK:ACK_RECEIVED");

        } else {
            // 普通文本回显
            conn.sendText(text);
        }
    });

    // ================================================================
    // onClose：清理定时器
    // ================================================================
    server.setOnClose([](ws::Connection& conn) {
        auto ctx = std::static_pointer_cast<ConnCtx>(conn.userData);
        if (ctx) {
            if (ctx->heartbeatTimerId) conn.loop()->cancelTimer(ctx->heartbeatTimerId);
            if (ctx->cmdTimerId)       conn.loop()->cancelTimer(ctx->cmdTimerId);
        }
        ws::log("[onClose] fd=" + std::to_string(conn.fd()) + " timers cleaned");
    });

    ws::log("[main] server starting on port " + std::to_string(port));
    if (!server.start("0.0.0.0", port, loopCount)) {
        std::cerr << "Server start failed\n";
        return 1;
    }
    server.run();
    return 0;
}
