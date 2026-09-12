// ============================================================
// ws.cpp —— WebSocket 协议层实现（epoll + reactor，One-Loop-Per-Thread 版）
//
// 本文件依赖 Linux 特有的 epoll（<sys/epoll.h>）和 eventfd（<sys/eventfd.h>），
// 仅在 Linux 下编译运行。
// 架构（相比单线程版的核心变化）：
//   1. Server 变成"主线程 Acceptor"：只负责 accept，然后把新连接分发给工作线程
//   2. 新增 EventLoop：每个工作线程一个独立 epoll + eventfd 事件循环
//   3. Connection 固定属于某一个 EventLoop，所有读写只在该 loop 线程发生
//   4. 跨线程通信只有一条通道：主线程 accept 后往目标 loop 的
//      pendingConns_（mutex 保护）+ eventfd 唤醒
//   5. 保留旧 readFrame/sendFrameTo 给命令行客户端（阻塞模式）使用
// ============================================================
#include "ws.h"
#include "ws_utils.h"
#include <iostream>
#include <cstring>
#include <random>
#include <chrono>
#include <unistd.h>        // close()
#include <errno.h>         // errno / EAGAIN
#include <fcntl.h>         // fcntl() / O_NONBLOCK
#include <sys/epoll.h>     // epoll 系列 API（Linux 专属）
#include <sys/eventfd.h>   // eventfd()：跨线程唤醒"门铃"（Linux 专属）
#include <sys/timerfd.h>   // timerfd_create / timerfd_settime（Linux 专属）
#include <sys/socket.h>    // accept/recv/send
#include <arpa/inet.h>     // sockaddr_in
#include <netinet/in.h>

