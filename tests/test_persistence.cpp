// test_persistence.cpp — ORM 持久化服务语义（settings/profiles 缓存 + 异步落库）。
//
// 覆盖：未 open 时读 fallback；open 后 hydrate；setSetting/saveProfile 同步可见
// （缓存）且标脏；flush 落库；close 后重开能读回；以及 user_version=0 的旧库经
// 0→1 迁移重建表后数据保留。
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
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

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
    std::filesystem::remove_all(path.string() + ".profiles", error);
}

struct Fixture {
    std::filesystem::path path = TempPath();
    std::filesystem::path files = path.string() + ".profiles";
    Fixture() {
        std::filesystem::create_directory(files);
        std::ofstream(files / "shared.yaml", std::ios::binary) << "remote and local stable content\n";
        std::ofstream(files / "unique.yaml", std::ios::binary) << "other remote stable content\n";
    }
    ~Fixture() { RemoveDatabase(path); }
};

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    Check(static_cast<bool>(input), "protected file is missing");
    return {std::istreambuf_iterator<char>(input), {}};
}

Task<void> WaitFor(std::function<bool()> ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{8};
    while (!ready()) {
        Check(std::chrono::steady_clock::now() < deadline, "async test barrier timed out");
        co_await Delay(std::chrono::milliseconds{1});
    }
}

void SeedProfiles(persistence::Persistence& store) {
    db::Profile remote;
    remote.id = 41; remote.name = "remote"; remote.file = "shared.yaml";
    remote.url = "https://example.test/remote";
    store.saveProfile(remote);
    db::Profile local;
    local.id = 42; local.name = "local"; local.type = "local"; local.file = "./shared.yaml";
    store.saveProfile(local);
    db::Profile other;
    other.id = 43; other.name = "other"; other.file = "unique.yaml";
    other.url = "https://example.test/other";
    store.saveProfile(other);
    store.setSetting("test.value", "A");
}

template<class Function> void Refused(Function action) {
    bool refused = false;
    try { action(); } catch (const std::runtime_error&) { refused = true; }
    Check(refused, "unready/failed persistence write must throw before changing cache/files");
}

Task<void> FailedOpenPhase() {
    const Fixture fixture;
    const auto shared = ReadFile(fixture.files / "shared.yaml");
    const auto unique = ReadFile(fixture.files / "unique.yaml");
    persistence::Persistence store;
    Refused([&] { store.saveProfile(db::Profile{}); });
    Refused([&] { store.deleteProfile(41, true); });
    Refused([&] { store.setSelectedProfile(0); });
    store.setSetting("before.open", "intent");
    Check(co_await store.open(fixture.path, fixture.files), "failure fixture initial open");
    Check(store.setting("before.open") == "intent", "pre-hydrate setting intent must survive");
    SeedProfiles(store);
    co_await store.close();
    auto opened = co_await sqlite::Database::OpenAsync(File{fixture.path.string()}, db_schema::openOptions());
    Check(static_cast<bool>(opened), "open raw failure fixture");
    sqlite::Database raw = std::move(*opened);
    Check(static_cast<bool>(co_await raw.ExecuteAsync("PRAGMA user_version = 999")), "future schema fixture");
    persistence::Persistence failed;
    Check(!co_await failed.open(fixture.path, fixture.files) && failed.degraded() && !failed.ready(),
          "future schema must fail closed without recreating a database");
    Refused([&] { failed.saveProfile(db::Profile{}); });
    Refused([&] { failed.deleteProfile(41, true); });
    Refused([&] { failed.setSelectedProfile(0); });
    Refused([&] { failed.setSetting("test.value", "unsafe"); });
    Check(failed.listProfiles().empty() && !failed.hasPendingProfiles(), "failed writes must not populate empty cache");
    auto rows = co_await raw.Select(db_schema::profiles()).AllAsync();
    Check(rows && rows->size() == 3, "failed open retains every original row");
    Check(ReadFile(fixture.files / "shared.yaml") == shared && ReadFile(fixture.files / "unique.yaml") == unique,
          "failed hydrate retains both referenced files byte for byte");
    // Test-only repair of the intentionally injected future version. Production
    // never rewrites an incompatible database to recover from a startup failure.
    Check(static_cast<bool>(co_await raw.ExecuteAsync("PRAGMA user_version = 1")), "reset injected version");
    Check(static_cast<bool>(co_await raw.CloseAsync()), "close failure observer");
    Check(co_await failed.open(fixture.path, fixture.files), "subsequent valid reopen must succeed");
    const auto recovered = failed.listProfiles();
    Check(recovered.size() == 3 && recovered[0].id == 41 && recovered[1].id == 42 && recovered[2].id == 43 &&
              recovered[0].type == "remote" && recovered[1].type == "local" &&
              recovered[0].file == "shared.yaml" && recovered[1].file == "./shared.yaml" &&
              failed.setting("test.value") == "A", "all stable IDs/sources/references survive failed open and reopen");
    co_await failed.close();
}

