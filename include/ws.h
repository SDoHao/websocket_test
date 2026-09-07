#pragma once
#include "net.h"
#include <functional>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
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

class Server;    // 前置声明：Connection/EventLoop 里要用指针
class EventLoop; // 前置声明：Connection 里要用指针

// ============================================================
// 服务端连接（非阻塞状态机版，One-Loop-Per-Thread）
// ============================================================
// 每个连接由一个 Connection 对象管理，它**固定属于某一个 EventLoop**：
//   - 该连接的所有读写、解析、回调，永远只发生在那个 EventLoop 的线程里
//   - 所以 Connection 内部完全不需要锁（单线程访问）
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

    Connection(int fd, EventLoop* loop);  // 构造时绑定所属 EventLoop
    ~Connection();

    int fd() const;
    State state() const;

    // ---- 事件入口（只由所属 EventLoop 的事件循环调用）----
    void handleRead();   // socket 可读：recv 数据进读缓冲，然后尝试解析
    void handleWrite();  // socket 可写：把写缓冲里剩余的数据尽量发出去
    void close();        // 关闭连接（幂等：重复调用安全）

    // ---- 业务接口（在 onMsg_ 回调里调用，且只能在所属 loop 线程内调用）----
    bool sendFrame(Opcode op, const uint8_t* data, size_t len);           // 发任意一帧
    bool sendText(const std::string& text);                               // 发文本帧
    bool replyCloseFrame(uint16_t code = 1000, const std::string& reason = ""); // 回关闭帧

private:
    void tryHandshake();       // 尝试从读缓冲解析握手请求头（数据不足就等下次）
    void tryParseMessages();   // 尝试从读缓冲解析数据帧（含分片重组）
    void enqueue(const uint8_t* data, size_t len);   // 字节追加进写缓冲，并登记可写事件
    void requestWrite();       // 告诉 epoll："我现在有数据要发"，登记 EPOLLOUT

    int fd_;                   // 本连接的 socket 文件描述符
    EventLoop* loop_;          // 所属事件循环（它决定了本连接在哪个线程处理）
    State state_ = State::Handshake;

    std::vector<uint8_t> readBuf_;    // 读缓冲：socket 收到的原始字节（未解析完的部分）
    std::vector<uint8_t> writeBuf_;   // 写缓冲：待发送的字节
    size_t writePos_ = 0;             // 写缓冲中"已经发出去"的位置（剩余 = writeBuf_[writePos_..]）

    // ---- 分片重组状态（逻辑与单线程版一致）----
    std::vector<uint8_t> fragBuf_;    // 正在拼接的消息数据
    Opcode fragOpcode_ = Opcode::Text;// 当前消息类型（首帧记录）
    bool inFrag_ = false;             // 是否正在拼接分片消息

    // ---- 握手/关闭状态 ----
    std::string reqRaw_;              // 累积收到的握手请求字节（收到 \r\n\r\n 前）
    bool closing_ = false;            // 已收到 Close 帧：等写缓冲发完再真正关闭（优雅关闭）
};

// ============================================================
// EventLoop：一个工作线程自己的事件循环（One-Loop-Per-Thread 的核心）
// ============================================================
// 每个 EventLoop 拥有：
//   - 一个独立线程 + 独立 epoll 实例（epollFd_）
//   - 一个 eventfd（wakeupFd_）：用于被"别的线程"唤醒（见 addConnection）
//   - 一组属于自己的连接（conns_）
// 所有对 conns_ 里连接的读写都发生在本线程，所以除了跨线程的
// pendingConns_ 队列（主线程 push / 本线程 pop）需要锁，其余无需加锁。
class EventLoop {
public:
    EventLoop(Server* server, int idx);
    ~EventLoop();

    void start();       // 创建 epoll + eventfd，并启动工作线程
    void runInLoop();   // 工作线程主函数：自己的 epoll_wait 事件循环
    void join();        // 等待工作线程结束
    void stop();        // 请求退出（可被任意线程调用，内部用 wakeup 唤醒）