namespace ws {

static std::mutex g_log_mtx;

// 写缓冲内存回收阈值（1 MB）。背景：clear() 只把 size 归零，capacity 不释放，
// 发完超大消息（如一张几 MB 的图）后内存会一直占着；超过阈值就整体释放。
static constexpr size_t kWriteBufReclaimBytes = 1024 * 1024;

void log(const std::string& s) {
    std::lock_guard<std::mutex> lock(g_log_mtx);
    std::cout << s << "\n" << std::flush;
}

// ============================================================
// 旧接口：阻塞版（保留给命令行客户端 ws_client.cpp）
// ============================================================
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

// 生成 n 字节随机数（用于客户端掩码 key）
static void random_bytes(uint8_t* out, size_t n) {
    static std::mt19937_64 rng([]{
        std::random_device rd;
        uint64_t t = (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
        return (uint64_t(rd()) << 32) ^ uint64_t(rd()) ^ t;
    }());
    for (size_t i = 0; i < n; ++i) out[i] = (uint8_t)(rng() & 0xff);
}

// ============================================================
// 新增：把一帧编码成字节（不进 socket，只写进 vector）
// ============================================================
// 原先的 sendFrameTo 是一边编码一边直接 send。reactor 版不能直接 send
// （可能一次发不完），所以要先把帧编码进 out 缓冲，再由调用方决定怎么发。
// mask=true 时：加 4 字节随机掩码 + payload 逐字节异或（客户端→服务端必须掩码）
static void encodeFrame(Opcode op, const uint8_t* data, size_t len, bool mask,
                        std::vector<uint8_t>& out) {
    out.clear();
    out.push_back(0x80 | static_cast<uint8_t>(op));   // FIN=1 + opcode
    uint8_t maskbit = mask ? 0x80 : 0x00;
    if (len < 126) {                                  // 长度 <= 125：直接放 7 位里
        out.push_back(maskbit | (uint8_t)len);
    } else if (len < 65536) {                         // 长度用 2 字节扩展
        out.push_back(maskbit | 126);
        out.push_back((uint8_t)((len >> 8) & 0xff));
        out.push_back((uint8_t)(len & 0xff));
    } else {                                          // 长度用 8 字节扩展
        out.push_back(maskbit | 127);
        for (int i = 7; i >= 0; --i) out.push_back((uint8_t)(len >> (8 * i)));
    }
    if (mask) {
        uint8_t mk[4];
        random_bytes(mk, 4);
        out.insert(out.end(), mk, mk + 4);
        for (size_t i = 0; i < len; ++i) out.push_back((uint8_t)data[i] ^ mk[i % 4]);
    } else {
        out.insert(out.end(), data, data + len);
    }
}

// 旧接口：编码后直接阻塞发送（客户端用）
bool sendFrameTo(net::TcpSocket& sock, Opcode op, const uint8_t* data, size_t len, bool mask) {
    std::vector<uint8_t> frame;
    encodeFrame(op, data, len, mask, frame);
    return sock.sendAll(frame.data(), frame.size());
}

// ============================================================
// 新增：从字节缓冲解析一个完整帧（服务端 reactor 用）
// ============================================================
// 和阻塞版 readFrame 的区别：数据从 buf 里"抠"，不碰 socket。
// 数据不够 → 返回 false（不是错误，是"还没到齐，下次再来"）。
bool parseFrameFromBuf(const std::vector<uint8_t>& buf, size_t& offset, Frame& frame) {
    size_t p = offset;  // p 是"游标"：当前解析到缓冲的哪个位置

    // ① 帧头最少 2 字节（FIN/opcode + 长度），不够就先等着
    if (buf.size() - p < 2) return false;

    uint8_t b0 = buf[p++];              // 第一字节：FIN + RSV + opcode
    uint8_t b1 = buf[p++];              // 第二字节：MASK + 长度
    frame.fin = (b0 & 0x80) != 0;
    frame.opcode = static_cast<Opcode>(b0 & 0x0F);
    bool masked = (b1 & 0x80) != 0;     // 客户端来的帧都带掩码
    uint64_t len = b1 & 0x7F;

    // ② 扩展长度：126 = 后面还有 2 字节；127 = 后面还有 8 字节
    if (len == 126) {
        if (buf.size() - p < 2) return false;
        len = ((uint64_t)buf[p] << 8) | buf[p + 1];
        p += 2;
    } else if (len == 127) {
        if (buf.size() - p < 8) return false;
        len = 0;
        for (int i = 0; i < 8; ++i) len = (len << 8) | buf[p + i];
        p += 8;
    }

    // ③ 掩码 key（4 字节）
    uint8_t mask_key[4] = {0};
    if (masked) {
        if (buf.size() - p < 4) return false;
        memcpy(mask_key, &buf[p], 4);
        p += 4;
    }

    // ④ payload 还没到齐 → 等更多数据（半包，正常）
    if (buf.size() - p < len) return false;

    // ⑤ 到齐了：把 payload 拷出来，解掩码
    frame.payload.assign(buf.begin() + p, buf.begin() + p + len);
    p += len;
    if (masked) for (size_t i = 0; i < frame.payload.size(); ++i) frame.payload[i] ^= mask_key[i % 4];

    offset = p;   // 游标前进到帧末尾，表示这一帧已经被消费
    return true;
}

// ============================================================
// TimerQueue（基于 timerfd + 最小堆的定时器）
// ============================================================
std::atomic<uint64_t> TimerQueue::s_nextId_{1};

TimerQueue::~TimerQueue() {
    if (timerFd_ >= 0) ::close(timerFd_);
}

bool TimerQueue::init() {
    timerFd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timerFd_ < 0) {
        log("[TimerQueue] timerfd_create failed: " + std::string(std::strerror(errno)));
        return false;
    }
    return true;
}

uint64_t TimerQueue::nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

uint64_t TimerQueue::addTimer(uint64_t delayMs, Callback cb) {
    uint64_t id = s_nextId_.fetch_add(1);
    uint64_t when = nowMs() + delayMs;
    heap_.push(Entry{when, id, std::move(cb)});
    resetTimerfd();   // 新任务可能更早到期，重新对准 timerfd
    return id;
}

bool TimerQueue::cancelTimer(uint64_t timerId) {
    if (timerId == 0) return false;
    // 惰性删除：把 ID 插入集合，等 handleExpired 弹出到这个 ID 时跳过执行。
    cancelled_.insert(timerId);
    return true;
}

void TimerQueue::handleExpired() {
    // 先读 timerfd，清掉可读状态（不读会一直触发）
    uint64_t val;
    ::read(timerFd_, &val, sizeof(val));

    uint64_t now = nowMs();
    // 弹出所有已到期的定时器
    while (!heap_.empty() && heap_.top().when <= now) {
        Entry e = heap_.top();
        heap_.pop();
        // erase 返回删了几个：1=在集合里（已取消）→ 跳过；0=不在 → 执行
        if (cancelled_.erase(e.id)) continue;
        if (e.cb) e.cb();   // 执行回调
    }
    resetTimerfd();   // 重新对准下一个最早到期时间
}

void TimerQueue::resetTimerfd() {
    struct itimerspec its{};
    if (!heap_.empty()) {
        uint64_t now = nowMs();
        uint64_t when = heap_.top().when;
        if (when > now) {
            its.it_value.tv_sec = (when - now) / 1000;
            its.it_value.tv_nsec = ((when - now) % 1000) * 1000000;
        } else {
            // 已过期：立即触发（设 1ns）
            its.it_value.tv_nsec = 1;
        }
    }
    // it_value 全 0 = 取消定时（堆空时）
    ::timerfd_settime(timerFd_, 0, &its, nullptr);
}

// ============================================================
// Connection（非阻塞状态机版，绑定所属 EventLoop）
// ============================================================
Connection::Connection(int fd, EventLoop* loop) : fd_(fd), loop_(loop) {}

Connection::~Connection() {}

int Connection::fd() const { return fd_; }
Connection::State Connection::state() const { return state_; }

// 可读事件入口：epoll 通知"这个 socket 有数据到了"
void Connection::handleRead() {
    char tmp[8192];
    while (true) {
        // 非阻塞 recv：每次尽量多收。收完（EAGAIN）就退出循环
        ssize_t n = ::recv(fd_, tmp, sizeof(tmp), 0);
        if (n > 0) {
            readBuf_.insert(readBuf_.end(), tmp, tmp + n);   // 数据先进读缓冲
            continue;
        } else if (n == 0) {
            close();   // 对端关闭连接
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // 读完了，正常
            if (errno == EINTR) continue;                        // 被信号打断，重试
            log("[fd=" + std::to_string(fd_) + "] recv error: " + std::strerror(errno));
            close();
            return;
        }
    }
    // 数据都进缓冲了，尝试解析（可能解析出多个完整帧）
    if (state_ == State::Handshake) tryHandshake();
    if (state_ == State::Reading)   tryParseMessages();
}

// 可写事件入口：epoll 通知"这个 socket 可以发送数据了"
void Connection::handleWrite() {
    while (writePos_ < writeBuf_.size()) {
        // 从写缓冲的"未发送位置"开始发。
        // reinterpret_cast：vector<uint8_t> 的 data() 是 unsigned char*，
        // send() 需要 const char*（winsock）或 const void*（Linux），显式转换最稳。
        ssize_t n = ::send(fd_, reinterpret_cast<const char*>(writeBuf_.data() + writePos_),
                           writeBuf_.size() - writePos_, 0);
        if (n > 0) {
            writePos_ += (size_t)n;   // 记录已发的位置
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;                    // 发不完了：等下次 EPOLLOUT 再继续
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            close();                  // 发送出错，关闭
            return;
        }
    }
    if (writePos_ == writeBuf_.size()) {
        // 全部发完了：清空缓冲，不再关心 EPOLLOUT，只等读
        writeBuf_.clear();
        writePos_ = 0;
        // 内存回收：clear() 只把 size 归零，capacity（已分配的内存）还占着。
        // 超过阈值就交换一个空 vector，把旧内存整体释放（capacity 归 0）。
        if (writeBuf_.capacity() > kWriteBufReclaimBytes) {
            std::vector<uint8_t>().swap(writeBuf_);
        }
        if (closing_) { close(); return; }   // 优雅关闭：关闭帧也发完了，真正关闭
        loop_->modEvent(fd_, EPOLLIN);       // 把事件兴趣改回"只读"
    }
}

// 关闭连接（幂等：已关闭的再调用直接返回）
void Connection::close() {
    if (state_ == State::Closed) return;
    state_ = State::Closed;
    log("[fd=" + std::to_string(fd_) + "] connection closed");
    // 通知业务层：连接即将关闭（无论是因为收到 Close 帧、对端断开、还是主动 close）
    // → 让 onConnect 启动的定时器有机会在 onClose 里被 cancelTimer
    loop_->deliverClose(this);
    // 从 epoll 和连接表移除；对象本身延迟到本 loop 事件循环末尾删除（防 use-after-free）
    loop_->removeConnection(fd_);
}

// 业务接口：发送任意一帧（服务端 → 客户端，不掩码）
bool Connection::sendFrame(Opcode op, const uint8_t* data, size_t len) {
    if (state_ == State::Closed) return false;   // 已关闭就别发了
    std::vector<uint8_t> frame;
    encodeFrame(op, data, len, false, frame);    // 编码成字节
    enqueue(frame.data(), frame.size());         // 放进写缓冲（不直接 send）
    return true;
}

bool Connection::sendText(const std::string& text) {
    return sendFrame(Opcode::Text, (const uint8_t*)text.data(), text.size());
}

bool Connection::replyCloseFrame(uint16_t code, const std::string& reason) {
    if (closing_) return false;   // 幂等：已经回过关闭帧就不重复回
    closing_ = true;
    std::vector<uint8_t> payload;   // 关闭帧的 payload = 2 字节关闭码 + 可选原因文本
    payload.push_back((code >> 8) & 0xFF);
    payload.push_back(code & 0xFF);
    payload.insert(payload.end(), reason.begin(), reason.end());
    return sendFrame(Opcode::Close, payload.data(), payload.size());
}

// 把字节追加进写缓冲，并登记 EPOLLOUT（让事件循环来发）
void Connection::enqueue(const uint8_t* data, size_t len) {
    writeBuf_.insert(writeBuf_.end(), data, data + len);
    requestWrite();
}

// 告诉 epoll："我既有数据要读，也有数据要发"
void Connection::requestWrite() {
    loop_->modEvent(fd_, EPOLLIN | EPOLLOUT);
}

// ---- 握手状态机 ----
void Connection::tryHandshake() {
    // 把读缓冲的字节追加到请求字符串（握手头是文本，可以当字符串处理）
    reqRaw_.append(reinterpret_cast<const char*>(readBuf_.data()), readBuf_.size());
    readBuf_.clear();

    // 找请求头结束标志 \r\n\r\n
    size_t hdrEnd = reqRaw_.find("\r\n\r\n");
    if (hdrEnd == std::string::npos) {
        if (reqRaw_.size() >= 8192) {  // 请求头超过 8KB，恶意请求，拒绝
            log("[fd=" + std::to_string(fd_) + "] handshake header too large");
            close();
        }
        return;  // 头还没收全，等下次数据
    }

    // 解析 Sec-WebSocket-Key（大小写不敏感查找）
    std::string lower_raw = reqRaw_;
    for (auto& ch : lower_raw) if (ch >= 'A' && ch <= 'Z') ch += 32;
    std::string key_marker = "sec-websocket-key: ";
    size_t pos = lower_raw.find(key_marker);
    if (pos == std::string::npos) {   // 没有 key 字段，不是合法握手
        log("[fd=" + std::to_string(fd_) + "] handshake failed: no key");
        close();
        return;
    }
    size_t start = pos + key_marker.size();
    while (start < reqRaw_.size() && (reqRaw_[start] == ' ' || reqRaw_[start] == '\t')) start++;
    size_t end = reqRaw_.find("\r\n", start);
    std::string key = reqRaw_.substr(start, end - start);

    // 计算 Accept = base64( sha1( key + 固定GUID ) )
    std::string combined = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t hash[20];
    ws_utils::sha1((const uint8_t*)combined.data(), combined.size(), hash);
    std::string accept = ws_utils::base64_encode(hash, 20);

    // 构造 101 响应，写进写缓冲（不阻塞，由事件循环发送）
    std::string resp = "HTTP/1.1 101 Switching Protocols\r\n"
                       "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                       "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
    enqueue(reinterpret_cast<const uint8_t*>(resp.data()), resp.size());
    state_ = State::Reading;
    log("[fd=" + std::to_string(fd_) + "] Handshake succeeded");

    // 通知业务层：新连接已建立（握手成功）→ 可在这里启动 per-connection 定时器
    loop_->deliverConnect(this);

    // 注意：浏览器的握手请求里可能紧跟着就带了数据帧（一个 TCP 包全发过来了），
    // 所以要继续尝试解析剩余数据，别丢帧。
    tryParseMessages();
}

// ---- 数据帧解析（含分片重组）----
void Connection::tryParseMessages() {
    size_t offset = 0;   // 从缓冲的 0 位置开始解析

    while (true) {
        Frame frame;
        if (!parseFrameFromBuf(readBuf_, offset, frame)) break;  // 缓冲里没有完整帧了

        // ---- 控制帧（Close/Ping/Pong）：不可分片，直接交付业务 ----
        if (frame.opcode == Opcode::Close ||
            frame.opcode == Opcode::Ping  ||
            frame.opcode == Opcode::Pong) {
            loop_->deliverMessage(this, frame);
            if (frame.opcode == Opcode::Close) {
                loop_->deliverClose(this);       // 用户onClose回调，用户可选择自定义code/reason调用replyCloseFrame
                if (!closing_) {      
                    replyCloseFrame(1000, "");  // 用户没有回复Close帧，框架自动回复标准1000关闭
                }
                if (writeBuf_.empty()) {
                    close();                    // 没东西要发了，直接关
                }
                break;                          // 停止解析后续输入字节，协议强制
            }
            continue;
        }
        // ---- 数据帧分片重组（逻辑与之前线程版一致）----
        if (frame.opcode == Opcode::Cont) {
            if (!inFrag_) {   // 没有首帧就来续帧 = 协议错误
                log("[fd=" + std::to_string(fd_) + "] Protocol error: unexpected continuation frame");
                close();
                return;
            }
            fragBuf_.insert(fragBuf_.end(), frame.payload.begin(), frame.payload.end());
        } else {
            fragOpcode_ = frame.opcode;                       // 首帧：记录消息类型
            fragBuf_ = std::move(frame.payload);              // 首帧数据直接搬进重组缓冲
            inFrag_ = true;
        }

        if (frame.fin) {   // FIN=1：一条完整消息拼好了
            Frame complete;
            complete.opcode = fragOpcode_;
            complete.fin = true;
            complete.payload = std::move(fragBuf_);
            fragBuf_.clear();
            inFrag_ = false;
            loop_->deliverMessage(this, complete);   // 交付给业务回调
        }
    }

    // 把"已经解析消费掉"的字节从读缓冲里清掉，节省内存
    if (offset > 0) {
        readBuf_.erase(readBuf_.begin(), readBuf_.begin() + offset);
    }
}

// ============================================================
// EventLoop（一个工作线程自己的事件循环）
// ============================================================
EventLoop::EventLoop(Server* server, int idx) : server_(server), idx_(idx) {}

EventLoop::~EventLoop() {
    // 线程的回收由 Server::run 统一负责（stop + join）；
    // 析构时线程必须已结束，否则是使用错误。
}

// 启动：创建 epoll + eventfd，并启动工作线程
void EventLoop::start() {
    // ① 本 loop 自己的 epoll 实例
    epollFd_ = ::epoll_create1(0);
    if (epollFd_ < 0) {
        log("[loop#" + std::to_string(idx_) + "] epoll_create failed: " + std::strerror(errno));
        return;
    }

    // ② eventfd：跨线程唤醒"门铃"。
    //    主线程要往本 loop 塞新连接时，如果本线程正阻塞在 epoll_wait 上，
    //    只有往一个"本 loop 关心的 fd"写数据才能唤醒它——eventfd 就是干这个的。
    //    EFD_NONBLOCK：读它时没计数就立刻返回，不阻塞。
    wakeupFd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeupFd_ < 0) {
        log("[loop#" + std::to_string(idx_) + "] eventfd failed: " + std::strerror(errno));
        return;
    }

