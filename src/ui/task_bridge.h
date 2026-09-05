// task_bridge.h — UI 线程 ↔ 任务线程的协程桥（HuxerUI TaskScope 结构化并发）。
//
// 线程契约：
// - State 只在 UI 线程读写；任务线程与引擎线程绝不得触碰。
// - UseTaskScope().Launch 启动的协程体运行在 UI 线程；co_await huxerui::Delay
//   恢复时仍回到 UI 线程——这是异步结果回写 State 的唯一通道。
// - 阻塞 / CPU 重活经 RunOnTaskThread 派给进程级任务线程池：UI 协程挂起不卡帧，
//   完成（或异常）后协程在 UI 线程恢复，直接拿返回值 / try-catch。
// - 内核 REST 调用（clashflux.api）与内核启停（store 层）都是同步阻塞式，全部
//   经 RunOnTaskThread 派到任务线程；mihomo 推送流由 IXWebSocket 自管线程
//  （事件经 PollWhile 泵取回）；内核子进程自带监视线程（输出行经 PollWhile
//   drain）。
//
// 取消语义：组合卸载时 TaskScope 取消协程（协程在 Delay 悬挂点销毁）；任务线程
// 上未完成的工作在共享结果槽上收尾，结果随槽销毁丢弃，不会回写已卸载页面。
#pragma once

#include <huxerui/huxerui.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace clashflux::ui {

namespace detail {

// 进程级任务线程池：懒启动，进程退出时随静态析构 stop + join。
class TaskPool {
public:
    static TaskPool& Instance() {
        static TaskPool pool;
        return pool;
    }

    void Post(std::function<void()> job) {
        {
            std::lock_guard lock{mu_};
            jobs_.push(std::move(job));
        }
        cv_.notify_one();
    }

private:
    TaskPool() {
        const unsigned hw = std::thread::hardware_concurrency();
        const std::size_t n = std::clamp<std::size_t>(hw == 0 ? 4 : hw, 2, 8);
        workers_.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            workers_.emplace_back([this] { WorkerLoop(); });
        }
    }

    ~TaskPool() {
        {
            std::lock_guard lock{mu_};
            stop_ = true;
        }
        cv_.notify_all();
        for (std::thread& t : workers_) t.join();
    }

    TaskPool(const TaskPool&) = delete;
    TaskPool& operator=(const TaskPool&) = delete;

    void WorkerLoop() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock lock{mu_};
                cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
                if (stop_ && jobs_.empty()) return;
                job = std::move(jobs_.front());
                jobs_.pop();
            }
            job();
        }
    }

    std::mutex mu_;
    std::condition_variable cv_;
    std::queue<std::function<void()>> jobs_;
    std::vector<std::thread> workers_;
    bool stop_ = false;
};

// RunOnTaskThread 的共享结果槽：任务线程写一次，UI 协程按节拍读取。
template <class R>
struct ResultSlot {
    std::mutex mu;
    bool done = false;
    std::optional<R> value;
    std::exception_ptr error;
};

template <>
struct ResultSlot<void> {
    std::mutex mu;
    bool done = false;
    std::exception_ptr error;
};

// 类型擦除后的实现：协程帧只持有 std::function（有外部链接），避免头文件模板
// 的协程帧嵌入匿名命名空间 lambda 类型触发 -Wsubobject-linkage。
template <class R>
huxerui::Task<R> RunOnTaskThreadImpl(std::function<R()> fn) {
    auto slot = std::make_shared<ResultSlot<R>>();
    TaskPool::Instance().Post([slot, fn = std::move(fn)]() mutable {
        // fn 本体不持锁运行（否则 UI 线程读槽时会被整个任务卡住）；
        // 只有写结果 / 置 done 这两个短临界区持锁，保证 happens-before。
        try {
            if constexpr (std::is_void_v<R>) {
                fn();
            } else {
                R value = fn();
                std::lock_guard lock{slot->mu};
                slot->value.emplace(std::move(value));
            }
        } catch (...) {
            std::lock_guard lock{slot->mu};
            slot->error = std::current_exception();
        }
        {
            std::lock_guard lock{slot->mu};
            slot->done = true;
        }
    });
    for (;;) {
        co_await huxerui::Delay(std::chrono::duration<double>{0.01});
        std::lock_guard lock{slot->mu};
        if (slot->done) break;
    }
    if (slot->error) std::rethrow_exception(slot->error);
    if constexpr (!std::is_void_v<R>) co_return std::move(*slot->value);
}

} // namespace detail

// 在任务线程池上执行 fn，UI 协程挂起等待；fn 完成（或抛异常）后协程在 UI 线程
// 恢复——返回值经 co_await 拿到，异常在 UI 线程 rethrow（可 try-catch 包住整个
// co_await）。等待用 Delay 轮询结果槽（10ms 一拍），恢复点恒为 UI 线程。
template <class F>
huxerui::Task<std::invoke_result_t<F>> RunOnTaskThread(F fn) {
    using R = std::invoke_result_t<F>;
    return detail::RunOnTaskThreadImpl<R>(std::function<R()>(std::move(fn)));
}

// 引擎轮询桥：每 interval 在 UI 线程执行一次 tick()；tick 返回 true 继续等，
// false 结束。tick 内做 drain/poll + 写 State（全程 UI 线程，安全）。
// 组合卸载时 TaskScope 取消协程，tick 不会再被执行。
inline huxerui::Task<void> PollWhile(std::chrono::duration<double> interval,
                                     std::function<bool()> tick) {
    while (tick()) co_await huxerui::Delay(interval);
}

} // namespace clashflux::ui
