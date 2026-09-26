// test_sqlite_orm.cpp — huxerui::sqlite ORM 接入验证（全新建库路径）。
//
// 生产 schema（src/sqlite_schema.*）声明 profiles/settings，主键由应用分配
// （非 AUTOINCREMENT），没有 Migration：0.2.x 的旧结构不兼容且数据不保留，
// 由 Persistence::open 检测到后删库重建（对应测试在 test_persistence）。
//
// 覆盖：全新库建表 → Insert/Find/Select/Count/Where/原生 Query → 关库重开
// 数据仍在。用 HuxerUI 公开无窗口测试库驱动异步任务。

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
const sqlite::OpenOptions& kOpenOptions = db_schema::openOptions();

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

std::filesystem::path TempPath() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("clashflux-sqlite-orm-" + std::to_string(unique) + ".db");
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
                                             sqlite::Migrations{},
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
                                             sqlite::Migrations{},
                                             kOpenOptions),
        "reopen: open");
    const std::int64_t count =
        Require(co_await database.Select(kProfiles).CountAsync(), "reopen: count");
    Check(count == 2, "reopen: data did not persist");
    Require(co_await database.CloseAsync(), "reopen: close");
}

Task<void> RunTest() {
    const std::filesystem::path path = TempPath();
    try {
        co_await FreshSchemaPhase(path);
        co_await ReopenPhase(path);
        RemoveDatabase(path);
        Finish();
    } catch (const std::exception& exception) {
        RemoveDatabase(path);
        Finish(exception.what());
    } catch (...) {
        RemoveDatabase(path);
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
