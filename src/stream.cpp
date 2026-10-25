// stream.cpp — clashflux.stream 实现单元（IXWebSocket）。
//
// 三条通道共享一个实现：IX 回调只写内部槽位（持锁），UI 泵 drain。
// 日志队列上限 2000 行（UI 跟不上时丢最旧的，保住内存）。
module;

#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXNetSystem.h>
#include "wire_codec.h"
#include "runtime_health.h"
#include <condition_variable>
#include <thread>

module clashflux.stream;

import std;
import clashflux.config;
import clashflux.core;
import clashflux.utils;
import clashflux.api;

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

PersistentLogStore& logStore() {
    static PersistentLogStore store{cfg::coreWorkDir() / "logs.log"};
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
                pending.store(false); // A rejected delivery must not latch the channel.
            }
        }
    } catch (...) {
        // Copying a registered callback may itself throw before delivery.
        // Release the reservation even when no observer could be invoked.
        try { updateRegistry().pending[static_cast<int>(kind)].store(false); } catch (...) {}
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

std::vector<LogLine> readLogHistory(PersistentLogStore& store) {
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
    clashflux::health::Channel health;
    std::string url;

    void start(const std::string& endpoint) {
        url = endpoint;
        // 断线必须自愈：内核重启、控制器短暂不可用都会断开。以前禁用自动重连，
        // 流断了就永久死了（页面只能一直显示"未就绪"），直到内核再次重启。
        // IX 的重连等待可被 stop() 取消，所以 stopCore/startCore 的重启路径不受影响。
        socket.enableAutomaticReconnection();
        socket.setMinWaitBetweenReconnectionRetries(1000);
        socket.setMaxWaitBetweenReconnectionRetries(30000);
        socket.setUrl(endpoint);
        socket.setHandshakeTimeout(5);
        socket.start();
    }

    void stop() {
        open.store(false);
        socket.stop();
    }
};

} // namespace

void logApplication(std::string level, std::string payload) noexcept {
    try {
        if (payload.empty()) return;
        appendLog(logStore(), LogLine{
            .level = normalizeLevel(std::move(level)),
            .payload = core::stripAnsi(std::move(payload)),
            .at = nowUnix(),
        });
        notifyStreamUpdate(StreamKind::Logs);
    } catch (...) {
        // Diagnostics are best-effort and must not crash the app.
    }
}

void logCore(std::string level, std::string payload) noexcept {
    logApplication(std::move(level), std::move(payload));
}

std::vector<LogLine> logHistory() {
    return readLogHistory(logStore());
}

std::vector<LogLine> drainLogs() {
    return drainLogQueue(logStore());
}

void clearLogs() {
    clearLogHistory(logStore());
}

struct CoreStreams::Impl {
    Channel logs;
    Channel traffic;
    Channel connections;

    mutable std::mutex mutex;
    std::mutex lifecycle;
    std::mutex wakeMutex;
    std::condition_variable_any wake;
    std::jthread monitor;
    bool active = false;
    std::uint64_t epoch = 0, observationEpoch = 0;
    unsigned pauses = 0;
    std::string base, secret, nativeBase;
    clashflux::health::Control control = clashflux::health::Control::Unknown;
    clashflux::health::Control nativeControl = clashflux::health::Control::Unsupported;
    clashflux::health::Tick controlAt = -1, nativeAt = -1;
    std::uint64_t nativeStartedAt = 0;
    std::vector<std::string> healthEvents;
    TrafficPoint latestTraffic;
    bool trafficDirty = false;
    std::string latestConnections;
    clashflux::wire::ConnectionTotals latestConnectionTotals;
    bool connectionTotalsDirty = false;

