// test_persistence.cpp — ORM 持久化服务语义（settings/profiles 缓存 + 异步落库）。
//
// 覆盖：未 open 时读 fallback；open 后 hydrate；setSetting/saveProfile 同步可见
// （缓存）且标脏；flush 落库；close 后重开能读回；以及 0.2.x 旧结构被删库重建。
// 用 HuxerUI 公开无窗口测试库驱动异步任务。

#include <huxerui/huxerui.h>
#include <huxerui/sqlite.h>
#include <huxerui/testing/ui_test.h>

#include "sqlite_schema.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

import clashflux.db;
import clashflux.persistence;

using namespace huxerui;
namespace sqlite = huxerui::sqlite;
namespace db_schema = clashflux::db_schema;
namespace persistence = clashflux::persistence;

namespace {

// 测试必须有限时间结束：HuxerUI 测试夹具靠 Pump 驱动协作式任务，一旦某个平台
// 上的唤醒没送到，任务会一直等下去而主线程也卡在 Pump 里（Windows CI 曾空转
// 40 分钟）。看门狗线程把任何形式的卡死变成明确的失败，并打印最后到达的进度。
std::atomic<int> g_progress{0};

void Trace(int stage, std::string_view message) {
    g_progress.store(stage, std::memory_order_relaxed);
    std::fprintf(stderr, "test_persistence: [%d] %.*s\n", stage,
                 static_cast<int>(message.size()), message.data());
    std::fflush(stderr);
}

void StartWatchdog() {
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds{90});
        std::fprintf(stderr, "test_persistence: 看门狗超时，最后进度 %d\n",
                     g_progress.load(std::memory_order_relaxed));
        std::fflush(stderr);
        std::_Exit(1);
    }).detach();
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

void Check(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string{message});
    }
}

std::filesystem::path TempPath() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("clashflux-persistence-" + std::to_string(unique) + ".db");
}

void RemoveDatabase(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::remove(path, error);
    std::filesystem::remove(path.string() + "-wal", error);
    std::filesystem::remove(path.string() + "-shm", error);
}

