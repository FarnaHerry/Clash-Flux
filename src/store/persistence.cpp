// persistence.cpp — Persistence 实现单元（huxerui::sqlite ORM）。
module;
#include <huxerui/sqlite.h>
#include "sqlite_schema.h"

module clashflux.persistence;

import std;
import clashflux.model;

namespace clashflux::persistence {

namespace sqlite = huxerui::sqlite;

namespace {

db_schema::ProfileRow ToRow(const model::Profile& profile) {
    return db_schema::ProfileRow{
        .id = profile.id,
        .name = profile.name,
        .url = profile.url,
        .file = profile.file,
        .selected = profile.selected,
        .updated_at = profile.updatedAt,
        .error = profile.error,
        .type = profile.type,
        .description = profile.description,
        .timeout_secs = profile.timeoutSecs,
        .interval_mins = profile.intervalMins,
        .auto_update = profile.autoUpdate,
        .use_system_proxy = profile.useSystemProxy,
        .use_core_proxy = profile.useCoreProxy,
        .allow_invalid_cert = profile.allowInvalidCert,
        .homepage = profile.homepage,
        .used_bytes = profile.usedBytes,
        .total_bytes = profile.totalBytes,
        .native_config = profile.nativeConfig,
        .native_routes = profile.nativeRoutes,
    };
}

model::Profile ToProfile(const db_schema::ProfileRow& row) {
    model::Profile profile;
    profile.id = row.id;
    profile.name = row.name;
    profile.url = row.url;
    profile.file = row.file;
    profile.selected = row.selected;
    profile.updatedAt = row.updated_at;
    profile.error = row.error;
    profile.type = row.type;
    profile.description = row.description;
    profile.timeoutSecs = row.timeout_secs;
    profile.intervalMins = row.interval_mins;
    profile.autoUpdate = row.auto_update;
    profile.useSystemProxy = row.use_system_proxy;
    profile.useCoreProxy = row.use_core_proxy;
    profile.allowInvalidCert = row.allow_invalid_cert;
    profile.homepage = row.homepage;
    profile.usedBytes = row.used_bytes;
    profile.totalBytes = row.total_bytes;
    profile.nativeConfig = row.native_config;
    profile.nativeRoutes = row.native_routes;
    return profile;
}

} // namespace

StartupTheme readStartupTheme(const std::filesystem::path& file) {
    StartupTheme theme;
    std::error_code error;
    const bool exists = std::filesystem::exists(file, error);
    if (error) { theme.error = "读取启动主题失败：" + error.message(); return theme; }
    if (!exists) return theme;
    bool hasSettings = false;
    auto inspected = sqlite::Database::QueryReadOnlySync(huxerui::File{file.string()},
        "SELECT name FROM sqlite_master WHERE type='table' AND name='settings'",
        [&hasSettings](const sqlite::RowView&) -> sqlite::Result<void> {
            hasSettings = true;
            return {};
        });
    if (!inspected) { theme.error = "读取启动主题失败：" + inspected.Error().Message(); return theme; }
    if (!hasSettings) return theme;
    auto read = sqlite::Database::QueryReadOnlySync(huxerui::File{file.string()},
        "SELECT value FROM settings WHERE key='ui.theme_mode'",
        [&theme](const sqlite::RowView& row) -> sqlite::Result<void> {
            auto value = row.Get<std::string>("value");
            if (!value) return value.Error();
            theme.mode = *value == "0" ? 0 : *value == "2" ? 2 : 1;
            return {};
        });
    if (!read) theme.error = "读取启动主题失败：" + read.Error().Message();
    return theme;
}

struct Persistence::Impl {
    std::optional<sqlite::Database> database;
    bool ready = false;
    // 打开失败的状态，仅供诊断；不允许继续订阅写入。
    bool degraded = false;
    mutable std::mutex mutex;
    std::unordered_map<std::string, std::string> settings;
    std::unordered_map<std::string, std::uint64_t> dirty;
    std::vector<model::Profile> profiles;
    std::unordered_map<std::int64_t, std::uint64_t> dirtyProfiles;
    struct Deletion { std::uint64_t revision; std::string file; bool removeFile; };
    std::unordered_map<std::int64_t, Deletion> deletedProfiles;
    std::filesystem::path profileDirectory;
    std::uint64_t settingsRevision = 0;
    bool flushingSettings = false, flushingProfiles = false, closing = false;
    struct FlushGuard {
        Impl* impl;
        bool Impl::* flag;
        ~FlushGuard() { std::lock_guard lock(impl->mutex); impl->*flag = false; }
    };
    void requireProfilesReady() const {
        if (!ready || !database || closing)
            throw std::runtime_error("订阅持久化未就绪，操作已拒绝；原数据库与订阅文件保持不变");
    }
    // 订阅修订号：三个变更入口各 +1，供 UI 侧做脏检查（见 profilesRevision）。
    std::uint64_t profilesRevision = 0;
    std::string lastError;
};

Persistence::Persistence() : impl_(std::make_unique<Impl>()) {}
Persistence::~Persistence() = default;

huxerui::Task<bool> Persistence::open(const std::filesystem::path& file,
                                     const std::filesystem::path& profileDirectory) {
    Impl* impl = impl_.get();
    // 老库（user_version=0 的 SQLiteCpp / 早期 ORM 结构）由 0→1 迁移在事务内
    // 重建表并保留数据；真正迁移不了的库 open 失败并记录错误——应用不删除任何
    // 文件；订阅写入在 hydrate 完成前一律拒绝，失败停止正常启动。
    auto opened = co_await sqlite::Database::OpenAsync(
        huxerui::File{file.string()}, db_schema::schema(),
        db_schema::migrations(), db_schema::openOptions());
    if (!opened) {
        std::lock_guard lock(impl->mutex);
        impl->ready = false;
        impl->degraded = true;
        impl->database.reset();
        impl->lastError = "打开数据库失败（无法迁移的旧结构）：" +
                          opened.Error().Message();
        co_return false;
    }
    impl->database.emplace(std::move(*opened));

    auto rows = co_await impl->database->Select(db_schema::settings()).AllAsync();
    if (!rows) {
        std::lock_guard lock(impl->mutex);
        impl->ready = false;
        impl->degraded = true;
        impl->lastError = "读取设置失败：" + rows.Error().Message();
        co_return false;
    }

    auto profileRows = co_await impl->database->Select(db_schema::profiles())
                           .OrderBy(db_schema::profiles()
                                        .Column<&db_schema::ProfileRow::id>(),
                                    sqlite::SortDirection::Ascending)
                           .AllAsync();
    if (!profileRows) {
        std::lock_guard lock(impl->mutex);
        impl->ready = false;
        impl->degraded = true;
        impl->lastError = "读取订阅失败：" + profileRows.Error().Message();
        co_return false;
    }

    // 打开前可能已有 setSetting（例如先写意图再启动内核）：hydrate 不能把它们
    // 冲掉，装完缓存后重新覆盖并保持标脏。
    {
        std::lock_guard lock(impl->mutex);
        std::unordered_map<std::string, std::string> preOpenDirty;
        for (const auto& [key, revision] : impl->dirty) {
            const auto found = impl->settings.find(key);
            if (found != impl->settings.end()) {
                preOpenDirty.emplace(found->first, found->second);
            }
        }
        impl->settings.clear();
        for (const db_schema::SettingRow& row : *rows) {
            impl->settings[row.key] = row.value;
        }
        for (auto& [key, value] : preOpenDirty) {
            impl->settings[key] = std::move(value);
        }
        impl->profiles.clear();
        impl->profiles.reserve(profileRows->size());
        for (const db_schema::ProfileRow& row : *profileRows) {
            impl->profiles.push_back(ToProfile(row));
        }
        impl->dirtyProfiles.clear();
        impl->deletedProfiles.clear();
        impl->profileDirectory = profileDirectory;
        impl->closing = false;
        impl->ready = true;
        impl->degraded = false;
        impl->lastError.clear();
        // hydrate 也算一次内容变化：修订号是「列表内容版本」，而不是「写入次数」。
        // 否则首帧（hydrate 之前）读到空表的消费者，在 hydrate 之后如果没有任何
        // 写操作就再也不会收到变化信号（订阅列表停在空表——已发生过的线上问题）。
        ++impl->profilesRevision;
    }
    co_return true;
}

huxerui::Task<void> Persistence::close() {
    Impl* impl = impl_.get();
    { std::lock_guard lock(impl->mutex); impl->closing = true; }
    co_await flushSettings();
    co_await flushProfiles();
    if (impl->database.has_value()) {
        auto closed = co_await impl->database->CloseAsync();
        if (!closed) {
            std::lock_guard lock(impl->mutex);
            impl->lastError = "关闭数据库失败：" + closed.Error().Message();
        }
    }
    std::lock_guard lock(impl->mutex);
    impl->database.reset();
    impl->ready = false;
}

bool Persistence::ready() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return impl_->ready && impl_->database.has_value() && !impl_->closing;
}

