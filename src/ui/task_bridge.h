// task_bridge.h — UI 线程 ↔ 任务线程的协程桥（HuxerUI TaskScope 结构化并发）。
//
// 线程契约：
// - State 只在 UI 线程读写；任务线程与引擎线程绝不得触碰。
// - UseTaskScope().Launch 启动的协程体运行在 UI 线程；co_await huxerui::Delay
//   恢复时仍回到 UI 线程——这是异步结果回写 State 的唯一通道。
// - 阻塞 / CPU 重活经 RunOnTaskThread 派到独立领域线程池（DomainWorkerPool），
//   经 HuxerUI AwaitCallback 在 UI 线程恢复并拿返回值 / try-catch：
//   订阅刷新等路径会在任务线程持有 lifecycleMutex_/downloadMutex_ 并经
//   commitProfileOnWorker() 等待 SQLite TransactionAsync；若与 HuxerUI 自身的
//   1–4 线程 WorkerExecutor 共用线程池，高并发轮询占满 WorkerExecutor 会饿死
//   SQLite 事务导致永久转圈死锁。隔离后 WorkerExecutor 专供框架 I/O 与数据库。
// - 内核 REST 调用（clashflux.api）与内核启停（store 层）都是同步阻塞式，全部
//   经 RunOnTaskThread 派到任务线程；内核推送流由 IXWebSocket 自管线程
//  （事件经 PollWhile 泵取回）；内核子进程自带监视线程（输出行经 PollWhile
//   drain）。
//
// 取消语义：组合卸载时 TaskScope 取消协程（协程在挂起点销毁）；尚未开始的
// 领域任务会被标记取消并跳过执行，已在执行的调用完成后由 AwaitCallback 丢弃结果。
#pragma once

#include <huxerui/huxerui.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace clashflux::ui {

namespace detail {

struct PendingDomainWork {
    std::atomic<bool> cancelled{false};
    std::function<void()> run;
    huxerui::CallbackCompletion<void> completion;
    std::shared_ptr<std::stop_source> stopSource;

    PendingDomainWork(std::function<void()> task,
                      huxerui::CallbackCompletion<void> done,
                      std::shared_ptr<std::stop_source> stop = {})
        : run(std::move(task)), completion(std::move(done)), stopSource(std::move(stop)) {}
};

class DomainWorkerPool final {
public:
    DomainWorkerPool()
        : workerCount_(std::clamp<std::size_t>(
              std::max<std::size_t>(std::thread::hardware_concurrency(), 4U), 4U, 8U)) {
        workers_.reserve(workerCount_);
        for (std::size_t index = 0; index < workerCount_; ++index) {
            workers_.emplace_back([this] { Run(); });
        }
    }

    ~DomainWorkerPool() {
        std::deque<std::shared_ptr<PendingDomainWork>> remaining;
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            remaining.swap(queue_);
        }
        condition_.notify_all();
        for (const auto& pending : remaining) {
            if (!pending || pending->cancelled.load(std::memory_order_acquire)) continue;
            pending->run = {};
            pending->completion.Fail(
                std::make_exception_ptr(std::runtime_error("Clash-Flux worker pool is closed")));
        }
        for (std::thread& worker : workers_) {
            if (worker.joinable()) worker.join();
        }
    }

    DomainWorkerPool(const DomainWorkerPool&) = delete;
    DomainWorkerPool& operator=(const DomainWorkerPool&) = delete;

    [[nodiscard]] std::size_t WorkerCount() const noexcept { return workerCount_; }

    bool Submit(std::shared_ptr<PendingDomainWork> pending) {
        if (!pending) return false;
        {
            std::lock_guard lock(mutex_);
            if (stopping_) return false;
            queue_.push_back(std::move(pending));
        }
        condition_.notify_one();
        return true;
    }

private:
    void Run() {
        for (;;) {
            std::shared_ptr<PendingDomainWork> pending;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                if (stopping_ && queue_.empty()) return;
                pending = std::move(queue_.front());
                queue_.pop_front();
            }
            if (!pending) continue;
            if (pending->cancelled.load(std::memory_order_acquire)) {
                pending->run = {};
                continue;
            }
            if (pending->run) {
                pending->run();
                pending->run = {};
            }
            pending->completion.Complete();
        }
    }

    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<std::shared_ptr<PendingDomainWork>> queue_;
    std::vector<std::thread> workers_;
    std::size_t workerCount_;
    bool stopping_ = false;
};

inline DomainWorkerPool& SharedDomainWorkerPool() {
    static DomainWorkerPool pool;
    return pool;
}

inline huxerui::Task<void> AwaitDomainJob(
    std::function<void()> job,
    std::shared_ptr<std::stop_source> stopSource = {}) {
    co_await huxerui::AwaitCallback<void>(
        [job = std::move(job), stopSource = std::move(stopSource)](
            huxerui::CallbackCompletion<void> completion) mutable
        -> std::function<void()> {
            auto pending = std::make_shared<PendingDomainWork>(
                std::move(job), std::move(completion), std::move(stopSource));
            if (!SharedDomainWorkerPool().Submit(pending)) {
                pending->completion.Fail(
                    std::make_exception_ptr(std::runtime_error("Clash-Flux worker pool is closed")));
                return {};
            }
            return [pending]() noexcept {
                pending->cancelled.store(true, std::memory_order_release);
                if (pending->stopSource) {
                    try { pending->stopSource->request_stop(); } catch (...) {}
                }
            };
        });
}

