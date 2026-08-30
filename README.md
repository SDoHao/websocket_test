# WebSocket C++ 框架 (重构版)

## 架构说明
本项目将原本耦合的 WebSocket 代码抽象为三层架构：
1. **ws_utils**: 底层密码学工具 (SHA1, Base64)
2. **net**: TCP 网络层 (RAII 封装 Socket，解决粘包)
3. **ws**: WebSocket 协议层与服务器引擎 (事件回调模型)

## 运行
```bash
./ws_server 8080
```
