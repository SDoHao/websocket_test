# 定时器保姆级教程

> 假设你完全不懂定时器，从头讲起。每一段代码都有注释。

---

## 第一章：什么是定时器？

你每天早上用闹钟：设好 7:00，到点了闹钟响，你起床。

程序里的定时器一模一样：**你告诉它"X 毫秒后帮我执行一段代码"，它到点就执行。**

```
现在时间 ──────────► 3秒后 ──────────────────►
                      │
                      ▼
              执行你指定的函数
```

---

## 第二章：为什么用 timerfd？

Linux 提供了一种特殊文件叫 **timerfd**（timer file descriptor）。

它的特点：**你设好超时时间，到点了这个文件就变成"可读"状态。**

而我们的 epoll 正好就是专门监控"哪些文件可读"的！

所以把 timerfd 往 epoll 里一挂：

```
epoll 监控着：
  ├── fd=5 (客户端连接1) → 可读 = 有数据来了
  ├── fd=6 (客户端连接2) → 可读 = 有数据来了
  └── fd=7 (timerfd)     → 可读 = 定时器到点了！
```

这样定时器和网络事件**统一在 epoll 的事件循环里处理**，不需要额外开线程。

---

## 第三章：核心概念图

整个定时器系统的结构：

```
EventLoop（一个线程的事件循环）
  │
  ├── epoll（监控所有 fd 的事件）
  │     ├── 客户端连接 fd
  │     ├── wakeup fd（跨线程通知用）
  │     └── timerfd ← 定时器专用
  │
  └── TimerQueue（管理所有定时任务）
        │
        ├── 最小堆（按到期时间排序）
        │   ├── 任务A：12:00:03 到期 → 执行函数A
        │   ├── 任务B：12:00:05 到期 → 执行函数B
        │   └── 任务C：12:00:10 到期 → 执行函数C
        │
        └── timerfd 永远对准堆顶（最早到期的那个）
```

**关键点**：不管你加了 1 个还是 1000 个定时器，底层只有**一个 timerfd**，它总是对准最早到期的那个任务。到期后一次性弹出所有过期的任务执行，再重新对准下一个。

---

## 第四章：三个核心函数（逐行讲解）

### 4.1 addTimer —— 添加定时器

```cpp
// 参数：delayMs = 多少毫秒后执行，cb = 要执行的函数
// 返回值：timerId（一个唯一编号，用来取消定时器）
uint64_t TimerQueue::addTimer(uint64_t delayMs, Callback cb) {
    // ① 生成一个全局唯一的 ID（递增，永不重复）
    //    为什么要 ID？因为以后你可能想取消这个定时器
    //    比如：你下发了指令，启动了 3 秒超时定时器，
    //    但客户端 1 秒就回 ack 了，你要取消那个还没到期的定时器
    uint64_t id = s_nextId_.fetch_add(1);

    // ② 计算到期时间点 = 现在 + 延迟
    //    比如 now=10000ms, delayMs=3000 → when=13000ms
    //    意思是"在时间戳 13000ms 时执行"
    uint64_t when = nowMs() + delayMs;

    // ③ 把任务塞进最小堆
    //    最小堆 = 一种数据结构，堆顶永远是最小的（最早到期的）
    //    这样每次只需看堆顶就知道下一个该执行的任务
    heap_.push(Entry{when, id, std::move(cb)});

    // ④ 重新校准 timerfd
    //    因为新加的任务可能比之前所有任务都早到期
    //    比如之前堆顶是 5 秒后，新任务是 2 秒后
    //    那 timerfd 要改成 2 秒后触发
    resetTimerfd();

    return id;
}
```

### 4.2 cancelTimer —— 取消定时器

```cpp
// 参数：timerId = addTimer 返回的那个编号
bool TimerQueue::cancelTimer(uint64_t timerId) {
    if (timerId == 0) return false;

    // 惰性删除：把 ID 插入 cancelled_ 集合，
    // 等 handleExpired 弹出到这个 ID 时检查集合、跳过执行。
    //
    // 为什么不从堆里直接删？
    //   priority_queue 不支持随机访问删除，从堆中间删一个元素是 O(n)。
    //   而往集合里插入一个 ID 是 O(1)，等它自然到顶时跳过就行。
    //   这叫"惰性删除"（lazy deletion）。
    //
    // 为什么用 unordered_set 而不是 unordered_map<uint64_t, bool>？
    //   因为 bool 值永远是 true，"已取消"这个状态是"有/无"关系，
    //   语义上是一个集合（set），不是映射（map）。
    //   用 set 后，erase() 的返回值同时完成"检查是否存在"和"删除"两步。
    cancelled_.insert(timerId);
    return true;
}
```