struct Gate {
    std::atomic<bool> entered{false}, release{false}, writerDone{false};
    bool writerOk = false;
    ~Gate() { release = true; }
};
struct FlushResult { bool started = false, done = false, ok = false; };

Task<void> HoldWriter(sqlite::Database& raw, std::shared_ptr<Gate> gate) {
    const auto result = co_await raw.TransactionAsync([gate](sqlite::Transaction& transaction) -> sqlite::Result<void> {
        auto locked = transaction.Execute("UPDATE settings SET value = value WHERE key = 'test.value'");
        if (!locked) return locked.Error();
        gate->entered = true;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!gate->release) {
            if (std::chrono::steady_clock::now() >= deadline)
                return sqlite::Error{sqlite::ErrorCode::Busy, "test writer release timed out"};
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        return {};
    });
    gate->writerOk = static_cast<bool>(result);
    gate->writerDone = true;
}

Task<void> FlushWhileLocked(persistence::Persistence& store, bool settings, std::shared_ptr<FlushResult> result) {
    result->started = true;
    result->ok = settings ? co_await store.flushSettings() : co_await store.flushProfiles();
    result->done = true;
}

Task<void> InterleavedPhase(TaskScope tasks) {
    // Each case holds a second real SQLite writer while production flush takes
    // its snapshot and suspends. Mutations happen after the flush task yielded,
    // not after an arbitrary sleep; both commits and reopen are checked.
    for (int scenario = 0; scenario < 7; ++scenario) {
        const Fixture fixture;
        persistence::Persistence store;
        Check(co_await store.open(fixture.path, fixture.files), "interleaved open");
        SeedProfiles(store);
        Check(co_await store.flushSettings() && co_await store.flushProfiles(), "seed interleaved rows");
        auto opened = co_await sqlite::Database::OpenAsync(File{fixture.path.string()}, db_schema::openOptions());
        Check(static_cast<bool>(opened), "interleaved observer open");
        sqlite::Database raw = std::move(*opened);
        const auto gate = std::make_shared<Gate>();
        tasks.Launch([&raw, gate]() -> Task<void> { co_await HoldWriter(raw, gate); });
        co_await WaitFor([gate] { return gate->entered.load(); });
        auto profile = store.listProfiles()[0];
        const bool settings = scenario < 2;
        if (settings) store.setSetting("test.value", "snapshot");
        else if (scenario == 4 || scenario == 6) store.deleteProfile(scenario == 6 ? 43 : 41, true);
        else { profile.name = "snapshot"; store.saveProfile(profile); }
        const auto flush = std::make_shared<FlushResult>();
        tasks.Launch([&store, settings, flush]() -> Task<void> { co_await FlushWhileLocked(store, settings, flush); });
        co_await WaitFor([flush] { return flush->started; });
        Check(!flush->done, "production flush must be suspended behind held SQLite writer");
        if (settings) {
            store.setSetting("test.value", "newest");
            if (scenario == 1) store.setSetting("test.value", "snapshot"); // ABA, still a new version
        } else if (scenario == 6) {
            db::Profile pending;
            pending.id = 44; pending.name = "newest"; pending.type = "local"; pending.file = "unique.yaml";
            store.saveProfile(pending); // no persisted reference exists yet
        } else if (scenario == 3) store.deleteProfile(41, true);
        else { profile.name = "newest"; store.saveProfile(profile); }
        const auto secondFlush = std::make_shared<FlushResult>();
        if (scenario == 5) {
            tasks.Launch([&store, secondFlush]() -> Task<void> { co_await FlushWhileLocked(store, false, secondFlush); });
            co_await WaitFor([secondFlush] { return secondFlush->started; });
        }
        gate->release = true;
        co_await WaitFor([gate, flush, secondFlush, scenario] {
            return gate->writerDone.load() && flush->done && (scenario != 5 || secondFlush->done);
        });
        Check(gate->writerOk && flush->ok && (scenario != 5 || secondFlush->ok), "interleaved writes must commit");
        if (scenario != 5)
            Check(settings ? store.hasPendingSettings() : store.hasPendingProfiles(), "old acknowledgement must keep newer mutation dirty");
        Check(ReadFile(fixture.files / "shared.yaml") == "remote and local stable content\n" &&
                  ReadFile(fixture.files / "unique.yaml") == "other remote stable content\n", "interleaving must retain every referenced file");
        Check(co_await store.flushSettings() && co_await store.flushProfiles(), "flush latest mutation");
        co_await store.close();
        Check(static_cast<bool>(co_await raw.CloseAsync()), "close interleaved observer");
        persistence::Persistence reopened;
        Check(co_await reopened.open(fixture.path, fixture.files), "interleaved reopen");
        const auto rows = reopened.listProfiles();
        Check(rows.size() == (scenario == 3 ? 2 : 3) && rows.back().id == (scenario == 6 ? 44 : 43),
              "interleaved reopen retains all independent sources/stable IDs");
        if (settings) Check(reopened.setting("test.value") == (scenario == 1 ? "snapshot" : "newest"), "latest setting survives reopen");
        else if (scenario == 6) Check(rows.back().file == "unique.yaml" && rows.back().type == "local",
                                    "not-yet-persisted file owner survives cleanup and reopen");
        else if (scenario != 3) Check(rows.front().id == 41 && rows.front().name == "newest", "latest/restored profile survives reopen");
        else Check(rows.front().id == 42 && rows.front().type == "local", "update then delete removes only the chosen ID");
        co_await reopened.close();
    }
}

