// core_store.cppm — clashflux.store.core：内核编排 store（接口即实现，单例）。
//
// 持有 Db / CoreProcess / ClashApi / CoreStreams 四件套，是 UI 与内核之间的唯一
// 入口。线程契约：snapshot() / settings 读取等轻量方法任意线程可调；标注
// 「阻塞」的方法（startCore/stopCore/refreshRuntime/applyConfig）会做 spawn、
// 网络等待，UI 必须经 RunOnTaskThread 调用；State 写入只在 UI 线程（UI 层在
// 协程恢复点把 snapshot 写进 State）。
//
// 启动流程（startCore）：
//   1. 解析 sing-box 二进制（cfg::singboxBinary）——缺失 → Failed
//   2. 读启用订阅（无订阅 = 空）经 clashflux.singbox 编译为
//      coreWorkDir/config.json（不支持的节点/规则以 warnings 带回）
//   3. spawn `sing-box run -c <config> -D <workdir>`，轮询 /version 等
//      clash_api 就绪（≤30s，覆盖远程规则集首启下载；进程提前退出仍立即判失败）
//   4. 就绪 → Running，拉起 /logs /traffic /connections 三条 WS 流
module;
#if defined(__ANDROID__)
#include "android_profile_http.h"
#if !defined(NDEBUG)
#include <android/log.h>
#endif
#endif
export module clashflux.store.core;

import std;
import nlohmann.json;
import clashflux.config;
import clashflux.utils;
import clashflux.db;
import clashflux.persistence;
import clashflux.api;
import clashflux.core;
import clashflux.singbox;
import clashflux.rule_provider_cache;
import clashflux.stream;
import clashflux.sysproxy;
import clashflux.service;
import clashflux.routing;
import clashflux.vpn;
import clashflux.vpn_compensation;

namespace store {

#if defined(__ANDROID__)
// android_bridge: 0=stopped, 1=establishing, 2=attached, 3=failed.
extern "C" int clashflux_android_vpn_state() noexcept;
extern "C" int clashflux_android_core_state() noexcept;
extern "C" bool clashflux_android_start_core() noexcept;
extern "C" void clashflux_android_start_vpn() noexcept;
extern "C" void clashflux_android_stop_vpn() noexcept;
extern "C" bool clashflux_android_set_clash_mode(const char*) noexcept;
extern "C" bool clashflux_android_select_outbound(const char*, const char*) noexcept;
#endif

export struct ProfileFidelityReport {
    std::string profileName;
    std::vector<singbox::FidelityNote> notes;
    std::string error;
    bool supported = true;

    bool operator==(const ProfileFidelityReport&) const = default;
};

export struct CoreSnapshot {
    core::CoreState state = core::CoreState::Stopped;
    std::string binaryPath;      // 解析到的内核路径（空 = 未安装）
    std::string version;         // 内核版本（Running 时）
    std::string lastError;
    // 订阅编译降级报告（不支持的节点/规则；启动时更新）。warnings 是自由文本
    // 投影，fidelity 是结构化账本——UI/CLI 应按 fidelity 渲染（见
    // docs/singbox-layers-and-fidelity.md §2），不要解析 warnings 文本。
    std::vector<std::string> warnings;
    std::vector<singbox::FidelityNote> fidelity;
    std::shared_ptr<const std::vector<singbox::SourceObject>> sourceObjects;
    std::vector<std::string> participatingSources;
    std::uint64_t planRevision = 0; // 实际启动成功的策略版本，停止/预览时不代表生效

    // 运行配置快照（Running 时有效）
    std::string mode;            // rule / global / direct
    int mixedPort = 7899;
    bool allowLan = false;
    std::string logLevel = "info";
    bool tunEnabled = false;
    // Android sing-box libbox status stream.  Desktop continues to use the
    // sing-box clash_api WebSocket streams below.
    std::int64_t uploadRate = 0;
    std::int64_t downloadRate = 0;
    std::int64_t uploadTotal = 0;
    std::int64_t downloadTotal = 0;
    int connectionCount = 0;

    bool operator==(const CoreSnapshot&) const = default;  // State 变更检测
};

export class CoreStore {
public:
    CoreStore() = default;
    ~CoreStore() noexcept {
        // 正常关闭由 AppRoot 的异步收尾负责；这里保留一个进程退出时的
        // RAII 兜底，避免窗口被外部关闭、组合树提前卸载后 root 服务仍
        // 持有内核。兜底只覆盖本进程直接持有的形态（服务托管/直接
        // spawn）：adopted 不在其列——CLI `core start` 的 detached 驻留
        // 契约就是 CLI 退出后内核继续跑（停止走 `core stop` / pidfile）。
        // 析构阶段不能把异常带出进程；正常 UI 关闭仍由 AppRoot 的任务
        // 线程路径负责，避免阻塞交互线程。
        try {
#ifdef _WIN32
            // A status-only CLI client does not own an existing SCM core.
            const bool serviceOwnsCore = managedByService_ && !serviceDetached_;
#else
            const bool serviceOwnsCore =
                managedByService_ ||
                (!process_.running() && service::reachable() &&
                 service::coreRunning());
#endif
            if (serviceOwnsCore || process_.running()) stopCore();
        } catch (...) {
        }
    }
    CoreStore(const CoreStore&) = delete;
    CoreStore& operator=(const CoreStore&) = delete;

    // 惰性初始化（打开 Db、确保 secret 存在、准备 API 端点）。幂等。
#include "store/core_store_state.inc"
#include "store/core_store_routing.inc"
#include "store/core_store_lifecycle.inc"
private:
#include "store/core_store_helpers.inc"

    std::unique_ptr<db::Db> db_;
    std::unique_ptr<api::ClashApi> api_;
    core::CoreProcess process_;
    stream::CoreStreams streams_;
    std::string secret_;

    std::mutex mutex_;
    std::recursive_mutex lifecycleMutex_;
    std::vector<singbox::NativeConnection> nativeSessions_;
    std::once_flag initFlag_;
    CoreSnapshot snap_;
    std::string binaryPath_;
    std::optional<singbox::CompileOptions> preparedOptions_;
    std::optional<singbox::CompileOptions> lastAppliedOptions_;
    std::optional<singbox::CompileOptions> lastPreparedOptions_;
    std::shared_ptr<const std::vector<singbox::SourceObject>> previewSourceObjects_;
    std::map<std::filesystem::path, std::pair<std::filesystem::file_time_type, std::uintmax_t>> compiledSourceFiles_;
    std::string compiledPolicyText_;
    std::uint64_t compiledProfilesRevision_ = 0;
    std::string lastProfileYaml_;    // 最近一次 startCore 的订阅原文（applyTun 重启用）
    std::string compiledProxyGroups_;
    // 缓存对应的订阅文件指纹：文件被重新下载/替换（手机上"传输失败后手动补
    // 文件"就是这样）时必须重编，否则进程内会一直返回旧的空快照。
    std::filesystem::file_time_type compiledProxyGroupsStamp_{};
    std::uintmax_t compiledProxyGroupsSize_ = 0;
    bool managedByService_ = false;  // 内核由系统网络服务托管
#ifdef _WIN32
    bool serviceDetached_ = false;   // standalone CLI start survives Runtime exit
#endif
    bool adopted_ = false;           // 接管的外部内核实例（非本进程 spawn）
};

// 进程级单例（apitab g_requests 同款形态）。
export CoreStore& coreStore() {
    static CoreStore store;
    return store;
}

} // namespace store
