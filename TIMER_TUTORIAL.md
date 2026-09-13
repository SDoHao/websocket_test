# 定时器保姆级教程（timerfd 深度版）

> 假设你完全不懂定时器，从头讲起。每一段代码都有注释。
>
> **v2 更新说明**：旧版教程只讲了"为什么用 timerfd"，第六章示例却是纯用户态忙轮询（while + check），
> 一个 timerfd 系统调用都没用，等于白讲。本版新增 **第四章「timerfd 标准流程」**（对齐 TCP 六步流程，
> 逐个系统调用详解），并把示例改成**真正使用 timerfd** 的版本（阻塞版 + epoll 版，均对应项目 ws.cpp 的真实写法）。

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

### 2.1 不用 timerfd 会怎样？（为什么它是"最优解"）

| 方案 | 原理 | 致命问题 |
|---|---|---|
| `sleep(3)` 后执行 | 线程睡 3 秒 | 睡死了：这 3 秒里啥也干不了，收不了网络数据 |
| `select/poll/epoll_wait` 带超时参数 | 事件循环每次等最多 N 毫秒，回来查时间 | 时间粒度粗糙；每次都要重新算超时；epoll 超时精度是毫秒级，想定时 500µs 做不到 |
| 信号 `SIGALRM` / `setitimer` | 内核到点发信号打断进程 | 回调跑在信号上下文，不能碰锁、不能调非 async-signal-safe 函数，处处受限 |
| 用户态忙轮询（旧版教程写法） | `while(true){ check(); }` | **空转烧 CPU**：100% 占满一个核 |
| **timerfd + epoll（本教程）** | 内核到点把 fd 置可读，epoll 唤醒 | 不占 CPU、精度纳秒级、和网络事件同一套循环，**唯一正解** |

**一句话总结**：timerfd 把"时间"也变成了一个**文件描述符**，于是"等时间到"这件事就和"等数据来"完全统一了——
都是等 fd 可读。这就是 Linux 一切皆文件的哲学。

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

## 第四章：timerfd 标准流程 —— 六个系统调用逐一讲解

> 本章是 v2 的核心新增。TCP 有 `socket() → bind() → listen() → accept() → recv()/send() → close()`，
> timerfd **同样有一条对称的生命周期**。我们把每个系统调用逐个讲透，并标注它对应 TCP 的哪一步。

### 4.1 总览：timerfd 的一生

```
timerfd_create() → timerfd_settime() → read() → close()
创建定时器fd   → 设定到期时间    → 等待到期并读取 → 关闭定时器
```

配合 epoll 的完整版（项目 ws.cpp 的真实用法）：

```
timerfd_create() → timerfd_settime() → epoll_ctl(ADD) → epoll_wait() → read() → close()
创建定时器fd   → 设定到期时间    → 挂入epoll    → 阻塞等事件  → 读取计数 → 关闭
```

与 TCP 六步流程的逐项对照：

| TCP 标准流程 | 一句话 | timerfd 标准流程 | 一句话 |
|---|---|---|---|
| `socket()` | 创建套接字 fd | `timerfd_create()` | 创建定时器 fd |
| `bind()` | 绑定端口 | `timerfd_settime()` | 设定到期时间 |
| `listen()` | 开启监听 | （无对应） | timerfd 创建即"一直在监听时间"，不需要单独开启 |
| `accept()` | 等待连接到来 | `read()` | 阻塞等待"时间到来" |
| `recv()/send()` | 收发数据 | `read()` | 读取"到期了几次"（8 字节计数） |
| `close()` | 关闭连接 | `close()` | 关闭定时器 fd |

**两个关键差异**（搞懂这两个，timerfd 就通了）：

1. **没有 `listen()`**：TCP 的 socket 创建后，内核不知道你拿它干嘛，必须 `bind`（绑地址）+ `listen`（声明"我要等连接"）两步配置。而 timerfd 在 `timerfd_create()` 时就锁死了用途（定时），它天生就处于"等待到期"状态——**创建即监听**。所以 `bind` 的对应物是 `timerfd_settime`（把 fd 和一个到期时刻绑定），`listen` 则被吞进了创建动作里。

