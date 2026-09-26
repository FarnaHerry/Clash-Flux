// sqlite_schema.h — huxerui::sqlite ORM 的 Clash-Flux 持久化 schema。
//
// 单一事实来源：profiles / settings 两张表的列声明、schema 版本与 0→1 迁移
// 都在 sqlite_schema.cpp。db 模块与测试共用这里，避免两处 DDL 漂移。
//
// 日志仍然直接写 core/*.log 文件：日志是高频追加，放进 SQLite 会与
// settings/profiles 的写事务抢锁与 IO，得不偿失。
//
// 记录类型与 db::Profile 一一对应。nullability 由 std::optional 决定；ORM 要求
// 主键非可空，因此 id/key 用普通类型（老库的 PK 没有 NOT NULL，靠 0→1 迁移
// 重建表补齐）。
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
const huxerui::sqlite::Migrations& migrations();
const huxerui::sqlite::OpenOptions& openOptions();

} // namespace clashflux::db_schema
