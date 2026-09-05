// core_store.cppm — clashflux.store.core：内核编排 store（接口即实现，单例）。
//
// 持有 Db / CoreProcess / ClashApi / CoreStreams 四件套，是 UI 与内核之间的唯一
// 入口。线程契约：snapshot() / settings 读取等轻量方法任意线程可调；标注
// 「阻塞」的方法（startCore/stopCore/refreshRuntime/applyConfig）会做 spawn、
// 网络等待，UI 必须经 RunOnTaskThread 调用；State 写入只在 UI 线程（UI 层在
// 协程恢复点把 snapshot 写进 State）。
//
// 启动流程（startCore）：
//   1. 解析 mihomo 二进制（cfg::mihomoBinary）——缺失 → Failed
//   2. 读启用订阅的 YAML（无订阅 = 空）+ 托管注入块合成 coreWorkDir/config.yaml
//   3. spawn mihomo -d <workdir> -f <config>，轮询 /version 等控制器就绪（≤30s，
//      覆盖订阅带规则 provider 的慢冷启动；进程提前退出仍立即判失败）
//   4. 就绪 → Running，拉起 /logs /traffic /connections 三条 WS 流
export module clashflux.store.core;

import std;
import nlohmann.json;
import clashflux.config;
import clashflux.utils;
import clashflux.db;
import clashflux.api;
import clashflux.core;
import clashflux.stream;
import clashflux.sysproxy;
import clashflux.service;

namespace store {

export struct CoreSnapshot {
    core::CoreState state = core::CoreState::Stopped;
    std::string binaryPath;      // 解析到的内核路径（空 = 未安装）
    std::string version;         // 内核版本（Running 时）
    std::string lastError;
    // 运行配置快照（Running 时有效）
    std::string mode;            // rule / global / direct
    int mixedPort = 7899;
    bool allowLan = false;
    std::string logLevel = "info";
    bool tunEnabled = false;

    bool operator==(const CoreSnapshot&) const = default;  // State 变更检测
};

export class CoreStore {
public:
    CoreStore() = default;
    CoreStore(const CoreStore&) = delete;
    CoreStore& operator=(const CoreStore&) = delete;

    // 惰性初始化（打开 Db、确保 secret 存在、准备 API 端点）。幂等。
    void init() {
        std::lock_guard lock(mutex_);
        ensureOpen();
    }

    db::Db& db() {
        ensureOpen();
        return *db_;
    }

    api::ClashApi& api() {
        ensureOpen();
        return *api_;
    }

    stream::CoreStreams& streams() { return streams_; }
    core::CoreProcess& process() { return process_; }

    std::string secret() {
        ensureOpen();
        return secret_;
    }

    CoreSnapshot snapshot() {
        std::lock_guard lock(mutex_);
        CoreSnapshot s = snap_;
        s.binaryPath = binaryPath_;
        return s;
    }

    // ---- 设置（落 settings 表）----
    std::string setting(const std::string& key, const std::string& fallback = "") {
        ensureOpen();
        try {
            return db_->getSetting(key, fallback);
        } catch (...) {
            return fallback;
        }
    }

    void setSetting(const std::string& key, const std::string& value) {
        ensureOpen();
        try {
            db_->setSetting(key, value);
        } catch (...) {
        }
    }

    std::string mode() { return setting("core.mode", "rule"); }
    int mixedPort() {
        try {
            return std::stoi(setting("core.mixed_port", "7899"));
        } catch (...) {
            return 7899;
        }
    }
    bool allowLan() { return setting("core.allow_lan", "false") == "true"; }
    std::string logLevel() { return setting("core.log_level", "info"); }
    // TUN 开关（持久化；运行中经 applyTun PATCH 立即生效，否则下次启动生效）。
    bool tunEnabled() { return setting("core.tun_enabled", "false") == "true"; }
    // 系统代理开关（持久化；写入 KDE kioslaverc / GNOME gsettings）。
    bool systemProxyEnabled() {
        return setting("proxy.system_enabled", "false") == "true";
    }
    bool systemProxySupported() { return sysproxy::supported(); }

    // 切换 TUN（阻塞）。内核运行中 → PATCH /configs 立即生效，失败回滚设置；
    // 未运行 → 仅持久化（下次启动注入 tun 块）。成功更新快照。
    bool applyTun(bool enable) {
        ensureOpen();
        setSetting("core.tun_enabled", enable ? "true" : "false");
        {
            std::lock_guard lock(mutex_);
            snap_.tunEnabled = enable;
        }
        if (snapshot().state != core::CoreState::Running) return true;
        const nlohmann::json body = {{"tun",
                                      {{"enable", enable},
                                       {"stack", "mixed"},
                                       {"device", "clash-flux"},
                                       {"auto-route", true},
                                       {"auto-detect-interface", true},
                                       {"dns-hijack", {"any:53"}}}}};
        if (const auto r = api_->patchConfigs(body.dump()); r.ok) return true;
        // PATCH 失败（常见：无 CAP_NET_ADMIN 权限）→ 回滚，避免每次启动都带
        // 一个起不来的 tun 块。
        setSetting("core.tun_enabled", enable ? "false" : "true");
        const auto r2 = api_->configs();  // 以内核真实状态回写快照
        if (r2.ok) {
            const auto j = nlohmann::json::parse(r2.body, nullptr, false);
            if (j.is_object() && j.contains("tun") && j["tun"].is_object()) {
                std::lock_guard lock(mutex_);
                snap_.tunEnabled = j["tun"].value("enable", false);
            }
        }
        {
            std::lock_guard lock(mutex_);
            snap_.lastError =
                std::format("TUN {}失败（可能需要 root/CAP_NET_ADMIN）",
                            enable ? "开启" : "关闭");
        }
        return false;
    }