Task<void> DeletionPhaseSetup(const Fixture& fixture) {
    persistence::Persistence store;
    Check(co_await store.open(fixture.path, fixture.files), "deletion fixture open");
    SeedProfiles(store);
    Check(co_await store.flushSettings() && co_await store.flushProfiles(), "deletion seed flush");
    auto opened = co_await sqlite::Database::OpenAsync(File{fixture.path.string()}, db_schema::openOptions());
    Check(static_cast<bool>(opened), "deletion observer open");
    sqlite::Database raw = std::move(*opened);
    Check(static_cast<bool>(co_await raw.ExecuteAsync(
        "CREATE TRIGGER refuse_delete BEFORE DELETE ON profiles WHEN OLD.id = 43 BEGIN SELECT RAISE(ABORT, 'test delete refused'); END")), "install deletion failure trigger");
    Check(store.deleteProfile(43, true), "queue explicit delete");
    Check(std::filesystem::exists(fixture.files / "unique.yaml"), "delete must not clean file before flush");
    Check(!co_await store.flushProfiles() && store.hasPendingProfiles(), "failed SQL deletion retains retry marker");
    auto rows = co_await raw.Select(db_schema::profiles()).AllAsync();
    Check(rows && rows->size() == 3 && ReadFile(fixture.files / "unique.yaml") == "other remote stable content\n",
          "failed deletion retains persisted row and referenced file");
    co_await store.close(); // repeated failure must still retain the original file/row
    Check(static_cast<bool>(co_await raw.ExecuteAsync("DROP TRIGGER refuse_delete")), "remove injected delete failure");
    persistence::Persistence reopened;
    Check(co_await reopened.open(fixture.path, fixture.files),
          "failed deletion reopen: " + reopened.lastError());
    Check(reopened.listProfiles().size() == 3,
          "failed deletion is recoverable after successful reopen (rows=" + std::to_string(reopened.listProfiles().size()) + ")");
    Check(reopened.deleteProfile(41, true), "queue shared-file delete");
    Check(co_await reopened.flushProfiles(), "delete one shared-file source");
    Check(ReadFile(fixture.files / "shared.yaml") == "remote and local stable content\n", "remaining canonical/alias reference prevents cleanup");
    Check(reopened.deleteProfile(43, true), "queue independent delete");
    Check(co_await reopened.flushProfiles(), "delete independent source");
    rows = co_await raw.Select(db_schema::profiles()).AllAsync();
    Check(rows && rows->size() == 1 && rows->front().id == 42, "committed deletes must remove exactly IDs 41 and 43");
    Check(!std::filesystem::exists(fixture.files / "unique.yaml") && std::filesystem::exists(fixture.files / "shared.yaml"),
          "only committed unreferenced requested file is cleaned");
    db::Profile pending;
    pending.id = 44; pending.name = "pending shared source"; pending.type = "local"; pending.file = "shared.yaml";
    reopened.saveProfile(pending);
    Check(reopened.deleteProfile(42, true), "queue delete while another source owns shared file");
    Check(co_await reopened.flushProfiles(), "delete while another source owns shared file");
    Check(std::filesystem::exists(fixture.files / "shared.yaml"), "pending cached reference prevents cleanup");
    co_await reopened.close();
    Check(static_cast<bool>(co_await raw.CloseAsync()), "close deletion observer");
}

