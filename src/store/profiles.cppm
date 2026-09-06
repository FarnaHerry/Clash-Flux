// profiles.cppm — clashflux.store.profiles：订阅管理 store（接口即实现，单例）。
//
// 订阅 = db::Profile 行 + profiles/<id>.yaml 文件。importUrl 下载落盘；
// activate 把启用订阅交给 coreStore 重启内核生效。线程契约同 core_store：
// 标「阻塞」的方法（网络下载 / 内核重启）UI 必须经 RunOnTaskThread 调用。
export module clashflux.store.profiles;

import std;
import clashflux.config;
import clashflux.utils;
import clashflux.api;
import clashflux.db;
import clashflux.core;
import clashflux.store.core;

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

export class ProfilesStore {
public:
    ProfilesStore() = default;
    ProfilesStore(const ProfilesStore&) = delete;
    ProfilesStore& operator=(const ProfilesStore&) = delete;

    std::vector<db::Profile> list() {
        try {
            return coreStore().db().listProfiles();
        } catch (const std::exception& e) {
            lastError_ = e.what();
            return {};
        }
    }

    // 当前启用的订阅；无 = nullptr。
    std::optional<db::Profile> selected() {
        for (const auto& p : list()) {
            if (p.selected) return p;
        }
        return std::nullopt;
    }

    std::string lastError() const { return lastError_; }

    // 读取启用订阅的 YAML 原文（无订阅 / 读失败 = 空串）。
    std::string selectedYaml() {
        const auto sel = selected();
        if (!sel || sel->file.empty()) return "";
        std::ifstream in(cfg::profilesDir() / sel->file, std::ios::binary);
        if (!in) return "";
        return std::string(std::istreambuf_iterator<char>(in),
                           std::istreambuf_iterator<char>());
    }

    // 新增订阅（remote）：先建行拿 id，再下载到 profiles/<id>.yaml。阻塞（网络）。
    // options 携带弹窗里的订阅选项（type 强制 remote）。成功返回新 id；失败返回 0。
    std::int64_t importUrl(const std::string& name, const std::string& url,
                           const db::Profile& options = {}) {
        lastError_.clear();
        db::Profile p = options;
        p.id = 0;
        p.type = "remote";
        p.name = name.empty() ? url : name;
        p.url = url;
        try {
            p.id = coreStore().db().saveProfile(p);
        } catch (const std::exception& e) {
            lastError_ = e.what();
            return 0;
        }
        if (!download(p)) {
            try {
                coreStore().db().deleteProfile(p.id);
            } catch (...) {
            }
            return 0;
        }
        return p.id;
    }

    // 重新拉取订阅（阻塞）。本地导入（无 url）直接报错。
    bool refresh(std::int64_t id) {
        lastError_.clear();
        for (auto& p : list()) {
            if (p.id != id) continue;
            if (p.url.empty()) {
                lastError_ = "本地导入的订阅不支持更新";
                return false;
            }
            return download(p);
        }
        lastError_ = "订阅不存在";
        return false;
    }

    // 本地文件导入（local）：复制内容到 profiles/<id>.yaml。
    bool importFile(const std::string& name, const std::filesystem::path& source,
                    const db::Profile& options = {}) {
        lastError_.clear();
        db::Profile p = options;
        p.id = 0;
        p.type = "local";
        p.url.clear();
        p.name = name.empty() ? source.filename().string() : name;
        try {
            p.id = coreStore().db().saveProfile(p);
            p.file = std::format("{}.yaml", p.id);
            std::filesystem::copy_file(source, cfg::profilesDir() / p.file,
                                       std::filesystem::copy_options::overwrite_existing);
            p.updatedAt = nowUnix();
            coreStore().db().saveProfile(p);
        } catch (const std::exception& e) {
            lastError_ = e.what();
            if (p.id != 0) {
                try { coreStore().db().deleteProfile(p.id); } catch (...) {}
            }
            return false;
        }
        return true;
    }

