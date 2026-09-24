// stream.cpp — clashflux.stream 实现单元（IXWebSocket）。
//
// 三条通道共享一个实现：IX 回调只写内部槽位（持锁），UI 泵 drain。
// 日志队列上限 2000 行（UI 跟不上时丢最旧的，保住内存）。
module;

#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXNetSystem.h>

module clashflux.stream;

import std;
import clashflux.config;
import nlohmann.json;
import clashflux.utils;

namespace stream {
namespace {

constexpr std::size_t kMaxLogLines = 2000;

struct PersistentLogStore {
    explicit PersistentLogStore(std::filesystem::path logPath)
        : path(std::move(logPath)) {}

    std::mutex mutex;
    std::vector<LogLine> history;
    std::vector<LogLine> pending;
    bool loaded = false;
    std::filesystem::path path;
};

std::filesystem::path coreLogPath() {
    return cfg::coreWorkDir() / "kernel-ui.log";
}

std::filesystem::path applicationLogPath() {
    return cfg::coreWorkDir() / "app.log";
}

PersistentLogStore& coreLogStore() {
    static PersistentLogStore store{coreLogPath()};
    return store;
}

PersistentLogStore& applicationLogStore() {
    static PersistentLogStore store{applicationLogPath()};
    return store;
}

std::string normalizeLevel(std::string level);

void trimLogs(std::vector<LogLine>& lines) {
    if (lines.size() > kMaxLogLines) {
        lines.erase(lines.begin(),
                    lines.begin() + static_cast<std::ptrdiff_t>(
                                        lines.size() - kMaxLogLines));
    }
}

void loadLogsLocked(PersistentLogStore& store) {
    if (store.loaded) return;
    store.loaded = true;
    try {
        std::ifstream input(store.path, std::ios::binary);
        std::string line;
        while (std::getline(input, line)) {
            const auto json = nlohmann::json::parse(line, nullptr, false);
            if (!json.is_object() || !json.contains("payload") ||
                !json["payload"].is_string()) {
                continue;
            }
            store.history.push_back(LogLine{
                .level = normalizeLevel(json.value("level", "info")),
                .payload = json.value("payload", ""),
                .at = json.value("at", std::int64_t{0}),
            });
        }
        trimLogs(store.history);
    } catch (...) {
        store.history.clear();
    }
}

void persistLogsLocked(const PersistentLogStore& store) noexcept {
    try {
        std::ofstream output(store.path, std::ios::binary | std::ios::trunc);
        if (!output) return;
        for (const auto& line : store.history) {
            output << nlohmann::json{
                {"at", line.at}, {"level", line.level},
                {"payload", line.payload}}
                              .dump()
                     << '\n';
        }
    } catch (...) {
        // Diagnostics must never affect the operation being diagnosed.
    }
}

void appendLogLineLocked(const PersistentLogStore& store,
                         const LogLine& line) noexcept {
    try {
        std::ofstream output(store.path, std::ios::binary | std::ios::app);
        if (!output) return;
        output << nlohmann::json{
            {"at", line.at}, {"level", line.level},
            {"payload", line.payload}}
                          .dump()
                 << '\n';
    } catch (...) {
        // Diagnostics must never affect the operation being diagnosed.
    }
}

void appendLog(PersistentLogStore& store, LogLine line) {
    std::lock_guard lock(store.mutex);
    loadLogsLocked(store);
    const bool rewrite = store.history.size() >= kMaxLogLines;
    store.history.push_back(line);
    store.pending.push_back(std::move(line));
    trimLogs(store.history);
    trimLogs(store.pending);
    if (rewrite) {
        persistLogsLocked(store);
    } else {
        appendLogLineLocked(store, store.history.back());
    }
}

std::vector<LogLine> logHistory(PersistentLogStore& store) {
    std::lock_guard lock(store.mutex);
    loadLogsLocked(store);
    // The snapshot already includes queued entries; consume them to avoid
    // displaying the same lines again on the first UI poll.
    store.pending.clear();
    return store.history;
}

std::vector<LogLine> drainLogQueue(PersistentLogStore& store) {
    std::lock_guard lock(store.mutex);
    loadLogsLocked(store);
    return std::exchange(store.pending, {});
}

void clearLogHistory(PersistentLogStore& store) {
    std::lock_guard lock(store.mutex);
    store.history.clear();
    store.pending.clear();
    store.loaded = true;
    std::error_code error;
    std::filesystem::remove(store.path, error);
}

// mihomo 与 sing-box 的日志级别词表差异归一：WS /logs 帧的 type（sing-box
// 发 "warn"）统一映射成 UI 词表（warning）；测速/过滤沿用。
std::string normalizeLevel(std::string level) {
    if (level == "warn") return "warning";
    if (level == "trace") return "debug";
    return level;
}

// UI 级别词表（silent/error/warning/info/debug）→ sing-box /logs?level= 值。
std::string queryLevel(std::string level) {
    if (level == "warning") return "warn";
    if (level == "silent") return "error";  // sing-box 无 silent 订阅级别，取最静
    return level;
}

std::string withToken(const std::string& url, const std::string& secret) {
    if (secret.empty()) return url;
    return appendQuery(url, {{"token", secret}});
}

struct Channel {
    ix::WebSocket socket;
    std::atomic<bool> open{false};