### 4.3 handleExpired —— 处理到期的定时器

```cpp
// 当 epoll 发现 timerfd 可读时调用这个函数
void TimerQueue::handleExpired() {
    // ① 先读 timerfd，清掉"可读"状态
    //    timerfd 的特点是：到点了变成可读，但你必须读它，
    //    不然下次 epoll 会立刻又报它可读（就像你没关闹钟它会一直响）
    uint64_t val;
    ::read(timerFd_, &val, sizeof(val));

    // ② 获取当前时间
    uint64_t now = nowMs();

    // ③ 循环弹出所有已到期的任务
    //    堆顶的 when <= now 说明到期了
    while (!heap_.empty() && heap_.top().when <= now) {
        Entry e = heap_.top();   // 取出堆顶
        heap_.pop();             // 从堆里移除

        // erase 返回删了几个：1 = 在集合里（已取消）→ 跳过；0 = 不在 → 执行
        // 这一步同时完成"检查是否取消"和"清理标记"两件事
        if (cancelled_.erase(e.id)) continue;

        // 没被取消 → 执行回调函数
        if (e.cb) e.cb();
    }

    // ④ 重新校准 timerfd，对准下一个最早到期的任务
    //    如果堆空了，timerfd 就取消定时（设为 0）
    resetTimerfd();
}
```

### 4.4 resetTimerfd —— 校准 timerfd（私有函数）

```cpp
void TimerQueue::resetTimerfd() {
    struct itimerspec its{};

    if (!heap_.empty()) {
        // 堆不空 → timerfd 设为堆顶的到期时间
        uint64_t now = nowMs();
        uint64_t when = heap_.top().when;

        if (when > now) {
            // 还没到期 → 算差值
            // 比如 when=13000, now=10000 → 差 3000ms = 3秒
            its.it_value.tv_sec = (when - now) / 1000;        // 秒
            its.it_value.tv_nsec = ((when - now) % 1000) * 1000000; // 纳秒
        } else {
            // 已过期 → 立即触发（设 1 纳秒）
            its.it_value.tv_nsec = 1;
        }
    }
    // 如果堆空了，its 全 0 → timerfd 取消定时（不再触发）

    // 真正设置 timerfd 的超时时间
    ::timerfd_settime(timerFd_, 0, &its, nullptr);
}
```

---

## 第五章：怎么在代码里用？

在回调函数里（onConnect / onMessage / 定时器回调里），你可以这样用：

```cpp
// 获取 Connection 所属的 EventLoop
ws::Connection& conn = ...;

// ---- 添加一个 3 秒后执行的定时器 ----
uint64_t timerId = conn.loop()->addTimer(3000, []() {
    std::cout << "3 秒过去了！\n";
});

// ---- 如果不需要了，取消它 ----
conn.loop()->cancelTimer(timerId);
```

就这么简单。`addTimer` + `cancelTimer` 两个函数就能玩转定时器。

---

## 第六章：超简单示例

> 这个示例只演示定时器本身，不涉及 WebSocket 连接。
> 你可以把它当成最小可运行的学习用例。

