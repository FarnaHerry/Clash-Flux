# HuxerUI fork 维护与应用固定版本

框架维护仓库为 [FarnaHerry/HuxerUI](https://github.com/FarnaHerry/HuxerUI)，独立本地目录为
`/home/farna/dev/cpp/mcpp/HuxerUI-fork`。`farna/main` 是长期集成分支；`main` 保留官方
`HuxerUI/HuxerUI` 基线。更新采用 merge，不 rebase 或 force push 已发布的集成历史。

Clash-Flux 当前固定 `3e16bf4ccb02aa3dfd7699f3342fe3a911e3e550`，各平台 CI 共用
`.github/workflows/build.yml` 的 `HUXERUI_REPO` / `HUXERUI_COMMIT`。本机应用使用独立的
`third_party/huxerui` checkout，不直接构建可变的框架维护工作树。Android Java 与 native
必须来自这同一份源码；CLI 使用源码目录时显式传入 `--source`。

## 补丁归属

此固定提交包含 acgu 的五个补丁提交、28 个未提交文件快照、Android 宿主默认焦点高亮修复，
以及 Clash-Flux 的五项框架修复：拖动预览跟随抓取点、Pager 反向切换、隐藏虚拟页布局、
Linux 窗口帧生命周期和 Objective-C++ 的 WindowTitleBar 聚合初始化兼容。
这些修复已从应用 `cmake/patches/` 和 CMake/CI 应用步骤中移除，配置应用不会修改框架源码。

Lib-Charts、Lib-SQLite、Lib-Camera 是独立仓库，未合入的饼图、紧凑图表、启动只读查询和
ApplicationContext 兼容补丁继续在应用 `cmake/patches/` 维护，并在 CMake 加入依赖前应用。

本次迁移保留原 Clash-Flux 框架工作树在 `third_party/huxerui-before-farna`；其中原有六个
修改文件未丢弃。acgu 的原工作树也未改动。其它姊妹项目按各自验证结果逐个迁移。

## 检出与升级

新环境检出应用依赖：

```bash
git clone https://github.com/FarnaHerry/HuxerUI.git third_party/huxerui
git -C third_party/huxerui checkout 3e16bf4ccb02aa3dfd7699f3342fe3a911e3e550
```

在独立维护仓库中先 `git fetch upstream main`，审查官方更新和集成差异，再在 `farna/main`
执行 `git merge upstream/main`。官方已有等价修复时消除重复差异；不要把维护仓库切换分支的
过程传播给正在构建的应用 checkout。更新官方基线时只允许快进 `main`，不要将应用补丁合入它。

框架与应用验证通过后，发布集成提交，再把本机依赖、本文固定 SHA 和 workflow pin 一起更新。
Clash-Flux 必须重新运行 `cmake --build build --target clash-flux`、`./run.sh --version`、
`git diff --check`，并运行生产分页交互及桌面必跑回归。扩展库补丁按各自固定版本重新
检查 `git apply --check --unidiff-zero`。Android 构建必须保留 GEOIP/GEOSITE 打包及 Java/native
同源约束，桌面门禁和 iOS 暂缓政策保持不变。

## 验证边界

此次框架合入通过 Linux Debug 运行时与独立头文件构建、Release 框架构建和四个 common
CTest 套件（含 953 个 runtime 用例）。新增回归覆盖跨标签反向时的几何连续性、视口边缘
拖动抓取点和隐藏虚拟页重新显示后的测量。

Clash-Flux 已在此 SHA 重新编译，通过 `./run.sh --version`（v0.3.27）、全部 15 项
`clashflux-required` 桌面回归和 Android arm64 Debug APK 构建（显式 `--source third_party/huxerui`，
Java/native 同源且保留 `stageBundledRuleSets`）。保留的四份扩展库补丁均在各自固定提交的
干净源码上通过 `git apply --check --unidiff-zero`。

此前完整框架 CTest 为 30/32；两项失败已在官方基线复现，详情见 fork 的
[集成文档](https://github.com/FarnaHerry/HuxerUI/blob/farna/main/docs/development/farna-integration.md)。
Android 真机框架 instrumentation 尚未完成，Windows/macOS/iOS 原生运行也未在此 Linux
主机验证；不能把编译通过或 common 回归通过表述为这些平台运行验证通过。
