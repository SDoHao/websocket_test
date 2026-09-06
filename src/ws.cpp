// ============================================================
// ws.cpp —— WebSocket 协议层实现（epoll + reactor 版）
//
// 本文件依赖 Linux 特有的 epoll（<sys/epoll.h>），仅在 Linux 下编译运行。
// 改造要点：
//   1. 帧解析从"阻塞读 socket"改为"从内存缓冲解析"（parseFrameFromBuf）
//   2. Connection 变成非阻塞状态机：Handshake → Reading → Closed
//   3. Server 变成单线程 epoll 事件循环（reactor 模式）
//   4. 保留旧 readFrame/sendFrameTo 给命令行客户端（阻塞模式）使用
// ============================================================
#include "ws.h"
#include "ws_utils.h"
#include <iostream>
#include <cstring>
#include <random>
#include <chrono>
#include <unistd.h>       // close()
#include <errno.h>        // errno / EAGAIN
#include <fcntl.h>        // fcntl() / O_NONBLOCK（显式依赖，不靠头文件顺带包含）
#include <sys/epoll.h>    // epoll 系列 API（Linux 专属）
#include <sys/socket.h>   // accept/recv/send
#include <arpa/inet.h>    // sockaddr_in
#include <netinet/in.h>

namespace ws {

static std::mutex g_log_mtx;

// 写缓冲内存回收阈值（1 MB）。
// 背景：clear() 只把 size 归零，capacity（vector 已分配的内存）不会释放，
// 如果某个连接之前发过超大消息（比如一张几 MB 的图），这块内存会一直占着直到连接关闭。
// 发完缓冲后若 capacity 超过该阈值，就整体释放，把内存还给系统。
// 小于该值的消息（常规的小帧）永远不会触发，避免频繁分配开销。
static constexpr size_t kWriteBufReclaimBytes = 1024 * 1024;

void log(const std::string& s) {
    std::lock_guard<std::mutex> lock(g_log_mtx);
    std::cout << s << "\n";
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
// Connection（非阻塞状态机版）
// ============================================================
Connection::Connection(int fd, Server* server) : fd_(fd), server_(server) {}

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
            // 收到数据：追加进读缓冲（可能只是半包，先攒着）
            readBuf_.insert(readBuf_.end(), tmp, tmp + n);
        } else if (n == 0) {
            // 对端关闭了连接（TCP FIN）
            log("[fd=" + std::to_string(fd_) + "] peer closed");
            close();
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
        // 如果之前发过超大消息（如一张几 MB 的图），capacity 会一直保留到连接关闭，
        // 这就是"发完内存不还"的内存泄漏点。
        // 超过阈值就交换一个空 vector，把旧内存整体释放（capacity 归 0），
        // 下次 enqueue 需要时再重新分配（低频大消息场景下这个开销可忽略）。
        if (writeBuf_.capacity() > kWriteBufReclaimBytes) {
            std::vector<uint8_t>().swap(writeBuf_);
        }
        if (closing_) { close(); return; }   // 优雅关闭：关闭帧也发完了，真正关闭
        server_->modEvent(fd_, EPOLLIN);     // 把事件兴趣改回"只读"
    }
}

// 关闭连接（幂等：已关闭的再调用直接返回）
void Connection::close() {
    if (state_ == State::Closed) return;
    state_ = State::Closed;
    log("[fd=" + std::to_string(fd_) + "] connection closed");
    // 从 epoll 和连接表移除；对象本身延迟到事件循环末尾删除（防 use-after-free）
    server_->removeConnection(fd_);
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
    server_->modEvent(fd_, EPOLLIN | EPOLLOUT);
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
            server_->deliverMessage(this, frame);
            if (frame.opcode == Opcode::Close) {
                // 收到关闭帧：通知业务（demo 里会回一个 Close 帧），
                // 然后进入"优雅关闭"：等写缓冲发完再真正关闭
                closing_ = true;
                server_->deliverClose(this);
                if (writeBuf_.empty()) close();   // 没东西要发了，直接关
                break;                            // 不再解析后续数据
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
            server_->deliverMessage(this, complete);   // 交付给业务回调
        }
    }

    // 把"已经解析消费掉"的字节从读缓冲里清掉，节省内存
    if (offset > 0) {
        readBuf_.erase(readBuf_.begin(), readBuf_.begin() + offset);
    }
}

