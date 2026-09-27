// stream_updates.h — 把推送流的变化 Post 回 UI 线程。
//
// 推送（IXWebSocket / 日志写入）发生在外部线程，只能发信号；本文件是 UI 侧的
// 桥：观察者回调在推送线程执行，内部 Post 到 tasks 所属 UI 线程再跑 handler，
// 跑完 Acknowledge（见 stream.cppm 的通知 API）。页面据此按需 drain，不再用
// 0.25s/0.5s 定时器轮询队列。
//
// 注意：本头用到 stream::StreamKind，必须在 `import clashflux.stream;` 之后包含。
#pragma once

#include <huxerui/huxerui.h>

#include <cstdint>
#include <functional>

namespace clashflux::ui {

// 订阅推送流变化。handler 在 UI 线程执行，只应该 drain 自己关心的队列；
// 返回的 id 必须在 Lifecycle 清理里交给 UnsubscribeStreamUpdates 注销。
std::uint64_t SubscribeStreamUpdates(
    huxerui::TaskScope tasks,
    std::function<void(stream::StreamKind kind)> handler);
void UnsubscribeStreamUpdates(std::uint64_t id);

} // namespace clashflux::ui