    // 保存订阅信息与选项（订阅弹窗唯一保存入口）：fields 携带 name/url 与
    // type/描述/超时/间隔/自动更新/代理/证书开关；行身份字段（file/selected/
    // updatedAt/error）保留库中现值。仅落库——URL 等在下次「更新」拉取时生效。
    bool updateProfile(std::int64_t id, const db::Profile& fields) {
        lastError_.clear();
        auto p = findById(id);
        if (!p) {
            lastError_ = "订阅不存在";
            return false;
        }
        p->name = fields.name.empty()
                      ? (fields.url.empty() ? p->name : fields.url)
                      : fields.name;
        p->url = fields.url;
        p->type = fields.type.empty() ? p->type : fields.type;
        p->description = fields.description;
        p->timeoutSecs = fields.timeoutSecs;
        p->intervalMins = fields.intervalMins;
        p->autoUpdate = fields.autoUpdate;
        p->useSystemProxy = fields.useSystemProxy;
        p->useCoreProxy = fields.useCoreProxy;
        p->allowInvalidCert = fields.allowInvalidCert;
        try {
            coreStore().db().saveProfile(*p);
        } catch (const std::exception& e) {
            lastError_ = e.what();
            return false;
        }
        return true;
    }

    // 自动更新：把「允许自动更新 + 间隔已到」的 remote 订阅逐个拉新。
    // 全程阻塞（网络），由壳层泵经 RunOnTaskThread 周期调用。返回刷新条数。
    int refreshDue() {
        const std::int64_t now = nowUnix();
        int updated = 0;
        for (const auto& p : list()) {
            if (!p.autoUpdate || p.intervalMins <= 0) continue;
            if (p.url.empty()) continue;  // local 类型无 URL
            if (now - p.updatedAt < static_cast<std::int64_t>(p.intervalMins) * 60)
                continue;
            // 内核代理通道要求内核在跑；不在跑跳过本轮（下一轮再试）。
            if (p.useCoreProxy &&
                coreStore().snapshot().state != core::CoreState::Running) {
                continue;
            }
            if (refresh(p.id)) ++updated;
        }
        return updated;
    }

    // 读取指定订阅的 YAML 原文（不存在 / 读失败 = 空串）。
    std::string yamlOf(std::int64_t id) {
        const auto p = findById(id);
        if (!p || p->file.empty()) return "";
        std::ifstream in(cfg::profilesDir() / p->file, std::ios::binary);
        if (!in) return "";
        return std::string(std::istreambuf_iterator<char>(in),
                           std::istreambuf_iterator<char>());
    }

    // 保存订阅 YAML 原文（编辑规则/内容）：写文件 + 更新 updatedAt；
    // 若该订阅启用中且内核在跑，重启内核使改动生效。阻塞。
    bool saveYaml(std::int64_t id, const std::string& content) {
        lastError_.clear();
        const auto p = findById(id);
        if (!p || p->file.empty()) {
            lastError_ = "订阅不存在";
            return false;
        }
        try {
            std::ofstream out(cfg::profilesDir() / p->file,
                              std::ios::binary | std::ios::trunc);
            if (!out) {
                lastError_ = "无法写入订阅文件";
                return false;
            }
            out << content;
            out.flush();
            if (!out) {
                lastError_ = "写入订阅文件失败";
                return false;
            }
            db::Profile row = *p;
            row.updatedAt = nowUnix();
            row.error.clear();
            coreStore().db().saveProfile(row);
        } catch (const std::exception& e) {
            lastError_ = e.what();
            return false;
        }
        if (p->selected && coreStore().snapshot().state == core::CoreState::Running) {
            coreStore().stopCore();
            coreStore().startCore(selectedYaml());
        }
        return true;
    }

    // 启用订阅：标记 selected，若内核在跑则重启内核使配置生效。阻塞。
    bool activate(std::int64_t id) {        lastError_.clear();
        try {
            coreStore().db().setSelectedProfile(id);
        } catch (const std::exception& e) {
            lastError_ = e.what();
            return false;
        }
        if (coreStore().snapshot().state == core::CoreState::Running) {
            coreStore().stopCore();
            coreStore().startCore(selectedYaml());
        }
        return true;
    }

