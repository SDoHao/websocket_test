cmake_minimum_required(VERSION 3.15)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

file(GLOB_RECURSE APP_SOURCE CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/*.cpp"
)

add_executable(ws_server ${APP_SOURCE} ws_server.cpp)
add_executable(ws_client ${APP_SOURCE} ws_client.cpp)

target_include_directories(ws_server PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
)
target_include_directories(ws_client PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
)

target_link_libraries(ws_server PRIVATE
    pthread
)
target_link_libraries(ws_client PRIVATE
    pthread
)

# 静态链接 C++ 运行库：解决高版本 g++ 编译产物在旧系统（CentOS 7 自带 gcc 4.8.5）
# target_link_options(ws_server PRIVATE -static-libstdc++ -static-libgcc)
# target_link_options(ws_client PRIVATE -static-libstdc++ -static-libgcc)

if(WIN32)
    target_link_libraries(ws_server PRIVATE
        ws2_32
    )
    target_link_libraries(ws_client PRIVATE
        ws2_32
    )
endif()