    static clashflux::health::Tick now() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    Impl() {
#if !defined(__ANDROID__)
        monitor = std::jthread([this](std::stop_token token) { watch(token); });
#endif
    }
    ~Impl() {
        monitor.request_stop();
        wake.notify_all();
        if (monitor.joinable()) monitor.join();
        stopAll();
    }
    void record(std::uint64_t identity, std::string detail, std::uint64_t observation) {
        std::string line = "runtime-health epoch=" + std::to_string(identity) + " " + std::move(detail);
        {
            std::lock_guard lock(mutex);
            if (!active || epoch != identity || pauses || observationEpoch != observation) return;
            healthEvents.push_back(line);
            if (healthEvents.size() > 16) healthEvents.erase(healthEvents.begin());
        }
        logApplication("debug", std::move(line));
    }
    void watch(std::stop_token token) {
        using namespace clashflux::health;
        Tick previous = now();
        std::array<State, 2> reported{State::Paused, State::Paused};
        while (!token.stop_requested()) {
            {
                std::unique_lock waitLock(wakeMutex);
                wake.wait_for(waitLock, token, std::chrono::seconds(1), [] { return false; });
            }
            if (token.stop_requested()) break;
            std::uint64_t requestEpoch, requestObservation;
            std::string requestBase, requestSecret, requestNative;
            bool trafficDue, connectionsDue;
            std::array<State, 2> states;
            std::array<bool, 2> changed{};
            const Tick time = now();
            {
                std::lock_guard lock(mutex);
                if (!active || pauses != 0) { previous = time; continue; }
                if (time - previous > 5000) {
                    traffic.health.Resume(time);
                    connections.health.Resume(time);
                }
                previous = time;
                requestEpoch = epoch;
                requestObservation = observationEpoch;
                requestBase = base;
                requestSecret = secret;
                requestNative = nativeBase;
                trafficDue = traffic.health.RepairDue(time);
                connectionsDue = connections.health.RepairDue(time);
                if (trafficDue) traffic.health.ReserveRepair(time);
                if (connectionsDue) connections.health.ReserveRepair(time);
                states = {traffic.health.Read(time).state, connections.health.Read(time).state};
                for (int i = 0; i < 2; ++i) {
                    if (states[i] == reported[i]) continue;
                    reported[i] = states[i];
                    changed[i] = true;
                }
            }
            for (int i = 0; i < 2; ++i) if (changed[i])
                record(requestEpoch, std::string("channel=") + (i == 0 ? "traffic" : "connections") + " state=" + std::string(Name(states[i])), requestObservation);
            if (!trafficDue && !connectionsDue) continue;
            // No snapshot/lifecycle lock during the bounded network request.
            api::ClashApi probe(requestBase, requestSecret);
            const auto result = probe.probeVersion();
            const auto evidence = ProbeControl(result.ok, result.status, static_cast<bool>(clashflux::wire::DecodeVersion(result.body)));
            api::NativeRuntime native;
            if (!requestNative.empty()) {
                api::NativeApi client(requestNative, requestSecret);
                native = client.runtime(token);
            }
            const auto nativeEvidence = requestNative.empty() ? Control::Unsupported : NativeControl(native.ok, native.failurePhase);
            std::lock_guard owner(lifecycle);
            {
                std::lock_guard lock(mutex);
                if (!active || pauses != 0 || epoch != requestEpoch || observationEpoch != requestObservation || token.stop_requested()) continue;
                control = evidence;
                controlAt = now();
                nativeControl = nativeEvidence;
                nativeAt = requestNative.empty() ? -1 : now();
                nativeStartedAt = native.ok ? native.startedAt : 0;
            }
            record(requestEpoch, "control=" + std::string(Name(evidence)) + " native=" + std::string(Name(nativeEvidence)), requestObservation);
            for (Channel* channel : {&traffic, &connections}) {
                if (channel == &traffic ? !trafficDue : !connectionsDue) continue;
                {
                    std::lock_guard lock(mutex);
                    if (evidence == Control::AuthenticationError) channel->health.Block(Failure::Authentication);
                    if (evidence == Control::PermissionError) channel->health.Block(Failure::Permission);
                    if (evidence == Control::Unsupported) channel->health.Block(Failure::Unsupported);
                    if (evidence != Control::Responsive || channel->health.Read(now()).state != State::Stale) continue;
                    channel->health.Reopen(now());
                }
                // Owner thread only: never stop/join a socket from its callback.
                channel->stop();
                install(*channel, channel == &traffic ? StreamKind::Traffic : StreamKind::Connections);
                channel->start(channel->url);
                record(requestEpoch, std::string("repair=") + (channel == &traffic ? "traffic" : "connections"), requestObservation);
            }
        }
    }
    void install(Channel& channel, StreamKind kind) {
        const auto generation = channel.health.Generation();
        const auto runtimeEpoch = epoch;
        channel.socket.setOnMessageCallback([this, &channel, kind, generation, runtimeEpoch](const ix::WebSocketMessagePtr& msg) {
            {
                std::lock_guard lock(mutex);
                if (!active || epoch != runtimeEpoch || !channel.health.Accepts(generation)) return;
                if (msg->type == ix::WebSocketMessageType::Open) {
                    channel.open.store(true);
                    channel.health.Open(generation);
                    channel.health.Resume(now());
                } else if (msg->type == ix::WebSocketMessageType::Close || msg->type == ix::WebSocketMessageType::Error) {
                    channel.open.store(false);
                    auto failure = clashflux::health::Failure::Transport;
                    if (msg->type == ix::WebSocketMessageType::Error) {
                        const int status = msg->errorInfo.http_status;
                        if (status == 401) failure = clashflux::health::Failure::Authentication;
                        else if (status == 403) failure = clashflux::health::Failure::Permission;
                        else if (status == 404 || status == 405) failure = clashflux::health::Failure::Unsupported;
                        if (failure != clashflux::health::Failure::Transport) channel.socket.disableAutomaticReconnection();
                    }
                    channel.health.Error(generation, failure);
                }
            }
            if (msg->type == ix::WebSocketMessageType::Message) {
                if (kind == StreamKind::Traffic) pushTraffic(msg->str, generation, runtimeEpoch);
                else if (kind == StreamKind::Connections) pushConnections(msg->str, generation, runtimeEpoch);
                else pushLog(msg->str);
            } else notifyStreamUpdate(kind);
        });
    }

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