```cpp
// timer_demo.cpp —— 最简定时器示例
// 编译：g++ timer_demo.cpp -o timer_demo
// 运行：./timer_demo

#include <iostream>
#include <chrono>
#include <functional>
#include <atomic>
#include <unordered_set>

// ====== 简化版定时器（教学用，单线程，无 epoll）======
class SimpleTimer {
public:
    using Callback = std::function<void()>;  // 回调类型：无参数、无返回值的函数

    // 添加定时器
    // delayMs：多少毫秒后执行
    // cb：要执行的函数
    // 返回：定时器 ID
    uint64_t addTimer(uint64_t delayMs, Callback cb) {
        uint64_t id = nextId_++;                        // 生成唯一 ID
        uint64_t when = nowMs() + delayMs;              // 算出到期时间点
        timers_.push_back({when, id, cb});              // 存进列表
        return id;
    }

    // 取消定时器（标记为取消）
    void cancelTimer(uint64_t id) {
        cancelled_.insert(id);  // 插入集合 = 标记取消
    }

    // 检查并执行所有到期的定时器（主循环里反复调用）
    void check() {
        uint64_t now = nowMs();
        for (auto it = timers_.begin(); it != timers_.end(); ) {
            if (it->when <= now) {
                // 到期了
                // erase 返回 1 = 在集合里（已取消）→ 跳过
                // erase 返回 0 = 不在集合里 → 执行
                if (cancelled_.erase(it->id)) {
                    // 已取消，跳过（erase 同时清理了集合里的标记）
                } else {
                    it->cb();  // 没被取消，执行回调
                }
                it = timers_.erase(it);  // 从列表移除
            } else {
                ++it;  // 还没到期，看下一个
            }
        }
    }

    // 检查是否有定时器在等待
    bool empty() const { return timers_.empty(); }

private:
    struct Timer {
        uint64_t when;       // 到期时间点
        uint64_t id;         // 唯一 ID
        Callback cb;         // 回调函数
    };

    std::vector<Timer> timers_;
    std::unordered_set<uint64_t> cancelled_;  // 已取消的 ID 集合（惰性删除）
    std::atomic<uint64_t> nextId_{1};

    // 获取当前时间的毫秒数
    static uint64_t nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count();
    }
};

// ====== 主函数：演示定时器的用法 ======
int main() {
    SimpleTimer timer;

    // 场景 1：3 秒后打印一条消息
    uint64_t t1 = timer.addTimer(3000, []() {
        std::cout << "[定时器1] 3秒到了！\n";
    });

    // 场景 2：5 秒后打印另一条消息
    timer.addTimer(5000, []() {
        std::cout << "[定时器2] 5秒到了！\n";
    });

    // 场景 3：2 秒后取消定时器1（所以定时器1不会执行）
    timer.addTimer(2000, [&timer, t1]() {
        std::cout << "[定时器3] 取消定时器1\n";
        timer.cancelTimer(t1);
    });

    // 场景 4：1 秒后打印"开始"
    timer.addTimer(1000, []() {
        std::cout << "[定时器4] 1秒到了，开始！\n";
    });

    // 主循环：不断检查有没有定时器到期
    // （我们的 ws.cpp 用 epoll+timerfd 替代了这个循环，
    //   但原理一样：反复检查到期任务并执行）
    std::cout << "主循环启动，等待定时器触发...\n";
    while (!timer.empty()) {
        timer.check();
        // 教学用：忙等待（实际项目不会这样写，
        // ws.cpp 里用 epoll_wait 阻塞等待，不消耗 CPU）
    }

    std::cout << "所有定时器执行完毕\n";
    return 0;
}
```

运行结果：
```
主循环启动，等待定时器触发...
[定时器4] 1秒到了，开始！
[定时器3] 取消定时器1
[定时器2] 5秒到了！
所有定时器执行完毕
```

注意：定时器1 被取消了，所以"3秒到了"不会打印。

---

## 第七章：项目里的实战用法

### 7.1 心跳超时（自动断开僵死连接）

**场景**：客户端连上来之后不说话了（比如崩溃了），连接还挂着占资源。需要定期检查，超时就断开。

```cpp
// 在 onConnect 回调里：连接建立时启动心跳定时器
server.setOnConnect([](ws::Connection& conn) {
    // ① 创建 per-连接上下文，记录"最后一次活跃时间"
    auto ctx = std::make_shared<ConnCtx>();
    ctx->lastActiveMs = nowMs();     // 刚连上，活跃时间 = 现在
    conn.userData = ctx;              // 挂到连接上

    // ② 启动心跳定时器：5 秒后检查一次
    //    用"自重启"模式实现周期检查
    //    （addTimer 是一次性的，到点执行后就没了。
    //     要实现"每 5 秒检查一次"，就在回调里再 addTimer 一个新的）
    ws::Connection* p = &conn;
    auto hbCheck = std::make_shared<std::function<void()>>();

    // 下面这个函数 = "检查一次心跳，然后重新设定时器"
    *hbCheck = [p, hbCheck]() {
        auto ctx = std::static_pointer_cast<ConnCtx>(p->userData);
        if (!ctx) return;

        // 算出"距离上次活跃过了多久"
        int64_t elapsed = nowMs() - ctx->lastActiveMs.load();

        if (elapsed > 10000) {
            // 超过 10 秒没活跃 → 僵死连接，断开！
            p->close();
            return;  // close 了就不重新设定时器了
        }

        // 还活着 → 5 秒后再检查一次（自重启）
        ctx->heartbeatTimerId = p->loop()->addTimer(5000, *hbCheck);
    };

    // 第一次启动：5 秒后开始检查
    ctx->heartbeatTimerId = conn.loop()->addTimer(5000, *hbCheck);
});

// 在 onMessage 回调里：收到任何数据都刷新活跃时间
server.setOnMessage([](ws::Connection& conn, const ws::Frame& frame) {
    auto ctx = std::static_pointer_cast<ConnCtx>(conn.userData);
    if (ctx) ctx->lastActiveMs = nowMs();  // ← 刷新！
    // ... 处理消息 ...
});

// 在 onClose 回调里：取消定时器
server.setOnClose([](ws::Connection& conn) {
    auto ctx = std::static_pointer_cast<ConnCtx>(conn.userData);
    if (ctx && ctx->heartbeatTimerId)
        conn.loop()->cancelTimer(ctx->heartbeatTimerId);
});
```