2. **`read()` 一个顶俩**：TCP 里"等连接"（accept）和"收数据"（recv）是两个不同的调用；timerfd 里"等时间到"和"读数"是同一个 `read()`——因为对 timerfd 来说，**"到期事件"本身就是它唯一的"数据"**。阻塞模式下 `read()` 会一直睡到到期才返回，就像 `accept()` 一直睡到有连接才返回。

---

### 4.2 第一步：timerfd_create() —— 创建定时器 fd（对应 socket()）

```c
#include <sys/timerfd.h>

int timerfd_create(int clockid, int flags);
```

**作用**：向内核要一个"定时器文件描述符"。返回的 fd 和 socket fd 一样，都是进程文件描述符表里的一个整数编号。

**参数**：

| 参数 | 取值 | 含义 |
|---|---|---|
| `clockid` | `CLOCK_MONOTONIC` | **单调时钟**：只增不减，不受 `date -s` 改系统时间影响。相对定时（"3 秒后"）必须用它，**推荐** |
| | `CLOCK_REALTIME` | 墙上时钟：用户改系统时间它也会变。适合"闹钟"（某时刻响），但要用 `TFD_TIMER_ABSTIME` 配合 |
| `flags` | `0` | 阻塞模式：`read()` 没到期就睡着等 |
| | `TFD_NONBLOCK` | 非阻塞：`read()` 没到期立即返回 -1 / `EAGAIN`（**配 epoll 必须加**） |
| | `TFD_CLOEXEC` | 执行 exec 时自动关闭该 fd，防止泄漏给子进程 |

**返回值**：`int` fd；失败返回 `-1` 并设置 `errno`。

**常见错误**：`EINVAL`（clockid / flags 非法）、`EMFILE`（进程 fd 数已达上限）、`ENFILE`（系统 fd 表满）。

对照 `socket(AF_INET, SOCK_STREAM, 0)`：socket 创建网络 fd，timerfd_create 创建定时 fd。**都是"向内核要一个编号"**。

项目 ws.cpp 里的真实调用（`TimerQueue::init()`）：

```cpp
timerFd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
if (timerFd_ < 0) {
    log("[TimerQueue] timerfd_create failed: " + std::string(std::strerror(errno)));
    return false;
}
```

---

### 4.3 第二步：timerfd_settime() —— 设定到期时间（对应 bind()）

```c
#include <sys/timerfd.h>

int timerfd_settime(int fd, int flags,
                    const struct itimerspec *new_value,
                    struct itimerspec *old_value);
```

**作用**：把定时器 fd 和一个"到期时刻"绑定。TCP 的 `bind()` 绑定"地址 + 端口"，timerfd 的 `timerfd_settime()` 绑定"到期时间"。**改定时 = 重新调用它**，不需要重建 fd。

**参数**：

| 参数 | 含义 |
|---|---|
| `fd` | `timerfd_create` 返回的 fd |
| `flags` | `0` = 相对时间（从现在起 X 秒后到期）；`TFD_TIMER_ABSTIME` = 绝对时间（到某个具体时刻到期，配 `CLOCK_REALTIME` 做闹钟时用，避免"现在→目标时刻"的差值被时间调整干扰） |
| `new_value` | 新的定时设置（**要传什么时间**） |
| `old_value` | 传出上一次的设置（不想知道就传 `nullptr`，类比 `recv` 的 `from` 参数） |

**返回值**：`0` 成功；`-1` 失败，`errno` = `EINVAL`（fd 不是 timerfd 或参数非法）、`EBADF`（fd 已关闭）、`EFAULT`（指针无效）。

**核心结构体 itimerspec（必须背下来）**：

```c
struct timespec {            // 一个"时间点/时长"
    time_t tv_sec;           // 秒
    long   tv_nsec;          // 纳秒（0 ~ 999,999,999）
};

struct itimerspec {          // 定时器的完整设置
    struct timespec it_interval;  // 周期：到期后每隔多久再触发一次（全 0 = 一次性）
    struct timespec it_value;     // 首次到期时间（全 0 = 取消定时，即"关闹钟"）
};
```

**规则只有三条**：

1. `it_value` 非 0 → 定时器启动，到 `it_value` 时刻首次触发；
2. `it_interval` 非 0 → 首次触发后，每隔 `it_interval` 再触发一次（节拍器）；`it_interval` 全 0 → 只触发一次（闹钟）；
3. `it_value` 全 0 → **取消定时器**（disarm，相当于 TCP 的 close 前先停火）。

