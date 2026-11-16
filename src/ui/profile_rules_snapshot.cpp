// Shared application rule preparation; independent of mounted pages.
#include <huxerui/huxerui.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <fstream>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>
#include "task_bridge.h"

import clashflux.model;
import clashflux.config;
import clashflux.singbox;
import clashflux.data;
import clashflux.store.core;
import clashflux.stream;
import clashflux.routing;

#include "profiles_model.h"
#include "profile_rules_snapshot.h"

namespace clashflux::ui {
std::size_t ProfileRowsBytes(const singbox::ProfileRules& value) {
    auto bytes = sizeof(value) + value.error.capacity();
    for (const auto* rows : {&value.rules, &value.original}) {
        bytes += rows->capacity()*sizeof(singbox::ProfileRule);
        for (const auto& row : *rows) bytes += row.anchor.capacity() + row.raw.capacity() + row.type.capacity() +
            row.pattern.capacity() + row.target.capacity() + row.editId.capacity() + row.origin.capacity() + row.position.capacity();
    }
    return bytes;
}
std::size_t ProfileDataBytes(const ProfileRulesData& data) {
    auto bytes = sizeof(data) + data.content.capacity() + data.error.capacity() + ProfileRowsBytes(data.effective);
    if (data.target) bytes += data.target->retainedBytes();
    bytes += data.edits.rules.capacity()*sizeof(singbox::ProfileRuleEdit);
    for (const auto& edit : data.edits.rules) bytes += edit.id.capacity() + edit.operation.capacity() + edit.anchor.capacity() +
        edit.match.capacity() + edit.pattern.capacity() + edit.target.capacity() + edit.unavailable.capacity();
    bytes += data.library.resources.capacity()*sizeof(singbox::PersonalRuleSet);
    for (const auto& resource : data.library.resources) bytes += resource.id.capacity() + resource.name.capacity() +
        resource.type.capacity() + resource.format.capacity() + resource.url.capacity() + resource.content.capacity();
    return bytes;
}

void ProfilesModel::SyncRuleInputs(std::int64_t requestedId) {
    auto& data = clashflux::data::session();
    if (!data.ready()) return;
    const auto profilesRevision = data.profilesRevision();
    const auto settingsRevision = data.settingsRevision();
    const auto resourcesRevision = externalResourcesRevision.Get();
    if ((!requestedId || rules->entries.contains(requestedId)) &&
        rulesInputsReady && rulesProfilesRevision == profilesRevision &&
        rulesSettingsRevision == settingsRevision && rulesResourcesRevision == resourcesRevision) return;
    std::vector<std::int64_t> ids;
    for (const auto& [id, entry] : rules->entries) ids.push_back(id);
    if (requestedId && std::ranges::find(ids, requestedId) == ids.end()) ids.push_back(requestedId);
    const bool readProfiles = !rulesInputsReady || rulesProfilesRevision != profilesRevision ||
        (requestedId && !rules->entries.contains(requestedId));
    const auto profiles = readProfiles ? data.listProfiles() : std::vector<model::Profile>{};
    if (readProfiles) rulesPrewarm.Observe(profiles, [&](auto id) { return data.profileRevision(id); }, profilesRevision);
    const auto library = ids.empty() ? std::string{} : data.setting("rules.resources");
    for (const auto id : ids) {
        ProfileRulesInput input;
        if (readProfiles) {
            const auto profile = std::ranges::find(profiles, id, &model::Profile::id);
            if (profile == profiles.end() || profile->type == "pptp" || profile->file.empty()) {
                rules->Forget(id); continue;
            }
            input.file = profile->file; input.type = profile->type; input.name = profile->name;
        } else {
            const auto& previous = rules->entries.at(id).input;
            input.file = previous.file; input.type = previous.type; input.name = previous.name;
        }
        input.edits = data.setting("profile.rules." + std::to_string(id));
        input.library = library;
        input.resources = resourcesRevision;
        input.source = rulesPrewarm.Generation(id);
        if (const auto prior = rules->entries.find(id); prior != rules->entries.end() && prior->second.input != input)
            rulesPrewarm.Queue(id);
        rules->SetInput(id, std::move(input));
    }
    rulesProfilesRevision = profilesRevision;
    rulesSettingsRevision = settingsRevision;
    rulesResourcesRevision = resourcesRevision;
    rulesInputsReady = true;
    rules->Trim();
}

ProfileRulesSnapshot LoadPersonalRules(std::int64_t id, const ProfileRulesInput& input) {
    ProfileRulesSnapshot result{.id = id};
    auto data = std::make_shared<ProfileRulesData>();
    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now();
    auto acquired = started;
    {
        // Wait for acceptance/rollback, then release the lifecycle lock before
        // target conversion. Versioned content and captured edits stay immutable.
        auto lock = store::coreStore().lockPlanLifecycle();
        acquired = Clock::now();
        std::ifstream source(cfg::profilesDir() / input.file, std::ios::binary);
        if (!source) throw std::runtime_error("无法读取订阅版本文件");
        data->content.assign(std::istreambuf_iterator<char>(source), {});
        if (source.bad() || data->content.empty()) throw std::runtime_error("订阅版本文件为空或读取失败");
        data->edits = routing::DecodeProfileRules(input.edits);
        data->library = routing::DecodeRuleSetLibrary(input.library);
    }
    data->supported = singbox::DetectProfileFormat(data->content) != singbox::ProfileFormat::OpenVpn;
    const auto loaded = Clock::now();
    bool reused = false;
    if (data->supported) {
        auto compiled = store::coreStore().profileTargetSource(id, data->content, input.name, input.resources);
        reused = compiled.targetSourceReused;
        data->target = std::move(compiled.targetSource);
        data->error = std::move(compiled.error);
        if (data->target) {
            data->effective = singbox::ProjectTargetSource(*data->target, data->edits, data->library);
            data->error = data->effective.error;
        }
    }
    data->retained = ProfileDataBytes(*data);
    result.data = data;
    const auto projected = Clock::now();
    const auto micros = [](auto duration) { return std::chrono::duration_cast<std::chrono::microseconds>(duration).count(); };
    stream::logApplication("debug", "rules-load wait_us=" + std::to_string(micros(acquired - started)) +
        " read_us=" + std::to_string(micros(loaded - acquired)) + " target_us=" + std::to_string(micros(projected - loaded)) +
        " target_reused=" + std::to_string(reused) + " bytes=" + std::to_string(result.retainedBytes()) +
        " rows=" + std::to_string(data->effective.rules.size()) + " source_id=" + std::to_string(id) +
        " source_generation=" + std::to_string(input.source));
    return result;
}

std::vector<std::string> LoadPersonalRuleTargets(
    const ProfileRulesSnapshot& source, const ProfileRulesInput& input) {
    std::vector<std::string> targets{"DIRECT", "REJECT", "REJECT-DROP"};
    if (source.data->target) {
        for (const auto& object : source.data->target->objects())
            if (std::ranges::find(targets, object.objectId) == targets.end()) targets.push_back(object.objectId);
        return targets;
    }
    singbox::CompileOptions options;
    options.profileYaml = source.data->content;
    options.mainConnectionId = routing::ConnectionId(source.id);
    options.mainSourceName = input.name;
    auto objects = singbox::DetectProfileFormat(source.data->content) == singbox::ProfileFormat::SingBoxJson
        ? singbox::compileConfig(options) : singbox::inspectProfileSourceObjects(options);
    for (const auto& object : objects.sourceObjects)
        if (std::ranges::find(targets, object.objectId) == targets.end())
            targets.push_back(object.objectId);
    return targets;
}

void RequestPersonalRules(huxerui::TaskScope tasks,
    std::shared_ptr<ProfilesModel> profiles, std::int64_t id, bool editor, std::string readError, std::function<void()> finished) {
    profiles->SyncRuleInputs(id);
    const auto preparing = profiles->Preparation(id);
    if (profiles->Refreshing(id) || (preparing && preparing->phase == ProfilePreparationPhase::Preparing)) {
        profiles->rulesPrewarm.Queue(id);
        if (finished) finished();
        return;
    }
    const auto model = profiles->rules;
    const auto entry = model->entries.find(id);
    if (entry == model->entries.end()) { if (finished) finished(); return; }
    if (model->Ready(id)) {
        if (!editor || entry->second.targets || entry->second.targetsLoading ||
            !entry->second.value->data->supported) { if (finished) finished(); return; }
    } else if (entry->second.loading) return;
    const auto input = entry->second.input;
    RequestProfileRulesSnapshot(tasks, model, id,
        [id, input]() -> huxerui::Task<std::shared_ptr<const ProfileRulesSnapshot>> {
            co_return co_await RunOnTaskThread([id, input] {
                return std::make_shared<const ProfileRulesSnapshot>(LoadPersonalRules(id, input));
            });
        }, [profiles] { profiles->SyncRuleInputs(); },
        [id, readError](std::exception_ptr error) {
            ProfileRulesSnapshot failure{.id = id};
            auto data = std::make_shared<ProfileRulesData>();
            try { std::rethrow_exception(error); }
            catch (const std::exception& exception) { data->error = exception.what(); }
            catch (...) { data->error = readError; }
            data->retained = ProfileDataBytes(*data);
            failure.data = std::move(data);
            return std::make_shared<const ProfileRulesSnapshot>(std::move(failure));
        }, std::move(finished));
    if (!editor) return;
    const auto ticket = model->Begin(id, true);
    if (!ticket) return;
    const auto source = model->SnapshotOf(id);
    try {
        tasks.Launch([=]() -> huxerui::Task<void> {
            std::vector<std::string> targets;
            std::string error;
            std::shared_ptr<const ProfileRulesSnapshot> upgraded;
            try {
                auto prepared = co_await RunOnTaskThread([source, input] {
                    auto next = std::make_shared<ProfileRulesSnapshot>(*source);
                    next->editor = std::make_shared<const singbox::ProfileRules>(singbox::InspectProfileRules(source->data->content, source->data->edits, false));
                    next->editorBytes = ProfileRowsBytes(*next->editor);
                    return std::pair{std::move(next), LoadPersonalRuleTargets(*source, input)};
                });
                upgraded = std::move(prepared.first); targets = std::move(prepared.second);
            }
            catch (const std::exception& exception) { error = exception.what(); }
            catch (...) { error = readError; }
            profiles->SyncRuleInputs();
            model->PublishTargets(id, *ticket, std::move(targets), std::move(error), std::move(upgraded));
        });
    } catch (const std::exception& exception) {
        model->PublishTargets(id, *ticket, {}, exception.what());
    } catch (...) {
        model->PublishTargets(id, *ticket, {}, readError);
    }
}

// One application-owned prewarm read at a time. Busy sources stay queued;
// the existing profile pump or preparation completion retries them, no timer.
void StartProfileRulesPrewarm(huxerui::TaskScope tasks, std::shared_ptr<ProfilesModel> profiles) {
    if (!clashflux::data::session().ready()) return;
    profiles->SyncRuleInputs();
    auto& warm = profiles->rulesPrewarm;
    if (!warm.startup && profiles->ruleSourcesReady.Get()) {
        warm.startup = true;
        warm.Queue(profiles->ResolveRuleSource(0));
    }
    if (warm.running) return;
    const auto count = warm.pending.size();
    for (std::size_t index = 0; index < count; ++index) {
        const auto id = warm.pending.front(); warm.pending.pop_front();
        if (!warm.sources.contains(id)) continue;
        const auto preparing = profiles->Preparation(id);
        if (profiles->Refreshing(id) || (preparing && preparing->phase == ProfilePreparationPhase::Preparing)) {
            warm.Queue(id); continue;
        }
        profiles->SyncRuleInputs(id);
        const auto entry = profiles->rules->entries.find(id);
        if (entry == profiles->rules->entries.end() || profiles->rules->Ready(id)) continue;
        if (entry->second.loading) { warm.Queue(id); continue; }
        warm.running = true;
        stream::logApplication("debug", "rules-prewarm begin source_id=" + std::to_string(id) +
            " queued=" + std::to_string(warm.pending.size()));
        RequestPersonalRules(tasks, profiles, id, false, "Rule snapshot preparation failed", [tasks, profiles, id] {
            profiles->rulesPrewarm.running = false;
            stream::logApplication("debug", "rules-prewarm finish source_id=" + std::to_string(id) +
                " ready=" + std::to_string(profiles->rules->Ready(id)) +
                " reads_started=" + std::to_string(profiles->rules->readsStarted));
            const auto current = profiles->rules->entries.find(id);
            if (current != profiles->rules->entries.end() && !profiles->rules->Ready(id) && !current->second.loading)
                profiles->rulesPrewarm.Queue(id);
            StartProfileRulesPrewarm(tasks, profiles);
        });
        return;
    }
}
} // namespace clashflux::ui