    // ③ 把门铃登记进自己的 epoll：关心"可读"（= 有人敲铃）
    epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = wakeupFd_;
    ::epoll_ctl(epollFd_, EPOLL_CTL_ADD, wakeupFd_, &ev);

    // ④ 初始化定时器：创建 timerfd 并登记进 epoll
    if (timerQueue_.init()) {
        epoll_event tev;
        tev.events = EPOLLIN;
        tev.data.fd = timerQueue_.fd();
        ::epoll_ctl(epollFd_, EPOLL_CTL_ADD, timerQueue_.fd(), &tev);
    }

    // ⑤ 启动工作线程，线程主函数是 runInLoop
    thread_ = std::thread(&EventLoop::runInLoop, this);
}

// 工作线程主函数：自己的 epoll_wait 事件循环（阻塞，直到 stop）
void EventLoop::runInLoop() {
    loopThreadId_ = std::this_thread::get_id();   // 记录"我是本 loop 的线程"
    const int MAX_EVENTS = 1024;
    epoll_event events[MAX_EVENTS];

    while (!stop_) {   // stop_ 是原子变量：其他线程设 true 后本循环退出
        // 阻塞等待本 loop 名下所有 fd 的事件（-1 = 永远等；被 eventfd 唤醒也会返回）
        int n = ::epoll_wait(epollFd_, events, MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) continue;   // 被信号打断，继续等
            log("[loop#" + std::to_string(idx_) + "] epoll_wait error: " + std::strerror(errno));
            break;
        }

        for (int i = 0; i < n; ++i) {
            epoll_event& ev = events[i];
            int fd = ev.data.fd;

            // 门铃响了：主线程往本 loop 塞了新连接 → 处理待办队列
            if (fd == wakeupFd_) {
                handleWakeup();
                continue;
            }

            // 定时器到期：timerfd 可读 → 执行到期回调
            if (fd == timerQueue_.fd()) {
                timerQueue_.handleExpired();
                continue;
            }

            // 查本 loop 的连接表；找不到说明已被删除，跳过
            auto it = conns_.find(fd);
            if (it == conns_.end()) continue;
            Connection* c = it->second;

            // 错误/挂断：直接关闭
            if (ev.events & (EPOLLERR | EPOLLHUP)) {
                c->close();
                continue;
            }
            // 可读：收数据（内部可能触发业务回调）
            if (ev.events & EPOLLIN) c->handleRead();
            if (c->state() == Connection::State::Closed) continue;  // 读的时候已关闭，跳过写
            // 可写：发数据
            if (ev.events & EPOLLOUT) c->handleWrite();
        }

        // 每轮事件处理完，统一回收"已标记关闭"的连接对象。
        // 为什么延迟？因为 close() 可能在 handleRead/handleWrite 内部被调用，
        // 如果当场 delete，函数返回后还会访问这个对象（use-after-free）。
        for (Connection* c : pendingDelete_) delete c;
        pendingDelete_.clear();
    }

    closeAll();   // 退出循环后清理本 loop 的所有连接
}