举三个例子：

```cpp
// 例 1：3 秒后触发一次（一次性闹钟）
struct itimerspec its{};
its.it_value.tv_sec = 3;      // 3 秒后首次到期
// it_interval 保持全 0 → 只触发一次
timerfd_settime(tfd, 0, &its, nullptr);

// 例 2：1 秒后开始，之后每 2 秒触发一次（周期节拍器）
its.it_value.tv_sec = 1;      // 1 秒后首次
its.it_interval.tv_sec = 2;   // 之后每 2 秒一次
timerfd_settime(tfd, 0, &its, nullptr);

// 例 3：取消定时器（关闹钟）
struct itimerspec zero{};
timerfd_settime(tfd, 0, &zero, nullptr);
```

项目 ws.cpp 里的真实调用（`TimerQueue::resetTimerfd()`）：

```cpp
void TimerQueue::resetTimerfd() {
    struct itimerspec its{};
    if (!heap_.empty()) {
        uint64_t now = nowMs();
        uint64_t when = heap_.top().when;      // 堆顶 = 最早到期的任务
        if (when > now) {
            its.it_value.tv_sec   = (when - now) / 1000;               // 毫秒 → 秒
            its.it_value.tv_nsec  = ((when - now) % 1000) * 1000000;   // 余数毫秒 → 纳秒
        } else {
            its.it_value.tv_nsec = 1;    // 已过期：设 1 纳秒，让 epoll 立刻报可读
        }
    }
    // 堆空 → its 全 0 → 相当于"取消定时器"
    ::timerfd_settime(timerFd_, 0, &its, nullptr);
}
```

注意 `tv_nsec = 1` 这个细节：定时器已到期的任务是"立刻要执行"的，不能设 0（0 = 取消），
所以设最小正整数 1 纳秒，内核会在下一个时钟滴答立即把它置为可读。

---

### 4.4 第三步：read() —— 等待到期、读取计数（对应 accept() + recv()）

```c
#include <unistd.h>

uint64_t expirations;                              // 必须是无符号 64 位整数！
ssize_t n = read(timerfd, &expirations, sizeof(expirations));
```

**作用**：等"时间到来"，并读取"自上次读取以来到期了几次"。

**为什么 buffer 必须是 `uint64_t`**：timerfd 的"数据"是一个 64 位计数。每次到期，内核把这个计数 +1，
`read` 把它读出来。传错大小的 buffer 会返回 `EINVAL`。

**返回值 / 行为**：

| 模式 | 到期前 read 的行为 | 到期后 read 的行为 |
|---|---|---|
| 阻塞（create 时 flags=0） | 线程睡着等，**像 `accept()` 一样卡住** | 立即返回 8 字节，`expirations` = 到期累计次数 |
| 非阻塞（`TFD_NONBLOCK`） | 立即返回 -1，`errno = EAGAIN`（**和 `recv` 的 EAGAIN 一模一样**） | 立即返回 8 字节 |

**"到期次数"是什么意思？** 对一次性定时器，永远是 1；对周期定时器，如果你 5 秒后才读，
而它每 1 秒触发一次，`expirations` 会是 5（错过的都算上）。但**对一次性定时器，即使超时很久才读，也只会是 1**。

**为什么必须读它（超重要的坑）**：timerfd 到期后会**一直保持"可读"状态**，直到你 `read` 把它清掉。
不读的话，epoll 会立刻再次报告它可读 → 死循环。就像闹钟不按掉会一直响。

> 类比 TCP：`accept()` = 等连接到来（阻塞睡），`recv()` = 收数据。timerfd 的 `read()` 一个顶俩——
> 既等"时间到来"，又收"到期计数"。**"到期事件"就是它唯一的"数据"。**

项目 ws.cpp 里的真实调用（`TimerQueue::handleExpired()` 第一行）：

```cpp
void TimerQueue::handleExpired() {
    uint64_t val;
    ::read(timerFd_, &val, sizeof(val));   // 清掉"可读"状态（不读会一直触发）
    // ... 执行所有到期任务 ...
}
```

---

### 4.5 第四步：close() —— 关闭（对应 close()）

```c
#include <unistd.h>
close(timerfd);
```