bool Persistence::degraded() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return impl_->degraded;
}

std::string Persistence::setting(const std::string& key,
                                 const std::string& fallback) const {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->settings.find(key);
    return found == impl_->settings.end() ? fallback : found->second;
}

void Persistence::setSetting(const std::string& key, const std::string& value) {
    if (key.empty()) return;
    std::lock_guard lock(impl_->mutex);
    if (impl_->degraded || impl_->closing)
        throw std::runtime_error("设置持久化不可用，操作已拒绝");
    impl_->settings[key] = value;
    impl_->dirty[key] = ++impl_->settingsRevision;
}

bool Persistence::hasPendingSettings() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return !impl_->dirty.empty();
}

huxerui::Task<bool> Persistence::flushSettings() {
    Impl* impl = impl_.get();
    for (;;) {
        { std::lock_guard lock(impl->mutex);
          if (!impl->flushingSettings) { impl->flushingSettings = true; break; } }
        co_await huxerui::Delay(std::chrono::milliseconds{1});
    }
    Impl::FlushGuard guard{impl, &Impl::flushingSettings};
    struct Pending { std::string key, value; std::uint64_t revision; };
    std::vector<Pending> pending;
    {
        std::lock_guard lock(impl->mutex);
        if (!impl->ready || !impl->database.has_value()) {
            if (impl->degraded) {
                impl->lastError =
                    "持久化不可用（数据库未打开/迁移失败），写入已拒绝";
                co_return false;
            }
            co_return true;  // 尚未 open：没有需要落库的东西
        }
        pending.reserve(impl->dirty.size());
        for (const auto& [key, revision] : impl->dirty) {
            const auto found = impl->settings.find(key);
            if (found != impl->settings.end()) {
                pending.push_back({found->first, found->second, revision});
            }
        }
    }
    for (const auto& [key, value, revision] : pending) {
        auto result = co_await impl->database->InsertAsync(
            db_schema::settings(), db_schema::SettingRow{key, value},
            sqlite::ConflictPolicy::Replace);
        if (!result) {
            std::lock_guard lock(impl->mutex);
            impl->lastError = "写入设置失败：" + result.Error().Message();
            co_return false;
        }
        // 只确认本次快照；等待期间的新写入（包括 ABA）保留待写版本。
        std::lock_guard lock(impl->mutex);
        if (const auto found = impl->dirty.find(key);
            found != impl->dirty.end() && found->second == revision) impl->dirty.erase(found);
    }
    co_return true;
}