// 等待工作线程结束（Server::run 退出时调用）
void EventLoop::join() {
    if (thread_.joinable()) thread_.join();
}

// 请求退出（可被任意线程调用）：置标志 + 敲铃唤醒阻塞中的 epoll_wait
void EventLoop::stop() {
    stop_ = true;
    wakeup();
}

// 跨线程安全：把新连接的 fd 交给本 loop 接管。
// ① 锁内放进待办队列 ② 敲铃唤醒工作线程（它醒来后在 doPendingConns 里正式接管）
void EventLoop::addConnection(int fd) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        pendingConns_.push_back(fd);
    }
    wakeup();
}

// 敲铃：往 eventfd 写 8 字节计数。eventfd 是计数器，写几就加几，
// 只要计数 > 0，epoll_wait 就会认为它"可读"并立即唤醒。
void EventLoop::wakeup() {
    uint64_t one = 1;
    ssize_t n = ::write(wakeupFd_, &one, sizeof(one));
    (void)n;   // 写失败（如已关闭）无所谓，忽略
}

// 门铃可读：清掉计数，然后处理待办队列
void EventLoop::handleWakeup() {
    uint64_t val = 0;
    ssize_t n = ::read(wakeupFd_, &val, sizeof(val));  // eventfd 读一次即清零
    (void)n;
    doPendingConns();
}

