// profiles_cache.h — ProfilesCache 的定义。成员含 StateList<db::Profile>，
// 而 Profile 是 clashflux.model 的模块内类型（头文件里无法前向声明指代，
// 声明出的会是与模块类型不同的不完整类型），因此本头只能出现在
// import clashflux.db 之后；huxerui.h 与 std 头由包含方在此前引入。
// 只被 src/ui 下需要读写缓存内容的 .cpp 包含；其余翻译单元只见 ui.h 里的
// 前向声明。
#pragma once

namespace clashflux::ui {

// 必须由 AppRoot 用**有效句柄**构造（见 app.cpp：list 取自 UseStateList 镜像、
// selection_pending 直接取 profilesModel->selectionPending）。默认构造出来的是
// 空 cell/无效 StateList，读取即抛——不要 `ProfilesCache{}` 出临时对象。
struct ProfilesCache {
    huxerui::StateList<db::Profile> list;
    huxerui::State<bool> selection_pending;
};

} // namespace clashflux::ui