    void remove(std::int64_t id) {
        lastError_.clear();
        std::string file;
        bool wasSelected = false;
        for (const auto& p : list()) {
            if (p.id == id) {
                file = p.file;
                wasSelected = p.selected;
            }
        }
        try {
            coreStore().db().deleteProfile(id);
        } catch (const std::exception& e) {
            lastError_ = e.what();
            return;
        }
        if (!file.empty()) {
            std::error_code ec;
            std::filesystem::remove(cfg::profilesDir() / file, ec);
        }
        // 删掉的是启用订阅且内核在跑：用空配置重启（代理全断比跑旧配置直观）。
        if (wasSelected && coreStore().snapshot().state == core::CoreState::Running) {
            coreStore().stopCore();
            coreStore().startCore("");
        }
    }

private:
    std::optional<db::Profile> findById(std::int64_t id) {
        for (const auto& p : list()) {
            if (p.id == id) return p;
        }
        return std::nullopt;
    }

    // 解析 subscription-userinfo：upload=..; download=..; total=..; expire=..
    // （单位字节；键值两侧空白容忍；未知键忽略）。
    void parseUserInfo(const std::string& value, std::int64_t& used,
                       std::int64_t& total) {
        std::int64_t upload = 0;
        std::int64_t download = 0;
        std::int64_t totalIn = 0;
        std::int64_t begin = 0;
        while (begin <= value.size()) {
            const auto end = value.find(';', begin);
            const std::string pair = value.substr(
                begin, end == std::string::npos ? std::string::npos : end - begin);
            const auto eq = pair.find('=');
            if (eq != std::string::npos) {
                const std::string key = pair.substr(0, eq);
                std::string num = pair.substr(eq + 1);
                while (!num.empty() && (num.front() == ' ' || num.front() == '\t')) {
                    num.erase(num.begin());
                }
                std::int64_t parsed = 0;
                const auto [ptr, ec] = std::from_chars(
                    num.data(), num.data() + num.size(), parsed);
                if (ec == std::errc()) {
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
    }

    // 下载 p.url 到 profiles/<id>.yaml 并更新行（updated_at/error）。
    // 代理三态按订阅选项组装：内核代理 > 系统环境代理 > 强制直连。
    bool download(db::Profile& p) {
        p.file = std::format("{}.yaml", p.id);
        api::ClashApi::DownloadOptions opts;
        opts.timeoutSecs = p.timeoutSecs > 0 ? p.timeoutSecs : 60;
        opts.allowInvalidCert = p.allowInvalidCert;
        if (p.useCoreProxy) {
            opts.proxyUrl =
                std::format("http://127.0.0.1:{}", coreStore().snapshot().mixedPort);
        } else {
            opts.allowProxyEnv = p.useSystemProxy;
        }
        const auto r = coreStore().api().downloadToFile(
            p.url, cfg::profilesDir() / p.file, opts);
        if (!r.ok) {
            p.error = r.error;
            try { coreStore().db().saveProfile(p); } catch (...) {}
            lastError_ = std::format("下载失败：{}", r.error);
            return false;
        }
        // 订阅响应头：subscription-userinfo（流量）/ profile-web-page-url
        // （提供方首页）。缺头时清零/清空——以最新一跳为准。
        if (const auto it = r.headers.find("subscription-userinfo");
            it != r.headers.end()) {
            parseUserInfo(it->second, p.usedBytes, p.totalBytes);
        } else {
            p.usedBytes = 0;
            p.totalBytes = 0;
        }
        if (const auto it = r.headers.find("profile-web-page-url");
            it != r.headers.end()) {
            p.homepage = it->second;
        } else {
            p.homepage.clear();
        }
        p.error.clear();
        p.updatedAt = nowUnix();
        try {
            coreStore().db().saveProfile(p);
        } catch (const std::exception& e) {
            lastError_ = e.what();
            return false;
        }
        return true;
    }

    std::string lastError_;
};

export ProfilesStore& profilesStore() {
    static ProfilesStore store;
    return store;
}

} // namespace store
