// test_sqlite_orm.cpp — huxerui::sqlite ORM 接入验证。
//
// 三条路径：
//   1) 全新库：用生产 schema（src/sqlite_schema.*）建表，跑一遍 CRUD，关库后
//      重开确认数据还在（此时 user_version 已是 schema 版本，不再走迁移）。
//   2) 旧 SQLiteCpp 库：老 DDL 建的库 user_version=0，且 PK 列在
//      PRAGMA table_info 里是 notnull=0，而 ORM 对非空列（含 PK）一律生成
//      NOT NULL，ValidateSchema 会拒绝。必须经 Migration 0→1 在事务内重建表，
//      验证迁移后 schema 通过、订阅与设置数据无损。
//   3) 真实老库快照（仅本地，CLASHFLUX_ORM_LEGACY_DB 指向一份 0.2.x 库）：
//      迁移前后行数必须一致，验证线上数据零丢失。
//
// 用 HuxerUI 公开无窗口测试库驱动异步任务；DB 操作跑在 ORM 自己的
// WorkerSequence 上，主线程循环 Pump 推进。

#include <huxerui/huxerui.h>
#include <huxerui/sqlite.h>
#include <huxerui/testing/ui_test.h>

#include "sqlite_schema.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// MSVC 14.51 起把 C4737（协程无法完成 required tail call）报成错误；它只是性能
// 提示，不该阻断构建。测试仍按普通优化级别编译。
#if defined(_MSC_VER)
#pragma warning(disable : 4737)
#endif

using namespace huxerui;
namespace sqlite = huxerui::sqlite;
namespace db_schema = clashflux::db_schema;
using db_schema::ProfileRow;
using db_schema::SettingRow;

namespace {

// 同 test_persistence：测试必须有限时间结束，看门狗把卡死变成明确失败。
void StartWatchdog() {
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds{90});
        std::fprintf(stderr, "test_sqlite_orm: 看门狗超时\n");
        std::fflush(stderr);
        std::_Exit(1);
    }).detach();
}

const sqlite::Table<ProfileRow>& kProfiles = db_schema::profiles();
const sqlite::Table<SettingRow>& kSettings = db_schema::settings();
const sqlite::Schema& kSchema = db_schema::schema();
const sqlite::Migrations& kMigrations = db_schema::migrations();
const sqlite::OpenOptions& kOpenOptions = db_schema::openOptions();

// 老库形状（SQLiteCpp 时代 db.cpp 的建表 DDL 逐步演化而来：PK 无 NOT NULL，
// 后期列由 ALTER TABLE 追加）。
const std::vector<std::string> kLegacySchema{
    "CREATE TABLE IF NOT EXISTS profiles ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL, "
    "url TEXT NOT NULL DEFAULT '', file TEXT NOT NULL DEFAULT '', "
    "selected INTEGER NOT NULL DEFAULT 0, "
    "updated_at INTEGER NOT NULL DEFAULT 0, "
    "error TEXT NOT NULL DEFAULT '')",
    "CREATE TABLE IF NOT EXISTS settings ("
    "key TEXT PRIMARY KEY, value TEXT NOT NULL DEFAULT '')",
    "ALTER TABLE profiles ADD COLUMN type TEXT NOT NULL DEFAULT 'remote'",
    "ALTER TABLE profiles ADD COLUMN description TEXT NOT NULL DEFAULT ''",
    "ALTER TABLE profiles ADD COLUMN timeout_secs INTEGER NOT NULL DEFAULT 60",
    "ALTER TABLE profiles ADD COLUMN interval_mins INTEGER NOT NULL DEFAULT 0",
    "ALTER TABLE profiles ADD COLUMN auto_update INTEGER NOT NULL DEFAULT 0",
    "ALTER TABLE profiles ADD COLUMN use_system_proxy INTEGER NOT NULL DEFAULT 0",
    "ALTER TABLE profiles ADD COLUMN use_core_proxy INTEGER NOT NULL DEFAULT 0",
    "ALTER TABLE profiles ADD COLUMN allow_invalid_cert INTEGER NOT NULL DEFAULT 0",
    "ALTER TABLE profiles ADD COLUMN native_config TEXT NOT NULL DEFAULT ''",
    "ALTER TABLE profiles ADD COLUMN native_routes TEXT NOT NULL DEFAULT ''",
    "ALTER TABLE profiles ADD COLUMN homepage TEXT NOT NULL DEFAULT ''",
    "ALTER TABLE profiles ADD COLUMN used_bytes INTEGER NOT NULL DEFAULT 0",
    "ALTER TABLE profiles ADD COLUMN total_bytes INTEGER NOT NULL DEFAULT 0",
};

