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

struct Persistence::Impl {
    std::optional<sqlite::Database> database;
    bool ready = false;
    mutable std::mutex mutex;
    std::unordered_map<std::string, std::string> settings;
    std::unordered_set<std::string> dirty;
    std::vector<model::Profile> profiles;
    std::unordered_set<std::int64_t> dirtyProfiles;
    std::unordered_set<std::int64_t> deletedProfiles;
    std::string lastError;
};

Persistence::Persistence() : impl_(std::make_unique<Impl>()) {}
Persistence::~Persistence() = default;

huxerui::Task<bool> Persistence::open(const std::filesystem::path& file) {
    Impl* impl = impl_.get();
    auto opened = co_await sqlite::Database::OpenAsync(
        huxerui::File{file.string()}, db_schema::schema(),
        db_schema::migrations(), db_schema::openOptions());
    if (!opened) {
        std::lock_guard lock(impl->mutex);
        impl->ready = false;
        impl->database.reset();
        impl->lastError = "打开数据库失败：" + opened.Error().Message();
        co_return false;
    }
    impl->database.emplace(std::move(*opened));

    auto rows = co_await impl->database->Select(db_schema::settings()).AllAsync();
    if (!rows) {
        std::lock_guard lock(impl->mutex);
        impl->ready = false;
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
        impl->lastError = "读取订阅失败：" + profileRows.Error().Message();
        co_return false;
    }

    // 打开前可能已有 setSetting（例如先写意图再启动内核）：hydrate 不能把它们
    // 冲掉，装完缓存后重新覆盖并保持标脏。
    std::unordered_map<std::string, std::string> preOpenDirty;
    {
        std::lock_guard lock(impl->mutex);
        for (const std::string& key : impl->dirty) {
            const auto found = impl->settings.find(key);
            if (found != impl->settings.end()) {
                preOpenDirty.emplace(found->first, found->second);
            }
        }
    }

    {
        std::lock_guard lock(impl->mutex);
        impl->settings.clear();
        for (const db_schema::SettingRow& row : *rows) {
            impl->settings[row.key] = row.value;
        }
        impl->dirty.clear();
        for (auto& [key, value] : preOpenDirty) {
            impl->settings[key] = std::move(value);
            impl->dirty.insert(key);
        }
        impl->profiles.clear();
        impl->profiles.reserve(profileRows->size());
        for (const db_schema::ProfileRow& row : *profileRows) {
            impl->profiles.push_back(ToProfile(row));
        }
        impl->dirtyProfiles.clear();
        impl->deletedProfiles.clear();
        impl->ready = true;
        impl->lastError.clear();
    }
    co_return true;
}

huxerui::Task<void> Persistence::close() {
    Impl* impl = impl_.get();
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
    return impl_->ready && impl_->database.has_value();
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
    impl_->settings[key] = value;
    impl_->dirty.insert(key);
}

bool Persistence::hasPendingSettings() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return !impl_->dirty.empty();
}

huxerui::Task<bool> Persistence::flushSettings() {
    Impl* impl = impl_.get();
    std::vector<std::pair<std::string, std::string>> pending;
    {
        std::lock_guard lock(impl->mutex);
        if (!impl->ready || !impl->database.has_value()) co_return true;
        pending.reserve(impl->dirty.size());
        for (const std::string& key : impl->dirty) {
            const auto found = impl->settings.find(key);
            if (found != impl->settings.end()) {
                pending.emplace_back(found->first, found->second);
            }
        }
    }
    for (const auto& [key, value] : pending) {
        auto result = co_await impl->database->InsertAsync(
            db_schema::settings(), db_schema::SettingRow{key, value},
            sqlite::ConflictPolicy::Replace);
        if (!result) {
            std::lock_guard lock(impl->mutex);
            impl->lastError = "写入设置失败：" + result.Error().Message();
            co_return false;
        }
        // 只清除本次真正写入的键；期间新标脏的同名键会重新入队。
        std::lock_guard lock(impl->mutex);
        impl->dirty.erase(key);
    }
    co_return true;
}

std::vector<model::Profile> Persistence::listProfiles() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->profiles;
}

std::int64_t Persistence::saveProfile(model::Profile profile) {
    std::lock_guard lock(impl_->mutex);
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
    impl_->dirtyProfiles.insert(profile.id);
    impl_->deletedProfiles.erase(profile.id);
    return profile.id;
}

bool Persistence::deleteProfile(std::int64_t id) {
    std::lock_guard lock(impl_->mutex);
    std::erase_if(impl_->profiles, [id](const model::Profile& existing) {
        return existing.id == id;
    });
    impl_->dirtyProfiles.erase(id);
    impl_->deletedProfiles.insert(id);
    return true;
}

bool Persistence::setSelectedProfile(std::int64_t id) {
    std::lock_guard lock(impl_->mutex);
    for (model::Profile& profile : impl_->profiles) {
        profile.selected = profile.id == id;
        // 选中态可能从 1→0，统一标脏交给 flush upsert。
        impl_->dirtyProfiles.insert(profile.id);
    }
    return true;
}

bool Persistence::hasPendingProfiles() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return !impl_->dirtyProfiles.empty() || !impl_->deletedProfiles.empty();
}

huxerui::Task<bool> Persistence::flushProfiles() {
    Impl* impl = impl_.get();
    std::vector<model::Profile> updates;
    std::vector<std::int64_t> removals;
    {
        std::lock_guard lock(impl->mutex);
        if (!impl->ready || !impl->database.has_value()) co_return true;
        removals.assign(impl->deletedProfiles.begin(), impl->deletedProfiles.end());
        for (const std::int64_t id : impl->dirtyProfiles) {
            const auto found = std::ranges::find_if(
                impl->profiles, [id](const model::Profile& profile) {
                    return profile.id == id;
                });
            if (found != impl->profiles.end()) updates.push_back(*found);
        }
    }

    for (const model::Profile& profile : updates) {
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
        impl->dirtyProfiles.erase(profile.id);
    }
    for (const std::int64_t id : removals) {
        auto result = co_await impl->database->DeleteAsync(db_schema::profiles(), id);
        if (!result) {
            std::lock_guard lock(impl->mutex);
            impl->lastError = "删除订阅失败：" + result.Error().Message();
            co_return false;
        }
        std::lock_guard lock(impl->mutex);
        impl->deletedProfiles.erase(id);
    }
    co_return true;
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
