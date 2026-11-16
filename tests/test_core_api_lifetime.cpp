#include <huxerui/huxerui.h>
#include <huxerui/testing/ui_test.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>

import clashflux.config;
import clashflux.model;
import clashflux.core;
import clashflux.singbox;
import clashflux.data;
import clashflux.store.core;
import clashflux.store.profiles;
import clashflux.routing;
import clashflux.vpn;
#include "../src/ui/profiles_model.h"
#include "../src/ui/profile_rules_snapshot.h"

using namespace huxerui;
using namespace std::chrono_literals;
namespace {
std::string endpoint, failure;
std::filesystem::path root;
bool finished = false;
TaskScope applicationTasks;
std::shared_ptr<clashflux::ui::ProfilesModel> ruleViews;
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
Task<void> AwaitRulePrewarm() {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (ruleViews->rulesPrewarm.running || !ruleViews->rulesPrewarm.pending.empty()) {
        Require(std::chrono::steady_clock::now() < deadline, "rule prewarm did not finish");
        co_await Delay(1ms);
    }
}
Task<void> AwaitRuleRead(std::uint64_t expected) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (ruleViews->rules->readsStarted < expected) {
        Require(std::chrono::steady_clock::now() < deadline, "prewarm did not start reading");
        co_await Delay(1ms);
    }
}
Task<void> CheckRulePrewarm() {
    namespace ui = clashflux::ui;
    auto& data = clashflux::data::session();
    Require(co_await data.open(cfg::databaseFile(), cfg::profilesDir()), "prewarm open");
    auto& core = store::coreStore(); core.init(); core.ensureSecret();
    const auto initialCore = core.snapshot();
    const auto initialSettings = data.settingsRevision();
    const auto write = [](const std::string& file, const std::string& domain) {
        std::ofstream output(cfg::profilesDir()/file);
        output << "rules: ['DOMAIN," << domain << ",DIRECT','MATCH,DIRECT']\n";
        Require(bool(output), "cannot write rule prewarm fixture");
    };
    model::Profile first; first.id=1; first.name="seed"; first.type="local"; first.file="1.seed.yaml"; first.selected=true;
    model::Profile second=first; second.id=2; second.name="other"; second.file="2.seed.yaml"; second.selected=false;
    write(first.file, "seed.test"); write(second.file, "other.test");
    Require(co_await data.commitProfile(first, 0), "seed first prewarm source");
    Require(co_await data.commitProfile(second, 0), "seed second prewarm source");
    ruleViews->list = data.listProfiles(); ruleViews->loaded = true;
    ruleViews->ruleSources = std::vector<std::int64_t>{1,2}; ruleViews->ruleSourcesReady = true;
    ui::StartProfileRulesPrewarm(applicationTasks, ruleViews);
    co_await AwaitRuleRead(1);
    Require(ruleViews->rules->readsStarted == 1 && !ruleViews->rules->entries.contains(2),
        "startup must warm one eligible source, not every saved subscription");
    ui::RequestPersonalRules(applicationTasks, ruleViews, 1, false, "read failed");
    co_await AwaitRulePrewarm();
    const auto warmed = ruleViews->rules->SnapshotOf(1);
    Require(warmed && warmed->data->target && warmed->data->error.empty() && ruleViews->rules->readsStarted == 1 &&
        !ruleViews->rules->entries.contains(2), "opening during prewarm must share its single real source read");
    ui::RequestPersonalRules(applicationTasks, ruleViews, 1, false, "read failed");
    Require(ruleViews->rules->SnapshotOf(1) == warmed && ruleViews->rules->readsStarted == 1,
        "first page request after prewarm must reuse its exact ready snapshot without IO or conversion");
    ui::RequestPersonalRules(applicationTasks, ruleViews, 1, true, "read failed");
    const auto editorDeadline = std::chrono::steady_clock::now() + 3s;
    while (!ruleViews->rules->EditorReady(1)) {
        Require(std::chrono::steady_clock::now() < editorDeadline, "target editor did not finish");
        co_await Delay(1ms);
    }
    const auto editor = ruleViews->rules->SnapshotOf(1);
    Require(editor && editor->editor && editor->data == warmed->data && ruleViews->rules->readsStarted == 1 &&
        editor->editor->rules.size() == warmed->data->effective.rules.size() &&
        editor->editor->rules.front().editable && editor->editor->rules.front().pattern == "seed.test",
        "production editor reuses the prewarmed source and effective rows while adding edit permissions");

    model::Profile imported=first; imported.id=3; imported.name="imported"; imported.file="3.import.yaml"; imported.selected=false;
    write(imported.file, "imported.test");
    Require(co_await data.commitProfile(imported, 0), "commit imported source");
    const auto preparing = *ruleViews->BeginPreparation(3, imported.file);
    ui::StartProfileRulesPrewarm(applicationTasks, ruleViews);
    ui::RequestPersonalRules(applicationTasks, ruleViews, 3, false, "read failed");
    Require(ruleViews->rules->readsStarted == 1 && ruleViews->rulesPrewarm.pending.size() == 1,
        "committed imports wait for resource preparation instead of converting its incomplete dependencies");
    ruleViews->FinishPreparation(3, preparing.first, true, {});
    ui::StartProfileRulesPrewarm(applicationTasks, ruleViews); co_await AwaitRulePrewarm();
    Require(ruleViews->rules->Ready(3) && ruleViews->rules->readsStarted == 2,
        "ready imported source prepares its target before a page opens");
    const auto original = ruleViews->rules->SnapshotOf(3);
    const auto refresh = *ruleViews->BeginRefresh(3);
    imported.file="3.middle.yaml"; write(imported.file, "middle.test");
    Require(co_await data.commitProfile(imported, data.profileRevision(3)), "commit intermediate refresh");
    ui::StartProfileRulesPrewarm(applicationTasks, ruleViews);
    imported.file="3.latest.yaml"; write(imported.file, "latest.test");
    Require(co_await data.commitProfile(imported, data.profileRevision(3)), "commit latest refresh");
    ui::StartProfileRulesPrewarm(applicationTasks, ruleViews);
    ui::RequestPersonalRules(applicationTasks, ruleViews, 3, false, "read failed");
    Require(ruleViews->rules->readsStarted == 2 && ruleViews->rulesPrewarm.pending.size() == 1 &&
        ruleViews->rules->SnapshotOf(3) == original && !ruleViews->rules->Ready(3),
        "busy refreshes retain the accepted view and coalesce repeated versions to one pending source");
    ruleViews->FinishRefresh(3, refresh);
    ui::StartProfileRulesPrewarm(applicationTasks, ruleViews); co_await AwaitRulePrewarm();
    const auto latest = ruleViews->rules->SnapshotOf(3);
    Require(latest->data->content().find("latest.test") != std::string::npos && ruleViews->rules->readsStarted == 3 &&
        original->data->content().find("imported.test") != std::string::npos, "refresh prewarm reads only the latest accepted file and retains old caller data");
    // Same body/file and even the same updatedAt can be a successful 304 whose
    // providers were revalidated. The accepted record version still invalidates.
    Require(co_await data.commitProfile(imported, data.profileRevision(3)), "commit same-file accepted refresh");
    ui::StartProfileRulesPrewarm(applicationTasks, ruleViews); co_await AwaitRulePrewarm();
    const auto same = ruleViews->rules->SnapshotOf(3);
    Require(ruleViews->rules->readsStarted == 4 && same != latest && same->data->target == latest->data->target,
        "same-file accepted revisions regenerate the view but reuse a dependency-free converted baseline");
    data.setSetting("core.mode", "global");
    ui::StartProfileRulesPrewarm(applicationTasks, ruleViews);
    Require(ruleViews->rules->readsStarted == 4, "runtime mode changes do not queue source parsing");
    Require(data.setSelectedProfile(2), "selection fixture");
    ui::StartProfileRulesPrewarm(applicationTasks, ruleViews);
    Require(ruleViews->rules->readsStarted == 4 && ruleViews->rules->SnapshotOf(3) == same,
        "the production all-row selection revision does not invalidate unrelated target artifacts");

    // An idle over-budget artifact must be evicted once, never continually
    // requeued just because no page currently pins it.
    ruleViews->rules->budgetBytes = 2048;
    imported.id=4; imported.file="4.budget.yaml"; write(imported.file, "budget.test");
    Require(co_await data.commitProfile(imported, 0), "commit budget fixture");
    ui::StartProfileRulesPrewarm(applicationTasks, ruleViews); co_await AwaitRulePrewarm();
    Require(!ruleViews->rules->entries.contains(4) && ruleViews->rules->readsStarted == 5 &&
        ruleViews->rules->RetainedBytes() <= 2048, "the exact idle budget still applies after the speculative size check");
    ui::StartProfileRulesPrewarm(applicationTasks, ruleViews);
    Require(ruleViews->rules->readsStarted == 5 && ruleViews->rulesPrewarm.pending.empty(), "eviction must not cause a background rebuild loop");
    ruleViews->rules->budgetBytes = 32*1024*1024;
    imported.id=5; imported.file="5.missing.yaml";
    Require(co_await data.commitProfile(imported, 0), "commit missing-file fixture");
    ui::StartProfileRulesPrewarm(applicationTasks, ruleViews); co_await AwaitRulePrewarm();
    Require(ruleViews->rules->Ready(5) && !ruleViews->rules->SnapshotOf(5)->data->target &&
        !ruleViews->rules->SnapshotOf(5)->data->error.empty(), "missing files cannot become a successful empty target configuration");
    write(imported.file, "recovered.test"); ruleViews->rules->Retry(5); ruleViews->rulesPrewarm.Queue(5);
    ui::StartProfileRulesPrewarm(applicationTasks, ruleViews); co_await AwaitRulePrewarm();
    Require(ruleViews->rules->SnapshotOf(5)->data->target && ruleViews->rules->SnapshotOf(5)->data->error.empty(), "prewarm failures remain explicitly retryable");
    imported.id=6; imported.file="6.empty.yaml"; std::ofstream(cfg::profilesDir()/imported.file).close();
    Require(co_await data.commitProfile(imported, 0), "commit empty-file fixture");
    ui::StartProfileRulesPrewarm(applicationTasks, ruleViews); co_await AwaitRulePrewarm();
    Require(!ruleViews->rules->SnapshotOf(6)->data->target && !ruleViews->rules->SnapshotOf(6)->data->error.empty(), "empty files remain errors with their record intact");

    auto lifecycle = core.lockPlanLifecycle();
    imported.id=7; imported.file="7.a.yaml"; write(imported.file, "aba.test");
    Require(co_await data.commitProfile(imported, 0), "commit ABA source");
    const auto beforeAba = ruleViews->rules->readsStarted;
    ui::StartProfileRulesPrewarm(applicationTasks, ruleViews);
    co_await AwaitRuleRead(beforeAba+1);
    const auto stale = ruleViews->rules->entries.at(7).generation;
    imported.file="7.b.yaml"; write(imported.file, "temporary.test");
    Require(co_await data.commitProfile(imported, data.profileRevision(7)), "commit ABA middle");
    ui::StartProfileRulesPrewarm(applicationTasks, ruleViews);
    imported.file="7.a.yaml";
    Require(co_await data.commitProfile(imported, data.profileRevision(7)), "commit ABA return");
    ui::StartProfileRulesPrewarm(applicationTasks, ruleViews);
    lifecycle.unlock(); co_await AwaitRulePrewarm();
    Require(ruleViews->rules->readsStarted == beforeAba+2 && ruleViews->rules->entries.at(7).published > stale &&
        ruleViews->rules->SnapshotOf(7)->data->content().find("aba.test") != std::string::npos,
        "real worker ABA rejects the first ticket and prepares only the latest accepted input");
    imported.id=8; imported.file="8.deleted.yaml"; write(imported.file, "deleted.test");
    Require(co_await data.commitProfile(imported, 0), "commit queued deletion source");
    ruleViews->BeginPreparation(8, imported.file);
    ui::StartProfileRulesPrewarm(applicationTasks, ruleViews);
    const auto beforeDelete = ruleViews->rules->readsStarted;
    Require(data.deleteProfile(8, false), "remove queued source");
    ui::StartProfileRulesPrewarm(applicationTasks, ruleViews);
    Require(!ruleViews->rulesPrewarm.sources.contains(8) && ruleViews->rulesPrewarm.pending.empty() &&
        ruleViews->rules->readsStarted == beforeDelete, "deleted queued IDs never start a stale conversion");

    ruleViews->rules->budgetBytes = 2048;
    imported.id=9; imported.file="9.deferred.yaml";
    write(imported.file, "deferred-prewarm-source-keeps-every-rule.test");
    Require(co_await data.commitProfile(imported, 0), "commit deferred source");
    ui::StartProfileRulesPrewarm(applicationTasks, ruleViews); co_await AwaitRulePrewarm();
    Require(ruleViews->rules->readsStarted == beforeDelete && !ruleViews->rules->Ready(9),
        "a source above the speculative size limit must go idle without reading");
    Require(co_await data.commitProfile(imported, data.profileRevision(9)), "accept deferred source again");
    ui::StartProfileRulesPrewarm(applicationTasks, ruleViews);
    ruleViews->rules->Pin(9);
    ui::RequestPersonalRules(applicationTasks, ruleViews, 9, false, "read failed");
    Require(ruleViews->rules->readsStarted == beforeDelete+1,
        "page demand starts immediately while the new version's speculative size check is pending");
    const auto demandDeadline = std::chrono::steady_clock::now() + 3s;
    while (!ruleViews->rules->Ready(9)) {
        Require(std::chrono::steady_clock::now() < demandDeadline, "deferred rules did not load on demand");
        co_await Delay(1ms);
    }
    co_await AwaitRulePrewarm();
    const auto deferred = ruleViews->rules->SnapshotOf(9);
    Require(ruleViews->rules->readsStarted == beforeDelete+1 && deferred->data->error.empty() &&
        deferred->data->effective.rules.size() == 2 &&
        deferred->data->effective.rules.front().pattern == "deferred-prewarm-source-keeps-every-rule.test",
        "the first page request must read the deferred source once and load all its rules");
    ruleViews->rules->Unpin(9);
    ui::StartProfileRulesPrewarm(applicationTasks, ruleViews); co_await AwaitRulePrewarm();
    Require(!ruleViews->rules->entries.contains(9) && ruleViews->rules->readsStarted == beforeDelete+1,
        "a deferred snapshot can still be evicted after the page unpins it");
    const auto after = core.snapshot();
    Require(after.state == initialCore.state && after.planRevision == initialCore.planRevision &&
        after.runtimeEpoch == initialCore.runtimeEpoch && std::filesystem::exists(cfg::profilesDir()/"1.seed.yaml") &&
        std::filesystem::exists(cfg::profilesDir()/"3.import.yaml") && data.listProfiles().size() == 8 &&
        data.settingsRevision() == initialSettings+1,
        "prewarm preserves unrelated records, version files, settings and applied runtime");
    co_await data.close();
    std::puts("rules-prewarm: real files/commits, one read on first open, resource wait, coalescing, budgets, failures and ABA passed");
}

