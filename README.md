# WebSocket C++ 框架（One-Loop-Per-Thread 多线程版）

从零手写的 C++17 WebSocket 服务端框架 + 命令行客户端，无第三方依赖，用于学习网络编程。

## 特性

- **服务端：epoll + reactor，One-Loop-Per-Thread 多线程事件循环**
  - 主线程（Acceptor）：只负责 accept，round-robin 负载均衡分发新连接
  - N 个工作线程（EventLoop）：每个线程独立 epoll 事件循环，各自处理名下连接
  - 跨线程交接：待办队列（mutex）+ eventfd 唤醒，全项目仅此一处加锁
  - 非阻塞 IO + 读缓冲（解决半包）+ 写缓冲（解决发送阻塞）
  - 完整 RFC 6455：握手校验、掩码、分片重组、Ping/Pong、优雅关闭
- **客户端：阻塞模式命令行工具**
  - `-f <文件>` 二进制图传（jpg 原样透传，回显字节级对比）
  - `-t <文本>` 文本消息
  - `-p` 心跳测试（Ping/Pong）
- **浏览器测试页** `index.html`：选 jpg 发送二进制帧并渲染回显图片

## 架构

```
主线程（Acceptor）                   工作线程（EventLoop × N）
┌────────────────────┐      ┌──────────────────────────────────┐
│ 监听socket ──► epoll_wait  │  loop#0: epoll_wait + eventfd    │
│  accept 新连接        │      │    ├ 连接A（状态机：握手→读帧→关闭）│
│  round-robin 分配     │─────►│    └ 连接B                      │
│  addConnection(fd)   │ 跨线  │  loop#1: epoll_wait + eventfd   │
└────────────────────┘ 程交接 │    ├ 连接C                      │
                            │    └ 连接D                      │
                            └──────────────────────────────────┘
```

- 每个连接**固定属于一个 EventLoop**，该连接的一切处理只发生在那个线程 → Connection 内部零锁
- 不同连接的回调可能并行执行 → 回调里访问跨连接共享数据需自行加锁

## 构建（仅 Linux）

```bash
cmake -S . -B build && cmake --build build
# 或 ./build.sh
```

> 服务端依赖 Linux 专有的 `epoll` 和 `eventfd`，无法在 Windows 原生编译运行。

## 运行

```bash
# 服务端（第二参数可选：工作线程数，不传 = CPU 核数）
./build/ws_server 8080
./build/ws_server 8080 2

# 客户端测试（默认连接 127.0.0.1:8080）
./build/ws_client -f 0.jpg      # 图传：发 jpg → 收回显 → 字节对比
./build/ws_client -t hello      # 文本回显
./build/ws_client -p            # 心跳测试
./build/ws_client 127.0.0.1 9000 -f 1.jpg   # 指定 host/port
```

浏览器测试：起服务端后打开 `index.html`，连接 `ws://127.0.0.1:8080`。

## 目录结构

```
├── include/
│   ├── net.h          # TCP 网络层（RAII + 非阻塞设置）
│   ├── ws.h           # 协议层：Connection 状态机 / EventLoop / Server
│   ├── ws_client.h    # 客户端（阻塞模式）
│   └── ws_utils.h     # 工具层：SHA1 / Base64
├── src/
│   ├── net.cpp
│   ├── ws.cpp         # 核心：帧解析 + 事件循环 + 多线程分发
│   ├── ws_client.cpp
│   └── ws_utils.cpp
├── ws_server.cpp      # 服务端入口（echo 服务器）
├── ws_client.cpp      # 客户端入口（参数循环）
└── index.html         # 浏览器测试页
```

## 说明

- 学习项目：服务端仅 Linux（epoll 模型），后期可迁移跨平台 ASIO
- 已知限制见 `ARCHITECTURE.md` 第 8 节（无心跳超时、无 TLS、无 payload 上限等）
- 详细架构、函数清单与修改指南见 `ARCHITECTURE.md`（本地保留，不入 git）
