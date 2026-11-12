// profiles.cppm — clashflux.store.profiles：订阅管理 store（接口即实现，单例）。
//
// 订阅 = 已提交的 model::Profile 行 + 独立版本来源文件。
// activate 把启用订阅交给 coreStore 重启内核生效。线程契约同 core_store：
// 标「阻塞」的方法（网络下载 / 内核重启）UI 必须经 RunOnTaskThread 调用。
module;
#include "subscription_expiry.h"

export module clashflux.store.profiles;

import std;
import clashflux.config;
import clashflux.utils;
import clashflux.api;
import clashflux.model;
import clashflux.data;
import clashflux.core;
import clashflux.store.core;
import clashflux.singbox;
import clashflux.routing;
import clashflux.stream;

namespace store {

// ---- 订阅 YAML 的 rules 文本级操作（编辑规则弹窗用）-------------------------
// 文本级而非完整 YAML 解析：与 generateConfig 的键剔除同一量级，够用且零依赖。

// 解析顶层 rules: 块下的每一条 "- <规则>"（不含前导横杠；注释/空行跳过）。
export std::vector<std::string> parseRules(const std::string& yaml) {
    std::vector<std::string> rules;
    bool inRules = false;
    std::size_t begin = 0;
    while (begin <= yaml.size()) {
        const auto end = yaml.find('\n', begin);
        std::string line = yaml.substr(
            begin, end == std::string::npos ? std::string::npos : end - begin);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && line[0] != ' ' && line[0] != '\t' &&
            line[0] != '#') {
            // 顶层键：切换是否处于 rules 块。
            const auto colon = line.find(':');
            inRules = colon != std::string::npos &&
                      std::string_view(line).substr(0, colon) == "rules";
        } else if (inRules) {
            std::size_t j = 0;
            while (j < line.size() && (line[j] == ' ' || line[j] == '\t')) ++j;
            if (line.compare(j, 2, "- ") == 0) {
                std::string rule = line.substr(j + 2);
                while (!rule.empty() &&
                       (rule.back() == ' ' || rule.back() == '\t')) {
                    rule.pop_back();
                }
                if (!rule.empty()) rules.push_back(std::move(rule));
            }
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return rules;
}

// 在 rules 块的前/后插入一条规则；无 rules 块时新建。规则文本会被清理
//（去首尾空白/行首 "- "/拒绝换行），失败原样返回。
export std::string insertRule(const std::string& yaml, const std::string& rule,
                              bool prepend) {
    std::string clean = rule;
    while (!clean.empty() &&
           (clean.front() == ' ' || clean.front() == '\t')) {
        clean.erase(clean.begin());
    }
    while (!clean.empty() &&
           (clean.back() == ' ' || clean.back() == '\t' ||
            clean.back() == '\r')) {
        clean.pop_back();
    }
    while (clean.starts_with("- ")) clean.erase(0, 2);
    if (clean.empty() || clean.find('\n') != std::string::npos) return yaml;

    std::vector<std::string> lines;
    {
        std::size_t begin = 0;
        while (begin <= yaml.size()) {
            const auto end = yaml.find('\n', begin);
            lines.push_back(yaml.substr(
                begin,
                end == std::string::npos ? std::string::npos : end - begin));
            if (end == std::string::npos) break;
            begin = end + 1;
        }
    }

    // 找顶层 rules: 行。
    std::size_t rulesIdx = lines.size();
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string_view l = lines[i];
        if (!l.empty() && l[0] != ' ' && l[0] != '\t' && l[0] != '#') {
            const auto colon = l.find(':');
            if (colon != std::string::npos && l.substr(0, colon) == "rules") {
                rulesIdx = i;
                break;
            }
        }
    }
    if (rulesIdx == lines.size()) {
        std::string out = yaml;
        if (!out.empty() && out.back() != '\n') out += '\n';
        out += std::format("rules:\n  - {}\n", clean);
        return out;
    }

    // 块内第一条/最后一条 "- " 行与缩进。
    std::size_t first = std::string::npos;
    std::size_t last = std::string::npos;
    std::string indent = "  ";
    for (std::size_t i = rulesIdx + 1; i < lines.size(); ++i) {
        const std::string_view l = lines[i];
        if (!l.empty() && l[0] != ' ' && l[0] != '\t' && l[0] != '#') break;
        std::size_t j = 0;
        while (j < l.size() && (l[j] == ' ' || l[j] == '\t')) ++j;
        if (l.compare(j, 2, "- ") == 0) {
            if (first == std::string::npos) {
                first = i;
                indent = std::string(l.substr(0, j));
            }
            last = i;
        }
    }
    const std::size_t insertAt =
        (prepend || last == std::string::npos)
            ? (first == std::string::npos ? rulesIdx + 1 : first)
            : last + 1;
    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(insertAt),
                 std::format("{}- {}", indent, clean));