时间线：
```
0s   连接建立，lastActive=0s，启动定时器
5s   定时器触发：elapsed=5s < 10s → 还活着，重新设定时器
     （假设客户端期间发过数据，lastActive 被刷新到 3s）
     实际 elapsed = 5s - 3s = 2s < 10s → 活着
10s  定时器触发：elapsed=10s - 3s = 7s < 10s → 还活着
15s  定时器触发：elapsed=15s - 3s = 12s > 10s → 僵死！close()
```

### 7.2 指令超时（等 ack，超时重发）

**场景**：给机械臂下发运动指令，等它回 ack 确认。如果 3 秒没回，重发。最多重发 3 次。

```cpp
// 客户端发来 "MOVE" → 下发指令 + 启动超时定时器
if (text == "MOVE") {
    ws::Connection* p = &conn;

    // 超时回调：3 秒没收到 ack → 重发或报错
    auto onTimeout = std::make_shared<std::function<void()>>();
    *onTimeout = [p, onTimeout]() {
        auto ctx = std::static_pointer_cast<ConnCtx>(p->userData);
        if (!ctx || ctx->cmdTimerId == 0) return;  // 已被 ack 取消

        ctx->cmdRetryCount++;
        if (ctx->cmdRetryCount >= 3) {
            // 超过 3 次重发 → 放弃，报错
            p->sendText("ERROR:MOVE_TIMEOUT");
            ctx->cmdTimerId = 0;
            return;
        }

        // 还没到 3 次 → 重发指令 + 重新启动定时器
        p->sendText("CMD:MOVE arm_to(100,200,150)");
        ctx->cmdTimerId = p->loop()->addTimer(3000, *onTimeout);
    };

    // 第一次下发指令
    conn.sendText("CMD:MOVE arm_to(100,200,150)");

    // 启动 3 秒超时定时器
    ctx->cmdTimerId = conn.loop()->addTimer(3000, *onTimeout);
}

// 客户端发来 "ACK" → 取消超时定时器
if (text == "ACK") {
    if (ctx->cmdTimerId) {
        conn.loop()->cancelTimer(ctx->cmdTimerId);  // ← 取消！
        ctx->cmdTimerId = 0;
    }
    conn.sendText("OK:ACK_RECEIVED");
}
```

时间线（客户端不回 ack 的情况）：
```
0s   下发 CMD:MOVE，启动 3s 定时器
3s   定时器触发，retry=1/3，重发 CMD，重新启动 3s 定时器
6s   定时器触发，retry=2/3，重发 CMD，重新启动 3s 定时器
9s   定时器触发，retry=3/3 ≥ 3，发送 ERROR:MOVE_TIMEOUT，放弃
```

时间线（客户端 1s 后回 ack 的情况）：
```
0s   下发 CMD:MOVE，启动 3s 定时器（timerId=123）
1s   收到 ACK → cancelTimer(123)，定时器被取消
3s   （定时器不会触发，因为被取消了）
     回复 OK:ACK_RECEIVED
```

---

## 第八章：lambda 回调里的捕获陷阱

定时器回调用 lambda 写，有几个**容易踩的坑**：

### 坑 1：捕获了局部变量的引用，但变量已经销毁

