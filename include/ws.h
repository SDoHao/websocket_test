#pragma once
#include "net.h"
#include <functional>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <cstdint>

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

// ============================================================
// 旧接口（保留给命令行客户端 ws_client.cpp 使用，阻塞模式）。
// 服务端 reactor 不再使用它们。
// ============================================================
// 从 socket 阻塞读一个完整帧（客户端→服务端方向，兼容带/不带 mask）
bool readFrame(net::TcpSocket& sock, Frame& frame);
// 发送一帧；mask=true 时客户端必须传（RFC 6455 规定客户端帧必须掩码）
bool sendFrameTo(net::TcpSocket& sock, Opcode op, const uint8_t* data, size_t len, bool mask);

// ============================================================
// 新增：从字节缓冲中解析一个完整帧（服务端 reactor 用）
// ============================================================
// 参数：
//   buf    —— 读缓冲（socket 收到的原始字节）
//   offset —— 从 buf 的哪个位置开始解析；解析成功后前进到帧末尾
//   frame  —— 解析结果输出
// 返回值：
//   true  —— 成功解析出一个完整帧（数据已被消费，offset 已前进）
//   false —— 缓冲里的数据还不够拼出一个完整帧（正常现象，等下次收到数据再试）
// 注意：本函数不碰 socket，纯粹从内存缓冲里"抠"帧，所以永远不会阻塞。
bool parseFrameFromBuf(const std::vector<uint8_t>& buf, size_t& offset, Frame& frame);

class Server;  // 前置声明：Connection 里要用 Server* 指针

// ============================================================
// 服务端连接（非阻塞状态机版）
// ============================================================
// 每个连接由一个 Connection 对象管理。它不再自己开线程阻塞读，
// 而是被 Server（reactor 事件循环）驱动：有事件时调用 handleRead/handleWrite。
// 它内部有两个缓冲：
//   readBuf_  —— socket 收到的字节先攒在这，攒够一个完整帧才解析（解决半包）
//   writeBuf_ —— 要发的字节先写在这，一次发不完就留着，等 epoll 通知可写再发（解决发送阻塞）
class Connection {
public:
    // 连接状态机：握手 → 正常收发 → 关闭
    enum class State : uint8_t {
        Handshake,  // 正在接收 HTTP 握手请求头（收到 \r\n\r\n 才算完成）
        Reading,    // 握手完成，正常收发数据帧
        Closed      // 已关闭（等事件循环统一回收对象）
    };

    Connection(int fd, Server* server);
    ~Connection();

    int fd() const;
    State state() const;

    // ---- 事件入口（只由 Server 的事件循环调用）----
    void handleRead();   // socket 可读：recv 数据进读缓冲，然后尝试解析
    void handleWrite();  // socket 可写：把写缓冲里剩余的数据尽量发出去
    void close();        // 关闭连接（幂等：重复调用安全）

    // ---- 业务接口（在 onMsg_ 回调里调用）----
    bool sendFrame(Opcode op, const uint8_t* data, size_t len);           // 发任意一帧
    bool sendText(const std::string& text);                               // 发文本帧
    bool replyCloseFrame(uint16_t code = 1000, const std::string& reason = ""); // 回关闭帧

private:
    void tryHandshake();       // 尝试从读缓冲解析握手请求头（数据不足就等下次）
    void tryParseMessages();   // 尝试从读缓冲解析数据帧（含分片重组）
    void enqueue(const uint8_t* data, size_t len);   // 字节追加进写缓冲，并登记可写事件
    void requestWrite();       // 告诉 epoll："我现在有数据要发"，登记 EPOLLOUT

    int fd_;                   // 本连接的 socket 文件描述符
    Server* server_;           // 反向指回 Server（用它注册/注销事件、交付回调）
    State state_ = State::Handshake;

    std::vector<uint8_t> readBuf_;    // 读缓冲：socket 收到的原始字节（未解析完的部分）
    std::vector<uint8_t> writeBuf_;   // 写缓冲：待发送的字节
    size_t writePos_ = 0;             // 写缓冲中"已经发出去"的位置（剩余 = writeBuf_[writePos_..]）

    // ---- 分片重组状态（逻辑与之前线程版一致，只是改为从缓冲解析）----
    std::vector<uint8_t> fragBuf_;    // 正在拼接的消息数据
    Opcode fragOpcode_ = Opcode::Text;// 当前消息类型（首帧记录）
    bool inFrag_ = false;             // 是否正在拼接分片消息

    // ---- 握手/关闭状态 ----
    std::string reqRaw_;              // 累积收到的握手请求字节（收到 \r\n\r\n 前）
    bool closing_ = false;            // 已收到 Close 帧：等写缓冲发完再真正关闭（优雅关闭）
};

// ============================================================
// 服务端引擎（epoll + reactor，单线程事件循环）
// ============================================================
// 工作方式：
//   1. start() 绑定监听端口，创建 epoll 实例，把监听 socket 登记进去
//   2. run() 里唯一的线程阻塞在 epoll_wait() 上
//   3. 内核通知有事件 → 分发到对应的 Connection（读/写/关闭）
//   4. 新连接到达 → acceptNew() 创建 Connection 并登记进 epoll
class Server {
public:
    bool start(const std::string& ip, uint16_t port);  // 绑定监听 + 创建 epoll
    void run();  // 事件循环（唯一线程在这里阻塞，直到进程退出）

    void setOnMessage(std::function<void(Connection&, const Frame&)> cb); // 注册收消息回调
    void setOnClose(std::function<void(Connection&)> cb);                 // 注册关闭回调

    // ---- 供 Connection 调用的内部接口 ----
    void modEvent(int fd, uint32_t events);  // 修改某 fd 在 epoll 里关心的事件集合
    void removeConnection(int fd);           // 从 epoll/连接表移除；对象延迟到循环末尾删除
    void deliverMessage(Connection* c, const Frame& f);  // 转发给 onMsg_
    void deliverClose(Connection* c);                    // 转发给 onClose_

private:
    void acceptNew();  // 接受新连接（监听 socket 可读时调用）
    void closeAll();   // 关闭所有连接（事件循环退出时清理）

    net::TcpSocket listenSock_;                   // 监听 socket（RAII，析构自动关闭）
    int epollFd_ = -1;                            // epoll 实例句柄
    std::unordered_map<int, Connection*> conns_;  // fd → Connection 映射表
    std::vector<Connection*> pendingDelete_;      // 延迟删除队列（防止对象被提前销毁）

    std::function<void(Connection&, const Frame&)> onMsg_;
    std::function<void(Connection&)> onClose_;
};
}