    std::string out;
    for (const auto& l : lines) {
        out += l;
        out += '\n';
    }
    return out;
}

// 平台下载器（HuxerUI HttpClient，Android 订阅通道）取回的响应：网络与线程
// 模型留在 UI 层，store 只做校验/落盘/行更新。ok 对齐 ApiResult::ok（2xx）。
export struct FetchedProfile {
    bool ok = false;
    long status = 0;
    std::string error;  // 传输失败信息（ok=false 时有效）
    std::multimap<std::string, std::string> headers;
    std::string body;
};

export struct ProfileRefreshFailure {
    std::int64_t profileId = 0;
    std::string profileName;
    std::string error;
};

export struct ProfileRefreshBatch {
    std::vector<std::int64_t> updated;
    std::vector<ProfileRefreshFailure> failures;
};

export class ProfilesStore {
public:
    ProfilesStore() = default;
    ProfilesStore(const ProfilesStore&) = delete;
    ProfilesStore& operator=(const ProfilesStore&) = delete;

    std::vector<model::Profile> list() {
        try {
            return clashflux::data::session().listProfiles();
        } catch (const std::exception& e) {
            lastError_ = e.what();
            return {};
        }
    }

    bool setOrder(const std::vector<std::int64_t>& orderIds) {
        try {
            clashflux::data::session().setProfileOrder(orderIds);
            lastError_.clear();
            return true;
        } catch (const std::exception& e) {
            lastError_ = e.what();
            return false;
        }
    }

    // 当前启用的订阅；无 = nullptr。
    std::optional<model::Profile> selected() {
        for (const auto& p : list()) {
            if (p.selected && p.type != "pptp") return p;
        }
        return std::nullopt;
    }

    inline std::string lastError() const { return lastError_; }

    // 读取启用订阅的 YAML 原文（无订阅 / 读失败 = 空串）。
    std::string selectedYaml() {
        const auto sel = selected();
        if (!sel || sel->file.empty()) return "";
        std::ifstream in(cfg::profilesDir() / sel->file, std::ios::binary);
        if (!in) return "";
        return std::string(std::istreambuf_iterator<char>(in),
                           std::istreambuf_iterator<char>());
    }

    // 远程订阅的空名字兜底：解析 URL 主机名作为可读名字（去掉 www. 前缀、
    // 凭据、端口与路径），解析不出主机时回落「远程订阅」标签。
    std::string RemoteFallbackName(const std::string& url) {
        std::string host = url;
        const auto scheme = host.find("://");
        if (scheme != std::string::npos) host = host.substr(scheme + 3);
        const auto path = host.find_first_of("/?#");
        if (path != std::string::npos) host = host.substr(0, path);
        const auto userinfo = host.rfind('@');
        if (userinfo != std::string::npos) host = host.substr(userinfo + 1);
        const auto port = host.rfind(':');
        if (port != std::string::npos) host = host.substr(0, port);
        if (host.empty()) return "远程订阅";
        if (host.starts_with("www.")) host = host.substr(4);
        return host;
    }

    // Remote imports reserve an identity; no accepted row exists before validation/COMMIT.
    std::int64_t importUrl(const std::string& name, const std::string& url,
                           const model::Profile& options = {}) {
        const auto id = createRemote(name, url, options);
        if (!id) return 0;
        auto p = findById(id);
        if (!p || !download(*p)) { discardDraft(id); return 0; }
        return id;
    }

