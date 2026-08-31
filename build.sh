#!/bin/bash
echo "🔨 正在编译 WebSocket 框架..."
g++ -std=c++17 -pthread -O2 -Iinclude src/*.cpp ws_server.cpp -o ws_server
g++ -std=c++17 -pthread -O2 -Iinclude src/*.cpp ws_client.cpp -o ws_client

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