Task<void> CheckRuleEditor() {
    auto& data = clashflux::data::session();
    Require(co_await data.open(cfg::databaseFile(), cfg::profilesDir()), "rule editor database open");
    auto& core = store::coreStore();
    core.init();
    core.ensureSecret();
    const std::string body = "rules: ['DOMAIN,anchor.test,DIRECT','DOMAIN,retained.test,DIRECT','MATCH,DIRECT']\n";
    model::Profile profile;
    profile.id = 1; profile.name = "rule editor"; profile.type = "local"; profile.file = "1.rules.yaml";
    std::ofstream(cfg::profilesDir() / profile.file, std::ios::binary) << body;
    Require(co_await data.commitProfile(profile, 0), "rule editor source commit");
    auto other = profile;
    other.id = 2; other.name = "independent"; other.file = "2.rules.yaml";
    std::ofstream(cfg::profilesDir() / other.file, std::ios::binary) << body;
    Require(co_await data.commitProfile(other, 0), "rule editor independent source commit");
    co_await RunWorker([&] {
        auto& profiles = store::profilesStore();
        const auto captured = core.profileTargetSource(profile.id, body, profile.name, 0);
        Require(bool(captured.targetSource), "rule editor target capture");
        const auto runtimeBefore = core.snapshot();
        const auto record = data.profileRevision(profile.id);
        std::string error;
        singbox::ProfileRuleEdit rule{.id="replacement", .operation="replace",
            .match="domain", .pattern="replacement.test", .target="DIRECT"};
        Require(singbox::BindTargetSourceEdit(*captured.targetSource, "target:1", rule, error), "rule editor bind");
        singbox::ProfileRuleEdits edits;
        edits.rules.push_back(rule);
        Require(!profiles.saveRuleEdits(profile.id, edits, body, profile.file, record - 1, 0, error),
            "stale form revision must fail before writing");
        const auto saved = profiles.saveRuleEdits(profile.id, edits, body, profile.file, record, 0, error);
        Require(saved, error.c_str());
        const auto accepted = profiles.ruleEdits(profile.id);
        Require(accepted.revision == 1 && accepted.rules == edits.rules, "accepted save uses legacy edit with a new revision");
        const auto preview = profiles.previewRuleSource(profile.id);
        Require(preview.error.empty() && preview.targetRulesReused == 1 &&
            preview.json.find("replacement.test") != std::string::npos &&
            preview.json.find("retained.test") != std::string::npos &&
            preview.json.find("anchor.test") == std::string::npos &&
            preview.json.find("\"secret\"") == std::string::npos, "preview composes accepted edits and hides control secrets");
        const auto settings = data.settingsRevision();
        edits = accepted;
        edits.rules.front().match = "domain_regex";
        edits.rules.front().pattern = "[";
        Require(!profiles.saveRuleEdits(profile.id, edits, body, profile.file, record, 0, error) && !error.empty(),
            "invalid kernel regex must fail the full candidate check");
        Require(data.settingsRevision() == settings && profiles.ruleEdits(profile.id) == accepted,
            "failed kernel check must not publish or dirty proposed edits");
        Require(!profiles.saveYaml(profile.id, "rules: ['DOMAIN,changed.test,DIRECT','MATCH,DIRECT']"),
            "source refresh must reject an incompatible persisted anchor");
        singbox::ProfileRuleEdits secondary;
        secondary.rules.push_back({.id = "secondary", .operation = "disable", .anchor = "clash:DOMAIN,retained.test,DIRECT"});
        const auto savedSecondary = profiles.saveRuleEdits(other.id, secondary, body, other.file,
            data.profileRevision(other.id), 0, error);
        Require(savedSecondary, error.c_str());
        Require(data.setSelectedProfile(profile.id), "select main for the auxiliary refresh fixture");
        vpn::VpnPolicy policy;
        policy.rules.push_back({.match = vpn::MatchKind::ExactDomain, .pattern = "secondary.test",
            .connectionId = routing::ConnectionId(other.id), .id = "secondary-route"});
        data.setSetting("vpn.global_policy", routing::EncodePolicy(policy));
        Require(core.sourceParticipates(other.id), "fixture source must be used as a secondary");
        Require(!profiles.saveYaml(other.id, "rules: ['DOMAIN,anchor.test,DIRECT','MATCH,DIRECT']") &&
            profiles.lastError().find("冲突") != std::string::npos,
            "secondary refresh validates saved anchors even when its internal rules do not run");
        const auto revisionBeforeRefresh = data.profileRevision(other.id);
        Require(!profiles.saveYaml(other.id, "rules: ['MATCH,DIRECT']\nrules: ['MATCH,DIRECT']") &&
            data.profileRevision(other.id) == revisionBeforeRefresh,
            "local source update still rejects duplicate document keys before committing");
        Require(profiles.yamlOf(profile.id) == body && profiles.yamlOf(other.id) == body &&
            core.snapshot().state == runtimeBefore.state && core.snapshot().planRevision == runtimeBefore.planRevision,
            "failed candidates and preview leave files, independent profiles and running-plan state intact");
    });
    Require(co_await data.flushSettings(), "flush accepted rule edits");
    Require(co_await data.flushProfiles(), "flush selected main");
    co_await data.close();
    Require(co_await data.open(cfg::databaseFile(), cfg::profilesDir()), "reopen rule editor database");
    const auto persisted = store::profilesStore().ruleEdits(profile.id);
    Require(persisted.revision == 1 && persisted.rules.size() == 1 &&
        persisted.rules.front().anchor == "clash:DOMAIN,anchor.test,DIRECT" &&
        data.listProfiles().size() == 2, "rule editor reopen preserves the legacy anchor and all source rows");
    co_await data.close();
    std::puts("rule-editor: real candidate check, no failed writes, preview reuse, refresh rejection and reopen passed");
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
    {
        // Exercise the exact source-view API used by the production rules page.
        const std::string targetInput = "rules: ['DOMAIN,target-view.test,DIRECT','MATCH,DIRECT']\n";
        const auto before = core.snapshot();
        const auto settings = data.settingsRevision();
        const auto original = Read(source);
        const auto baseline = co_await RunWorker([&] { return core.profileTargetSource(101, targetInput, "target view", 0); });
        Require(baseline.targetSource && !baseline.targetSourceReused && baseline.error.empty() && baseline.json.empty(), "source-view first capture");
        const auto again = co_await RunWorker([&] { return core.profileTargetSource(101, targetInput, "target view", 0); });
        Require(again.targetSource == baseline.targetSource && again.targetSourceReused, "source-view remount must reuse the exact artifact");
        Require(data.settingsRevision() == settings, "source-view capture must not write settings");
        const auto previousMode = data.setting("core.mode", "rule");
        data.setSetting("core.mode", "direct");
        const auto changedMode = co_await RunWorker([&] { return core.profileTargetSource(101, targetInput, "target view", 0); });
        Require(changedMode.targetSource == baseline.targetSource && changedMode.targetSourceReused, "runtime settings must not rebuild the source-view baseline");
        data.setSetting("core.mode", previousMode);
        const auto afterSettings = data.settingsRevision();
        const auto resource = co_await RunWorker([&] { return core.profileTargetSource(101, targetInput, "target view", 1); });
        Require(resource.targetSource && !resource.targetSourceReused && resource.targetSource != baseline.targetSource, "source-view resource revision must invalidate");
        const auto bad = co_await RunWorker([&] { return core.profileTargetSource(101, "rules: [unterminated", "target view", 0); });
        Require(!bad.targetSource && !bad.error.empty(), "source-view failure must not publish an empty artifact");
        const auto recovered = co_await RunWorker([&] { return core.profileTargetSource(101, targetInput, "target view", 0); });
        Require(recovered.targetSource == baseline.targetSource && recovered.targetSourceReused, "source-view failure must preserve earlier valid artifact");
        const auto after = core.snapshot();
        Require(after.planRevision == before.planRevision && after.runtimeEpoch == before.runtimeEpoch &&
            after.state == before.state && Read(source) == original && data.settingsRevision() == afterSettings,
            "source-view reads must not mutate subscriptions, settings, active runtime or applied tracing");
    }
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
    data.setSetting("core.mode", "rule");
    Require((co_await RunWorker([&] { return core.prepareProfileResources(1); })).ready, "return to prior context");
    Require(Reuses() == reuses + 3, "alternating contexts retain separately keyed accepted compilations");
    data.setSetting("core.mode", "global");
    Require((co_await RunWorker([&] { return core.prepareProfileResources(1); })).ready, "return to second context");
    Require(Reuses() == reuses + 4, "bounded cache retains both recent runtime contexts");
    const auto beforeByteChange = Reuses();
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
    Require(Reuses() == beforeByteChange, "same timestamp and size cannot hide changed dependency bytes");
    {
        std::ofstream heldSource(source, std::ios::binary);
        Require(static_cast<bool>(heldSource), "cannot open held source");
        heldSource << Read(root / "held-source.yaml");
    }
    store::ProfilePreparationResult held;
    std::jthread preparing([&] { held = core.prepareProfileResources(1); });
    // Give slower Windows runners room to start the held request; the
    // cancellation assertion is still the hard deadline.
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (!std::filesystem::exists(root / "provider-held")) {
        Require(std::chrono::steady_clock::now() < deadline, "held provider did not start");
        co_await Delay(5ms);
    }
    model::Profile independent;
    independent.id = 2; independent.name = "independent"; independent.type = "local"; independent.file = "2.yaml";
    data.saveProfile(independent);
    std::ofstream(cfg::profilesDir() / independent.file) << "rules: ['MATCH,DIRECT']\n";
    const auto independentBegan = std::chrono::steady_clock::now();
    const auto independentResult = co_await RunWorker([&] { return core.prepareProfileResources(2); });
    Require(independentResult.ready && std::chrono::steady_clock::now() - independentBegan < 3s,
            "another source's held download must not hold the global preparation cache lock");
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
        if (endpoint == "--rules-prewarm") {
            co_await CheckRulePrewarm(); finished = true; co_return;
        }
        if (endpoint == "--rules-editor") {
            co_await CheckRuleEditor(); finished = true; co_return;
        }
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
    applicationTasks = UseApplicationTaskScope();
    const auto tasks = UseTaskScope();
    Lifecycle([tasks] { tasks.Launch(CheckLifetime); return [] {}; });
    return Spacer();
}
}
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    endpoint = argv[1]; root = argv[2];
    testing::UiTest ui{Application{TestApp, {.application_hooks={[](ApplicationContext&) {
        ruleViews = std::make_shared<clashflux::ui::ProfilesModel>();
    }}}}};
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
