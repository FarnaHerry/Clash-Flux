// sqlite_schema.cpp — schema 声明与打开选项的唯一定义（无迁移，见头文件说明）。
#include "sqlite_schema.h"

#include <chrono>

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

// schema 版本 1，且**没有迁移**：0.2.x 的 SQLiteCpp 旧结构与当前结构不兼容，
// 也不值得保留（订阅/设置就几行），检测到旧库时由 Persistence::open 直接删库
// 重建。因此这里不维护任何 Migration 链，schema 变了就升版本并接受重建。
const sqlite::Schema kSchema{1, kProfiles, kSettings};

const sqlite::OpenOptions kOpenOptions{
    .journal_mode = sqlite::JournalMode::Wal,
    .busy_timeout = std::chrono::seconds{5},
    .create_parent_directories = true,
};

} // namespace

const sqlite::Table<ProfileRow>& profiles() { return kProfiles; }
const sqlite::Table<SettingRow>& settings() { return kSettings; }
const sqlite::Schema& schema() { return kSchema; }
const sqlite::OpenOptions& openOptions() { return kOpenOptions; }

} // namespace clashflux::db_schema