Task<void> DeletionPhaseUnsafeCleanup(const Fixture& fixture) {
    persistence::Persistence finalStore;
    Check(co_await finalStore.open(fixture.path, fixture.files), "final delete reopen");
    const auto finalRows = finalStore.listProfiles();
    Check(finalRows.size() == 1 && finalRows[0].id == 44 && finalRows[0].file == "shared.yaml", "delete/reopen retains remaining source identity/reference");
    Check(finalStore.deleteProfile(44, true), "queue last explicit shared deletion");
    Check(co_await finalStore.flushProfiles(), "last explicit shared deletion");
    Check(!std::filesystem::exists(fixture.files / "shared.yaml"), "last committed reference permits cleanup");
    const auto outside = fixture.files.parent_path() / (fixture.files.filename().string() + "-outside.yaml");
    std::ofstream(outside, std::ios::binary) << "protected outside file\n";
    struct OutsideFile { std::filesystem::path path; ~OutsideFile() { std::error_code ec; std::filesystem::remove(path, ec); } } outsideCleanup{outside};
    db::Profile escaped;
    escaped.id = 45; escaped.file = "../" + outside.filename().string();
    finalStore.saveProfile(escaped);
    Check(co_await finalStore.flushProfiles(), "persist outside reference fixture");
    Check(finalStore.deleteProfile(45, true), "queue unsafe path deletion");
    Check(co_await finalStore.flushProfiles(), "delete metadata with unsafe path");
    Check(ReadFile(outside) == "protected outside file\n", "explicit deletion never cleans outside configured profile root");
    std::error_code linkError;
    std::filesystem::create_symlink(outside, fixture.files / "escape.yaml", linkError);
    if (!linkError) {
        escaped.id = 46; escaped.file = "escape.yaml";
        finalStore.saveProfile(escaped);
        Check(co_await finalStore.flushProfiles(), "persist escaped symlink fixture");
        Check(finalStore.deleteProfile(46, true), "queue escaped symlink deletion");
        Check(co_await finalStore.flushProfiles(), "delete escaped symlink metadata");
        Check(ReadFile(outside) == "protected outside file\n" && std::filesystem::is_symlink(fixture.files / "escape.yaml"),
              "unsafe symlink cleanup is conservatively skipped");
    }
    co_await finalStore.close();
}

