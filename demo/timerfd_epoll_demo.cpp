// timerfd_epoll_demo.cpp — timerfd + epoll + 最小堆（对齐 ws.cpp 的 TimerQueue）
// 编译：g++ -std=c++17 timerfd_epoll_demo.cpp -o timerfd_epoll_demo
// 运行：./timerfd_epoll_demo
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
#include <cerrno>
#include <cstdio>

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
        resetTimerfd();
        return id;
    }

    // 取消定时器：惰性删除（标记 ID，到期时跳过）
    void cancelTimer(uint64_t id) { cancelled_.insert(id); }

    // timerfd 可读时调用：弹出所有到期任务执行，再重新对准
    void handleExpired() {
        uint64_t val;
        ::read(tfd_, &val, sizeof(val));

        uint64_t now = nowMs();
        while (!heap_.empty() && heap_.top().when <= now) {
            Entry e = heap_.top();
            heap_.pop();
            if (cancelled_.erase(e.id)) continue;
            e.cb();
        }
        resetTimerfd();
    }

    bool empty() const { return heap_.empty(); }

private:
    struct Entry {
        uint64_t when;
        uint64_t id;
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
                its.it_value.tv_nsec = 1;
            }
        }
        ::timerfd_settime(tfd_, 0, &its, nullptr);
    }
};

int main() {
    MiniTimerQueue timer;
    if (!timer.init()) { perror("timerfd_create"); return 1; }

    int epfd = ::epoll_create1(0);
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = timer.fd();
    ::epoll_ctl(epfd, EPOLL_CTL_ADD, timer.fd(), &ev);

    uint64_t t1 = timer.addTimer(3000, []() {
        std::cout << "[定时器1] 3秒到了！\n";
    });
    timer.addTimer(5000, []() {
        std::cout << "[定时器2] 5秒到了！\n";
    });
    timer.addTimer(2000, [&timer, t1]() {
        std::cout << "[定时器3] 取消定时器1\n";
        timer.cancelTimer(t1);
    });
    timer.addTimer(1000, []() {
        std::cout << "[定时器4] 1秒到了，开始！\n";
    });


    std::cout << "主循环启动，epoll_wait 阻塞等待 timerfd 到期...\n";
    while (!timer.empty()) {
        epoll_event events[4];
        int n = ::epoll_wait(epfd, events, 4, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }
        for (int i = 0; i < n; ++i) {
            if (events[i].data.fd == timer.fd())
                timer.handleExpired();
        }
    }

    std::cout << "所有定时器执行完毕\n";
    ::close(timer.fd());
    ::close(epfd);
    return 0;
}