    void start(const std::string& url) {
        socket.disableAutomaticReconnection();
        socket.setUrl(url);
        socket.setHandshakeTimeout(5);
        socket.start();
    }

    void stop() {
        open.store(false);
        socket.stop();
    }
};

} // namespace

void logCore(std::string level, std::string payload) noexcept {
    try {
        if (payload.empty()) return;
        appendLog(coreLogStore(), LogLine{
            .level = normalizeLevel(std::move(level)),
            .payload = std::move(payload),
            .at = nowUnix(),
        });
    } catch (...) {
        // Kernel diagnostics are best-effort and must not crash the app.
    }
}

std::vector<LogLine> coreLogHistory() {
    return logHistory(coreLogStore());
}

std::vector<LogLine> drainCoreLogs() {
    return drainLogQueue(coreLogStore());
}

void clearCoreLogs() {
    clearLogHistory(coreLogStore());
}

void logApplication(std::string level, std::string payload) noexcept {
    try {
        if (payload.empty()) return;
        appendLog(applicationLogStore(), LogLine{
            .level = normalizeLevel(std::move(level)),
            .payload = std::move(payload),
            .at = nowUnix(),
        });
    } catch (...) {
        // Application diagnostics are best-effort and must not crash the app.
    }
}

std::vector<LogLine> applicationLogHistory() {
    return logHistory(applicationLogStore());
}

std::vector<LogLine> drainApplicationLogs() {
    return drainLogQueue(applicationLogStore());
}

void clearApplicationLogs() {
    clearLogHistory(applicationLogStore());
}

struct CoreStreams::Impl {
    Channel logs;
    Channel traffic;
    Channel connections;

    std::mutex mutex;
    TrafficPoint latestTraffic;
    bool trafficDirty = false;
    std::string latestConnections;
    bool connectionsDirty = false;

    ~Impl() { stopAll(); }

    void stopAll() {
        logs.stop();
        traffic.stop();
        connections.stop();
    }

    void pushLog(const std::string& text) {
        LogLine line;
        line.at = nowUnix();
        const auto j = nlohmann::json::parse(text, nullptr, false);
        if (j.is_object()) {
            line.level = normalizeLevel(j.value("type", "info"));
            line.payload = j.value("payload", "");
        } else {
            line.level = "info";
            line.payload = text;
        }
        logCore(std::move(line.level), std::move(line.payload));
    }

    void pushTraffic(const std::string& text) {
        const auto j = nlohmann::json::parse(text, nullptr, false);
        if (!j.is_object()) return;
        std::lock_guard lock(mutex);
        latestTraffic.up = j.value("up", std::int64_t{0});
        latestTraffic.down = j.value("down", std::int64_t{0});
        latestTraffic.at = nowUnix();
        trafficDirty = true;
    }