std::vector<model::Profile> Persistence::listProfiles() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->profiles;
}

std::int64_t Persistence::saveProfile(model::Profile profile) {
    std::lock_guard lock(impl_->mutex);
    impl_->requireProfilesReady();
    if (profile.id == 0) {
        std::int64_t next = 1;
        for (const model::Profile& existing : impl_->profiles) {
            next = std::max(next, existing.id + 1);
        }
        profile.id = next;
    }
    const auto found = std::ranges::find_if(
        impl_->profiles, [&profile](const model::Profile& existing) {
            return existing.id == profile.id;
        });
    if (found == impl_->profiles.end()) {
        impl_->profiles.push_back(profile);
        std::ranges::sort(impl_->profiles, {}, &model::Profile::id);
    } else {
        *found = profile;
    }
    impl_->dirtyProfiles[profile.id] = ++impl_->profilesRevision;
    impl_->deletedProfiles.erase(profile.id);
    return profile.id;
}

bool Persistence::deleteProfile(std::int64_t id, bool removeFile) {
    std::lock_guard lock(impl_->mutex);
    impl_->requireProfilesReady();
    const auto found = std::ranges::find(impl_->profiles, id, &model::Profile::id);
    if (found == impl_->profiles.end()) return false;
    Impl::Deletion deletion{++impl_->profilesRevision, found->file, removeFile};
    std::erase_if(impl_->profiles, [id](const model::Profile& existing) {
        return existing.id == id;
    });
    impl_->dirtyProfiles.erase(id);
    impl_->deletedProfiles[id] = std::move(deletion);
    return true;
}

bool Persistence::setSelectedProfile(std::int64_t id) {
    std::lock_guard lock(impl_->mutex);
    impl_->requireProfilesReady();
    const auto revision = ++impl_->profilesRevision;
    for (model::Profile& profile : impl_->profiles) {
        profile.selected = profile.id == id;
        // 选中态可能从 1→0，统一标脏交给 flush upsert。
        impl_->dirtyProfiles[profile.id] = revision;
    }
    return true;
}

bool Persistence::hasPendingProfiles() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return !impl_->dirtyProfiles.empty() || !impl_->deletedProfiles.empty();
}