    // 切换系统代理（阻塞 shell 调用）。成功持久化设置。
    bool applySystemProxy(bool enable) {
        ensureOpen();
        std::string err;
        const bool ok =
            enable ? sysproxy::enable("127.0.0.1", mixedPort(), err)
                   : sysproxy::disable(err);
        if (!ok) {
            std::lock_guard lock(mutex_);
            snap_.lastError = "系统代理" + std::string(enable ? "开启" : "关闭") +
                              "失败：" + err;
            return false;
        }
        setSetting("proxy.system_enabled", enable ? "true" : "false");
        return true;
    }

    // ---- 内核控制（阻塞：UI 必须 RunOnTaskThread）----

    // 启动内核；profileYaml 为启用订阅的原文（无订阅传空）。
    // detached=true（CLI core start）：直连 spawn 走 setsid 脱离会话驻留，
    // 本进程退出不带走内核。
    void startCore(const std::string& profileYaml, bool detached = false) {
        ensureOpen();
        {
            std::lock_guard lock(mutex_);
            binaryPath_ = cfg::mihomoBinary().string();
            if (binaryPath_.empty() && !service::available()) {
                snap_.state = core::CoreState::Failed;
                snap_.lastError = "未找到 mihomo 内核（engines/ 或 PATH），也未安装服务";
                return;
            }
            snap_.state = core::CoreState::Starting;
            snap_.lastError.clear();
        }

        const std::filesystem::path workDir = cfg::coreWorkDir();
        const std::filesystem::path configFile = workDir / "config.yaml";
        {
            std::ofstream out(configFile, std::ios::binary | std::ios::trunc);
            if (!out) {
                fail("无法写入运行时配置: " + configFile.string());
                return;
            }
            out << core::generateConfig(profileYaml, cfg::controllerAddress(),
                                        secret_, mixedPort(), mode(), allowLan(),
                                        logLevel(), tunEnabled());
        }

        // 三种拉起方式（按优先级）：
        //   1. root 服务托管（装了服务模式 → TUN 等特权操作开箱可用）
        //   2. 接管已在跑的内核（CLI detached spawn / 上次 GUI 残留）：避免
        //      重复 spawn 撞 9097 与混合端口
        //   3. 直接 spawn（默认）
        if (service::available()) {
            std::string err;
            if (!service::startCore(configFile, err)) {
                fail("服务托管启动失败：" + err);
                return;
            }
            managedByService_ = true;
        } else if (api_->version().ok) {
            adopted_ = true;
        } else if (detached) {
            // CLI 驻留形态：setsid 脱离会话 + 日志重定向 + pidfile，CLI 退出
            // 内核仍在。本进程不持句柄，按接管形态管理（活判/停止走 pidfile）。
            std::string err;
            if (!core::spawnDetached(binaryPath_, workDir, configFile, err)) {
                fail(err);
                return;
            }
            adopted_ = true;
        } else if (!process_.start(binaryPath_, workDir, configFile)) {
            fail(process_.lastError());
            return;
        }

        // 等控制器就绪（≤30s）：订阅带规则 provider 时冷启动要拉 geodata/
        // 规则集（可能还走尚未就绪的代理），5s 窗口会误判慢启动为失败；
        // 进程已退出仍立即失败，30s 只是给慢启动的上限。
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        bool ready = false;
        while (std::chrono::steady_clock::now() < deadline) {
            if (!coreAlive()) {
                fail(std::format("内核启动后立即退出（exit {}）",
                                 managedByService_ || adopted_
                                     ? -1
                                     : process_.exitCode()));
                return;
            }
            if (const auto r = api_->version(); r.ok) {
                const auto j = nlohmann::json::parse(r.body, nullptr, false);
                std::lock_guard lock(mutex_);
                snap_.version = j.is_object() ? j.value("version", "") : "";
                ready = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
        }
        if (!ready) {
            if (managedByService_) {
                std::string err;
                service::stopCore(err);
                managedByService_ = false;
            } else {
                process_.stop();
            }
            fail("external-controller 30s 内未就绪");
            return;
        }

        streams_.start(cfg::controllerWsUrl(), secret_, logLevel());
        {
            std::lock_guard lock(mutex_);
            snap_.state = core::CoreState::Running;
        }
        refreshRuntime();
        // 系统代理开关处于开：内核就绪后重指到当前端口（best effort，
        // 失败不判启动失败，记 lastError）。
        if (systemProxyEnabled()) {
            std::string err;
            if (!sysproxy::enable("127.0.0.1", mixedPort(), err)) {
                std::lock_guard lock(mutex_);
                snap_.lastError = "系统代理应用失败：" + err;
            }
        }
    }

    void stopCore() {
        // 先摘系统代理：内核停掉后系统仍指向旧端口会断网。
        if (systemProxyEnabled()) {
            std::string err;
            sysproxy::disable(err);
        }
        streams_.stop();
        if (managedByService_) {
            std::string err;
            service::stopCore(err);
            managedByService_ = false;
        } else if (process_.running()) {
            process_.stop();
        } else {
            // 接管/外部实例（本进程 detached spawn，或另一进程——CLI、上次
            // GUI 残留——拉起的内核）：本进程没有句柄，经 pidfile 终止。
            adopted_ = false;
            if (const long pid = readPidFile(); pid > 0) core::killPid(pid);
        }
        // 等监视线程收尾（最多 ~2s 宽限 + waitpid）。
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (process_.running() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        std::error_code ec;
        std::filesystem::remove(cfg::coreWorkDir() / "mihomo.pid", ec);
        std::lock_guard lock(mutex_);
        snap_.state = core::CoreState::Stopped;
        snap_.version.clear();
    }

    // 拉 /configs 刷新运行配置快照（阻塞：UI 必须 RunOnTaskThread）。
    void refreshRuntime() {
        ensureOpen();
        const auto r = api_->configs();
        if (!r.ok) return;
        const auto j = nlohmann::json::parse(r.body, nullptr, false);
        if (!j.is_object()) return;
        std::lock_guard lock(mutex_);
        snap_.mode = j.value("mode", "");
        snap_.mixedPort = j.value("mixed-port", snap_.mixedPort);
        snap_.allowLan = j.value("allow-lan", snap_.allowLan);
        snap_.logLevel = j.value("log-level", snap_.logLevel);
        if (j.contains("tun") && j["tun"].is_object()) {
            snap_.tunEnabled = j["tun"].value("enable", false);
        }
    }

    // 切换出站模式（阻塞）。成功即更新快照。
    bool applyMode(const std::string& m) {
        ensureOpen();
        const nlohmann::json body = {{"mode", m}};
        const auto r = api_->patchConfigs(body.dump());
        if (!r.ok) return false;
        setSetting("core.mode", m);
        std::lock_guard lock(mutex_);
        snap_.mode = m;
        return true;
    }

    // 崩溃检测：UI 泵每拍调用；Running 但内核已不在 → Failed。
    void checkAlive() {
        std::lock_guard lock(mutex_);
        if (snap_.state == core::CoreState::Running && !coreAlive()) {
            snap_.state = core::CoreState::Failed;
            snap_.lastError =
                std::format("内核异常退出（exit {}）",
                            managedByService_ || adopted_ ? -1
                                                          : process_.exitCode());
        }
    }

private:
    // 三种托管形态的存活判定：服务托管问服务、接管的外部实例看 pidfile、
    // 直接 spawn 看进程句柄。接管形态没有 pidfile（对端不是本应用拉的）
    // 时只好信任——WS 断流会在 UI 层表现为无数据。
    bool coreAlive() {
        if (managedByService_) return service::coreRunning();
        if (adopted_) {
            const long pid = readPidFile();
            if (pid <= 0) return true;  // 无 pidfile：信任
            return std::filesystem::exists(
                std::format("/proc/{}", pid));  // POSIX /proc 判定
        }
        return process_.running();
    }

    long readPidFile() {
        std::ifstream in(cfg::coreWorkDir() / "mihomo.pid");
        long pid = 0;
        in >> pid;
        return pid;
    }

    void ensureOpen() {
        if (db_) return;
        db_ = std::make_unique<db::Db>(cfg::databaseFile());
        secret_ = db_->getSetting("core.secret", "");
        if (secret_.empty()) {
            secret_ = cfg::randomSecret();
            db_->setSetting("core.secret", secret_);
        }
        api_ = std::make_unique<api::ClashApi>(cfg::controllerBaseUrl(), secret_);
    }

    void fail(std::string error) {
        std::lock_guard lock(mutex_);
        snap_.state = core::CoreState::Failed;
        snap_.lastError = std::move(error);
    }

    std::unique_ptr<db::Db> db_;
    std::unique_ptr<api::ClashApi> api_;
    core::CoreProcess process_;
    stream::CoreStreams streams_;
    std::string secret_;

    std::mutex mutex_;
    CoreSnapshot snap_;
    std::string binaryPath_;
    bool managedByService_ = false;  // 内核由 root 服务托管
    bool adopted_ = false;           // 接管的外部内核实例（非本进程 spawn）
};

// 进程级单例（apitab g_requests 同款形态）。
export CoreStore& coreStore() {
    static CoreStore store;
    return store;
}

} // namespace store
