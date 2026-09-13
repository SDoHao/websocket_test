// timerfd_demo.cpp — timerfd 完整生命周期（阻塞版，4 个系统调用）
// 编译：g++ -std=c++17 timerfd_demo.cpp -o timerfd_demo
// 运行：./timerfd_demo
#include <sys/timerfd.h>
#include <unistd.h>
#include <iostream>
#include <cstdio>
#include <cerrno>

int main() {
    // ① timerfd_create：创建定时器 fd（对应 socket）
    int tfd = ::timerfd_create(CLOCK_MONOTONIC, 0);
    if (tfd < 0) { perror("timerfd_create"); return 1; }
    std::cout << "[1] timerfd_create: fd=" << tfd << "\n";

    // ② timerfd_settime：设定 3 秒后到期一次（对应 bind）
    struct itimerspec its{};
    its.it_value.tv_sec  = 3;
    its.it_value.tv_nsec = 0;
    its.it_interval.tv_sec  = 0;
    its.it_interval.tv_nsec = 0;
    if (::timerfd_settime(tfd, 0, &its, nullptr) < 0) {
        perror("timerfd_settime"); close(tfd); return 1;
    }
    std::cout << "[2] timerfd_settime: 3 秒后触发一次\n";


    // ③ read：阻塞等待到期 + 读取计数（对应 accept + recv）
    //    没到期前进程在这里睡着，和 accept 阻塞等连接一模一样
    uint64_t expirations = 0;
    ssize_t n = ::read(tfd, &expirations, sizeof(expirations));
    if (n != (ssize_t)sizeof(expirations)) {
        perror("read"); close(tfd); return 1;
    }
    std::cout << "[3] read 返回：到期了！触发次数 = " << expirations << "\n";

    // ④ close：关闭定时器 fd（对应 close）
    ::close(tfd);
    std::cout << "[4] close: timerfd 已关闭\n";
    return 0;
}