    // Android keeps the draft across the platform HTTP await, outside the accepted list.
    std::int64_t createRemote(const std::string& name, const std::string& url,
                              const model::Profile& options = {}) {
        lastError_.clear();
        try {
            model::Profile p = options;
            p.id = clashflux::data::session().reserveProfileId();
            p.selected = false; p.file.clear(); p.type = "remote";
            p.name = name.empty() ? RemoteFallbackName(url) : name;
            p.url = url;
            std::lock_guard lock(draftsMutex_);
            drafts_.emplace(p.id, p);
            return p.id;
        } catch (const std::exception& e) { lastError_ = e.what(); return 0; }
    }

    bool completeRemote(std::int64_t id, const FetchedProfile& fetched,
                        bool importing, bool forceProviders = false,
                        std::uint64_t expectedRevision = 0) {
        auto planLock = coreStore().lockPlanLifecycle();
        lastError_.clear();
        if (clashflux::data::session().profileRevision(id) != expectedRevision) {
            lastError_ = "订阅在下载期间已变化，请重新更新"; return false;
        }
        auto previous = findById(id);
        if (!previous) { lastError_ = "订阅不存在"; return false; }
        if (!fetched.ok || fetched.status < 200 || fetched.status >= 300) {
            lastError_ = fetched.error.empty() ? std::format("HTTP {}", fetched.status) : fetched.error;
            if (importing) discardDraft(id);
            return false;
        }
        auto next = *previous;
        applyHeaders(next, fetched.headers);
        const bool ok = saveContentVersion(next, fetched.body, importing ? std::nullopt : previous,
                                          expectedRevision, forceProviders, !importing);
        if (importing) discardDraft(id);
        return ok;
    }

    // 重新拉取订阅（阻塞，用户手动入口）。手动更新保留同步强刷 provider 语义。
    bool refresh(std::int64_t id) {
        // download re-reads this row under the lifecycle lock before fetching
        // and again before committing. Network I/O must not block other rules.
        std::optional<model::Profile> profile;
        {
            auto planLock = coreStore().lockPlanLifecycle();
            lastError_.clear();
            profile = findById(id);
            if (!profile) {
                if (lastError_.empty()) lastError_ = "订阅不存在";
                return false;
            }
        }
        return download(*profile, true);
    }

    // Local import uses the same version-file commit path, without a provisional row.
    std::int64_t importFile(const std::string& name, const std::filesystem::path& source,
                    const model::Profile& options = {}) {
        std::ifstream input(source, std::ios::binary);
        if (!input) { lastError_ = "无法读取订阅文件"; return false; }
        const std::string content(std::istreambuf_iterator<char>(input), {});
        if (input.bad()) { lastError_ = "读取订阅文件失败"; return false; }
        return importContent(name.empty() ? source.filename().string() : name, content, options);
    }

    std::int64_t importContent(const std::string& name, const std::string& content,
                               const model::Profile& options = {}) {
        auto planLock = coreStore().lockPlanLifecycle();
        lastError_.clear();
        try {
            auto p = options;
            p.id = clashflux::data::session().reserveProfileId();
            p.selected = false; p.type = "local"; p.url.clear(); p.file.clear();
            p.name = name.empty() ? "手动配置" : name;
            // An inactive local import may be inspected/edited before activation.
            if (!saveContentVersion(p, content, std::nullopt, 0, false, false)) return 0;
            return p.id;
        } catch (const std::exception& e) { lastError_ = e.what(); return 0; }
    }

    // 新增 PPTP 原生连接订阅：不下载 YAML，连接参数
    // 和路由由 nativeConfig/nativeRoutes 保存，具体引擎由 VPN store 解释。
    std::int64_t importNative(const std::string& name, const std::string& type,
                              const std::string& nativeConfig,
                              const std::string& nativeRoutes,
                              const model::Profile& options = {}) {
        lastError_.clear();
        if (type != "pptp") {
            lastError_ = "原生连接只支持 PPTP；OpenVPN 请通过普通文件或远程订阅导入";
            return 0;
        }
        model::Profile p = options;
        p.id = 0;
        p.selected = false;
        p.type = type;
        p.url.clear();
        p.file.clear();
        p.name = name.empty() ? type : name;
        p.nativeConfig = nativeConfig;
        p.nativeRoutes = nativeRoutes;
        try {
            p.id = clashflux::data::session().reserveProfileId();
            if (!clashflux::data::session().commitProfileOnWorker(p, 0)) {
                lastError_ = clashflux::data::session().lastError(); return 0;
            }
        } catch (const std::exception& e) {
            lastError_ = e.what();
            return 0;
        }
        return p.id;
    }

