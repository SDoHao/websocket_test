#!/bin/bash
echo "🔨 正在编译 WebSocket 框架..."
g++ -std=c++17 -pthread -O2 -Iinclude src/*.cpp ws_server.cpp -o ws_server

# windows
# g++ -std=c++17 -O2 -Iinclude src/*.cpp ws_server.cpp -o ws_server.exe -lws2_32 -pthread 

if [ $? -eq 0 ]; then
    echo "✅ 编译成功！可执行文件: ./ws_server"
    echo "🚀 运行命令: ./ws_server 8080"
else
    echo "❌ 编译失败，请检查错误信息。"
fi