**作用**：关闭定时器 fd，释放内核资源。和 socket 一样：**用完必须关，否则 fd 泄漏**。
fd 是有限资源（`ulimit -n`，默认通常 1024），泄漏多了就 `EMFILE` 再也建不了新 fd。

关闭后：
- 定时器自动停止（等于隐式"取消"）；
- 如果它还挂在某个 epoll 上，**自动从该 epoll 移除**（内核会清理）；
- 之后对这个 fd 的任何操作返回 `EBADF`。

> 类比 TCP：`close()` 关连接、释放端口。timerfd 的 `close()` 关定时器、释放 fd。一模一样。

项目 ws.cpp 里的真实调用（`TimerQueue` 析构）：

```cpp
TimerQueue::~TimerQueue() {
    if (timerFd_ >= 0) ::close(timerFd_);   // 生命周期结束，关闭 fd
}
```

---

### 4.6 和 epoll 配合：完整的"监听循环"（对应 accept() 循环）

阻塞版 timerfd 只能"一个定时器等到死"，真正项目里当然不会这么用。正确姿势是把 timerfd 挂进 epoll，
让它和客户端连接 fd **一起等**：

```cpp
// ① 创建 timerfd（非阻塞，配 epoll 必须）
int tfd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);

// ② 设定 3 秒后到期
struct itimerspec its{};
its.it_value.tv_sec = 3;
::timerfd_settime(tfd, 0, &its, nullptr);

// ③ 创建 epoll，把 timerfd 挂进去（对应 TCP 的 accept 循环：谁可读处理谁）
int epfd = ::epoll_create1(0);
epoll_event ev{};
ev.events = EPOLLIN;        // 关心"可读"：timerfd 到期 = 可读
ev.data.fd = tfd;
::epoll_ctl(epfd, EPOLL_CTL_ADD, tfd, &ev);

// ④ 事件循环：网络 fd 和 timerfd 一起等
epoll_event events[8];
while (true) {
    int n = ::epoll_wait(epfd, events, 8, -1);   // 阻塞等：有连接数据/定时器到期都会返回
    for (int i = 0; i < n; ++i) {
        if (events[i].data.fd == tfd) {          // 是定时器到期
            uint64_t val;
            ::read(tfd, &val, sizeof(val));      // 先清可读状态！
            std::cout << "定时器到期，expirations=" << val << "\n";
        }
        // else：处理客户端连接 fd 的数据 ...
    }
}
```

**这和 ws.cpp 的 `EventLoop::runInLoop()` 是同一套结构**——只是项目里把"处理 timerfd"封装进了
`timerQueue_.handleExpired()`：

```cpp
// ws.cpp EventLoop::runInLoop() 事件分发（节选）
int n = ::epoll_wait(epollFd_, events, MAX_EVENTS, -1);
for (int i = 0; i < n; ++i) {
    int fd = events[i].data.fd;
    if (fd == wakeupFd_)          { handleWakeup();     continue; }  // eventfd 门铃
    if (fd == timerQueue_.fd())   {                     // ← timerfd 可读
        timerQueue_.handleExpired();                    //   执行到期回调
        continue;
    }
    auto it = conns_.find(fd);
    if (it == conns_.end()) continue;
    // ... 客户端连接的读写 ...
}
```

而 timerfd 是**怎么进 epoll 的**？看 `EventLoop::start()`：

```cpp
// ws.cpp EventLoop::start()（节选）
if (timerQueue_.init()) {                       // ← timerfd_create
    epoll_event tev;
    tev.events = EPOLLIN;
    tev.data.fd = timerQueue_.fd();
    ::epoll_ctl(epollFd_, EPOLL_CTL_ADD, timerQueue_.fd(), &tev);   // ← epoll_ctl ADD
}
```

到这里，timerfd 的"一生"就完整了：**创建 → 设定 → 挂 epoll → 事件循环里 read → close**。
接下来看它怎么被包装成好用的 `addTimer / cancelTimer` 接口。

---

### 4.7 周期定时器：it_interval 实战

周期性任务（如每 5 秒刷一次心跳）两种写法：

**写法 A：内核周期定时（推荐，定时精确、不漂移）**

```cpp
struct itimerspec its{};
its.it_value.tv_sec = 5;      // 5 秒后第一次
its.it_interval.tv_sec = 5;   // 之后每 5 秒一次
timerfd_settime(tfd, 0, &its, nullptr);
```

