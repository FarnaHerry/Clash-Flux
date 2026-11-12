#include <huxerui/huxerui.h>
#include <huxerui/testing/ui_test.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>

import clashflux.config;
import clashflux.model;
import clashflux.core;
import clashflux.data;
import clashflux.store.core;

using namespace huxerui;
using namespace std::chrono_literals;
namespace {
std::string endpoint, failure;
std::filesystem::path root;
bool finished = false;
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
std::string Read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}
std::size_t Reuses() {
    const auto log = Read(cfg::coreWorkDir() / "logs.log");
    const std::string token = "profile-compilation reused=true";
    std::size_t count = 0, at = 0;
    while ((at = log.find(token, at)) != std::string::npos) { ++count; at += token.size(); }
    return count;
}
Task<void> CheckPreparation() {
    auto& data = clashflux::data::session();
    Require(co_await data.open(cfg::databaseFile(), cfg::profilesDir()), "preparation open");
    model::Profile profile;
    profile.id = 1; profile.name = "prepared"; profile.type = "local"; profile.file = "1.yaml";
    data.saveProfile(profile);
    const auto source = cfg::profilesDir() / profile.file;
    std::filesystem::copy_file(root / "source.yaml", source);
    auto& core = store::coreStore(); core.init(); core.ensureSecret();
    const auto first = co_await RunWorker([&] { return core.prepareProfileResources(1); });
    Require(first.ready, first.error.c_str());
    const auto reuses = Reuses();
    Require(co_await RunWorker([&] {
        std::string error; return core.applyMainProfile(1, Read(source), error);
    }), "prepared source activation");
    Require(Reuses() == reuses + 1, "activation must reuse background compilation/check instead of compiling it again");
    // A changed runtime intent must invalidate an otherwise identical source.
    data.setSetting("core.mode", "global");
    Require((co_await RunWorker([&] { return core.prepareProfileResources(1); })).ready, "changed settings prepare");
    Require(Reuses() == reuses + 1, "changed mode must not reuse the old candidate");
    // Reusing the new context is allowed; touching raw cache bytes must invalidate it.
    Require((co_await RunWorker([&] { return core.prepareProfileResources(1); })).ready, "same context prepare");
    Require(Reuses() == reuses + 2, "identical context must reuse");
    for (const auto& entry : std::filesystem::directory_iterator(cfg::coreWorkDir() / "rule-providers")) {
        if (entry.path().extension() != ".cache") continue;
        const auto stamp = std::filesystem::last_write_time(entry.path());
        auto bytes = Read(entry.path());
        const auto at = bytes.find("cache.test");
        if (at != std::string::npos) bytes.replace(at, 10, "other.test");
        std::ofstream(entry.path(), std::ios::binary) << bytes;
        std::filesystem::last_write_time(entry.path(), stamp);
    }
    Require((co_await RunWorker([&] { return core.prepareProfileResources(1); })).ready, "changed cache prepare");
    Require(Reuses() == reuses + 2, "same timestamp and size cannot hide changed dependency bytes");
    std::ofstream(source) << Read(root / "held-source.yaml");
    store::ProfilePreparationResult held;
    std::jthread preparing([&] { held = core.prepareProfileResources(1); });
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!std::filesystem::exists(root / "provider-held")) {
        Require(std::chrono::steady_clock::now() < deadline, "held provider did not start");
        co_await Delay(5ms);
    }
    std::stop_source cancel;
    std::jthread cancelling([&] { std::this_thread::sleep_for(30ms); cancel.request_stop(); });
    const auto began = std::chrono::steady_clock::now();
    const auto cancelled = co_await RunWorker([&] { return core.prepareProfileResources(1, cancel.get_token()); });
    Require(!cancelled.ready && std::chrono::steady_clock::now() - began < 1s,
            "queued preparation cancellation must not wait for another source's network IO");
    std::ofstream(root / "release-provider").put('1');
    co_await RunWorker([&] { preparing.join(); cancelling.join(); });
    Require(held.ready, held.error.c_str());
    Require(core.snapshot().state != ::core::CoreState::Running, "preparation/activation while stopped must never start a core");
    co_await data.close();
}