// 结果槽：worker 写一次、协程读一次（没有轮询、没有条件变量——完成由 AwaitCallback
// 续体保证）。放在命名空间作用域，协程帧里只有 shared_ptr<ResultSlot<R>>，
// 结果类型不会进入内部调用者的协程帧。
template <class R>
struct ResultSlot {
    std::optional<R> value;
    std::exception_ptr error;
};

template <>
struct ResultSlot<void> {
    std::exception_ptr error;
};

// 真正发起 worker 调用的协程：形参与提交给线程池的任务都擦除成 std::function，
// 结果经 ResultSlot 传递——协程帧里不含调用方匿名命名空间里的类型，否则会带
// 内部链接成员触发 -Wsubobject-linkage。
template <class R>
huxerui::Task<R> RunOnWorker(std::function<R()> fn) {
    auto slot = std::make_shared<ResultSlot<R>>();
    std::function<void()> job = [slot, fn = std::move(fn)]() mutable {
        try {
            if constexpr (std::is_void_v<R>) {
                fn();
            } else {
                slot->value.emplace(fn());
            }
        } catch (...) {
            slot->error = std::current_exception();
        }
    };
    co_await AwaitDomainJob(std::move(job));
    if (slot->error) std::rethrow_exception(slot->error);
    if constexpr (!std::is_void_v<R>) co_return std::move(*slot->value);
}

template <class R>
huxerui::Task<R> RunOnWorkerWithStop(std::function<R(std::stop_token)> fn) {
    auto slot = std::make_shared<ResultSlot<R>>();
    auto stopSource = std::make_shared<std::stop_source>();
    std::function<void()> job = [slot, stopSource, fn = std::move(fn)]() mutable {
        try {
            const auto token = stopSource->get_token();
            if constexpr (std::is_void_v<R>) {
                fn(token);
            } else {
                slot->value.emplace(fn(token));
            }
        } catch (...) {
            slot->error = std::current_exception();
        }
    };
    co_await AwaitDomainJob(std::move(job), stopSource);
    if (slot->error) std::rethrow_exception(slot->error);
    if constexpr (!std::is_void_v<R>) co_return std::move(*slot->value);
}

template <class T>
struct AsyncBridgeState {
    std::atomic<bool> settled{false};
};

template <class T>
huxerui::Task<T> AwaitErasedAsync(
    std::function<std::function<void()>(std::function<void(T)>)> start) {
    co_return co_await huxerui::AwaitCallback<T>(
        [start = std::move(start)](huxerui::CallbackCompletion<T> completion) mutable
        -> std::function<void()> {
            auto state = std::make_shared<AsyncBridgeState<T>>();
            auto completionPtr = std::make_shared<huxerui::CallbackCompletion<T>>(
                std::move(completion));
            std::function<void()> cancel;
            try {
                cancel = start([state, completionPtr](T value) mutable {
                    if (!state->settled.exchange(true, std::memory_order_acq_rel)) {
                        completionPtr->Complete(std::move(value));
                    }
                });
            } catch (...) {
                if (!state->settled.exchange(true, std::memory_order_acq_rel)) {
                    completionPtr->Fail(std::current_exception());
                }
                return {};
            }
            return [state, cancel = std::move(cancel)]() noexcept {
                state->settled.store(true, std::memory_order_release);
                if (cancel) {
                    try { cancel(); } catch (...) {}
                }
            };
        });
}

} // namespace detail

template <class F>
huxerui::Task<std::invoke_result_t<F>> RunOnTaskThread(F fn) {
    using R = std::invoke_result_t<F>;
    // 本函数**不是**协程：它只做类型擦除再转交，调用方的 lambda 类型不会进入
    // 任何协程帧。
    return detail::RunOnWorker<R>(std::function<R()>(std::move(fn)));
}

template <class F>
huxerui::Task<std::invoke_result_t<F, std::stop_token>> RunOnTaskThreadWithStop(F fn) {
    using R = std::invoke_result_t<F, std::stop_token>;
    return detail::RunOnWorkerWithStop<R>(
        std::function<R(std::stop_token)>(std::move(fn)));
}

// 桥接基于回调的真异步操作（例如 curl_multi）到 HuxerUI 协程，并在取消时调用
// start 返回的取消句柄。同样在进入协程前擦除类型，避免 -Wsubobject-linkage。
template <class T, class StartFn>
huxerui::Task<T> AwaitAsyncResult(StartFn start) {
    return detail::AwaitErasedAsync<T>(
        std::function<std::function<void()>(std::function<void(T)>)>(std::move(start)));
}

// 引擎轮询桥：每 interval 在 UI 线程执行一次 tick()；tick 返回 true 继续等，
// false 结束。tick 内做 drain/poll + 写 State（全程 UI 线程，安全）。
// 组合卸载时 TaskScope 取消协程，tick 不会再被执行。
//
// 用途仅限「确实周期性」的工作（订阅自动更新、有界等待）；由服务/事件推进的
// 共享数据不应再用它采样。
inline huxerui::Task<void> PollWhile(std::chrono::duration<double> interval,
                                     std::function<bool()> tick) {
    while (tick()) co_await huxerui::Delay(interval);
}

} // namespace clashflux::ui