到期后内核自动重新 arm，**误差不会累积**（内核按绝对时刻算下一次）。

**写法 B：自重启（ws.cpp 教程里的写法）**

每次到期回调里再 `addTimer` 一个新的一次性定时器。好处是"每两次之间的间隔"由你控制（比如每次动态计算），
坏处是有额外调度误差。项目心跳检测用的就是写法 B，因为它需要"每次检查后决定要不要续期"。

---

### 4.8 错误处理速查表

| 调用 | errno | 含义 | 处理 |
|---|---|---|---|
| `timerfd_create` | `EINVAL` | clockid/flags 非法 | 检查参数，别用 `CLOCK_PROCESS_CPUTIME_ID` 之类 |
| | `EMFILE` / `ENFILE` | fd 表满了 | 排查 fd 泄漏（有没有忘记 close） |
| `timerfd_settime` | `EINVAL` | new_value 非法（如 nsec ≥ 1e9） | 检查 itimerspec 字段范围 |
| | `EBADF` | fd 已关闭 | 生命周期 bug：使用已 close 的 fd |
| `read`（非阻塞） | `EAGAIN` | 还没到期 | **正常**，不是错误，继续等 epoll |
| `read` | `EINVAL` | buffer 不是 8 字节 | 必须用 `uint64_t` |

---

## 第五章：TimerQueue —— 把 1000 个定时器压进 1 个 timerfd

> 第四章讲透了 timerfd 本身。但 timerfd 一个 fd 只能设**一个**到期时间。
> 业务上要"3 秒后做 A、5 秒后做 B、10 秒后做 C"怎么办？
> 答案：**一个 timerfd + 一个最小堆**——堆里放所有任务，timerfd 永远对准堆顶（最早到期的），
> 到期弹出所有过期任务，再对准下一个。这就是 `ws.cpp` 里 `TimerQueue` 的全部秘密。

```
timerfd 永远指向这个 ↓
        ┌─────┐
        │ 3s  │ ← 堆顶：最早到期
        ├─────┤
        │ 5s  │
        ├─────┤
        │ 10s │
        └─────┘
  任务到期 → 弹出 → 执行回调 → 重新对准下一个
```

### 5.1 addTimer —— 添加定时器

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

    // ④ 重新校准 timerfd ← 这是它和 timerfd 系统调用唯一的接触点
    //    因为新加的任务可能比之前所有任务都早到期
    //    比如之前堆顶是 5 秒后，新任务是 2 秒后
    //    那 timerfd 要改成 2 秒后触发（内部就是 timerfd_settime）
    resetTimerfd();

    return id;
}
```

### 5.2 cancelTimer —— 取消定时器

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

**注意**：取消定时器**不会碰 timerfd**（不需要 `timerfd_settime`）——被取消的任务还躺在堆里，
只是执行时被跳过。所以即使取消了堆顶任务，timerfd 依然会按时触发一次（触发后发现自己没事干）。
这是"惰性删除"的代价：**取消不立即生效于 timerfd，但最多迟一个周期**，对业务无感。

### 5.3 handleExpired —— 处理到期的定时器

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

### 5.4 resetTimerfd —— 校准 timerfd（和系统调用最近的一层）

```cpp
void TimerQueue::resetTimerfd() {
    struct itimerspec its{};

    if (!heap_.empty()) {
        // 堆不空 → timerfd 设为堆顶的到期时间
        uint64_t now = nowMs();
        uint64_t when = heap_.top().when;

        if (when > now) {
            // 还没到期 → 算差值（相对时间，flags=0）
            // 比如 when=13000, now=10000 → 差 3000ms = 3秒
            its.it_value.tv_sec = (when - now) / 1000;              // 秒
            its.it_value.tv_nsec = ((when - now) % 1000) * 1000000; // 余数毫秒→纳秒
        } else {
            // 已过期 → 立即触发（设 1 纳秒；不能设 0，0=取消定时器）
            its.it_value.tv_nsec = 1;
        }
    }
    // 如果堆空了，its 全 0 → timerfd 取消定时（不再触发）

    // 真正设置 timerfd 的超时时间 ← 整个类里唯一的 timerfd_settime 调用
    ::timerfd_settime(timerFd_, 0, &its, nullptr);
}
```

**为什么把 `nowMs()` 用 `steady_clock`？** 它和 `CLOCK_MONOTONIC` 同源（单调时钟），
不会因为用户 `date -s` 改时间而跳变，所以"到期时间点"和"timerfd 的差值计算"永远不会错位。
如果混用墙上时钟（`system_clock`）和 `CLOCK_MONOTONIC`，改一次系统时间定时器就全乱了。

---

## 第六章：怎么在代码里用？

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
内部链路是：`addTimer` → 压堆 → `resetTimerfd` → `timerfd_settime` → epoll 等可读 →
`handleExpired` → `read` 清状态 → 执行回调。**你在业务层只看到两个函数，底层就是第四章那套流程。**

---

## 第七章：超简单示例 —— 这次真的用上了 timerfd

> 旧版教程的第六章给了一个"SimpleTimer"，但它其实是**纯用户态忙轮询**：
> `while (!timer.empty()) { timer.check(); }` 空转烧 CPU，一个 timerfd 系统调用都没用，
> 和教程标题"定时器"名不副实。本版重写为两个**真正调用 timerfd** 的示例，
> 分别对应第四章的"最小生命周期"和"epoll 配合"，代码就放在项目根目录：
> `timerfd_demo.cpp`（阻塞版）和 `timerfd_epoll_demo.cpp`（epoll 版）。

### 7.1 示例一：最小生命周期（阻塞版）—— 对应 4.1 的四个系统调用

> 编译：`g++ -std=c++17 timerfd_demo.cpp -o timerfd_demo`　运行：`./timerfd_demo`

```cpp
// timerfd_demo.cpp —— timerfd 完整生命周期（阻塞版，4 个系统调用）
#include <sys/timerfd.h>   // timerfd_create / timerfd_settime
#include <unistd.h>        // read / close
#include <iostream>
#include <cstring>
#include <cerrno>