Task<void> DeletionPhaseRetry(const Fixture& fixture) {
    persistence::Persistence finalStore;
    Check(co_await finalStore.open(fixture.path, fixture.files), "retry phase reopen");
    const auto outside = fixture.files.parent_path() / (fixture.files.filename().string() + "-outside.yaml");
    std::ofstream(outside, std::ios::binary) << "protected outside file\n";
    struct OutsideFile { std::filesystem::path path; ~OutsideFile() { std::error_code ec; std::filesystem::remove(path, ec); } } outsideCleanup{outside};
    auto opened = co_await sqlite::Database::OpenAsync(File{fixture.path.string()}, db_schema::openOptions());
    Check(static_cast<bool>(opened), "retry phase observer open");
    sqlite::Database raw = std::move(*opened);
    // A rejected upsert must retain the pending version and other source files.
    Check(static_cast<bool>(co_await raw.ExecuteAsync(
        "CREATE TRIGGER refuse_insert BEFORE INSERT ON profiles WHEN NEW.id = 47 BEGIN SELECT RAISE(ABORT, 'test insert refused'); END")), "install upsert failure trigger");
    db::Profile retry;
    std::ofstream(fixture.files / "retry.yaml", std::ios::binary) << "protected retry source\n";
    retry.id = 47; retry.name = "retry source"; retry.type = "local"; retry.file = "retry.yaml";
    finalStore.saveProfile(retry);
    Check(!co_await finalStore.flushProfiles() && finalStore.hasPendingProfiles(), "failed upsert keeps pending version");
    Check(ReadFile(outside) == "protected outside file\n", "failed upsert never alters source files");
    Check(ReadFile(fixture.files / "retry.yaml") == "protected retry source\n", "failed upsert retains its referenced file");
    Check(static_cast<bool>(co_await raw.ExecuteAsync("DROP TRIGGER refuse_insert")), "remove injected insert failure");
    Check(co_await finalStore.flushProfiles() && !finalStore.hasPendingProfiles(), "retry persists acknowledged version");
    std::ofstream(fixture.files / "retained-import.yaml", std::ios::binary) << "retained failed import\n";
    auto abandoned = retry;
    abandoned.id = 48; abandoned.file = "retained-import.yaml";
    finalStore.saveProfile(abandoned);
    Check(co_await finalStore.flushProfiles(), "persist import cleanup fixture");
    Check(finalStore.deleteProfile(48), "queue non-explicit import row cleanup");
    Check(co_await finalStore.flushProfiles(), "commit non-explicit row cleanup");
    Check(ReadFile(fixture.files / "retained-import.yaml") == "retained failed import\n",
          "non-explicit row cleanup never deletes a completed source file");
    Check(static_cast<bool>(co_await raw.CloseAsync()), "close deletion observer");
    co_await finalStore.close();
    persistence::Persistence retryReopen;
    Check(co_await retryReopen.open(fixture.path, fixture.files), "retry source reopen");
    Check(retryReopen.listProfiles().size() == 1 && retryReopen.listProfiles()[0].id == 47 &&
              retryReopen.listProfiles()[0].name == "retry source", "failed/retried upsert survives reopen with stable identity");
    Check(ReadFile(fixture.files / "retry.yaml") == "protected retry source\n", "successful retry/reopen preserves source bytes");
    co_await retryReopen.close();
}

Task<void> DeletionPhase() {
    const Fixture fixture;
    co_await DeletionPhaseSetup(fixture);
    co_await DeletionPhaseUnsafeCleanup(fixture);
    co_await DeletionPhaseRetry(fixture);
}

