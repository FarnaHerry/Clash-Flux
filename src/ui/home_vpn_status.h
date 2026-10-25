#pragma once

#include "box_cat.h"
#include "core_operation.h"

namespace clashflux::ui {

struct HomeVpnStatus {
    BoxCatPhase phase;
    const char* label;
    bool connected;
    bool changing;
    bool failed;
};

// The background VPN snapshot is authoritative; a running core alone is not a VPN.
inline HomeVpnStatus ResolveHomeVpnStatus(int vpnState, const CoreCommand& command) {
    const bool connected = vpnState == 2;
    const bool changing = command.action == CoreAction::Tun || vpnState == 1;
    const bool stopping = command.action == CoreAction::Tun && !command.enabled;
    const bool failed = vpnState == 3 && !changing;
    return {
        failed ? BoxCatPhase::Failed
            : changing ? (stopping ? BoxCatPhase::Stopping : BoxCatPhase::Starting)
            : connected ? BoxCatPhase::Running : BoxCatPhase::Sleeping,
        stopping ? "正在停止" : changing ? "正在启动"
            : failed ? "启动失败" : connected ? "运行中" : "未连接",
        connected, changing, failed,
    };
}

} // namespace clashflux::ui
