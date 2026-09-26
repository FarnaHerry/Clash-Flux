// sqlite_schema.cpp — schema 声明、版本与 0→1 迁移的唯一定义。
#include "sqlite_schema.h"

#include <chrono>
#include <vector>

namespace clashflux::db_schema {
namespace {

namespace sqlite = huxerui::sqlite;

const sqlite::Table<ProfileRow> kProfiles{
    "profiles",
    // 主键由应用分配（不是 AUTOINCREMENT）：ORM 的 InsertAsync 会跳过自增主键，
    // 而订阅文件的命名依赖保存后立刻拿到 id，所以 id 必须在应用侧生成。
    sqlite::Column<&ProfileRow::id>{"id", sqlite::PrimaryKey{}},
    sqlite::Column<&ProfileRow::name>{"name"},
    sqlite::Column<&ProfileRow::url>{"url"},
    sqlite::Column<&ProfileRow::file>{"file"},
    sqlite::Column<&ProfileRow::selected>{"selected"},
    sqlite::Column<&ProfileRow::updated_at>{"updated_at"},
    sqlite::Column<&ProfileRow::error>{"error"},
    sqlite::Column<&ProfileRow::type>{"type"},
    sqlite::Column<&ProfileRow::description>{"description"},
    sqlite::Column<&ProfileRow::timeout_secs>{"timeout_secs"},
    sqlite::Column<&ProfileRow::interval_mins>{"interval_mins"},
    sqlite::Column<&ProfileRow::auto_update>{"auto_update"},
    sqlite::Column<&ProfileRow::use_system_proxy>{"use_system_proxy"},
    sqlite::Column<&ProfileRow::use_core_proxy>{"use_core_proxy"},
    sqlite::Column<&ProfileRow::allow_invalid_cert>{"allow_invalid_cert"},
    sqlite::Column<&ProfileRow::homepage>{"homepage"},
    sqlite::Column<&ProfileRow::used_bytes>{"used_bytes"},
    sqlite::Column<&ProfileRow::total_bytes>{"total_bytes"},
    sqlite::Column<&ProfileRow::native_config>{"native_config"},
    sqlite::Column<&ProfileRow::native_routes>{"native_routes"},
};

const sqlite::Table<SettingRow> kSettings{
    "settings",
    sqlite::Column<&SettingRow::key>{"key", sqlite::PrimaryKey{}},
    sqlite::Column<&SettingRow::value>{"value"},
};

// Migration 0→1：SQLiteCpp 时代的老库 user_version=0、表已存在，而 PK 列在
// PRAGMA table_info 里是 notnull=0；ORM 对非空列（含 PK）一律生成 NOT NULL，
// ValidateSchema 会拒绝。SQLite 不能 ALTER 加 NOT NULL，按官方做法在事务内
// 建新表 → 搬数据 → 删旧表 → 改名。列顺序/类型必须与上面的声明一致。
sqlite::Result<void> RebuildLegacyTables(sqlite::MigrationContext& context) {
    const std::vector<std::string> statements{
        "CREATE TABLE profiles_v1 ("
        "id INTEGER NOT NULL PRIMARY KEY, "
        "name TEXT NOT NULL, url TEXT NOT NULL, file TEXT NOT NULL, "
        "selected INTEGER NOT NULL, updated_at INTEGER NOT NULL, "
        "error TEXT NOT NULL, type TEXT NOT NULL, description TEXT NOT NULL, "
        "timeout_secs INTEGER NOT NULL, interval_mins INTEGER NOT NULL, "
        "auto_update INTEGER NOT NULL, use_system_proxy INTEGER NOT NULL, "
        "use_core_proxy INTEGER NOT NULL, allow_invalid_cert INTEGER NOT NULL, "
        "homepage TEXT NOT NULL, used_bytes INTEGER NOT NULL, "
        "total_bytes INTEGER NOT NULL, native_config TEXT NOT NULL, "
        "native_routes TEXT NOT NULL)",
        "INSERT INTO profiles_v1 ("
        "id, name, url, file, selected, updated_at, error, type, description, "
        "timeout_secs, interval_mins, auto_update, use_system_proxy, "
        "use_core_proxy, allow_invalid_cert, homepage, used_bytes, total_bytes, "
        "native_config, native_routes) SELECT "
        "id, name, url, file, selected, updated_at, error, type, description, "
        "timeout_secs, interval_mins, auto_update, use_system_proxy, "
        "use_core_proxy, allow_invalid_cert, homepage, used_bytes, total_bytes, "
        "native_config, native_routes FROM profiles",
        "DROP TABLE profiles",
        "ALTER TABLE profiles_v1 RENAME TO profiles",
        "CREATE TABLE settings_v1 (key TEXT NOT NULL PRIMARY KEY, "
        "value TEXT NOT NULL)",
        "INSERT INTO settings_v1 (key, value) SELECT key, value FROM settings",
        "DROP TABLE settings",
        "ALTER TABLE settings_v1 RENAME TO settings",
    };
    for (const std::string& sql : statements) {
        auto result = context.Execute(sql);
        if (!result) {
            return result.Error();
        }
    }
    return {};
}

const sqlite::Schema kSchema{1, kProfiles, kSettings};

const sqlite::Migrations kMigrations{
    sqlite::Migration{0, 1, &RebuildLegacyTables},
};

const sqlite::OpenOptions kOpenOptions{
    .journal_mode = sqlite::JournalMode::Wal,
    .busy_timeout = std::chrono::seconds{5},
    .create_parent_directories = true,
};

} // namespace

const sqlite::Table<ProfileRow>& profiles() { return kProfiles; }
const sqlite::Table<SettingRow>& settings() { return kSettings; }
const sqlite::Schema& schema() { return kSchema; }
const sqlite::Migrations& migrations() { return kMigrations; }
const sqlite::OpenOptions& openOptions() { return kOpenOptions; }

} // namespace clashflux::db_schema