void Check(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string{message});
    }
}

template <class T>
T Require(sqlite::Result<T> result, std::string_view operation) {
    if (!result) {
        throw std::runtime_error(std::string{operation} + ": " +
                                 result.Error().Message());
    }
    return std::move(*result);
}

void Require(sqlite::Result<void> result, std::string_view operation) {
    if (!result) {
        throw std::runtime_error(std::string{operation} + ": " +
                                 result.Error().Message());
    }
}

struct Harness {
    std::mutex mutex;
    bool finished = false;
    std::string failure;
};

Harness g_harness;

void Finish(std::string failure = {}) {
    std::lock_guard lock(g_harness.mutex);
    g_harness.finished = true;
    g_harness.failure = std::move(failure);
}

bool Finished() {
    std::lock_guard lock(g_harness.mutex);
    return g_harness.finished;
}

std::string Failure() {
    std::lock_guard lock(g_harness.mutex);
    return g_harness.failure;
}

std::filesystem::path TempPath(std::string_view tag) {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("clashflux-sqlite-orm-" + std::string{tag} + "-" +
            std::to_string(unique) + ".db");
}

void RemoveDatabase(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::remove(path, error);
    std::filesystem::remove(path.string() + "-wal", error);
    std::filesystem::remove(path.string() + "-shm", error);
}

Task<void> FreshSchemaPhase(const std::filesystem::path& path) {
    sqlite::Database database = Require(
        co_await sqlite::Database::OpenAsync(File{path.string()}, kSchema,
                                             kMigrations,
                                             kOpenOptions),
        "fresh: open");

    // 主键由应用分配，插入时显式给 id。
    Require(co_await database.InsertAsync(
                kProfiles, ProfileRow{.id = 1,
                                      .name = "主订阅",
                                      .url = "https://example.com/a.yaml",
                                      .file = "1.yaml",
                                      .selected = true}),
            "fresh: insert profile");
    Require(co_await database.InsertAsync(
                kProfiles, ProfileRow{.id = 2, .name = "备用", .file = "2.yaml"}),
            "fresh: insert second profile");
    Require(co_await database.InsertAsync(
                kSettings, SettingRow{.key = "ui.theme_mode", .value = "1"}),
            "fresh: insert setting");
    Require(co_await database.InsertAsync(
                kSettings, SettingRow{.key = "core.tun_enabled", .value = "true"}),
            "fresh: insert setting 2");

    const std::int64_t count =
        Require(co_await database.Select(kProfiles).CountAsync(), "fresh: count");
    Check(count == 2, "fresh: unexpected profile count");

    const auto rows = Require(
        co_await database.Select(kProfiles)
            .Where(kProfiles.Column<&ProfileRow::selected>() == true)
            .AllAsync(),
        "fresh: select selected");
    Check(rows.size() == 1 && rows.front().name == "主订阅",
          "fresh: selected profile did not round-trip");

    const std::optional<ProfileRow> found =
        Require(co_await database.FindAsync(kProfiles, std::int64_t{1}), "fresh: find");
    Check(found.has_value() && found->url == "https://example.com/a.yaml",
          "fresh: profile did not round-trip");

    const auto theme = Require(
        co_await database.QueryAsync<std::string>(
            "SELECT value FROM settings WHERE key = ?",
            [](const sqlite::RowView& row) { return row.Get<std::string>(0); },
            std::string{"ui.theme_mode"}),
        "fresh: query setting");
    Check(theme.size() == 1 && theme.front() == "1",
          "fresh: setting did not round-trip");

    Require(co_await database.CloseAsync(), "fresh: close");
}

