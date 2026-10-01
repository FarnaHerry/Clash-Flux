// stream.cpp — clashflux.stream 实现单元（IXWebSocket）。
//
// 三条通道共享一个实现：IX 回调只写内部槽位（持锁），UI 泵 drain。
// 日志队列上限 2000 行（UI 跟不上时丢最旧的，保住内存）。
module;

#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXNetSystem.h>
#include "wire_codec.h"

module clashflux.stream;

import std;
import clashflux.config;
import clashflux.core;
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
    // 压实计数：到上限后不再每行整文件重写（见 appendLog）。
    std::size_t appendsSinceCompaction = 0;
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

// ---- 变化通知注册表（推送线程 → 观察者）------------------------------------
struct StreamUpdateRegistry {
    std::mutex mutex;
    std::unordered_map<std::uint64_t, StreamUpdateObserver> observers;
    std::uint64_t nextId = 1;
    // 每一路一个"未确认"标记：合并同一路的连续通知。
    std::atomic<bool> pending[3]{};
};

StreamUpdateRegistry& updateRegistry() {
    static StreamUpdateRegistry registry;
    return registry;
}

// 在推送线程调用：向所有观察者广播一次（同一路的未确认通知只发一次）。
// noexcept：调用方是 logCore/logApplication（本身 noexcept），通知失败绝不能
// 让"记一条日志"把进程带走（例如拷贝观察者列表时 bad_alloc）。
void notifyStreamUpdate(StreamKind kind) noexcept {
    try {
        StreamUpdateRegistry& registry = updateRegistry();
        std::atomic<bool>& pending = registry.pending[static_cast<int>(kind)];
        if (pending.exchange(true)) return;
        std::vector<StreamUpdateObserver> observers;
        {
            std::lock_guard lock(registry.mutex);
            observers.reserve(registry.observers.size());
            for (auto& [id, observer] : registry.observers) {
                observers.push_back(observer);
            }
        }
        if (observers.empty()) {
            pending.store(false);
            return;
        }
        for (StreamUpdateObserver& observer : observers) {
            // 观察者的异常绝不能打断推送线程。
            try {
                observer(kind);
            } catch (...) {
            }
        }
    } catch (...) {
    }
}

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
            auto decoded = clashflux::wire::DecodeStoredLog(line);
            if (!decoded) continue;
            store.history.push_back(LogLine{
                .level = normalizeLevel(std::move(decoded.value.level)),
                .payload = core::stripAnsi(decoded.value.payload),
                .at = decoded.value.at,
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
            const auto encoded = clashflux::wire::EncodeStoredLog(
                {line.at, line.level, line.payload});
            if (encoded) output << encoded.value << '\n';
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
        const auto encoded = clashflux::wire::EncodeStoredLog(
            {line.at, line.level, line.payload});
        if (encoded) output << encoded.value << '\n';
    } catch (...) {
        // Diagnostics must never affect the operation being diagnosed.
    }
}

