// service.cppm — clashflux.service：服务模式（接口即实现，单文件模块）。
//
// 对齐 Clash Verge Rev 的 service mode：TUN/PPTP 需要 root/CAP_NET_ADMIN，一次性
// 首次安装可通过 pkexec 执行 `clash-flux service install` 安装一个 systemd
// 服务；服务以 root 常驻（`clash-flux service run`），统一代替用户态 GUI/CLI
// 管理 sing-box 和系统 PPTP 连接。已安装服务的版本升级由 root 服务
// 自身处理，不再让客户端重复申请 pkexec。
// 客户端协议、安装升级、守护循环和跨平台 stub 分置在同目录 .inc 文件，
// 仍由本模块单元编译，避免改变模块导出边界。
//
// 进程间通道：unix socket /run/clash-flux/service.sock（安装用户 + root 可连）。
// 协议为每条连接一行命令（\n 结尾）、一行回复：
//   START <configPath>  → OK / ERR <原因>
//   STOP                → OK / ERR <原因>
//   STATUS              → RUNNING <pid> / STOPPED
//   VERSION             → clash-flux-service 1 app <Clash-Flux版本>
//   UPGRADE             → OK / ERR <原因>（从当前客户端自身升级 root 服务）
//   PPTP_AVAILABLE      → YES / NO
//   PPTP_STATUS <hex id> → CONNECTED / DISCONNECTED
//   PPTP_START <hex id> <hex config> [hex route ...] → OK <hex if> <hex gateway>
//   PPTP_ROUTES <hex id> [hex route ...]            → OK / ERR <原因>
//   PPTP_STOP <hex id>                              → OK / ERR <原因>
//
// 安全模型：安装时记录 pkexec/sudo 的原始用户 UID；socket 只允许该 UID 和
// root，daemon 还通过 SO_PEERCRED 二次校验。升级时只使用 SO_PEERCRED 得到的
// 客户端进程自身可执行文件和相邻的 sing-box，服务只 spawn 固定 sing-box 二进制；
// （<服务exe目录>/engines/sing-box → <服务exe目录>/sing-box，安装后形态即
// /usr/local/lib/clash-flux/engines/sing-box），参数固定为
// `run -c <config> -D <parent(config)>`，configPath 必须是不含 ".." 的 .json
// 绝对路径。PPTP 请求的字段经过十六进制编码，服务端只接受固定协议字段。
module;

// 服务模式仅 Linux（systemd + unix socket）；非 Linux 平台下方导出同签名 stub。
#if defined(__linux__) && !defined(__ANDROID__)
#include <unistd.h>     // fork, execv, setsid, geteuid, readlink, access, unlink, close
#include <sys/socket.h> // socket, bind, listen, accept, connect
#include <sys/un.h>     // sockaddr_un
#include <sys/stat.h>   // mkdir, chmod
#include <sys/wait.h>   // waitpid
#include <sys/poll.h>   // poll
#include <signal.h>     // sigaction, kill, SIGTERM ...
#include <fcntl.h>      // open, O_APPEND ...
#include <errno.h>
#include <stdio.h>      // fprintf, stderr, snprintf
#include <string.h>     // strerror
#include <stdlib.h>     // system
#include <limits.h>     // PATH_MAX
#endif

export module clashflux.service;

import std;
import clashflux.config;
import clashflux.pptp;

#ifndef CLASHFLUX_VERSION
#define CLASHFLUX_VERSION "unknown"
#endif

namespace service {

export constexpr std::string_view kUnitName = "clash-flux.service";
export constexpr std::string_view kSocketPath = "/run/clash-flux/service.sock";
export constexpr std::string_view kInstallDir = "/usr/local/lib/clash-flux";
export constexpr std::string_view kProtocolVersion = "1";

// VERSION 握手的结果。reachable 表示 socket 上确实有可识别的服务；
// compatible 还要求协议版本和应用版本都与当前客户端一致。
export struct ServiceInfo {
    bool reachable = false;
    std::string protocolVersion;
    std::string applicationVersion;
    std::string error;

    bool compatible() const { return reachable && error.empty(); }
};

// Returned by native VPN start calls on every platform.  The service backend
// is Linux-only, but the client API and its non-Linux stubs must share the
// same public signature so the Android legacy source can include this module.
export struct PptpSessionInfo {
    std::string interfaceName;
    std::string gateway;
};

#if defined(__linux__) && !defined(__ANDROID__)

#include "service_client.inc"
#include "service_admin.inc"
#include "service_daemon.inc"

#else  // !__linux__：服务模式仅 Linux，导出同签名 stub（core_store/cli/UI 无条件 import）。
#include "service_stub.inc"
#endif

} // namespace service