    // 保存订阅信息与选项（订阅弹窗唯一保存入口）：fields 携带 name/url 与
    // type/描述/超时/间隔/自动更新/代理/证书开关；行身份字段（file/selected/
    // updatedAt/error）保留库中现值。仅落库——URL 等在下次「更新」拉取时生效。
    bool updateProfile(std::int64_t id, const model::Profile& fields) {
        lastError_.clear();
        auto planLock = coreStore().lockPlanLifecycle();
        const auto revision = clashflux::data::session().profileRevision(id);
        auto p = findById(id);
        if (!p) {
            lastError_ = "订阅不存在";
            return false;
        }
        if (!fields.type.empty() && fields.type != p->type && coreStore().sourceParticipates(id)) {
            lastError_ = "被编排引用的来源不能变更类型；请先解除引用"; return false;
        }
        p->name = fields.name.empty() ? p->name : fields.name;
        p->url = fields.url;
        p->type = fields.type.empty() ? p->type : fields.type;
        p->description = fields.description;
        p->timeoutSecs = fields.timeoutSecs;
        p->intervalMins = fields.intervalMins;
        p->autoUpdate = fields.autoUpdate;
        p->useSystemProxy = fields.useSystemProxy;
        p->useCoreProxy = fields.useCoreProxy;
        p->allowInvalidCert = fields.allowInvalidCert;
        if (!fields.nativeConfig.empty() || fields.type == "pptp") {
            p->nativeConfig = fields.nativeConfig;
            p->nativeRoutes = fields.nativeRoutes;
        }
        try {
            if (!clashflux::data::session().commitProfileOnWorker(*p, revision)) {
                lastError_ = clashflux::data::session().lastError(); return false;
            }
        } catch (const std::exception& e) {
            lastError_ = e.what();
            return false;
        }
        return true;
    }

    // 到期待更新的订阅清单（无网络，纯查询）：允许自动更新 + 间隔已到；
    // 内核代理通道要求内核在跑，不在跑跳过本轮（下一轮再试）。Android 泵
    // 用它拿清单后经 HuxerUI HttpClient 逐个抓取（completeRemote 收尾）。
    std::vector<model::Profile> dueForUpdate() {
        const std::int64_t now = nowUnix();
        std::vector<model::Profile> due;
        for (const auto& p : list()) {
            if (!p.autoUpdate || p.intervalMins <= 0) continue;
            if (p.url.empty()) continue;  // local 类型无 URL
            if (now - p.updatedAt < static_cast<std::int64_t>(p.intervalMins) * 60)
                continue;
            if (p.useCoreProxy &&
                coreStore().snapshot().state != core::CoreState::Running) {
                continue;
            }
            due.push_back(p);
        }
        return due;
    }

    // 自动更新（桌面 curl 通道）：把 dueForUpdate() 命中的订阅逐个拉新。
    // 全程阻塞（网络），由壳层泵经 RunOnTaskThread 周期调用，返回本轮结果。
    ProfileRefreshBatch refreshDue() {
        ProfileRefreshBatch batch;
        for (const auto& due : dueForUpdate()) {
            auto p = due;
            if (download(p, false)) {
                batch.updated.push_back(p.id);
            } else {
                batch.failures.push_back(ProfileRefreshFailure{
                    .profileId = p.id,
                    .profileName = p.name,
                    .error = lastError_.empty() ? "下载失败" : lastError_});
            }
        }
        return batch;
    }