void appendLog(PersistentLogStore& store, LogLine line) {
    std::lock_guard lock(store.mutex);
    loadLogsLocked(store);
    // 到上限后 history 恒为 kMaxLogLines：曾经因此每追加一行就整份重写文件
    // （O(n) 写放大，debug 级别日志下每秒几十次）。改成每追加 1/4 上限的量才
    // 压实一次，文件最多比上限多这么多行。
    const bool atCap = store.history.size() >= kMaxLogLines;
    const bool rewrite =
        atCap && ++store.appendsSinceCompaction >= kMaxLogLines / 4;
    if (rewrite) store.appendsSinceCompaction = 0;
    store.history.push_back(line);
    store.pending.push_back(std::move(line));
    trimLogs(store.history);
    trimLogs(store.pending);
    if (rewrite) {
        persistLogsLocked(store);
    } else {
        appendLogLineLocked(store, store.history.back());
    }
    // 锁外通知：观察者不得触碰 State，UI 层负责 Post 回 UI 线程。
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
        // 断线必须自愈：内核重启、控制器短暂不可用都会断开。以前禁用自动重连，
        // 流断了就永久死了（页面只能一直显示"未就绪"），直到内核再次重启。
        // IX 的重连等待可被 stop() 取消，所以 stopCore/startCore 的重启路径不受影响。
        socket.enableAutomaticReconnection();
        socket.setMinWaitBetweenReconnectionRetries(1000);
        socket.setMaxWaitBetweenReconnectionRetries(30000);
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
        // 统一在这里去掉 ANSI 颜色转义：Android 的 libbox 日志带颜色（桌面进程
        // 输出在 core.cpp 已单独处理过），不清理会原样显示成 "[36mINFO[0m"。
        payload = core::stripAnsi(std::move(payload));
        appendLog(coreLogStore(), LogLine{
            .level = normalizeLevel(std::move(level)),
            .payload = std::move(payload),
            .at = nowUnix(),
        });
        notifyStreamUpdate(StreamKind::Logs);
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
        notifyStreamUpdate(StreamKind::Logs);
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
    clashflux::wire::ConnectionTotals latestConnectionTotals;
    bool connectionTotalsDirty = false;

    ~Impl() { stopAll(); }

    void stopAll() {
        logs.stop();
        traffic.stop();
        connections.stop();
    }

    void pushLog(const std::string& text) {
        LogLine line;
        line.at = nowUnix();
        auto decoded = clashflux::wire::DecodeLog(text);
        if (decoded) {
            line.level = normalizeLevel(std::move(decoded.value.type));
            line.payload = std::move(decoded.value.payload);
        } else {
            line.level = "info";
            line.payload = text;
        }
        logCore(std::move(line.level), std::move(line.payload));
    }

    void pushTraffic(const std::string& text) {
        const auto decoded = clashflux::wire::DecodeTraffic(text);
        if (!decoded) return;
        {
            std::lock_guard lock(mutex);
            latestTraffic.up = decoded.value.up;
            latestTraffic.down = decoded.value.down;
            latestTraffic.at = nowUnix();
            trafficDirty = true;
        }
        // 出锁后再通知：观察者可能（间接）回调本对象。
        notifyStreamUpdate(StreamKind::Traffic);
    }

    void pushConnections(const std::string& text) {
        // Decode on the WebSocket worker; the home UI only consumes integers.
        const auto totals = clashflux::wire::DecodeConnectionTotals(text);
        {
            std::lock_guard lock(mutex);
            latestConnections = text;
            if (totals) {
                latestConnectionTotals = totals.value;
                connectionTotalsDirty = true;
            }
        }
        notifyStreamUpdate(StreamKind::Connections);
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
        impl_->connectionTotalsDirty = false;
    }

    Impl* impl = impl_.get();
    impl_->logs.socket.setOnMessageCallback(
        [impl](const ix::WebSocketMessagePtr& msg) {
            switch (msg->type) {
                case ix::WebSocketMessageType::Open:
                    impl->logs.open.store(true);
                    notifyStreamUpdate(StreamKind::Logs);
                    break;
                case ix::WebSocketMessageType::Message:
                    impl->pushLog(msg->str);
                    break;
                case ix::WebSocketMessageType::Close:
                case ix::WebSocketMessageType::Error:
                    impl->logs.open.store(false);
                    notifyStreamUpdate(StreamKind::Logs);
                    break;
                default: break;
            }
        });
    impl_->traffic.socket.setOnMessageCallback(
        [impl](const ix::WebSocketMessagePtr& msg) {
            switch (msg->type) {
                case ix::WebSocketMessageType::Open:
                    impl->traffic.open.store(true);
                    notifyStreamUpdate(StreamKind::Traffic);
                    break;
                case ix::WebSocketMessageType::Message:
                    impl->pushTraffic(msg->str);
                    break;
                case ix::WebSocketMessageType::Close:
                case ix::WebSocketMessageType::Error:
                    impl->traffic.open.store(false);
                    notifyStreamUpdate(StreamKind::Traffic);
                    break;
                default: break;
            }
        });
    impl_->connections.socket.setOnMessageCallback(
        [impl](const ix::WebSocketMessagePtr& msg) {
            switch (msg->type) {
                case ix::WebSocketMessageType::Open:
                    impl->connections.open.store(true);
                    notifyStreamUpdate(StreamKind::Connections);
                    break;
                case ix::WebSocketMessageType::Message:
                    impl->pushConnections(msg->str);
                    break;
                case ix::WebSocketMessageType::Close:
                case ix::WebSocketMessageType::Error:
                    impl->connections.open.store(false);
                    notifyStreamUpdate(StreamKind::Connections);
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

bool CoreStreams::takeConnectionTotals(std::int64_t& upload, std::int64_t& download) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->connectionTotalsDirty) return false;
    upload = impl_->latestConnectionTotals.uploadTotal;
    download = impl_->latestConnectionTotals.downloadTotal;
    impl_->connectionTotalsDirty = false;
    return true;
}

bool CoreStreams::readConnections(std::string& out) const {
    std::lock_guard lock(impl_->mutex);
    if (impl_->latestConnections.empty()) return false;
    out = impl_->latestConnections;
    return true;
}

std::uint64_t addStreamUpdateObserver(StreamUpdateObserver observer) {
    if (!observer) return 0;
    StreamUpdateRegistry& registry = updateRegistry();
    {
        std::lock_guard lock(registry.mutex);
        const std::uint64_t id = registry.nextId++;
        registry.observers.emplace(id, std::move(observer));
    }
    // 新观察者（页面重挂载）必须能收到下一帧：清掉可能残留的"未确认"标记。
    // 旧观察者的 Post 因 scope 关闭被丢弃时标记会留在 true，不清就永远不再通知。
    for (std::atomic<bool>& pending : registry.pending) pending.store(false);
    return registry.nextId - 1;
}

void removeStreamUpdateObserver(std::uint64_t id) {
    if (id == 0) return;
    StreamUpdateRegistry& registry = updateRegistry();
    std::lock_guard lock(registry.mutex);
    registry.observers.erase(id);
}

void acknowledgeStreamUpdate(StreamKind kind) {
    updateRegistry().pending[static_cast<int>(kind)].store(false);
}

} // namespace stream
