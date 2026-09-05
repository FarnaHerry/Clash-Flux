// profiles.cppm — clashflux.store.profiles：订阅管理 store（接口即实现，单例）。
//
// 订阅 = db::Profile 行 + profiles/<id>.yaml 文件。importUrl 下载落盘；
// activate 把启用订阅交给 coreStore 重启内核生效。线程契约同 core_store：
// 标「阻塞」的方法（网络下载 / 内核重启）UI 必须经 RunOnTaskThread 调用。
export module clashflux.store.profiles;

import std;
import clashflux.config;
import clashflux.utils;
import clashflux.db;
import clashflux.core;
import clashflux.store.core;

namespace store {

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

    // 新增订阅：先建行拿 id，再下载到 profiles/<id>.yaml。阻塞（网络）。
    // 成功返回新 id；失败返回 0 并填 lastError()。
    std::int64_t importUrl(const std::string& name, const std::string& url) {
        lastError_.clear();
        db::Profile p;
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

    // 本地文件导入：复制内容到 profiles/<id>.yaml。
    bool importFile(const std::string& name, const std::filesystem::path& source) {
        lastError_.clear();
        db::Profile p;
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

    // 编辑订阅信息（名称 / URL）：仅落库——名称立即生效，URL 变化在下次
    // 「更新」拉取时生效。name 为空时以 url 兜底（同 importUrl 惯例）。
    bool updateInfo(std::int64_t id, const std::string& name, const std::string& url) {
        lastError_.clear();
        auto p = findById(id);
        if (!p) {
            lastError_ = "订阅不存在";
            return false;
        }
        p->name = name.empty() ? (url.empty() ? p->name : url) : name;
        p->url = url;
        try {
            coreStore().db().saveProfile(*p);
        } catch (const std::exception& e) {
            lastError_ = e.what();
            return false;
        }
        return true;
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

    // 下载 p.url 到 profiles/<id>.yaml 并更新行（updated_at/error）。
    bool download(db::Profile& p) {
        p.file = std::format("{}.yaml", p.id);
        const auto r = coreStore().api().downloadToFile(
            p.url, cfg::profilesDir() / p.file, 60);
        if (!r.ok) {
            p.error = r.error;
            try { coreStore().db().saveProfile(p); } catch (...) {}
            lastError_ = std::format("下载失败：{}", r.error);
            return false;
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