// 把待办队列里的新连接全部正式接管：
// 建 Connection 对象 → 登记进连接表 → 登记进本 loop 的 epoll
void EventLoop::doPendingConns() {
    // 锁内把队列"搬"出来（swap 是 O(1)，拿完立刻放锁，尽量减少持锁时间）
    std::vector<int> fds;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        fds.swap(pendingConns_);
    }

    for (int fd : fds) {
        Connection* c = new Connection(fd, this);   // 绑定本 loop
        conns_[fd] = c;

        // 登记进 epoll：初始只关心"可读"（客户端会先发握手请求）
        epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.fd = fd;
        ::epoll_ctl(epollFd_, EPOLL_CTL_ADD, fd, &ev);

        log("[loop#" + std::to_string(idx_) + "] New connection fd=" + std::to_string(fd));
    }
}

int EventLoop::idx() const { return idx_; }

// 判断调用者是否就是本 loop 的线程（防误用的调试工具）
bool EventLoop::isInLoopThread() const {
    return std::this_thread::get_id() == loopThreadId_;
}

// 修改某 fd 在 epoll 里关心的事件（EPOLL_CTL_MOD）
void EventLoop::modEvent(int fd, uint32_t events) {
    epoll_event ev;
    ev.events = events;
    ev.data.fd = fd;
    ::epoll_ctl(epollFd_, EPOLL_CTL_MOD, fd, &ev);
}

