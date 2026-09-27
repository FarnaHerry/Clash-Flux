// task_bridge.h — UI 线程 ↔ 任务线程的协程桥（HuxerUI TaskScope 结构化并发）。
//
// 线程契约：
// - State 只在 UI 线程读写；任务线程与引擎线程绝不得触碰。
// - UseTaskScope().Launch 启动的协程体运行在 UI 线程；co_await huxerui::Delay
//   恢复时仍回到 UI 线程——这是异步结果回写 State 的唯一通道。
// - 阻塞 / CPU 重活经 RunOnTaskThread 派给框架的进程级 worker 池
//   （huxerui::RunWorker）：UI 协程挂起不卡帧，完成（或异常）后协程在 UI 线程
//   恢复，直接拿返回值 / try-catch。
// - 内核 REST 调用（clashflux.api）与内核启停（store 层）都是同步阻塞式，全部
//   经 RunOnTaskThread 派到任务线程；内核推送流由 IXWebSocket 自管线程
//  （事件经 PollWhile 泵取回）；内核子进程自带监视线程（输出行经 PollWhile
//   drain）。
//
// 取消语义：组合卸载时 TaskScope 取消协程（协程在 Delay 悬挂点销毁）；worker
// 上排队未开始的工作被丢弃，已在执行的调用完成后其结果不再恢复已卸载的协程。
#pragma once

#include <huxerui/huxerui.h>

#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace clashflux::ui {

// 在任务线程上执行 fn，UI 协程挂起等待；fn 完成（或抛异常）后协程在 UI 线程
// 恢复——返回值经 co_await 拿到，异常在 UI 线程 rethrow（可 try-catch 包住整个
// co_await）。
//
// 实现直接转交框架的 huxerui::RunWorker：它把可调用对象投到进程级 worker 池，
// 结果/异常在等待中的 UI 线程恢复。此前这里自建线程池 + 结果槽，并用
// co_await Delay(0.01) 每 10ms 采样一次结果槽——那既是一次无谓的定时器链，
// 又把每个阻塞调用的返回延迟量化到 10ms（池满时排队等待还会叠加）。
//
// 线程契约不变：fn 必须自持数据、不得访问 State/View/组合期对象、不得读写在
// UI 线程上的平台资源；需要回写 State 的代码放在 co_await 之后（那时已经在 UI
// 线程）。
namespace detail {

// 结果槽：worker 写一次、协程读一次（没有轮询、没有条件变量——完成由 RunWorker
// 的续体保证）。放在命名空间作用域，协程帧里只有 shared_ptr<ResultSlot<R>>，
// 结果类型不会进入框架 RunWorker 的帧。
template <class R>
struct ResultSlot {
    std::optional<R> value;
    std::exception_ptr error;
};

template <>
struct ResultSlot<void> {
    std::exception_ptr error;
};

// 真正发起 worker 调用的协程：形参与提交给框架的任务都擦除成 std::function，
// 结果经 ResultSlot 传递——协程帧里不含调用方匿名命名空间里的类型，否则
// RunWorker/本协程的帧会带内部链接成员触发 -Wsubobject-linkage（原实现同样是
// 这个两层结构，这里保留了它的意图）。
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
    co_await huxerui::RunWorker(std::move(job));
    if (slot->error) std::rethrow_exception(slot->error);
    if constexpr (!std::is_void_v<R>) co_return std::move(*slot->value);
}

} // namespace detail

template <class F>
huxerui::Task<std::invoke_result_t<F>> RunOnTaskThread(F fn) {
    using R = std::invoke_result_t<F>;
    // 本函数**不是**协程：它只做类型擦除再转交，调用方的 lambda 类型不会进入
    // 任何协程帧。
    return detail::RunOnWorker<R>(std::function<R()>(std::move(fn)));
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