Task<void> ReopenPhase(const std::filesystem::path& path) {
    sqlite::Database database = Require(
        co_await sqlite::Database::OpenAsync(File{path.string()}, kSchema,
                                             kMigrations,
                                             kOpenOptions),
        "reopen: open");
    const std::int64_t count =
        Require(co_await database.Select(kProfiles).CountAsync(), "reopen: count");
    Check(count == 2, "reopen: data did not persist");
    Require(co_await database.CloseAsync(), "reopen: close");
}

Task<void> LegacyMigrationPhase(const std::filesystem::path& path) {
    // 1) 用老 DDL 造一个 SQLiteCpp 形状的库 + 样本数据。
    {
        sqlite::Database database = Require(
            co_await sqlite::Database::OpenAsync(File{path.string()}, kOpenOptions),
            "legacy: open raw");
        for (const std::string& sql : kLegacySchema) {
            Require(co_await database.ExecuteAsync(sql), "legacy: schema");
        }
        Require(co_await database.ExecuteAsync(
                    "INSERT INTO profiles (name, url, file, type, selected) "
                    "VALUES ('老订阅', 'https://example.com/legacy.yaml', '1.yaml', "
                    "'remote', 1)"),
                "legacy: insert profile");
        Require(co_await database.ExecuteAsync(
                    "INSERT INTO settings (key, value) VALUES ('ui.theme_mode', '2')"),
                "legacy: insert setting");
        Require(co_await database.CloseAsync(), "legacy: close raw");
    }

    // 2) 用生产 schema + Migration 0→1 打开：必须成功，且数据无损。
    {
        auto opened = co_await sqlite::Database::OpenAsync(
            File{path.string()}, kSchema, kMigrations, kOpenOptions);
        if (!opened) {
            throw std::runtime_error("legacy: open with migration failed: " +
                                     opened.Error().Message() + " (" +
                                     opened.Error().Operation() + ")");
        }
        sqlite::Database database = std::move(*opened);

        const std::int64_t profile_count = Require(
            co_await database.Select(kProfiles).CountAsync(), "legacy: count");
        Check(profile_count == 1, "legacy: profile row lost by migration");

        const auto names = Require(
            co_await database.QueryAsync<std::string>(
                "SELECT name FROM profiles ORDER BY id",
                [](const sqlite::RowView& row) { return row.Get<std::string>(0); }),
            "legacy: names");
        Check(names.size() == 1 && names.front() == "老订阅",
              "legacy: profile content lost by migration");

        const auto theme = Require(
            co_await database.QueryAsync<std::string>(
                "SELECT value FROM settings WHERE key = ?",
                [](const sqlite::RowView& row) { return row.Get<std::string>(0); },
                std::string{"ui.theme_mode"}),
            "legacy: setting value");
        Check(theme.size() == 1 && theme.front() == "2",
              "legacy: setting lost by migration");

        // 迁移后写入必须可用（重建后的表接受显式主键）。
        const sqlite::InsertResult inserted = Require(
            co_await database.InsertAsync(
                kProfiles, ProfileRow{.id = 2, .name = "迁移后新增",
                                      .file = "2.yaml"}),
            "legacy: insert after migration");
        Check(inserted.rows_affected == 1,
              "legacy: explicit-id insert failed after table rebuild");

        Require(co_await database.CloseAsync(), "legacy: close");
    }

    // 3) 再开一次：user_version=1，直接校验 schema，迁移只应执行一次。
    {
        sqlite::Database database = Require(
            co_await sqlite::Database::OpenAsync(File{path.string()}, kSchema,
                                                 kMigrations, kOpenOptions),
            "legacy reopen: open");
        const std::int64_t count = Require(
            co_await database.Select(kProfiles).CountAsync(), "legacy reopen: count");
        Check(count == 2, "legacy reopen: migrated data missing");
        Require(co_await database.CloseAsync(), "legacy reopen: close");
    }
}