int main() {
    // ① timerfd_create：创建定时器 fd（对应 socket）
    //    CLOCK_MONOTONIC = 单调时钟；flags=0 = 阻塞模式
    int tfd = ::timerfd_create(CLOCK_MONOTONIC, 0);
    if (tfd < 0) { perror("timerfd_create"); return 1; }
    std::cout << "[1] timerfd_create: fd=" << tfd << "\n";

    // ② timerfd_settime：设定 3 秒后到期一次（对应 bind）
    //    it_value = 首次到期时间；it_interval 全 0 = 一次性
    struct itimerspec its{};
    its.it_value.tv_sec  = 3;   // 3 秒后到期
    its.it_value.tv_nsec = 0;
    its.it_interval.tv_sec  = 0;
    its.it_interval.tv_nsec = 0;
    if (::timerfd_settime(tfd, 0, &its, nullptr) < 0) {
        perror("timerfd_settime"); close(tfd); return 1;
    }
    std::cout << "[2] timerfd_settime: 3 秒后触发一次\n";

    // ③ read：阻塞等待到期 + 读取计数（对应 accept + recv）
    //    没到期前，进程在这里睡着（和 accept 阻塞等连接一模一样）
    uint64_t expirations = 0;
    ssize_t n = ::read(tfd, &expirations, sizeof(expirations));
    if (n != (ssize_t)sizeof(expirations)) {
        perror("read"); close(tfd); return 1;
    }
    std::cout << "[3] read 返回：到期了！自上次读取以来触发次数 = "
              << expirations << "\n";

    // ④ close：关闭定时器 fd（对应 close）
    ::close(tfd);
    std::cout << "[4] close: timerfd 已关闭\n";
    return 0;
}
```

运行输出：

```
[1] timerfd_create: fd=3
[2] timerfd_settime: 3 秒后触发一次
（这里卡了 3 秒——read 在阻塞等待）
[3] read 返回：到期了！自上次读取以来触发次数 = 1
[4] close: timerfd 已关闭
```

**这就是 timerfd 的全部**：四个系统调用，一个生命周期。会了这个，第四章就过关了。

### 7.2 示例二：timerfd + epoll + 最小堆 —— 复现旧版多定时器场景

> 编译：`g++ -std=c++17 timerfd_epoll_demo.cpp -o timerfd_epoll_demo`　运行：`./timerfd_epoll_demo`
>
> 复现旧版教程的 4 个定时器场景（1 秒开始 / 2 秒取消 / 3 秒 / 5 秒），
> 但这次底层是**真的 timerfd + epoll**，并且用的是 `ws.cpp` `TimerQueue` 的同款架构
> （一个 timerfd + 最小堆 + 惰性删除）。

```cpp
// timerfd_epoll_demo.cpp —— timerfd + epoll + 最小堆（对齐 ws.cpp 的 TimerQueue）
#include <sys/timerfd.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <iostream>
#include <queue>
#include <vector>
#include <functional>
#include <chrono>
#include <unordered_set>
#include <cstdint>