Task<void> RunTest() {
    const std::filesystem::path path = TempPath();
    Trace(1, "开始：打开数据库");
    try {
        {
            persistence::Persistence store;
            Check(!store.ready(), "store must not be ready before open");
            Check(store.setting("ui.theme_mode", "fallback") == "fallback",
                  "unopened store must return the fallback");

            Trace(2, "打开数据库");
            Check(co_await store.open(path), "open failed");
            Check(store.ready(), "store must be ready after open");
            Check(store.setting("ui.theme_mode", "fallback") == "fallback",
                  "missing key must return the fallback after hydrate");

            store.setSetting("ui.theme_mode", "2");
            store.setSetting("core.tun_enabled", "true");
            // 同步可见：读路径不依赖落库。
            Check(store.setting("ui.theme_mode", "") == "2",
                  "setSetting must be visible immediately");
            Check(store.hasPendingSettings(), "setSetting must mark the key dirty");

            Trace(3, "flush settings");
            Check(co_await store.flushSettings(), "flush failed");
            Check(!store.hasPendingSettings(), "flush must clear dirty keys");

            Trace(4, "profiles 缓存操作");
            // ---- profiles：插入/列出/更新/独占选中/删除 ----
            db::Profile first;
            first.name = "主订阅";
            first.url = "https://example.com/a.yaml";
            first.file = "1.yaml";
            first.selected = true;
            const std::int64_t first_id = store.saveProfile(first);
            Check(first_id > 0, "saveProfile must return the generated id");

            db::Profile second;
            second.name = "备用";
            second.file = "2.yaml";
            const std::int64_t second_id = store.saveProfile(second);
            Check(second_id == first_id + 1,
                  "second insert must continue the id sequence");

            auto list = store.listProfiles();
            Check(list.size() == 2, "listProfiles must return both rows");
            Check(list.front().id == first_id && list.front().name == "主订阅" &&
                      list.front().selected,
                  "listProfiles must be id-ascending with fields intact");

            first.id = first_id;
            first.name = "主订阅-改名";
            Check(store.saveProfile(first) == first_id,
                  "saveProfile must update an existing row by id");
            list = store.listProfiles();
            Check(list.front().name == "主订阅-改名",
                  "updated profile name did not persist");

            Check(store.setSelectedProfile(second_id),
                  "setSelectedProfile failed");
            list = store.listProfiles();
            Check(!list[0].selected && list[1].selected,
                  "setSelectedProfile must leave exactly one selected row");

            Check(store.deleteProfile(second_id), "deleteProfile failed");
            list = store.listProfiles();
            Check(list.size() == 1 && list.front().id == first_id,
                  "deleteProfile must remove exactly the requested row");

            Trace(5, "关闭数据库");
            co_await store.close();
            Check(!store.ready(), "store must not be ready after close");
        }

        {
            Trace(6, "重新打开数据库");
            persistence::Persistence store;
            Check(co_await store.open(path), "reopen failed");
            Check(store.setting("ui.theme_mode", "") == "2",
                  "setting did not persist through the ORM database");
            Check(store.setting("core.tun_enabled", "") == "true",
                  "second setting did not persist");
            const auto reopened = store.listProfiles();
            Check(reopened.size() == 1 && reopened.front().name == "主订阅-改名",
                  "profile did not persist through the ORM database");
            store.setSetting("ui.theme_mode", "1");
            Trace(7, "第二次 flush");
            Check(co_await store.flushSettings(), "second flush failed");
            co_await store.close();
        }

        {
            // 第三次打开：确认覆盖写生效。
            persistence::Persistence store;
            Check(co_await store.open(path), "third open failed");
            Check(store.setting("ui.theme_mode", "") == "1",
                  "overwritten setting did not persist");
            co_await store.close();
        }

        Trace(8, "旧结构不兼容路径");
        // 旧结构不兼容：open 必须失败并给出提示，且**不删除任何文件**
        // （旧库由用户自行清理）。
        {
            const std::filesystem::path legacy = TempPath();
            {
                auto opened = co_await sqlite::Database::OpenAsync(
                    File{legacy.string()}, db_schema::openOptions());
                Check(static_cast<bool>(opened), "legacy: open raw failed");
                sqlite::Database raw = std::move(*opened);
                for (const char* statement :
                     {"CREATE TABLE profiles (id INTEGER PRIMARY KEY AUTOINCREMENT, "
                      "name TEXT NOT NULL, url TEXT NOT NULL DEFAULT '', "
                      "file TEXT NOT NULL DEFAULT '', "
                      "selected INTEGER NOT NULL DEFAULT 0, "
                      "updated_at INTEGER NOT NULL DEFAULT 0, "
                      "error TEXT NOT NULL DEFAULT '')",
                      "CREATE TABLE settings (key TEXT PRIMARY KEY, "
                      "value TEXT NOT NULL DEFAULT '')",
                      "INSERT INTO profiles (name) VALUES ('旧订阅')",
                      "INSERT INTO settings (key, value) "
                      "VALUES ('ui.theme_mode', '2')"}) {
                    auto result = co_await raw.ExecuteAsync(statement);
                    Check(static_cast<bool>(result), "legacy: ddl failed");
                }
                auto closed = co_await raw.CloseAsync();
                Check(static_cast<bool>(closed), "legacy: close failed");
            }
            const auto sizeBefore =
                std::filesystem::file_size(legacy);
            persistence::Persistence store;
            Check(!co_await store.open(legacy),
                  "legacy database must fail to open");
            Check(!store.lastError().empty(),
                  "legacy open failure must carry a diagnostic");
            std::error_code error;
            Check(std::filesystem::exists(legacy, error),
                  "legacy database must not be deleted");
            Check(std::filesystem::file_size(legacy, error) == sizeBefore,
                  "legacy database must not be modified");
            co_await store.close();
            RemoveDatabase(legacy);
        }

        Trace(9, "完成");
        RemoveDatabase(path);
        Finish();
    } catch (const std::exception& exception) {
        RemoveDatabase(path);
        Finish(exception.what());
    } catch (...) {
        RemoveDatabase(path);
        Finish("unknown persistence test failure");
    }
}

View PersistenceTestApp() {
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
    const Application application{PersistenceTestApp};
    testing::UiTest ui{application, testing::UiTestOptions{.viewport = {480.0F, 320.0F}}};
    ui.Pump();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    while (!Finished() && std::chrono::steady_clock::now() < deadline) {
        ui.Pump(std::chrono::milliseconds(8));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    if (!Finished()) {
        std::fprintf(stderr, "test_persistence: timed out\n");
        std::fflush(stderr);
        std::_Exit(1);
    }
    const std::string failure = Failure();
    if (!failure.empty()) {
        std::fprintf(stderr, "test_persistence: %s\n", failure.c_str());
        std::fflush(stderr);
        std::_Exit(1);
    }
    std::printf("test_persistence: ok\n");
    return 0;
}
