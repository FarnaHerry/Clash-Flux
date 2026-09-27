// test_section_swipe.cpp — 页内分区滑动判定规则的单元测试。
//
// 复现并锁死一次真机问题：手机端「代理」是 Pager 的一级页，页内分组滑动的
// PointerIntercept 用 20pt 认领，而 Pager 的整页拖动（ScrollRecognitionState）在
// max(|dx|,|dy|) >= 6pt 且横向占优时就 Accept，且两侧识别器按注册顺序取第一个
// Accept——横滑总是先被 Pager 拿走，整页翻走。规则见 src/ui/section_swipe.h。
#include <cmath>
#include <cstdio>
#include <string_view>

#include "../src/ui/section_swipe.h"

namespace {

using clashflux::ui::SectionSwipeAction;
using clashflux::ui::SectionSwipeOnMove;
using clashflux::ui::SectionSwipeOnUp;
using clashflux::ui::SectionSwipeRules;

int failures = 0;

void check(bool condition, std::string_view what) {
    if (!condition) {
        std::printf("FAIL: %.*s\n", static_cast<int>(what.size()), what.data());
        ++failures;
    }
}

const char* Name(SectionSwipeAction action) {
    switch (action) {
    case SectionSwipeAction::Ignore: return "Ignore";
    case SectionSwipeAction::Claim: return "Claim";
    case SectionSwipeAction::Release: return "Release";
    case SectionSwipeAction::CommitPrev: return "CommitPrev";
    case SectionSwipeAction::CommitNext: return "CommitNext";
    }
    return "?";
}

void check_action(SectionSwipeAction actual, SectionSwipeAction expected, std::string_view what) {
    if (actual != expected) {
        std::printf("FAIL: %.*s（期望 %s，实际 %s）\n", static_cast<int>(what.size()),
                    what.data(), Name(expected), Name(actual));
        ++failures;
    }
}

} // namespace

int main() {
    const SectionSwipeRules rules;

    // 框架 Pager 的拖动在 6pt 就 Accept（mounted_node_internal.h 的
    // touch_gesture_slop）。认领距离必须不晚于它，否则横滑必然被 Pager 抢走。
    check(rules.claim_distance <= 6.0F,
          "认领距离不晚于框架 touch_gesture_slop(6pt)");

    // ---- 认领 ---------------------------------------------------------------
    check_action(SectionSwipeOnMove(rules, -4.0F, 0.5F, false, true, true),
                 SectionSwipeAction::Ignore, "4pt 未达认领距离：不认领");
    check_action(SectionSwipeOnMove(rules, -6.0F, 1.0F, false, true, true),
                 SectionSwipeAction::Claim, "6pt 横向主导：抢先认领");
    check_action(SectionSwipeOnMove(rules, -6.0F, 1.0F, false, false, true),
                 SectionSwipeAction::Claim, "左滑且存在下一个分区：认领");
    check_action(SectionSwipeOnMove(rules, 6.0F, 1.0F, false, true, false),
                 SectionSwipeAction::Claim, "右滑且存在上一个分区：认领");

    // ---- 不认领：方向与边界 --------------------------------------------------
    check_action(SectionSwipeOnMove(rules, -8.0F, 0.0F, false, false, false),
                 SectionSwipeAction::Ignore, "两端都没有相邻分区：让给外层 Pager");
    check_action(SectionSwipeOnMove(rules, -8.0F, 0.0F, false, true, false),
                 SectionSwipeAction::Ignore, "已是最后一个分区：左滑不认领");
    check_action(SectionSwipeOnMove(rules, 8.0F, 0.0F, false, false, true),
                 SectionSwipeAction::Ignore, "已是第一个分区：右滑不认领");

    // ---- 不认领：纵向 / 斜向留给内容区滚动 ----------------------------------
    check_action(SectionSwipeOnMove(rules, 6.0F, 5.0F, false, true, true),
                 SectionSwipeAction::Ignore, "斜向（未达 1.5 倍横向主导）：不认领");
    check_action(SectionSwipeOnMove(rules, 8.0F, 20.0F, false, true, true),
                 SectionSwipeAction::Ignore, "纵向拖动：不认领");
    check_action(SectionSwipeOnMove(rules, 6.0F, 4.0F, false, true, true),
                 SectionSwipeAction::Ignore, "恰好 1.5 倍是边界：不认领");

    // ---- 已认领后独占 --------------------------------------------------------
    check_action(SectionSwipeOnMove(rules, 8.0F, 40.0F, true, false, false),
                 SectionSwipeAction::Claim, "已认领的会话继续独占（哪怕是纵向位移）");

    // ---- 松手提交 ------------------------------------------------------------
    check_action(SectionSwipeOnUp(rules, -60.0F, true), SectionSwipeAction::CommitNext,
                 "左滑 60pt 松手：切下一个分区");
    check_action(SectionSwipeOnUp(rules, 60.0F, true), SectionSwipeAction::CommitPrev,
                 "右滑 60pt 松手：切上一个分区");
    check_action(SectionSwipeOnUp(rules, -30.0F, true), SectionSwipeAction::Release,
                 "左滑 30pt 未达提交距离：不换页");
    check_action(SectionSwipeOnUp(rules, -60.0F, false), SectionSwipeAction::Ignore,
                 "从未认领的手势（点击/纵向滚动）松手：什么都不做");

    if (failures == 0) {
        std::printf("test_section_swipe: ok\n");
        return 0;
    }
    std::printf("test_section_swipe: %d failure(s)\n", failures);
    return 1;
}