    // 读取指定订阅的 YAML 原文（不存在 / 读失败 = 空串）。
    // 直接接收已查询出的 Profile，避免 UI 列表线程在同一轮刷新里再次
    // 查询 SQLite；Android 的 WAL/Activity 重建期间二次查询可能返回空，
    // 导致规则页误显示“没有订阅规则”，而内核实际仍在使用该文件。
    std::string yamlOf(const model::Profile& profile) {
        if (profile.file.empty()) return "";
        std::ifstream in(cfg::profilesDir() / profile.file, std::ios::binary);
        if (!in) return "";
        return std::string(std::istreambuf_iterator<char>(in),
                           std::istreambuf_iterator<char>());
    }

    std::string yamlOf(std::int64_t id) {
        const auto p = findById(id);
        return p ? yamlOf(*p) : std::string{};
    }

    singbox::ProfileRuleEdits ruleEdits(std::int64_t id) {
        return routing::DecodeProfileRules(clashflux::data::session().setting(routing::ProfileRulesKey(id)));
    }

    bool saveRuleEdits(std::int64_t id, singbox::ProfileRuleEdits edits,
                       const std::string& expectedContent, std::string& error) {
        auto planLock = coreStore().lockPlanLifecycle();
        error.clear();
        try {
            auto& data = clashflux::data::session();
            if (!data.ready()) { error = "订阅尚未加载完成"; return false; }
            const auto profile = findById(id);
            if (!profile || profile->type == "pptp") { error = "订阅不存在或不支持个人规则"; return false; }
            const auto content = yamlOf(*profile);
            const auto previous = ruleEdits(id);
            if (content != expectedContent || previous.revision != edits.revision) {
                error = "订阅或个人规则已变化，请重新打开后编辑"; return false;
            }
            if (edits.revision == std::numeric_limits<std::uint64_t>::max()) { error = "规则修订号超出范围"; return false; }
            ++edits.revision;
            if (!singbox::ValidateProfileRuleEdits(edits, error)) return false;
            const auto plan = singbox::InspectProfileRules(content, edits);
            if (!plan.error.empty()) { error = plan.error; return false; }
            const auto key = routing::ProfileRulesKey(id);
            const auto previousText = data.setting(key);
            data.setSetting(key, routing::EncodeProfileRules(edits));
            bool ok = false;
            try {
                const bool runningMain = profile->selected && coreStore().snapshot().state == core::CoreState::Running;
                ok = runningMain ? coreStore().restartCore(content)
                                 : coreStore().validateProfileRuleSource(id, content, error);
                if (!ok && error.empty()) error = coreStore().snapshot().lastError;
            } catch (const std::exception& exception) { error = exception.what(); }
            if (!ok) {
                data.setSetting(key, previousText);
                if (error.empty()) error = "订阅个人规则未能应用";
                return false;
            }
            coreStore().invalidateProxyGroupsSnapshot();
            // Publish a content revision even when only this profile's settings changed.
            data.saveProfile(*profile);
            return true;
        } catch (const std::exception& exception) { error = exception.what(); return false; }
    }

    // 保存订阅 YAML 原文（高级文件编辑）：写文件 + 更新 updatedAt；
    // 若该订阅启用中且内核在跑，重启内核使改动生效。阻塞。
    // forceProviders 仅限用户手动更新：只强刷该来源自己的 provider。
    bool saveYaml(std::int64_t id, const std::string& content,
                  bool forceProviders = false) {
        lastError_.clear();
        auto planLock = coreStore().lockPlanLifecycle();
        const auto revision = clashflux::data::session().profileRevision(id);
        const auto profile = findById(id);
        if (!profile) { lastError_ = "订阅不存在"; return false; }
        return saveContentVersion(*profile, content, profile, revision, forceProviders);
    }

    // selected 是主来源的兼容持久化字段；候选通过检查后才切换，应用失败还原。
    bool activate(std::int64_t id, std::stop_token cancel = {}, std::string* failure = nullptr,
                  std::string* fidelitySummary = nullptr) {
        auto planLock = coreStore().lockPlanLifecycle(cancel);
        if (!planLock.owns_lock()) { if (failure) failure->clear(); return false; }
        std::string error, summary;
        const bool success = [&] {
            if (cancel.stop_requested()) return false;
            const auto profile = findById(id);
            if (!profile) { error = "订阅不存在"; return false; }
            if (profile->type == "pptp") {
                error = "当前原生 VPN 只作为规则来源参与，不能设为 main"; return false;
            }
            // Retargeting the still-active source cancels an unfinished candidate
            // without compiling the original source again.
            if (cancel.stop_possible() && profile->selected) return true;
            const auto content = yamlOf(*profile);
            if (content.empty()) { error = "主来源没有有效内容"; return false; }
            return coreStore().applyMainProfile(id, content, error, cancel, &summary);
        }();
        lastError_ = cancel.stop_requested() ? std::string{} : error;
        if (failure) *failure = lastError_;
        if (fidelitySummary) *fidelitySummary = success ? std::move(summary) : std::string{};
        return success;
    }

