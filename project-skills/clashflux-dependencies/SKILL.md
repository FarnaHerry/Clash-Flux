---
name: clashflux-dependencies
description: 修改 Clash-Flux CMake/CPS 包边界、固定依赖、HuxerUI 或扩展库 fork、sing-box 打包与跨平台来源身份时使用。
---

# 依赖与 fork 维护

按任务先读 [CPS 依赖](../../docs/cps-dependencies.md)、[HuxerUI fork](../../docs/huxerui-fork.md) 或 [sing-box 稳定升级](../../docs/singbox-stable-upgrade.md)，不将历史 SHA 当成可变分支消费。

## 构建包边界

- CMake 最低 4.4、验证基线 4.4.2；通用库来自 cmake/dependencies/Lock.cmake 固定 URL/SHA、独立 CPS producer 与 find_package。保留 HuxerUI codegen/资源/Android 图及应用模块源码编译，不把宿主包用于目标平台。
- 包边界变更执行 CPS 重定位、版本拒绝、缓存完整性和 TLS 回归；缓存隔离工具链、flags、配置与目标身份，具体命令见 CPS 文档，不只测当前缓存命中。
- 固定 Zstandard 归档的来源/SHA 与各平台同一静态目标必须一致；升级/添加构建依赖同步 third_party/README.md，MRS 的解码边界另见编译器技能。

## HuxerUI 与扩展

- HuxerUI 修复维护在 FarnaHerry/HuxerUI 的 farna/main，独立 ../HuxerUI-fork；main 保留官方基线。应用 third_party/huxerui 独立 checkout，与 CI 固定同一验证 SHA，不直接消费维护工作树。
- Lib-Charts/Lib-SQLite/Lib-Camera 同样维护在 FarnaHerry fork 的 farna/main，独立 ../HuxerUI-libs；应用 huxerui_use_library(URL,COMMIT) 固定获取，不在 CMake/CI 重复打已合入补丁。
- 同步先 fetch 上游最新 revision，逐项审查集成差异，merge 进 farna/main，不 rebase/force push；上游已修复的差异不继续保留。应用构建/回归验证新 revision 后才更新固定 SHA，所有 CI/原生/Android 图共用相同 URL/COMMIT。
- 升级必须保留 Lib-Charts 的 PieChartData/PieChart、紧凑高度与项目采用的图表 API/行为，实际验证首页图表。
- 框架升级保留 Pager 清过期拖动目标、反向离场页与隐藏虚拟页待测量/失效传播行为，运行生产 page_transition；不能在应用重贴这些框架补丁。
- Lib-SQLite 的只读启动查询也在 fork 维护；升级验证不建文件、不迁移、不改 journal、失败保留库以及多订阅重开，不用构建代替数据保护回归。
- Android Java/native 选同一源码，SDK 回落同身份；详情见 [Android 构建](../../docs/android-build.md)。

## sing-box

官方各平台资产 SHA256、版本/revision 与 Android libbox 同源，缓存按版本/资产摘要隔离；保留正式 tag 供 ReadTag，不凭旧 AAR 存在继续打包。
升级时同时用 [编译器技能](../clashflux-compiler/SKILL.md) 更新能力/保真度基线，按 [测试技能](../clashflux-test/SKILL.md) 完成回归后再声称升级可用。