Task<void> CheckLifetime() {
    try {
        if (endpoint == "--prepare-resources") {
            co_await CheckPreparation(); finished = true; co_return;
        }
        auto& data = clashflux::data::session();
        if (endpoint == "--seed") {
            Require(co_await data.open(cfg::databaseFile(), cfg::profilesDir()), "seed open failed");
            data.setSetting("core.secret", "existing-secret");
            for (int id : {1, 2}) {
                model::Profile profile;
                profile.id = id;
                profile.name = "preserved-" + std::to_string(id);
                profile.type = "local";
                profile.file = std::to_string(id) + ".yaml";
                profile.selected = id == 1;
                data.saveProfile(profile);
                std::ofstream(cfg::profilesDir() / profile.file, std::ios::binary)
                    << "rules: ['MATCH,DIRECT']\n";
            }
            co_await data.close();
            std::ofstream(root / "seeded").put('1');
            finished = true;
            co_return;
        }
        auto& core = store::coreStore();
        Require(!data.ready(), "fixture must start before hydration");
        auto& early = core.api();
        early.setEndpoint(endpoint, "early-request");
        std::atomic<bool> requestOk{false};
        std::jthread request([&] { requestOk = early.proxies().ok; });
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!std::filesystem::exists(root / "request-started")) {
            Require(std::chrono::steady_clock::now() < deadline, "early request did not start");
            co_await Delay(5ms);
        }
        Require(co_await data.open(cfg::databaseFile(), cfg::profilesDir()), "hydrate failed");
        core.init();
        core.ensureSecret();
        const bool sameClient = &early == &core.api();
        std::ofstream(root / "release-request").put('1');
        request.join();
        Require(sameClient, "hydration destroyed the client retained by an in-flight request");
        Require(requestOk.load(), "early request failed across hydration");
        Require(core.api().baseUrl() == cfg::controllerBaseUrl(), "hydrated endpoint missing");
        const auto secret = core.secret();
        Require(!secret.empty() && data.setting("core.secret") == secret, "secret was not hydrated/generated");
        if (std::filesystem::exists(root / "seeded"))
            Require(secret == "existing-secret", "existing secret was replaced during hydration");
        // Repeated credential changes must also retain the client and its mutex.
        for (int i = 0; i < 32; ++i) {
            data.setSetting("core.secret", "rotated-" + std::to_string(i));
            core.ensureSecret();
            Require(&early == &core.api(), "secret refresh replaced the retained client");
        }
        early.setEndpoint(endpoint, core.secret());
        const auto refreshed = co_await RunWorker([&early] { return early.proxies(); });
        Require(refreshed.ok, "retained client unusable after credential refresh");
        Require(co_await data.flushSettings(), "secret flush failed");
        co_await data.close();
        Require(co_await data.open(cfg::databaseFile(), cfg::profilesDir()), "reopen failed");
        Require(data.setting("core.secret") == "rotated-31", "persisted secret lost on reopen");
        co_await data.close();
    } catch (const std::exception& error) {
        failure = error.what();
    }
    finished = true;
}
View TestApp() {
    const auto tasks = UseTaskScope();
    Lifecycle([tasks] { tasks.Launch(CheckLifetime); return [] {}; });
    return Spacer();
}
}
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    endpoint = argv[1]; root = argv[2];
    testing::UiTest ui{Application{TestApp}};
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (!finished && std::chrono::steady_clock::now() < deadline) {
        ui.Pump(5ms);
        std::this_thread::sleep_for(1ms);
    }
    if (!finished || !failure.empty()) {
        std::fprintf(stderr, "core_api_lifetime: %s\n", finished ? failure.c_str() : "timeout");
        return 1;
    }
    std::puts("core_api_lifetime: retained request, credential refresh and reopen passed");
}