    void remove(std::int64_t id) {
        lastError_.clear();
        auto planLock = coreStore().lockPlanLifecycle();
        if (coreStore().sourceDeletionProtected(id)) {
            lastError_ = "活动代理或未确认状态的主订阅及被引用来源不能删除；请先停止内核或切换为待命，再解除其它引用";
            return;
        }
        bool found = false;
        std::optional<model::Profile> selectedProfile;
        for (const auto& p : list()) {
            if (p.id == id) {
                found = true;
                if (p.selected) selectedProfile = p;
            }
        }
        if (!found) { lastError_ = "订阅不存在"; return; }
        const bool clearStandby = selectedProfile && coreStore().standbyActive();
        const std::string previousContent = clearStandby ? yamlOf(*selectedProfile) : std::string{};
        if (clearStandby) {
            auto& persistence = clashflux::data::session();
            persistence.setSelectedProfile(0);
            if (!coreStore().clearStandbyCore()) {
                persistence.setSelectedProfile(id);
                lastError_ = "清空待命内核失败：" + coreStore().snapshot().lastError;
                return;
            }
        }
        try {
            clashflux::data::session().deleteProfile(id, true);
        } catch (const std::exception& e) {
            lastError_ = e.what();
            if (clearStandby) {
                clashflux::data::session().saveProfile(*selectedProfile);
                if (!coreStore().restartCore(previousContent))
                    lastError_ += "；原待命配置恢复失败：" + coreStore().snapshot().lastError;
            }
            return;
        }
        // File cleanup is performed by persistence only after the delete
        // commits and no other persisted or cached source references it.
        if (selectedProfile) coreStore().invalidateProxyGroupsSnapshot();
    }

private:
    void discardDraft(std::int64_t id) {
        std::lock_guard lock(draftsMutex_); drafts_.erase(id);
    }