// 从 epoll/连接表移除一个连接（对象延迟到本 loop 事件循环末尾删除）
void EventLoop::removeConnection(int fd) {
    auto it = conns_.find(fd);
    if (it == conns_.end()) return;
    Connection* c = it->second;

    ::epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, nullptr);  // 从 epoll 移除
    ::close(fd);                                        // 关闭 socket
    conns_.erase(it);                                   // 从连接表移除
    pendingDelete_.push_back(c);                        // 对象进延迟删除队列
}

// 把消息帧转发给业务回调（回调在"本 loop 线程"里执行）
void EventLoop::deliverMessage(Connection* c, const Frame& f) {
    if (server_->onMsg_) server_->onMsg_(*c, f);
}

// 把关闭事件转发给业务回调（幂等：同一连接只触发一次）
void EventLoop::deliverClose(Connection* c) {
    if (c->onCloseFired()) return;   // 已触发过，不重复
    c->markCloseFired();
    if (server_->onClose_) server_->onClose_(*c);
}

// 把新连接事件转发给业务回调（握手成功后触发）
void EventLoop::deliverConnect(Connection* c) {
    if (server_->onConnect_) server_->onConnect_(*c);
}

// 定时器：在所属 loop 线程内添加定时器（回调也在本 loop 线程内执行）
uint64_t EventLoop::addTimer(uint64_t delayMs, TimerQueue::Callback cb) {
    return timerQueue_.addTimer(delayMs, std::move(cb));
}

