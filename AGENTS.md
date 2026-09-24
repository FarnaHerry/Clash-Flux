# Clash-Flux AI 开发规范

本文件是仓库级 AI/agent 开发指引。任何 AI agent 在阅读、修改或评审本项目代码前，
必须先读取本文件，并遵守以下规则。

## 修改后的强制验证

每次修改完成后，无论改动大小、涉及何种文件（代码、UI、脚本还是配置），都必须先
在本地重新编译，不得依赖旧的可执行文件判断改动有效：

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
- HuxerUI 源码构建所需的本地修复应维护为 `cmake/patches/` 中的补丁，并接入所有
  对应平台的源码检出步骤；升级固定版本时先确认补丁仍可应用。
- iOS CI 用固定 HuxerUI revision 和 iOS Simulator SDK 编译 Clash-Flux 的
  `clash-flux_huxerui_ios_core` 静态目标（arm64）。它是非阻塞源码编译检查，不生成
  `.app`/IPA，也不属于 Release 门禁；当前配置不含 curl TLS 或 sing-box 子进程。
  完成可分发 iOS 应用壳、Network Extension 和 sing-box iOS 接入后，再评估 iOS 发布包。
- Android Gradle 构建会按固定 revision/SHA256 生成并打包国内 GEOIP/GEOSITE
  规则集；不得跳过 `stageBundledRuleSets` 或改为运行时下载。
- HuxerUI composable 函数体内不能使用条件编译；普通 UI 源文件按项目现有 DSL 约定编写。
- 可排序卡片的拖动预览要持续跟随原始抓取点；滚动网格应在滚动视口注册拖放目标，
  使卡片间隙和视口边缘的拖动继续有效并触发边缘自动滚动。
- 手机端二级页面（详情、编辑或从一级页内部入口继续进入的页面）必须覆盖一级底部导航，
  并在标题栏左侧提供返回箭头。系统返回键、返回手势与该箭头必须调用同一个返回动作，
  回到直接父级而不是退出应用；此规则适用于所有当前和未来的手机平台，不限于 Android。
  二级页使用独立的页面进入/返回（push/pop）动画，不得与一级页切换动画共用同一套效果；
  进入时保留父页面状态（滚动位置、已填写内容），返回后原样恢复。
- 阻塞的内核、网络、路由和系统设置操作必须放到任务线程，不能阻塞 UI 线程。
- Windows 后台系统操作必须安静执行，不得闪出命令行窗口。优先调用 Win32 API；确实
  需要启动子进程时使用 `CreateProcessW` 的 `CREATE_NO_WINDOW` 并重定向标准句柄，
  不要在后台使用 `std::system`、`_popen` 或会显示终端的 shell 启动方式。只有明确
  需要用户交互的授权流程（例如 UAC）才显示系统提示；启动、轮询和退出清理中的类似
  操作也必须遵守此规则。
- 修改完成后运行 `git diff --check`，并在回复中说明实际执行过的验证命令及结果。
- 除非用户明确要求，不要提交、打标签、推送或发布版本。

## 文档同步

如果构建、运行、发布或开发流程发生变化，必须同步更新 `README.md`、本文件和相关
`docs/` 文档，保持命令与实际工程一致。