    bool commitPrepared(model::Profile next, const std::optional<model::Profile>& previous,
                        std::uint64_t expectedRevision) {
        auto& data = clashflux::data::session();
        next.updatedAt = nowUnix(); next.error.clear();
        if (!api::SyncFile(cfg::profilesDir() / next.file, lastError_)) return false;
        try {
            if (!data.commitProfileOnWorker(next, expectedRevision)) {
                lastError_ = data.lastError(); return false;
            }
            coreStore().invalidateProxyGroupsSnapshot();
            // SQLite acceptance and kernel application are separate steps. Keep
            // both source versions so an apply failure can restore the old pointer.
            bool applied = true;
            try {
                if (previous && coreStore().sourceParticipates(next.id) &&
                    coreStore().snapshot().state == core::CoreState::Running) {
                    const auto began = std::chrono::steady_clock::now();
                    applied = coreStore().restartCore(coreStore().currentMainContent());
                    stream::logApplication("info", std::format("profile-update source_id={} ready={} apply_us={}",
                        next.id, applied, std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - began).count()));
                    if (!applied) lastError_ = coreStore().snapshot().lastError;
                }
            } catch (const std::exception& e) { applied = false; lastError_ = e.what(); }
            if (!applied) {
                const auto committed = data.profileRevision(next.id);
                if (!data.commitProfileOnWorker(*previous, committed)) {
                    lastError_ += "；恢复订阅引用失败：" + data.lastError();
                    coreStore().stopCore();
                    return false;
                }
                coreStore().invalidateProxyGroupsSnapshot();
                if (!coreStore().restartCore(coreStore().currentMainContent())) {
                    lastError_ += "；恢复运行配置失败：" + coreStore().snapshot().lastError;
                    coreStore().stopCore();
                }
                return false;
            }
            discardDraft(next.id);
            return true;
        } catch (const std::exception& e) { lastError_ = e.what(); return false; }
    }

    bool saveContentVersion(model::Profile next, const std::string& content,
                            const std::optional<model::Profile>& previous,
                            std::uint64_t expectedRevision, bool forceProviders,
                            bool validate = true) {
        if (!singbox::ValidateProfileDocument(content, lastError_)) return false;
        if (validate && !coreStore().validateSourceUpdate(next.id, content, lastError_, forceProviders))
            return false;
        next.file = std::format("{}.{}.yaml", next.id, cfg::randomSecret());
        std::error_code ec;
        std::filesystem::create_directories(cfg::profilesDir(), ec);
        if (ec) { lastError_ = "创建订阅目录失败：" + ec.message(); return false; }
        if (!writeContent(next, content, lastError_)) return false;
        return commitPrepared(std::move(next), previous, expectedRevision);
    }

    template<class Headers> static void applyHeaders(model::Profile& p, const Headers& headers) {
        p.usedBytes = p.totalBytes = p.expireAt = 0;
        if (const auto userinfo = findHeader(headers, "subscription-userinfo"))
            parseUserInfo(*userinfo, p.usedBytes, p.totalBytes, p.expireAt);
        p.homepage = findHeader(headers, "profile-web-page-url").value_or("");
    }

    static bool writeContent(const model::Profile& profile, const std::string& content, std::string& error) {
        const auto dest = cfg::profilesDir() / profile.file;
        const auto directory = std::filesystem::path(dest.string() + "." + cfg::randomSecret() + ".tmp");
        std::error_code ec;
        if (!std::filesystem::create_directory(directory, ec)) {
            error = "无法创建独占订阅暂存目录：" + ec.message(); return false;
        }
        struct TempDirectory {
            std::filesystem::path path;
            ~TempDirectory() { std::error_code ec; std::filesystem::remove_all(path, ec); }
        } cleanup{directory};
        const auto temp = directory / "payload";
        std::ofstream output(temp, std::ios::binary | std::ios::trunc);
        output.write(content.data(), static_cast<std::streamsize>(content.size())); output.close();
        if (!output) { error = "订阅文件写入失败"; return false; }
        return api::CommitFile(temp, dest, error);
    }

    std::optional<model::Profile> findById(std::int64_t id) {
        if (!clashflux::data::session().ready()) {
            lastError_ = "订阅持久化未就绪，操作已拒绝";
            return std::nullopt;
        }
        for (const auto& p : list()) {
            if (p.id == id) return p;
        }
        std::lock_guard lock(draftsMutex_);
        const auto draft = drafts_.find(id);
        if (draft != drafts_.end()) return draft->second;
        return std::nullopt;
    }

    template <class HeaderMap>
    static std::optional<std::string> findHeader(
        const HeaderMap& headers, std::string_view target) {
        for (const auto& [name, val] : headers) {
            if (name.size() == target.size()) {
                bool match = true;
                for (std::size_t i = 0; i < name.size(); ++i) {
                    if (std::tolower(static_cast<unsigned char>(name[i])) !=
                        std::tolower(static_cast<unsigned char>(target[i]))) {
                        match = false;
                        break;
                    }
                }
                if (match) return val;
            }
        }
        return std::nullopt;
    }

    // 解析 subscription-userinfo：upload=..; download=..; total=..; expire=..
    // （单位字节；expire 为 Unix 秒；显式空/0/-1 表示长期有效，缺失仍未知）。
    static void parseUserInfo(const std::string& value, std::int64_t& used,
                              std::int64_t& total, std::int64_t& expire) {
        std::int64_t upload = 0;
        std::int64_t download = 0;
        std::int64_t totalIn = 0;
        std::int64_t expireIn = 0;
        std::size_t begin = 0;
        const auto isSpace = [](char c) {
            return c == ' ' || c == '\t' || c == '\r' || c == '\n';
        };
        while (begin < value.size()) {
            const auto end = value.find(';', begin);
            const std::string pair = value.substr(
                begin, end == std::string::npos ? std::string::npos : end - begin);
            const auto eq = pair.find('=');
            if (eq != std::string::npos) {
                std::string key = pair.substr(0, eq);
                std::string num = pair.substr(eq + 1);
                while (!key.empty() && isSpace(key.front())) key.erase(key.begin());
                while (!key.empty() && isSpace(key.back())) key.pop_back();
                for (char& c : key) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
                while (!num.empty() && isSpace(num.front())) num.erase(num.begin());
                while (!num.empty() && isSpace(num.back())) num.pop_back();

                std::int64_t parsed = 0;
                const auto [ptr, ec] = std::from_chars(
                    num.data(), num.data() + num.size(), parsed);
                if (key == "expire") {
                    expireIn = clashflux::subscription::ParseExpiryValue(num);
                } else if (ec == std::errc()) {
                    if (key == "upload") upload = parsed;
                    else if (key == "download") download = parsed;
                    else if (key == "total") totalIn = parsed;
                }
            }
            if (end == std::string::npos) break;
            begin = end + 1;
        }
        used = upload + download;
        total = totalIn;
        expire = expireIn;
    }

    // Fetch into a fresh version file. Only a committed row makes it accepted.
    bool download(model::Profile& p, bool forceProviders = false) {
        std::lock_guard downloadLock(downloadMutex_);
        auto planLock = coreStore().lockPlanLifecycle();
        auto& data = clashflux::data::session();
        lastError_.clear();
        const auto revision = data.profileRevision(p.id);
        const auto initial = findById(p.id);
        if (!initial) { lastError_ = "订阅不存在"; return false; }
        if (initial->url.empty()) { lastError_ = "本地导入的订阅不支持更新"; return false; }
        const bool importing = revision == 0;
        p = *initial;
        const auto previousContent = yamlOf(*initial);
        const auto versionFile = std::format("{}.{}.yaml", p.id, cfg::randomSecret());
        api::ClashApi::DownloadOptions opts;
        opts.timeoutSecs = p.timeoutSecs > 0 ? p.timeoutSecs : 60;
        opts.maxBytes = 8 * 1024 * 1024;
        opts.allowInvalidCert = p.allowInvalidCert;
        if (p.useCoreProxy)
            opts.proxyUrl = std::format("http://127.0.0.1:{}", coreStore().snapshot().mixedPort);
        else opts.allowProxyEnv = p.useSystemProxy;
        planLock.unlock();
        opts.validate = [&](const std::filesystem::path& file) {
            std::ifstream input(file, std::ios::binary);
            if (!input) return std::string{"无法读取候选订阅"};
            const std::string content(std::istreambuf_iterator<char>(input), {});
            if (input.bad()) return std::string{"读取候选订阅失败"};
            planLock.lock();
            const auto current = findById(p.id);
            if (data.profileRevision(p.id) != revision || !current || *current != *initial || yamlOf(*current) != previousContent)
                return std::string{"订阅在下载期间已变化，请重新更新"};
            std::string error;
            if (importing) return singbox::ValidateProfileDocument(content, error) ? std::string{} : error;
            return coreStore().validateSourceUpdate(p.id, content, error, forceProviders)
                ? std::string{} : error;
        };
        api::ClashApi downloader{"", ""};
        const auto result = downloader.downloadToFile(initial->url, cfg::profilesDir() / versionFile, opts);
        stream::logApplication("info", std::format(
            "profile-download source_id={} manual={} ready={} network_us={} candidate_us={} file_commit_us={} download_bytes={} connections_known={} new_connections={}",
            p.id, forceProviders, result.ok, result.download.networkMicros, result.download.validationMicros,
            result.download.commitMicros, result.download.bytes, result.download.transportKnown, result.download.newConnections));
        if (!result.ok) { lastError_ = "下载失败：" + result.error; return false; }
        p.file = versionFile;
        applyHeaders(p, result.headers);
        return commitPrepared(p, importing ? std::nullopt : initial, revision);
    }

    std::mutex draftsMutex_;
    std::map<std::int64_t, model::Profile> drafts_;
    std::mutex downloadMutex_;
    std::string lastError_;
};

export ProfilesStore& profilesStore() {
    static ProfilesStore store;
    return store;
}

} // namespace store