// ====== 简化版 TimerQueue：一个 timerfd + 最小堆（和 ws.cpp 同款思路）======
class MiniTimerQueue {
public:
    using Callback = std::function<void()>;

    // 对应第四章 4.2：timerfd_create（配 epoll 必须 TFD_NONBLOCK）
    bool init() {
        tfd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
        return tfd_ >= 0;
    }
    int fd() const { return tfd_; }

    // 添加定时器：delayMs 毫秒后执行 cb，返回唯一 ID
    uint64_t addTimer(uint64_t delayMs, Callback cb) {
        uint64_t id = nextId_++;
        uint64_t when = nowMs() + delayMs;
        heap_.push(Entry{when, id, std::move(cb)});
        resetTimerfd();   // 新任务可能更早到期 → 重新对准 timerfd（timerfd_settime）
        return id;
    }

    // 取消定时器：惰性删除（标记 ID，到期时跳过）
    void cancelTimer(uint64_t id) { cancelled_.insert(id); }

    // timerfd 可读时调用：弹出所有到期任务执行，再重新对准
    void handleExpired() {
        uint64_t val;
        ::read(tfd_, &val, sizeof(val));   // 清可读状态（不读会一直触发）

        uint64_t now = nowMs();
        while (!heap_.empty() && heap_.top().when <= now) {
            Entry e = heap_.top();
            heap_.pop();
            if (cancelled_.erase(e.id)) continue;   // 已取消 → 跳过执行
            e.cb();                                 // 没取消 → 执行回调
        }
        resetTimerfd();
    }

    bool empty() const { return heap_.empty(); }

private:
    struct Entry {
        uint64_t when;      // 到期时间点（毫秒）
        uint64_t id;        // 定时器 ID
        Callback cb;
        bool operator>(const Entry& o) const { return when > o.when; }
    };

    int tfd_ = -1;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> heap_;
    std::unordered_set<uint64_t> cancelled_;
    uint64_t nextId_ = 1;

    static uint64_t nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // 对应第四章 4.3：把 timerfd 对准堆顶（最早到期的任务）
    void resetTimerfd() {
        struct itimerspec its{};
        if (!heap_.empty()) {
            uint64_t now = nowMs();
            uint64_t when = heap_.top().when;
            if (when > now) {
                its.it_value.tv_sec  = (when - now) / 1000;
                its.it_value.tv_nsec = ((when - now) % 1000) * 1000000;
            } else {
                its.it_value.tv_nsec = 1;   // 已过期：设 1ns 立即触发
            }
        }
        ::timerfd_settime(tfd_, 0, &its, nullptr);   // 堆空 = 全 0 = 取消定时
    }
};