// 仅本地：对真实老库快照跑迁移，验证线上数据零丢失（CI 不设环境变量）。
Task<void> RealLegacySnapshotPhase(const std::filesystem::path& source) {
    const std::filesystem::path copy = TempPath("real");
    std::error_code error;
    std::filesystem::copy_file(source, copy,
                               std::filesystem::copy_options::overwrite_existing,
                               error);
    if (error) {
        throw std::runtime_error("real legacy: copy failed: " + error.message());
    }
    // WAL 里可能还有未检查点的数据，一并带上才是完整快照。
    for (const char* suffix : {"-wal", "-shm"}) {
        const std::filesystem::path side = source.string() + suffix;
        if (std::filesystem::exists(side, error)) {
            std::filesystem::copy_file(
                side, copy.string() + suffix,
                std::filesystem::copy_options::overwrite_existing, error);
        }
    }
    try {
        // 迁移前先按原样读一次行数，迁移后必须完全一致（证明零丢失）。
        std::int64_t before_profiles = 0;
        std::int64_t before_settings = 0;
        {
            sqlite::Database raw = Require(
                co_await sqlite::Database::OpenAsync(File{copy.string()},
                                                     kOpenOptions),
                "real legacy: open raw");
            before_profiles = Require(
                co_await raw.Select(kProfiles).CountAsync(),
                "real legacy: raw profile count");
            before_settings = Require(
                co_await raw.Select(kSettings).CountAsync(),
                "real legacy: raw settings count");
            Require(co_await raw.CloseAsync(), "real legacy: raw close");
        }

        sqlite::Database database = Require(
            co_await sqlite::Database::OpenAsync(File{copy.string()}, kSchema,
                                                 kMigrations, kOpenOptions),
            "real legacy: open with migration");
        const std::int64_t settings = Require(
            co_await database.Select(kSettings).CountAsync(),
            "real legacy: settings count");
        Check(settings == before_settings,
              "real legacy: settings rows changed by migration");
        const std::int64_t profiles = Require(
            co_await database.Select(kProfiles).CountAsync(),
            "real legacy: profiles count");
        Check(profiles == before_profiles,
              "real legacy: profile rows lost by migration");
        Require(co_await database.CloseAsync(), "real legacy: close");
    } catch (...) {
        RemoveDatabase(copy);
        throw;
    }
    RemoveDatabase(copy);
}

Task<void> RunTest() {
    const std::filesystem::path fresh = TempPath("fresh");
    const std::filesystem::path legacy = TempPath("legacy");
    try {
        co_await FreshSchemaPhase(fresh);
        co_await ReopenPhase(fresh);
        co_await LegacyMigrationPhase(legacy);
        // 仅本地联调：对一份真实老库快照跑同一套迁移，验证真实数据无损。
        // CI 不设该变量，因此不依赖用户数据目录。
        if (const char* real = std::getenv("CLASHFLUX_ORM_LEGACY_DB");
            real != nullptr && *real != '\0') {
            co_await RealLegacySnapshotPhase(std::filesystem::path{real});
        }
        RemoveDatabase(fresh);
        RemoveDatabase(legacy);
        Finish();
    } catch (const std::exception& exception) {
        RemoveDatabase(fresh);
        RemoveDatabase(legacy);
        Finish(exception.what());
    } catch (...) {
        RemoveDatabase(fresh);
        RemoveDatabase(legacy);
        Finish("unknown sqlite ORM test failure");
    }
}

View SqliteOrmTestApp() {
    const TaskScope tasks = UseTaskScope();
    Lifecycle([tasks] {
        tasks.Launch([]() -> Task<void> { co_await RunTest(); });
        return [] {};
    });
    return Spacer();
}

} // namespace

int main() {
    StartWatchdog();
    const Application application{SqliteOrmTestApp};
    testing::UiTest ui{application, testing::UiTestOptions{.viewport = {480.0F, 320.0F}}};
    ui.Pump();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    while (!Finished() && std::chrono::steady_clock::now() < deadline) {
        ui.Pump(std::chrono::milliseconds(8));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    if (!Finished()) {
        std::fprintf(stderr, "test_sqlite_orm: timed out\n");
        std::fflush(stderr);
        std::_Exit(1);
    }
    const std::string failure = Failure();
    if (!failure.empty()) {
        std::fprintf(stderr, "test_sqlite_orm: %s\n", failure.c_str());
        std::fflush(stderr);
        std::_Exit(1);
    }
    std::printf("test_sqlite_orm: ok\n");
    return 0;
}