    void pushTraffic(const std::string& text, std::uint64_t generation, std::uint64_t runtimeEpoch) {
        const auto decoded = clashflux::wire::DecodeTraffic(text);
        {
            std::lock_guard lock(mutex);
            if (!active || epoch != runtimeEpoch || !traffic.health.Accepts(generation)) return;
            traffic.health.Frame(generation, now(), static_cast<bool>(decoded));
            if (!decoded) return;
            latestTraffic.up = decoded.value.up;
            latestTraffic.down = decoded.value.down;
            latestTraffic.at = nowUnix();
            trafficDirty = true;
        }
        // 出锁后再通知：观察者可能（间接）回调本对象。
        notifyStreamUpdate(StreamKind::Traffic);
    }

    void pushConnections(const std::string& text, std::uint64_t generation, std::uint64_t runtimeEpoch) {
        // Decode on the WebSocket worker; the home UI only consumes integers.
        const auto totals = clashflux::wire::DecodeConnectionTotals(text);
        {
            std::lock_guard lock(mutex);
            if (!active || epoch != runtimeEpoch || !connections.health.Accepts(generation)) return;
            connections.health.Frame(generation, now(), static_cast<bool>(totals));
            if (!totals) return;
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

CoreStreams::HealthPause::HealthPause(CoreStreams& owner) : owner_(owner) {
    std::lock_guard lifecycle(owner_.impl_->lifecycle);
    std::lock_guard lock(owner_.impl_->mutex);
    ++owner_.impl_->pauses;
    ++owner_.impl_->observationEpoch;
}
CoreStreams::HealthPause::~HealthPause() {
    std::lock_guard lock(owner_.impl_->mutex);
    if (--owner_.impl_->pauses == 0) {
        ++owner_.impl_->observationEpoch;
        owner_.impl_->traffic.health.Resume(Impl::now());
        owner_.impl_->connections.health.Resume(Impl::now());
    }
}
CoreStreams::HealthPause CoreStreams::pauseHealth() { return HealthPause{*this}; }

CoreStreams::~CoreStreams() = default;

void CoreStreams::start(const std::string& wsBase, const std::string& secret,
                        const std::string& logLevel, const std::string& nativeBase) {
    std::lock_guard owner(impl_->lifecycle);
    {
        std::lock_guard lock(impl_->mutex);
        impl_->active = false;
        ++impl_->epoch;
    }
    impl_->stopAll();
    {
        std::lock_guard lock(impl_->mutex);
        impl_->trafficDirty = false;
        impl_->latestConnections.clear();
        impl_->connectionTotalsDirty = false;
        impl_->base = wsBase.starts_with("ws://") ? "http://" + wsBase.substr(5) : wsBase;
        impl_->secret = secret;
        impl_->nativeBase = nativeBase;
        impl_->control = clashflux::health::Control::Unknown;
        impl_->nativeControl = nativeBase.empty() ? clashflux::health::Control::Unsupported : clashflux::health::Control::Unknown;
        impl_->controlAt = impl_->nativeAt = -1;
        impl_->nativeStartedAt = 0;
        impl_->logs.health.Reset(Impl::now(), false);
        impl_->traffic.health.Reset(Impl::now());
        impl_->connections.health.Reset(Impl::now());
        impl_->active = true;
    }
    impl_->install(impl_->logs, StreamKind::Logs);
    impl_->install(impl_->traffic, StreamKind::Traffic);
    impl_->install(impl_->connections, StreamKind::Connections);
    impl_->logs.start(withToken(appendQuery(wsBase + "/logs", {{"level", queryLevel(logLevel)}}), secret));
    impl_->traffic.start(withToken(wsBase + "/traffic", secret));
    impl_->connections.start(withToken(wsBase + "/connections", secret));
}

void CoreStreams::stop() {
    std::lock_guard owner(impl_->lifecycle);
    {
        std::lock_guard lock(impl_->mutex);
        impl_->active = false;
        ++impl_->epoch;
        impl_->logs.health.Stop();
        impl_->traffic.health.Stop();
        impl_->connections.health.Stop();
    }
    impl_->stopAll();
}

clashflux::health::Snapshot CoreStreams::health() const {
    std::lock_guard lock(impl_->mutex);
    clashflux::health::Snapshot result{impl_->epoch, impl_->control, impl_->traffic.health.Read(Impl::now()), impl_->connections.health.Read(Impl::now())};
    result.nativeControl = impl_->nativeControl;
    result.controlAgeMs = impl_->controlAt < 0 ? -1 : std::max<clashflux::health::Tick>(0, Impl::now() - impl_->controlAt);
    result.nativeAgeMs = impl_->nativeAt < 0 ? -1 : std::max<clashflux::health::Tick>(0, Impl::now() - impl_->nativeAt);
    result.nativeStartedAt = impl_->nativeStartedAt;
    if (impl_->pauses != 0) {
        result.traffic.state = clashflux::health::State::Paused;
        result.connections.state = clashflux::health::State::Paused;
    }
    return result;
}

std::vector<std::string> CoreStreams::recentHealthEvents() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->healthEvents;
}

bool CoreStreams::logsOpen() const { return impl_->logs.open.load(); }
bool CoreStreams::trafficOpen() const { return impl_->traffic.open.load(); }
bool CoreStreams::connectionsOpen() const { return impl_->connections.open.load(); }

std::vector<LogLine> CoreStreams::drainLogs() {
    return stream::drainLogs();
}

bool CoreStreams::takeTraffic(TrafficPoint& out) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->trafficDirty) return false;
    out = impl_->latestTraffic;
    impl_->trafficDirty = false;
    impl_->traffic.health.Consumed();
    return true;
}

bool CoreStreams::takeConnectionTotals(std::int64_t& upload, std::int64_t& download) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->connectionTotalsDirty) return false;
    upload = impl_->latestConnectionTotals.uploadTotal;
    download = impl_->latestConnectionTotals.downloadTotal;
    impl_->connectionTotalsDirty = false;
    impl_->connections.health.Consumed();
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
    std::uint64_t id;
    {
        std::lock_guard lock(registry.mutex);
        id = registry.nextId++;
        registry.observers.emplace(id, std::move(observer));
    }
    // 新观察者（页面重挂载）必须能收到下一帧：清掉可能残留的"未确认"标记。
    // 旧观察者的 Post 因 scope 关闭被丢弃时标记会留在 true，不清就永远不再通知。
    for (std::atomic<bool>& pending : registry.pending) pending.store(false);
    return id;
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
bool consumeStreamUpdate(StreamKind kind, const StreamUpdateObserver& consumer) noexcept {
    struct Acknowledge {
        StreamKind kind;
        ~Acknowledge() { acknowledgeStreamUpdate(kind); }
    } acknowledge{kind};
    try {
        if (consumer) consumer(kind);
        return true;
    } catch (...) { return false; }
}

} // namespace stream
