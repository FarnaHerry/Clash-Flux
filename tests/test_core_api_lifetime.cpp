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
import clashflux.persistence;
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
Task<void> CheckLifetime() {
    try {
        auto& data = clashflux::persistence::persistence();
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
                std::ofstream(cfg::profilesDir() / profile.file) << "rules: ['MATCH,DIRECT']\n";
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