// ============================================================
// Server（epoll + reactor 事件循环）
// ============================================================
bool Server::start(const std::string& ip, uint16_t port) {
    // ① 绑定监听端口
    if (!listenSock_.bindAndListen(ip, port)) {
        log("[Server] bind/listen failed on " + ip + ":" + std::to_string(port));
        return false;
    }
    // 监听 socket 也必须非阻塞：这样 accept 没连接时返回 EAGAIN，不会卡住循环
    listenSock_.setNonBlocking();

    // ② 创建 epoll 实例
    epollFd_ = ::epoll_create1(0);
    if (epollFd_ < 0) {
        log("[Server] epoll_create failed: " + std::string(std::strerror(errno)));
        return false;
    }

    // ③ 把监听 socket 登记进 epoll：关心"可读"（= 有新连接到来）
    epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = listenSock_.fd();
    if (::epoll_ctl(epollFd_, EPOLL_CTL_ADD, listenSock_.fd(), &ev) < 0) {
        log("[Server] epoll_ctl add listen failed");
        return false;
    }
    log("[Server] Listening on " + ip + ":" + std::to_string(port) + " (epoll reactor)");
    return true;
}

// 事件循环：唯一的线程在这里阻塞等待，直到进程退出
void Server::run() {
    const int MAX_EVENTS = 1024;
    epoll_event events[MAX_EVENTS];   // 内核返回的就绪事件数组

    while (true) {
        // 阻塞等待事件（-1 = 永远等，直到有事件）
        int n = ::epoll_wait(epollFd_, events, MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) continue;   // 被信号打断，继续等
            log("[Server] epoll_wait error: " + std::string(std::strerror(errno)));
            break;
        }
        for (int i = 0; i < n; ++i) {
            epoll_event& ev = events[i];
            int fd = ev.data.fd;

            // 监听 socket 可读 = 有客户端来连接
            if (fd == listenSock_.fd()) {
                acceptNew();
                continue;
            }

            // 查连接表，找不到说明已被删除，跳过
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

    closeAll();   // 退出循环后清理所有连接
}

// 接受新连接（监听 socket 可读时调用）
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

        // 创建 Connection，登记进连接表
        Connection* c = new Connection(cfd, this);
        conns_[cfd] = c;

        // 登记进 epoll：初始只关心"可读"（客户端会先发握手请求）
        epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.fd = cfd;
        ::epoll_ctl(epollFd_, EPOLL_CTL_ADD, cfd, &ev);

        log("[Server] New connection fd=" + std::to_string(cfd));
    }
}

// 修改某 fd 在 epoll 里关心的事件（EPOLL_CTL_MOD）
void Server::modEvent(int fd, uint32_t events) {
    epoll_event ev;
    ev.events = events;
    ev.data.fd = fd;
    ::epoll_ctl(epollFd_, EPOLL_CTL_MOD, fd, &ev);
}

// 从 epoll/连接表移除一个连接（对象延迟到事件循环末尾删除）
void Server::removeConnection(int fd) {
    auto it = conns_.find(fd);
    if (it == conns_.end()) return;
    Connection* c = it->second;

    ::epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, nullptr);  // 从 epoll 移除
    ::close(fd);                                        // 关闭 socket
    conns_.erase(it);                                   // 从连接表移除
    pendingDelete_.push_back(c);                        // 对象进延迟删除队列
}

// 把消息帧转发给业务回调
void Server::deliverMessage(Connection* c, const Frame& f) {
    if (onMsg_) onMsg_(*c, f);
}

// 把关闭事件转发给业务回调
void Server::deliverClose(Connection* c) {
    if (onClose_) onClose_(*c);
}

void Server::setOnMessage(std::function<void(Connection&, const Frame&)> cb) { onMsg_ = std::move(cb); }
void Server::setOnClose(std::function<void(Connection&)> cb) { onClose_ = std::move(cb); }

// 关闭所有连接（事件循环退出时调用）
void Server::closeAll() {
    for (auto& kv : conns_) {
        ::epoll_ctl(epollFd_, EPOLL_CTL_DEL, kv.first, nullptr);
        ::close(kv.first);
        delete kv.second;
    }
    conns_.clear();
}

}