```cpp
// ❌ 错误：conn 是局部引用，回调执行时 conn 可能已经不在了
server.setOnConnect([](ws::Connection& conn) {
    conn.loop()->addTimer(3000, [&conn]() {  // 捕获了引用
        conn.sendText("hello");  // CRASH！conn 可能已销毁
    });
});
```

```cpp
// ✅ 正确：用指针，配合 shared_ptr 保证生命周期
ws::Connection* p = &conn;
auto cb = std::make_shared<std::function<void()>>();
*cb = [p, cb]() {
    p->sendText("hello");  // p 指向 Connection，由 EventLoop 管理生命周期
};
conn.loop()->addTimer(3000, *cb);
```

### 坑 2：自重启时 lambda 需要引用自己

```cpp
// 要实现"每 5 秒执行一次"，需要回调里重新 addTimer
// 但回调需要引用自己 → 用 shared_ptr<function> 打破循环引用

auto hbCheck = std::make_shared<std::function<void()>>();  // 先创建空壳
*hbCheck = [p, hbCheck]() {           // hbCheck 捕获了自己（shared_ptr）
    // ... 检查心跳 ...
    // 重新设定时器，回调还是自己
    p->loop()->addTimer(5000, *hbCheck);
};
conn.loop()->addTimer(5000, *hbCheck);  // 启动
```

为什么用 `shared_ptr<function>` 而不是直接 `std::function`？
- `std::function` 不能捕获自己（类型不完整时没法引用）
- `shared_ptr` 延长了 lambda 的生命周期，确保回调执行时 lambda 还活着

---

## 第九章：API 速查表

```cpp
// === 在 Connection 的回调里（onConnect/onMessage/onClose）使用 ===

// 获取所属 EventLoop
conn.loop();

// 添加定时器（当前线程内执行回调）
// delayMs：毫秒数
// cb：回调函数
// 返回：timerId（用于取消）
uint64_t timerId = conn.loop()->addTimer(3000, []() {
    // 3 秒后执行这段代码
});

// 取消定时器
conn.loop()->cancelTimer(timerId);

// === Connection 上的 per-连接上下文 ===
// 存：
conn.userData = std::make_shared<MyContext>();
// 取：
auto ctx = std::static_pointer_cast<MyContext>(conn.userData);
```

---

## 第十章：调试复盘 —— 三个 Bug 的定位与修复

> 这一章记录了开发过程中实际遇到的三个 Bug，包括现象、定位过程、根因和修复。
> 也是简历里可以提到的"问题排查与解决"经验。

---

### Bug 1：cancelTimer 是空操作（逻辑 Bug）

**现象**：客户端发 ACK 后，服务端打印了"timer cancelled"，但 3 秒后定时器仍然触发了，导致指令超时逻辑继续运行。

**定位过程**：
1. 在 `cancelTimer` 里加日志，发现它每次都返回 `false`（没找到定时器）。
2. 读代码发现：`cancelTimer` 在 `cancelled_` 这个 map 里 **find** ID，但 ID 从来没被 **insert** 过 —— `addTimer` 不往 `cancelled_` 里插入。
3. `cancelled_` 永远是空的，所以 `find` 永远返回 `end()`，`cancelTimer` 永远返回 `false`。

**根因**：设计意图是"惰性删除"——`addTimer` 往 `cancelled_` 插入 `{id, false}`，`cancelTimer` 把它改成 `true`，`handleExpired` 检查并跳过。但 `addTimer` 里漏了插入那一步，导致整条链路断掉。

**修复**：`cancelTimer` 直接往 `cancelled_` 里 `insert(id)` 标记取消，不依赖 `addTimer` 预先插入。同时把 `unordered_map<uint64_t, bool>` 改成 `unordered_set<uint64_t>` —— bool 值永远是 true，语义上是一个集合而不是映射。

```
修复前：cancelled_.find(id) → 永远找不到 → 返回 false → 定时器照常触发
修复后：cancelled_.insert(id) → 标记取消 → handleExpired 里 erase(id) 返回 1 → 跳过执行
```

**Bug 类型**：逻辑错误 —— 函数实现与设计意图不符，依赖了一个从未发生的前置条件。

---

### Bug 2：连接断开时 Bus error (core dumped)（生命周期/use-after-free）

**现象**：客户端正常断开后，服务端过几秒崩溃，报 `Bus error (core dumped)`。