huxerui::Task<bool> Persistence::flushProfiles() {
    Impl* impl = impl_.get();
    for (;;) {
        { std::lock_guard lock(impl->mutex);
          if (!impl->flushingProfiles) { impl->flushingProfiles = true; break; } }
        co_await huxerui::Delay(std::chrono::milliseconds{1});
    }
    Impl::FlushGuard guard{impl, &Impl::flushingProfiles};
    std::vector<std::pair<model::Profile, std::uint64_t>> updates;
    std::unordered_map<std::int64_t, Impl::Deletion> removals;
    {
        std::lock_guard lock(impl->mutex);
        if (!impl->ready || !impl->database.has_value()) {
            if (impl->degraded) {
                impl->lastError =
                    "持久化不可用（数据库未打开/迁移失败），写入已拒绝";
                co_return false;
            }
            co_return true;  // 尚未 open：没有需要落库的东西
        }
        removals = impl->deletedProfiles;
        for (const auto& [id, revision] : impl->dirtyProfiles) {
            const auto found = std::ranges::find_if(
                impl->profiles, [id](const model::Profile& profile) {
                    return profile.id == id;
                });
            if (found != impl->profiles.end()) updates.emplace_back(*found, revision);
        }
    }

    for (const auto& [profile, revision] : updates) {
        const db_schema::ProfileRow row = ToRow(profile);
        // PK 由应用分配（非自增），InsertAsync 会带上 id；Replace 让它同时
        // 承担插入与按主键覆盖两种语义。
        auto result = co_await impl->database->InsertAsync(
            db_schema::profiles(), row, sqlite::ConflictPolicy::Replace);
        if (!result) {
            std::lock_guard lock(impl->mutex);
            impl->lastError = "写入订阅失败：" + result.Error().Message();
            co_return false;
        }
        std::lock_guard lock(impl->mutex);
        if (const auto found = impl->dirtyProfiles.find(profile.id);
            found != impl->dirtyProfiles.end() && found->second == revision) impl->dirtyProfiles.erase(found);
    }
    for (const auto& [id, deletion] : removals) {
        { std::lock_guard lock(impl->mutex);
          const auto found = impl->deletedProfiles.find(id);
          if (found == impl->deletedProfiles.end() || found->second.revision != deletion.revision) continue; }
        // Capture remaining persisted references in the same transaction as
        // deletion. No file is touched until the commit has succeeded.
        auto result = co_await impl->database->TransactionAsync(
            [id](sqlite::Transaction& transaction) -> sqlite::Result<std::vector<db_schema::ProfileRow>> {
                auto removed = transaction.Delete(db_schema::profiles(), id);
                if (!removed) return removed.Error();
                return transaction.Select(db_schema::profiles()).All();
            });
        if (!result) {
            std::lock_guard lock(impl->mutex);
            impl->lastError = "删除订阅失败：" + result.Error().Message();
            co_return false;
        }
        const auto rows = std::move(*result);
        const auto cleanupError = co_await huxerui::RunWorker([impl, id, deletion, rows] {
            std::lock_guard lock(impl->mutex);
            const auto found = impl->deletedProfiles.find(id);
            if (found == impl->deletedProfiles.end() || found->second.revision != deletion.revision)
                return std::string{}; // restored/changed during the await
            if (deletion.removeFile && !deletion.file.empty() && !impl->profileDirectory.empty()) {
                // Unsafe/unresolvable paths are retained, never guessed or repaired.
                const auto resolve = [&](const std::string& file) -> std::optional<std::filesystem::path> {
                    const std::filesystem::path relative(file);
                    if (file.empty() || file.find('\0') != std::string::npos || relative.is_absolute() ||
                        relative.has_root_name() || relative.has_root_directory()) return std::nullopt;
                    for (const auto& part : relative) if (part == "..") return std::nullopt;
                    std::error_code ec;
                    const auto root = std::filesystem::canonical(impl->profileDirectory, ec);
                    if (ec) return std::nullopt;
                    const auto path = std::filesystem::weakly_canonical(root / relative, ec);
                    if (ec) return std::nullopt;
                    const auto inside = path.lexically_relative(root);
                    if (inside.empty() || inside == "." || inside.is_absolute()) return std::nullopt;
                    for (const auto& part : inside) if (part == "..") return std::nullopt;
                    return path;
                };
                const auto target = resolve(deletion.file);
                // An unresolved reference makes cleanup unsafe as well.
                const auto references = [&](const auto& profile) {
                    if (profile.file.empty()) return false;
                    const auto path = resolve(profile.file);
                    return !path || (target && *path == *target);
                };
                if (target && !std::ranges::any_of(rows, references) &&
                    !std::ranges::any_of(impl->profiles, references)) {
                    std::error_code ec;
                    const auto original = impl->profileDirectory / deletion.file;
                    if (std::filesystem::is_regular_file(original, ec)) {
                        std::filesystem::remove(original, ec);
                        if (ec) return "订阅记录已删除，文件清理失败：" + ec.message();
                    }
                }
            }
            impl->deletedProfiles.erase(found);
            return std::string{};
        });
        if (!cleanupError.empty()) {
            std::lock_guard lock(impl->mutex); impl->lastError = cleanupError; co_return false;
        }
    }
    co_return true;
}

std::uint64_t Persistence::profilesRevision() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return impl_->profilesRevision;
}

std::string Persistence::lastError() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->lastError;
}

Persistence& persistence() {
    static Persistence instance;
    return instance;
}

} // namespace clashflux::persistence