    // 跨线程安全：把"新接受的连接 fd"交给本 loop 接管。
    // 主线程 accept 后调用它；它把 fd 放进待办队列并唤醒本 loop 线程。
    void addConnection(int fd);

    int idx() const;                        // loop 编号（日志用）
    bool isInLoopThread() const;            // 判断当前线程是否就是本 loop 的线程

    // ---- 供 Connection 调用的内部接口（只在 loop 线程内调用）----
    void modEvent(int fd, uint32_t events); // 修改某 fd 在 epoll 里关心的事件
    void removeConnection(int fd);          // 从 epoll/连接表移除；对象延迟删除
    void deliverMessage(Connection* c, const Frame& f); // 转发给业务回调
    void deliverClose(Connection* c);                   // 转发给关闭回调

private:
    void wakeup();        // 往 eventfd 写 1 字节计数（唤醒阻塞中的 epoll_wait）
    void handleWakeup();  // eventfd 可读：清计数 + 处理待办队列
    void doPendingConns();// 把待办队列里的新连接全部登记进本 loop 的 epoll
    void closeAll();      // 退出时清理本 loop 的所有连接

    Server* server_;      // 反向指回 Server（转发业务回调）
    int idx_;             // 本 loop 编号（0,1,2...，日志里方便看负载分布）
    int epollFd_ = -1;    // 本 loop 的 epoll 实例
    int wakeupFd_ = -1;   // eventfd：被其他线程唤醒的"门铃"
    std::thread thread_;  // 工作线程
    std::atomic<bool> stop_{false};   // 退出标志（原子：跨线程读写）
    std::thread::id loopThreadId_;    // 本 loop 线程的 id（isInLoopThread 用）

    std::mutex mtx_;                  // 只保护 pendingConns_（跨线程队列）
    std::vector<int> pendingConns_;   // 待接管的新连接 fd（主线程 push，本线程 pop）

    std::unordered_map<int, Connection*> conns_;  // fd → Connection（本线程独享）
    std::vector<Connection*> pendingDelete_;      // 延迟删除队列（本线程独享）
};

// ============================================================
// 服务端引擎（One-Loop-Per-Thread 多线程版）
// ============================================================
// 分工：
//   1. 主线程（MainLoop）：只负责监听端口 + accept + 负载均衡分发
//   2. 工作线程（EventLoop × N）：各自负责自己名下连接的读写
// 负载均衡：round-robin（轮流）把新连接分配给第 0、1、2... 个 loop。
// 线程数：默认 = CPU 核数（std::thread::hardware_concurrency()），
//         也可在 start() 显式指定。
class Server {
public:
    // loopCount：工作线程数；0 或负数 = 自动（CPU 核数）
    bool start(const std::string& ip, uint16_t port, int loopCount = 0);
    void run();   // 主线程事件循环（阻塞；只 accept，然后分发给各 loop）
    void stop();  // 请求退出（置标志，run 返回后自动停止并回收所有工作线程）

    void setOnMessage(std::function<void(Connection&, const Frame&)> cb); // 注册收消息回调
    void setOnClose(std::function<void(Connection&)> cb);                 // 注册关闭回调

private:
    void acceptNew();        // 接受新连接并分发给某个工作 loop
    EventLoop* nextLoop();   // round-robin 轮询选择下一个工作 loop

    net::TcpSocket listenSock_;         // 监听 socket（RAII，析构自动关闭）
    int mainEpollFd_ = -1;              // 主线程的 epoll（只登记监听 socket）
    std::vector<EventLoop*> loops_;     // 所有工作 loop
    size_t nextLoopIdx_ = 0;            // 轮询计数器（round-robin 用）
    std::atomic<bool> running_{true};   // 主循环运行标志

    std::function<void(Connection&, const Frame&)> onMsg_;
    std::function<void(Connection&)> onClose_;

    friend class EventLoop;  // EventLoop 需要读取上面的回调（deliverMessage 转发用）
};
}
