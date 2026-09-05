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

    // ---- 内核控制（阻塞：UI 必须 RunOnTaskThread）----

    // 启动内核；profileYaml 为启用订阅的原文（无订阅传空）。
    void startCore(const std::string& profileYaml) {
        ensureOpen();
        {
            std::lock_guard lock(mutex_);
            binaryPath_ = cfg::mihomoBinary().string();
            if (binaryPath_.empty()) {
                snap_.state = core::CoreState::Failed;
                snap_.lastError = "未找到 mihomo 内核（engines/ 或 PATH）";
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
            out << core::generateConfig(profileYaml, cfg::controllerAddress(), secret_,
                                        mixedPort(), mode(), allowLan(), logLevel());
        }

        if (!process_.start(binaryPath_, workDir, configFile)) {
            fail(process_.lastError());
            return;
        }

        // 等控制器就绪（≤30s）：订阅带规则 provider 时冷启动要拉 geodata/
        // 规则集（可能还走尚未就绪的代理），5s 窗口会误判慢启动为失败；
        // 进程已退出仍立即失败，30s 只是给慢启动的上限。
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        bool ready = false;
        while (std::chrono::steady_clock::now() < deadline) {
            if (!process_.running()) {
                fail(std::format("内核启动后立即退出（exit {}）", process_.exitCode()));
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
            process_.stop();
            fail("external-controller 30s 内未就绪");
            return;
        }

        streams_.start(cfg::controllerWsUrl(), secret_, logLevel());
        {
            std::lock_guard lock(mutex_);
            snap_.state = core::CoreState::Running;
        }
        refreshRuntime();
    }

    void stopCore() {
        streams_.stop();
        process_.stop();
        // 等监视线程收尾（最多 ~2s 宽限 + waitpid）。
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (process_.running() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
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

    // 崩溃检测：UI 泵每拍调用；Running 但进程已退出 → Failed。
    void checkAlive() {
        std::lock_guard lock(mutex_);
        if (snap_.state == core::CoreState::Running && !process_.running()) {
            snap_.state = core::CoreState::Failed;
            snap_.lastError = std::format("内核异常退出（exit {}）", process_.exitCode());
        }
    }

private:
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
};

// 进程级单例（apitab g_requests 同款形态）。
export CoreStore& coreStore() {
    static CoreStore store;
    return store;
}

} // namespace store
