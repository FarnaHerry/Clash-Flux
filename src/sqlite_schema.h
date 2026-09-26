// sqlite_schema.h — huxerui::sqlite ORM 的 Clash-Flux 持久化 schema。
//
// 单一事实来源：profiles / settings 两张表的列声明、schema 版本与打开选项都在
// sqlite_schema.cpp。db / persistence / 测试共用这里，避免两处 DDL 漂移。
//
// **不做向后兼容**：0.2.x 的 SQLiteCpp 旧结构与当前结构不兼容，数据也不值得
// 保留。应用既不迁移也不删除任何文件：旧库直接 open 失败，由用户自行删除；
// 因此这里不提供 Migration。
//
// 日志仍然直接写 core/*.log 文件：日志是高频追加，放进 SQLite 会与
// settings/profiles 的写事务抢锁与 IO，得不偿失。
//
// 记录类型与 model::Profile 一一对应。nullability 由 std::optional 决定；ORM
// 要求主键非可空，因此 id/key 用普通类型，主键由应用分配（非 AUTOINCREMENT）。
#pragma once

#include <huxerui/sqlite.h>

#include <cstdint>
#include <string>

namespace clashflux::db_schema {

struct ProfileRow {
    std::int64_t id = 0;
    std::string name;
    std::string url;
    std::string file;
    bool selected = false;
    std::int64_t updated_at = 0;
    std::string error;
    std::string type = "remote";
    std::string description;
    int timeout_secs = 60;
    int interval_mins = 0;
    bool auto_update = false;
    bool use_system_proxy = false;
    bool use_core_proxy = false;
    bool allow_invalid_cert = false;
    std::string homepage;
    std::int64_t used_bytes = 0;
    std::int64_t total_bytes = 0;
    std::string native_config;
    std::string native_routes;
};

struct SettingRow {
    std::string key;
    std::string value;
};

const huxerui::sqlite::Table<ProfileRow>& profiles();
const huxerui::sqlite::Table<SettingRow>& settings();
const huxerui::sqlite::Schema& schema();
const huxerui::sqlite::OpenOptions& openOptions();

} // namespace clashflux::db_schema