bool EventLoop::cancelTimer(uint64_t timerId) {
    return timerQueue_.cancelTimer(timerId);
}

// 关闭本 loop 的所有连接（线程退出时调用）
void EventLoop::closeAll() {
    for (auto& kv : conns_) {
        ::epoll_ctl(epollFd_, EPOLL_CTL_DEL, kv.first, nullptr);
        ::close(kv.first);
        delete kv.second;
    }
    conns_.clear();
}

// ============================================================
// Server（主线程 Acceptor + 工作 loop 管理）
// ============================================================
bool Server::start(const std::string& ip, uint16_t port, int loopCount) {
    // ① 绑定监听端口
    if (!listenSock_.bindAndListen(ip, port)) {
        log("[Server] bind/listen failed on " + ip + ":" + std::to_string(port));
        return false;
    }
    // 监听 socket 也必须非阻塞：这样 accept 没连接时返回 EAGAIN，不会卡住循环
    listenSock_.setNonBlocking();

    // ② 主线程的 epoll（只登记监听 socket，只管"来新连接"这一件事）
    mainEpollFd_ = ::epoll_create1(0);
    if (mainEpollFd_ < 0) {
        log("[Server] epoll_create failed: " + std::string(std::strerror(errno)));
        return false;
    }
    epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = listenSock_.fd();
    if (::epoll_ctl(mainEpollFd_, EPOLL_CTL_ADD, listenSock_.fd(), &ev) < 0) {
        log("[Server] epoll_ctl add listen failed");
        return false;
    }

    // ③ 创建工作线程（EventLoop）
    if (loopCount <= 0) loopCount = (int)std::thread::hardware_concurrency();  // 自动 = CPU 核数
    if (loopCount < 1) loopCount = 1;
    if (loopCount > 64) loopCount = 64;   // 防呆：上限 64，避免误传超大数字
    for (int i = 0; i < loopCount; ++i) {
        EventLoop* loop = new EventLoop(this, i);
        loops_.push_back(loop);
        loop->start();   // 每个 loop 自己开一个线程
    }

    log("[Server] Listening on " + ip + ":" + std::to_string(port)
        + " (One-Loop-Per-Thread, " + std::to_string(loopCount) + " loops)");
    return true;
}