int main() {
    MiniTimerQueue timer;
    if (!timer.init()) { perror("timerfd_create"); return 1; }

    // 创建 epoll，把 timerfd 挂进去（对应第四章 4.6）
    int epfd = ::epoll_create1(0);
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = timer.fd();
    ::epoll_ctl(epfd, EPOLL_CTL_ADD, timer.fd(), &ev);

    // ---- 复现旧版场景 ----
    // 场景 1：3 秒后打印
    uint64_t t1 = timer.addTimer(3000, []() {
        std::cout << "[定时器1] 3秒到了！\n";
    });
    // 场景 2：5 秒后打印
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

    // ---- 主循环：epoll_wait 阻塞等待（不是忙轮询！不占 CPU）----
    std::cout << "主循环启动，epoll_wait 阻塞等待 timerfd 到期...\n";
    while (!timer.empty()) {
        epoll_event events[4];
        int n = ::epoll_wait(epfd, events, 4, -1);   // 阻塞等，有到期才返回
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }
        for (int i = 0; i < n; ++i) {
            if (events[i].data.fd == timer.fd())
                timer.handleExpired();   // timerfd 可读 → 执行所有到期回调
        }
    }

    std::cout << "所有定时器执行完毕\n";
    ::close(timer.fd());
    ::close(epfd);
    return 0;
}
```

运行结果（和旧版一致，但这次是真的 timerfd 驱动）：

```
主循环启动，epoll_wait 阻塞等待 timerfd 到期...
[定时器4] 1秒到了，开始！
[定时器3] 取消定时器1
[定时器2] 5秒到了！
所有定时器执行完毕
```

注意：定时器1 被取消了，所以"3秒到了"不会打印。

### 7.3 为什么旧版 SimpleTimer 是"假的 timerfd"？（新旧对比）

| | 旧版 SimpleTimer（已废弃） | 新版 MiniTimerQueue（示例二） |
|---|---|---|
| 触发方式 | `while(!empty()){ check(); }` 忙轮询 | `epoll_wait` 阻塞等待 |
| CPU 占用 | **100% 空转烧一个核** | 0%（睡着，内核唤醒） |
| timerfd 系统调用 | **一个都没有** | `timerfd_create` + `timerfd_settime` + `read` + `close` 全套 |
| 时间精度 | 取决于循环频率 | 内核级纳秒精度 |
| 与网络事件统一 | 不能（单独一个循环） | 可以（和连接 fd 同一个 epoll） |
| 对应项目代码 | 无对应 | 就是 `TimerQueue` 的教学版 |

**结论**：旧版示例只是演示了"定时器"的业务概念（add/cancel/到期执行），但完全没教 timerfd。
真正的 timerfd 一定是"fd 可读事件"，必须和 epoll 配合才能体现价值——这就是示例二做的事。

---

## 第八章：项目里的实战用法

### 8.1 心跳超时（自动断开僵死连接）

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

**它和 timerfd 底层的关系**：每次 `addTimer` 都会把新任务压进 TimerQueue 的最小堆并调用
`resetTimerfd()`（=`timerfd_settime`）重新对准堆顶。5 秒检查 → 到期 → `handleExpired()` 弹堆执行回调
→ 回调里又 `addTimer` → 重新对准。整个周期循环就是第四章流程的重复。

### 8.2 指令超时（等 ack，超时重发）

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

**取消为什么"看不见"？** 回顾 5.2 的惰性删除：`cancelTimer(123)` 只是把 123 插进 `cancelled_` 集合，
被取消的任务还躺在最小堆里。3 秒后 timerfd 照样到期（它还对准着堆顶），`handleExpired` 弹出它时发现
ID 在集合里 → 跳过执行。**对业务来说效果就是"没触发"，但底层 timerfd 确实"响"了一下。**

---

## 第九章：lambda 回调里的捕获陷阱

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

## 第十章：API 速查表

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

### 10.1 timerfd 底层系统调用速查（完整讲解见第四章）

| 调用 | 作用 | 对应 TCP |
|---|---|---|
| `timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK)` | 创建定时器 fd | `socket()` |
| `timerfd_settime(fd, 0, &its, nullptr)` | 设定/修改到期时间（`it_value` 全 0 = 取消） | `bind()` |
| `read(fd, &val, 8)` | 等待到期并读取到期计数（必须用 `uint64_t`） | `accept()` + `recv()` |
| `close(fd)` | 关闭定时器 fd | `close()` |

关键常量：
- 时钟：`CLOCK_MONOTONIC`（单调，推荐）/ `CLOCK_REALTIME`（墙上，受改时间影响）
- flags：`TFD_NONBLOCK`（配 epoll 必加）/ `TFD_CLOEXEC`（exec 自动关）
- `timerfd_settime` 的 `TFD_TIMER_ABSTIME`：绝对时间模式（做闹钟用）
- `itimerspec.it_value`：首次到期；`it_interval`：周期（全 0 = 一次性）

---

## 第十一章：调试复盘 —— 三个 Bug 的定位与修复

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

# 编译并运行 timerfd 教学示例（第七章，Linux 下执行）
g++ -std=c++17 timerfd_demo.cpp -o timerfd_demo && ./timerfd_demo
g++ -std=c++17 timerfd_epoll_demo.cpp -o timerfd_epoll_demo && ./timerfd_epoll_demo