**定位过程**：
1. Bus error 通常是访问了已释放的内存。用 `gdb` 或看崩溃时机，发现总是在连接断开几秒后。
2. 读代码发现：客户端直接断开（recv 返回 0）时，`Connection::close()` 只调用了 `removeConnection`（把对象放进延迟删除队列），**但没有调用 `onClose` 回调**。
3. `onClose` 回调里才有 `cancelTimer` 的逻辑。既然 `onClose` 没被调用，心跳定时器就没有被取消。
4. 几秒后心跳定时器触发，回调里访问 `p->userData`，但此时 `Connection` 对象已经被 `pendingDelete_` 清理掉了 → **use-after-free** → Bus error。
5. 再加上 Bug 1（cancelTimer 是空操作），即使 `onClose` 被调用了也取消不了定时器，两个 Bug 叠加导致必现崩溃。

**根因**：`Connection::close()` 的关闭路径不完整 —— 只在"收到 Close 帧"时才触发 `onClose`，而"对端直接断开"这条路径漏了。定时器回调持有原始指针 `Connection* p`，对象被删后指针悬空。

**修复**（两步）：
1. `Connection::close()` 里调用 `deliverClose(this)`，保证任何关闭路径都触发 `onClose` 回调。
2. `deliverClose` 加幂等保护（`onCloseFired_` 标志），防止收到 Close 帧的路径重复调用。

```
修复前：客户端断开 → close() → removeConnection → delete Connection
                                    （没调 onClose）
        → 几秒后定时器触发 → 访问已删除的 Connection → Bus error

修复后：客户端断开 → close() → deliverClose（触发 onClose）→ cancelTimer
                             → removeConnection → delete Connection
        → 定时器已被取消 → 不会触发 → 安全
```

**Bug 类型**：对象生命周期管理错误 —— 定时器回调持有裸指针，对象销毁后指针悬空（use-after-free）。

---

### Bug 3：客户端 -k 测试没有检测服务端断开（测试覆盖缺陷）

**现象**：用 `./ws_client -k 15` 测试心跳超时，服务端日志显示 10 秒时已断开连接，但客户端一直打印"仍连接..."，15 秒后报告"测试结束"，完全没有感知到服务端断开。

**定位过程**：
1. 读 `testKeepalive` 函数代码，发现它只是 `sleep_for(1s)` + 打印"仍连接"，**从不调用 `readFrame`**。
2. 客户端的 socket 是阻塞模式，如果不主动 `recv`，根本不知道对端关没关。
3. 服务端在 10 秒时 `close()` 了连接，TCP 层发了 FIN，但客户端没读 socket 就不知道。

**根因**：测试函数只做了"等待"，没有做"检测"。阻塞式 `readFrame` 会卡住整个循环，所以不能直接用它。

**修复**：用 `poll()` 带 1 秒超时检测 socket 是否有事件（可读/断开），有事件就调 `readFrame` 确认；`readFrame` 返回 false 则说明服务端断开了。

```
修复前：sleep(1s) → 打印"仍连接" → 循环15次 → 报告"测试结束"（假的）
修复后：poll(fd, 1s) → 有事件 → readFrame → 返回false → 打印"[服务端断开]"
```

**Bug 类型**：测试逻辑缺陷 —— 测试函数没有验证被测行为，只做了时间等待没有做状态检测。

---

### 三个 Bug 的关联

这三个 Bug 不是孤立的，它们形成了一条**故障链**：

```
Bug 1 (cancelTimer 空操作)
  └→ 即使 onClose 被调用，定时器也取消不了
      └→ Bug 2 (close 不触发 onClose) 让情况更糟
          └→ 定时器在 Connection 删除后仍然触发 → 崩溃
              └→ Bug 3 (测试不检测断开) 让问题不容易被发现
```

修复顺序也是依次排除：先修 cancelTimer（让取消功能可用），再修 close 路径（让取消能被调用），最后修测试（让断开可被感知）。

---

## 附：编译和测试命令

```bash
# 编译
cd /home/websocket_test
bash build.sh

# 测试指令超时（不回 ACK，看服务端重发 3 次后报错）
./ws_server 8080        # 终端1：启动服务端
./ws_client -m 0        # 终端2：发 MOVE，不回 ACK

# 测试指令正常（2秒后回 ACK）
./ws_client -m 2

# 测试心跳超时（15秒不发数据，服务端 10秒后断开）
./ws_client -k 15
```