    void pushConnections(const std::string& text) {
        std::lock_guard lock(mutex);
        latestConnections = text;
        connectionsDirty = true;
    }
};

CoreStreams::CoreStreams() : impl_(std::make_unique<Impl>()) {
#if !defined(__ANDROID__)
    static std::once_flag flag;
    std::call_once(flag, [] { ix::initNetSystem(); });
#endif
}

CoreStreams::~CoreStreams() = default;

void CoreStreams::start(const std::string& wsBase, const std::string& secret,
                        const std::string& logLevel) {
    impl_->stopAll();
    {
        std::lock_guard lock(impl_->mutex);
        impl_->trafficDirty = false;
        impl_->latestConnections.clear();
        impl_->connectionsDirty = false;
    }

    Impl* impl = impl_.get();
    impl_->logs.socket.setOnMessageCallback(
        [impl](const ix::WebSocketMessagePtr& msg) {
            switch (msg->type) {
                case ix::WebSocketMessageType::Open:
                    impl->logs.open.store(true);
                    break;
                case ix::WebSocketMessageType::Message:
                    impl->pushLog(msg->str);
                    break;
                case ix::WebSocketMessageType::Close:
                case ix::WebSocketMessageType::Error:
                    impl->logs.open.store(false);
                    break;
                default: break;
            }
        });
    impl_->traffic.socket.setOnMessageCallback(
        [impl](const ix::WebSocketMessagePtr& msg) {
            switch (msg->type) {
                case ix::WebSocketMessageType::Open:
                    impl->traffic.open.store(true);
                    break;
                case ix::WebSocketMessageType::Message:
                    impl->pushTraffic(msg->str);
                    break;
                case ix::WebSocketMessageType::Close:
                case ix::WebSocketMessageType::Error:
                    impl->traffic.open.store(false);
                    break;
                default: break;
            }
        });
    impl_->connections.socket.setOnMessageCallback(
        [impl](const ix::WebSocketMessagePtr& msg) {
            switch (msg->type) {
                case ix::WebSocketMessageType::Open:
                    impl->connections.open.store(true);
                    break;
                case ix::WebSocketMessageType::Message:
                    impl->pushConnections(msg->str);
                    break;
                case ix::WebSocketMessageType::Close:
                case ix::WebSocketMessageType::Error:
                    impl->connections.open.store(false);
                    break;
                default: break;
            }
        });

    impl_->logs.start(withToken(appendQuery(wsBase + "/logs",
                                            {{"level", queryLevel(logLevel)}}),
                                secret));
    impl_->traffic.start(withToken(wsBase + "/traffic", secret));
    impl_->connections.start(withToken(wsBase + "/connections", secret));
}

void CoreStreams::stop() { impl_->stopAll(); }

bool CoreStreams::logsOpen() const { return impl_->logs.open.load(); }
bool CoreStreams::trafficOpen() const { return impl_->traffic.open.load(); }
bool CoreStreams::connectionsOpen() const { return impl_->connections.open.load(); }

std::vector<LogLine> CoreStreams::drainLogs() {
    return drainCoreLogs();
}

bool CoreStreams::takeTraffic(TrafficPoint& out) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->trafficDirty) return false;
    out = impl_->latestTraffic;
    impl_->trafficDirty = false;
    return true;
}

bool CoreStreams::takeConnections(std::string& out) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->connectionsDirty) return false;
    // 保留缓存供连接页在切页后立即恢复；首页和连接页可能同时消费同一条
    // 全量快照，不能通过 move 把另一个页面看到的数据清空。
    out = impl_->latestConnections;
    impl_->connectionsDirty = false;
    return true;
}

bool CoreStreams::readConnections(std::string& out) const {
    std::lock_guard lock(impl_->mutex);
    if (impl_->latestConnections.empty()) return false;
    out = impl_->latestConnections;
    return true;
}

} // namespace stream