Task<void> RunTest(TaskScope tasks) {
    const std::filesystem::path path = TempPath();
    const auto missingTheme = persistence::readStartupTheme(path);
    Check(missingTheme.mode == 1 && missingTheme.error.empty() && !std::filesystem::exists(path),
          "startup theme read must not create a missing database");
    const auto corruptThemePath = path.string() + ".corrupt";
    { std::ofstream corrupt(corruptThemePath, std::ios::binary); corrupt << "not a SQLite database"; }
    const auto corruptTheme = persistence::readStartupTheme(corruptThemePath);
    Check(!corruptTheme.error.empty(), "corrupt startup database must report failure");
    { std::ifstream original(corruptThemePath, std::ios::binary);
      const std::string bytes((std::istreambuf_iterator<char>(original)), {});
      Check(bytes == "not a SQLite database", "startup query must preserve a corrupt database"); }
    std::filesystem::remove(corruptThemePath);
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
            const auto startupTheme = persistence::readStartupTheme(path);
            Check(startupTheme.mode == 2 && startupTheme.error.empty(),
                  "synchronous startup theme must see the saved light mode before hydrate");
            Check(store.ready() && !store.hasPendingSettings(),
                  "read-only startup query must not alter the persistence session");

            for (const std::string mode : {"0", "1", "2"}) {
                store.setSetting("ui.theme_mode", mode);
                Check(co_await store.flushSettings(), "theme flush failed");
                const auto savedTheme = persistence::readStartupTheme(path);
                Check(savedTheme.error.empty() && savedTheme.mode == std::stoi(mode),
                      "startup theme must preserve system, dark and light modes");
            }
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

        Trace(8, "旧结构 0→1 迁移路径");
        // user_version=0 的老库（SQLiteCpp 时代形状：PK 无 NOT NULL，后期列由
        // ALTER 追加）必须由 0→1 迁移重建表并保留全部数据；应用不删除任何文件。
        {
            const std::filesystem::path legacy = TempPath();
            const std::filesystem::path profileFiles = legacy.string() + ".profiles";
            std::filesystem::create_directories(profileFiles);
            {
                std::ofstream remoteFile(profileFiles / "legacy-remote.yaml", std::ios::binary);
                std::ofstream localFile(profileFiles / "legacy-local.json", std::ios::binary);
                remoteFile << "proxies: []\nrules: [MATCH, DIRECT]\n";
                localFile << "{\"outbounds\": []}\n";
                Check(static_cast<bool>(remoteFile) && static_cast<bool>(localFile),
                      "legacy subscription files must be created for the migration check");
            }
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
                      "ALTER TABLE profiles ADD COLUMN type TEXT NOT NULL DEFAULT 'remote'",
                      "ALTER TABLE profiles ADD COLUMN description TEXT NOT NULL DEFAULT ''",
                      "ALTER TABLE profiles ADD COLUMN timeout_secs INTEGER NOT NULL DEFAULT 60",
                      "ALTER TABLE profiles ADD COLUMN interval_mins INTEGER NOT NULL DEFAULT 0",
                      "ALTER TABLE profiles ADD COLUMN auto_update INTEGER NOT NULL DEFAULT 0",
                      "ALTER TABLE profiles ADD COLUMN use_system_proxy INTEGER NOT NULL DEFAULT 0",
                      "ALTER TABLE profiles ADD COLUMN use_core_proxy INTEGER NOT NULL DEFAULT 0",
                      "ALTER TABLE profiles ADD COLUMN allow_invalid_cert INTEGER NOT NULL DEFAULT 0",
                      "ALTER TABLE profiles ADD COLUMN homepage TEXT NOT NULL DEFAULT ''",
                      "ALTER TABLE profiles ADD COLUMN used_bytes INTEGER NOT NULL DEFAULT 0",
                      "ALTER TABLE profiles ADD COLUMN total_bytes INTEGER NOT NULL DEFAULT 0",
                      "ALTER TABLE profiles ADD COLUMN native_config TEXT NOT NULL DEFAULT ''",
                      "ALTER TABLE profiles ADD COLUMN native_routes TEXT NOT NULL DEFAULT ''",
                      "INSERT INTO profiles (id, name, url, file, selected, updated_at, error, type, description, "
                      "timeout_secs, interval_mins, auto_update, use_system_proxy, use_core_proxy, allow_invalid_cert, "
                      "homepage, used_bytes, total_bytes, native_config, native_routes) VALUES "
                      "(41, '旧远程订阅', 'https://example.com/legacy.yaml', 'legacy-remote.yaml', 1, 1700000001, '', 'remote', "
                      "'保留的远程订阅', 75, 180, 1, 0, 1, 1, 'https://example.com', 1234, 9876, '', ''), "
                      "(42, '旧本地配置', '', 'legacy-local.json', 0, 1700000002, '上次刷新失败', 'local', "
                      "'保留的原生 JSON', 90, 0, 0, 1, 0, 0, '', 0, 0, '{\"outbounds\":[]}', '[{\"rule\":1}]')",
                      "INSERT INTO settings (key, value) "
                      "VALUES ('ui.theme_mode', '2')"}) {
                    auto result = co_await raw.ExecuteAsync(statement);
                    Check(static_cast<bool>(result), "legacy: ddl failed");
                }
                auto closed = co_await raw.CloseAsync();
                Check(static_cast<bool>(closed), "legacy: close failed");
            }
            const auto legacyTheme = persistence::readStartupTheme(legacy);
            Check(legacyTheme.mode == 2 && legacyTheme.error.empty(),
                  "startup theme must read the legacy database before migration");
            persistence::Persistence store;
            Check(co_await store.open(legacy),
                  "legacy database must open through the 0->1 migration: " +
                      store.lastError());
            Check(store.ready(), "legacy store must be ready after migration");
            Check(store.setting("ui.theme_mode", "") == "2",
                  "legacy setting lost by migration");
            const auto migrated = store.listProfiles();
            Check(migrated.size() == 2 && migrated[0].id == 41 && migrated[1].id == 42,
                  "migration must retain every subscription row and its stable id");
            if (migrated.size() == 2) {
                const auto& remote = migrated[0];
                const auto& local = migrated[1];
                Check(remote.name == "旧远程订阅" && remote.url == "https://example.com/legacy.yaml" &&
                          remote.file == "legacy-remote.yaml" && remote.selected && remote.updatedAt == 1700000001 &&
                          remote.type == "remote" && remote.description == "保留的远程订阅" &&
                          remote.timeoutSecs == 75 && remote.intervalMins == 180 && remote.autoUpdate &&
                          remote.useCoreProxy && remote.allowInvalidCert && remote.homepage == "https://example.com" &&
                          remote.usedBytes == 1234 && remote.totalBytes == 9876,
                      "remote subscription settings and usage data must survive migration");
                Check(local.name == "旧本地配置" && local.file == "legacy-local.json" && !local.selected &&
                          local.updatedAt == 1700000002 && local.error == "上次刷新失败" && local.type == "local" &&
                          local.description == "保留的原生 JSON" && local.timeoutSecs == 90 &&
                          local.useSystemProxy && local.nativeConfig == "{\"outbounds\":[]}" &&
                          local.nativeRoutes == "[{\"rule\":1}]",
                      "local subscription source, failure state and native configuration must survive migration");
            }
            Check(std::filesystem::exists(profileFiles / "legacy-remote.yaml") &&
                      std::filesystem::exists(profileFiles / "legacy-local.json"),
                  "migration must never delete the referenced subscription files");
            // 迁移后的库必须可写。
            store.setSetting("ui.theme_mode", "1");
            Check(co_await store.flushSettings(), "legacy: flush failed");
            co_await store.close();
            std::error_code error;
            Check(std::filesystem::exists(legacy, error),
                  "legacy database must not be deleted");
            persistence::Persistence reopened;
            Check(co_await reopened.open(legacy),
                  "legacy reopen after migration failed");
            Check(reopened.setting("ui.theme_mode", "") == "1",
                  "write after migration did not persist");
            const auto migratedAgain = reopened.listProfiles();
            Check(migratedAgain.size() == 2 && migratedAgain[0].id == 41 &&
                      migratedAgain[0].name == "旧远程订阅" && migratedAgain[1].id == 42 &&
                      migratedAgain[1].name == "旧本地配置" &&
                      std::filesystem::exists(profileFiles / "legacy-remote.yaml") &&
                      std::filesystem::exists(profileFiles / "legacy-local.json"),
                  "all migrated subscription rows and files must survive close/reopen");
            co_await reopened.close();
            RemoveDatabase(legacy);
        }

        Trace(9, "打开失败拒绝写入与多来源重开");
        co_await FailedOpenPhase();
        Trace(10, "真实 SQLite 锁下异步交错与版本确认");
        co_await InterleavedPhase(tasks);
        Trace(11, "删除失败/共享引用/确认后文件清理");
        co_await DeletionPhase();
        Trace(12, "完成");
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
        tasks.Launch([tasks]() -> Task<void> { co_await RunTest(tasks); });
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
