# Clash-Flux AI 开发规范

本文件是仓库级 AI/agent 开发指引。任何 AI agent 在阅读、修改或评审本项目代码前，
必须先读取本文件，并遵守以下规则。

## 修改后的强制验证

每次完成需求实现、修复、重构、UI 调整或构建脚本修改后，必须在本地重新编译，
不得依赖旧的可执行文件判断改动有效：

```bash
cmake --build build --target clash-flux
./run.sh --version
```

需要验证 GUI 启动时，再执行 `./run.sh`。`run.sh` 默认只运行已经存在的
`build/clash-flux`，不会隐式替代编译步骤；如果使用其他构建目录，必须显式指定：

```bash
CLASHFLUX_BIN=/绝对路径/clash-flux ./run.sh --version
```

编译失败、可执行文件不存在或 `run.sh` 验证失败时，不得在回复中声称改动已完成。

## 代码与工作区约定

- 修改前先检查 `git status`，保留用户已有改动，不得擅自 reset、restore 或清理无关文件。
- 优先使用 Ninja/CMake 的增量构建；UI 修改必须经过 HuxerUI codegen 和目标构建。
- HuxerUI composable 函数体内不能使用条件编译；普通 UI 源文件按项目现有 DSL 约定编写。
- 阻塞的内核、网络、路由和系统设置操作必须放到任务线程，不能阻塞 UI 线程。
- 修改完成后运行 `git diff --check`，并在回复中说明实际执行过的验证命令及结果。
- 除非用户明确要求，不要提交、打标签、推送或发布版本。

## 文档同步

如果构建、运行、发布或开发流程发生变化，必须同步更新 `README.md`、本文件和相关
`docs/` 文档，保持命令与实际工程一致。
