// 页内分区（二级标签）左右滑动的判定规则。
//
// 抽成纯函数是为了能单测，也为了把一条实测出来的关键不变量写在一处：
// **认领距离必须不晚于框架的 touch_gesture_slop（6pt）**。
//
// 依据（huxerui `runtime_pointer_interaction.cpp`）：按下时把命中路径上的识别器由深
// 到浅注册，Move 时按注册顺序取**第一个** Accept 的识别器。Pager 的整页拖动就是一个
// ScrollRecognitionState：横向占优（|dx| > |dy|）且 max(|dx|,|dy|) >= 6pt 即 Accept。
// 挂在内容区上的 PointerIntercept 注册得比 Pager 早，只有在**同一个 Move 事件**上抢
// 先 Accept 才能拿到会话——认领距离一旦大于 6pt，Pager 就会在某次更小位移的 Move 上
// 先把指针拿走（旧实现用 20pt，真机表现就是「一滑整页翻走」）。所以这里取 6pt，并
// 要求横向明显占优（|dx| > 1.5|dy|）：纵向与斜向拖动仍留给内容区自己的纵向滚动。
//
// 还只在**该方向确实存在相邻分区**时认领：最后一个分区继续左滑不再认领，会话留给
// Pager——页内先翻、翻到头再整页翻，与嵌套滚动一个手感。
#pragma once

#include <cmath>

namespace clashflux::ui {

struct SectionSwipeRules {
    float claim_distance = 6.0F;
    float claim_ratio = 1.5F;
    float commit_distance = 56.0F;
};

enum class SectionSwipeAction {
    Ignore,     // 不认领：交给内容区纵向滚动或外层 Pager
    Claim,      // 认领本次指针会话，之后所有 Move 都归分区滑动
    Release,    // 松手但没越过提交距离：只复位状态，不换页
    CommitPrev, // 松手：切到上一个分区
    CommitNext, // 松手：切到下一个分区
};

/// Move 阶段判定。owned 表示本会话已被分区滑动认领（此后一直独占，不再重新判定）。
inline SectionSwipeAction SectionSwipeOnMove(const SectionSwipeRules& rules, float dx, float dy,
                                             bool owned, bool has_prev, bool has_next) {
    if (owned) return SectionSwipeAction::Claim;
    const float abs_x = std::abs(dx);
    if (abs_x < rules.claim_distance || abs_x <= rules.claim_ratio * std::abs(dy)) {
        return SectionSwipeAction::Ignore;
    }
    const bool has_target = dx < 0.0F ? has_next : has_prev;
    return has_target ? SectionSwipeAction::Claim : SectionSwipeAction::Ignore;
}

/// Up 阶段判定。只有被认领过的会话才可能换页。
inline SectionSwipeAction SectionSwipeOnUp(const SectionSwipeRules& rules, float dx, bool owned) {
    if (!owned) return SectionSwipeAction::Ignore;
    if (dx <= -rules.commit_distance) return SectionSwipeAction::CommitNext;
    if (dx >= rules.commit_distance) return SectionSwipeAction::CommitPrev;
    return SectionSwipeAction::Release;
}

} // namespace clashflux::ui
