#!/bin/bash
# 静态链接 libstdc++/libgcc：解决手动安装的高版本 g++ 编译产物
# 在旧系统（如 CentOS 7 自带 gcc 4.8.5）运行时找不到新版 C++ 库的问题。
echo "🔨 正在编译 WebSocket 框架..."
# 检查 libstdc++.a 是否存在（判断 -static-libstdc++ 是否能找到静态库）
if ld -lstdc++ --verbose 2>/dev/null | grep -q "succeeded"; then
    echo "🔨 使用原始命令编译（静态链接 libstdc++）..."
    g++ -std=c++17 -pthread -O2 -static-libstdc++ -static-libgcc -Iinclude src/*.cpp ws_server.cpp -o ws_server
    g++ -std=c++17 -pthread -O2 -static-libstdc++ -static-libgcc -Iinclude src/*.cpp ws_client.cpp -o ws_client
else
    echo "⚠️ 静态库不可用，使用备用命令（显式链接 .so.6）..."
    g++ -std=c++17 -pthread -O2 -static-libgcc -Iinclude src/*.cpp ws_server.cpp -o ws_server /usr/lib64/libstdc++.so.6
    g++ -std=c++17 -pthread -O2 -static-libgcc -Iinclude src/*.cpp ws_client.cpp -o ws_client /usr/lib64/libstdc++.so.6
fi
# windows
# g++ -std=c++17 -O2 -Iinclude src/*.cpp ws_server.cpp -o ws_server.exe -lws2_32 -pthread 
# g++ -std=c++17 -O2 -Iinclude src/*.cpp ws_client.cpp -o ws_client.exe -lws2_32 -pthread

if [ $? -eq 0 ]; then
    echo "✅ 编译成功！可执行文件: ./ws_server ./ws_client"
    echo "🚀 运行服务端: ./ws_server 8080"
    echo "🚀 运行客户端: ./ws_client 127.0.0.1 8080 \"hello websocket\""
else
    echo "❌ 编译失败，请检查错误信息。"
fi