// 主线程事件循环：只 accept，然后 round-robin 分发给工作 loop
void Server::run() {
    const int MAX_EVENTS = 1024;
    epoll_event events[MAX_EVENTS];

    while (running_) {
        // 500ms 超时：让 stop() 最多 500ms 内生效（有监听事件会立即返回）
        int n = ::epoll_wait(mainEpollFd_, events, MAX_EVENTS, 500);
        if (n < 0) {
            if (errno == EINTR) continue;   // 被信号打断，继续等
            log("[Server] epoll_wait error: " + std::string(std::strerror(errno)));
            break;
        }
        for (int i = 0; i < n; ++i) {
            // 主线程只关心监听 socket：有新连接就 accept 并分发
            if (events[i].data.fd == listenSock_.fd()) acceptNew();
        }
    }

    // 主循环退出：先通知所有工作线程停止，再等它们结束并回收
    for (EventLoop* loop : loops_) loop->stop();
    for (EventLoop* loop : loops_) {
        loop->join();
        delete loop;
    }
    loops_.clear();
}

// 请求退出（可被任意线程调用；run 最多 500ms 后返回并清理）
void Server::stop() {
    running_ = false;
}

// 接受新连接（监听 socket 可读时调用，只在本线程执行）
void Server::acceptNew() {
    while (true) {
        sockaddr_in peer{};
        socklen_t len = sizeof(peer);
        // 非阻塞 accept：没有新连接时返回 -1 / EAGAIN，退出循环
        int cfd = ::accept(listenSock_.fd(), (sockaddr*)&peer, &len);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // 连接都接受完了
            if (errno == EINTR) continue;
            log("[Server] accept error");
            break;
        }

        // 新连接 socket 设为非阻塞（不能用 TcpSocket 包装，否则析构会关闭 fd）
        int flags = ::fcntl(cfd, F_GETFL, 0);
        ::fcntl(cfd, F_SETFL, flags | O_NONBLOCK);

        // ★ One-Loop-Per-Thread 的关键一步：负载均衡，把新连接交给某个工作 loop
        //   这个调用是跨线程的（主线程 → 工作线程），EventLoop::addConnection 内部
        //   用"待办队列 + eventfd 唤醒"完成线程间交接。
        EventLoop* loop = nextLoop();
        loop->addConnection(cfd);

        log("[Server] fd=" + std::to_string(cfd) + " -> loop#" + std::to_string(loop->idx()));
    }
}

// round-robin（轮流）：第 0,1,2,...,N-1,0,1,... 个 loop，让负载尽量均匀
EventLoop* Server::nextLoop() {
    EventLoop* loop = loops_[nextLoopIdx_ % loops_.size()];
    ++nextLoopIdx_;
    return loop;
}

void Server::setOnMessage(std::function<void(Connection&, const Frame&)> cb) { onMsg_ = std::move(cb); }
void Server::setOnClose(std::function<void(Connection&)> cb) { onClose_ = std::move(cb); }
void Server::setOnConnect(std::function<void(Connection&)> cb) { onConnect_ = std::move(cb); }

}
