# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Clash-Flux 是一个 **C++23 模块化 GUI 代理客户端**：复刻 Clash Verge Rev 的核心体验，
用 **HuxerUI**（组件式声明 UI）做桌面壳，经 mihomo（Clash.Meta）内核的
external-controller REST API + WebSocket 推送流交互，全程 C++。构建系统 CMake
（脚手架与姊妹项目 `../apitab` 同源）。分层：UI（src/ui/*.cpp 普通源走 hcg
codegen）/ 领域 store（clashflux.store.* 模块）/ 内核接入层（core/api/stream）。

## HuxerUI 开发参考

UI 工作先读 skill：`.claude/skills/huxerui-app-development/SKILL.md`（references/
含 dsl-style、components、fundamentals、layout-and-ui 等分册）。要点：

- HuxerUI 接入“SDK 工程契约驱动、源码解析优先”：开发构建优先
  `third_party/huxerui` 源码（git clone 上游，`add_subdirectory` 编译，不入库）；
  缺 GTK ≥4.14 / libepoxy ≥1.5 / libsoup ≥3.0 开发包时自动回落已安装 SDK
  （`HUXERUI_HOME`）或 `third_party/tarballs` 的 Linux 0.2.0 离线包。强制 SDK：
  `-DCLASHFLUX_HUXERUI_FORCE_SDK=ON`。本机（Fedora）缺 libepoxy-devel，当前走
  已安装 SDK 通道。
- 源码通道需用当前源码本地构建的宿主工具（`third_party/huxerui-tools/
  linux/x86_64/{hcg,hrc}`，从 apitab 拷贝；上游 linux 预置可能落后于源码，
  pull 上游后要重编，方法见 apitab CLAUDE.md）。
- UI 层是**普通 .cpp**（不要 .cppm：codegen 只扫 .cpp/.cc/.cxx）；composable
  函数不加 `inline`；入口根在 src/app.cpp + src/ui/app.cpp。
- **composable 函数体内不能有条件编译**（hcg 不支持 #ifdef 穿行）——版本号等
  宏在文件作用域先展开成常量。
- `nlohmann::json` 经 `import nlohmann.json` 模块：**不要用 `.items()` 结构化
  绑定**（迭代代理的 `get<>` 不在导出集里，模块下编译失败），用迭代器
  `it.key()`/`it.value()`。
- 存进 `State<T>` 的类型需要 `operator==`（`= default` 即可），否则 State
  写回时编译失败。

## 构建 / 运行

```bash
cmake -B build -G Ninja            # 配置（默认 Release；调试加 -DCMAKE_BUILD_TYPE=Debug）
cmake --build build -j             # 编译
ctest --test-dir build             # 冒烟测试（test_smoke）
./run.sh                           # 启动 GUI（INTEL_FORCE_PROBE=1）
huxerui run linux                  # HuxerUI CLI 流程（构建到 .huxerui/build/linux/）
```

- 工具链：系统 GCC（本机 16.2.1）+ libstdc++，CMake ≥ 4.4（`import std` 仍是
  experimental：UUID 表在 `cmake/CxxImportStdGate.cmake`）。
- **依赖全部 vendor 在 `third_party/`**（tarball + SHA256，configure 期解包到
  `build/vendor/`，清单见 `third_party/README.md`）：HuxerUI 0.2.0、curl 8.22.0
  （REST + 订阅下载）、IXWebSocket 12.0.1（client-only，内核推送流）、
  SQLiteCpp 3.3.3（订阅/设置）、nlohmann::json 3.12.0、OpenSSL 3.5.1
  （linux x86_64 兜底静态包）。
- **mihomo 由项目自带**：configure 期下载官方 release（v1.19.30 linux x86_64，
  SHA256 钉死，见 CMakeLists「自带 mihomo 内核」块），POST_BUILD 拷到
  `<exe>/engines/mihomo`；`-DCLASHFLUX_BUNDLE_MIHOMO=OFF` 关闭。运行时解析
  优先级 `<exe>/engines/mihomo → <exe>/mihomo → <repo>/engines/mihomo（开发
  形态）→ PATH`（`src/config.cppm::cfg::mihomoBinary()`）。`engines/` 已
  gitignore。

## 架构

| 模块 | 文件 | 职责 |
|------|------|------|
| `clashflux.config` | `src/config.cppm` | 数据目录（~/.local/share/clash-flux）/ mihomo 二进制解析 / 控制器端点（127.0.0.1:9097）/ secret 生成 / 深色检测 |
| `clashflux.utils` | `src/utils.cppm` | 纯 string/number 帮助函数 + percentEncode / appendQuery |
| `clashflux.db` | `src/db.cppm/.cpp` | SQLiteCpp：profiles（订阅）/ settings（KV）两表 |
| `clashflux.api` | `src/api.cppm/.cpp` | mihomo REST 客户端（curl，同步阻塞、每调用独立 handle）：version/configs/patchConfigs/proxies/selectProxy/delay/rules/connections/providers/订阅下载 |
| `clashflux.core` | `src/core.cppm/.cpp` | mihomo 子进程生命周期（posix_spawn + 监视线程；SIGTERM→2s→SIGKILL）+ generateConfig（订阅 YAML 剔除托管顶层键 + 注入块合成 runtime config） |
| `clashflux.stream` | `src/stream.cppm/.cpp` | /logs /traffic /connections 三条 WS 流（IX 自管线程，事件入槽，UI PollWhile 泵取） |
| `clashflux.store.core` | `src/store/core_store.cppm` | 编排单例 `coreStore()`：持有 Db/ClashApi/CoreProcess/CoreStreams；startCore/stopCore/applyMode/refreshRuntime/checkAlive；settings KV |
| `clashflux.store.profiles` | `src/store/profiles.cppm` | 订阅单例 `profilesStore()`：importUrl/importFile/refresh/activate/remove（activate/remove 触发内核重启） |
| `clashflux.ui.*`（普通 C++） | `src/ui/*.cpp` | app（壳：标题栏+图标侧栏+IndexedPages+托盘，岛屿风）/ common（岛屿原语 IslandSurface/DialogCard/页面骨架/卡片/状态胶囊）/ home/profiles/proxies/rules/connections/logs/settings 七页 / task_bridge.h（协程桥） |
| `src/app.cpp` | 普通 TU | `Application{AppRoot, AppOptions}`（Custom chrome，标题栏 28pt） |

## 关键约定（改代码前必读）

1. **`import std;` 后禁止再 `#include` 标准头**。C/系统/第三方头放全局模块片段
   （`module;` 与 `module clashflux.x;` 之间）。普通 UI .cpp / ui.h 头用哪个
   std 设施就自己 `#include` 哪个（libstdc++ 传递包含在 libc++ 上不存在）。
2. **UI 层遵守 skill 的 DSL 风格**：普通 .cpp、composable 不加 inline、View 按值
   传递、具名 View 链式调用前 `std::move`（`.With` 等是右值限定）。
3. **受控值以应用状态为权威**；TextField 保留完整 TextEditingValue；动态兄弟用
   稳定 `.Key(...)`。
4. **线程契约**（src/ui/task_bridge.h）：State 只在 UI 线程读写；api/core/store
   的阻塞方法必须 `co_await RunOnTaskThread(fn)` 派到任务线程池；WS 流与进程
   输出经 `PollWhile(interval, tick)` 泵回 UI。**事件处理器内禁止同步写会导致
   点击节点被卸载的 State**——经 `tasks.Launch` + `co_await Delay(0)` 推迟。
5. **占位不能用 Spacer().With(Frame)**（Spacer 自带 Grow(1) 会平分空间）——
   用空 `Row{}`/`Column{}`；页面根要 `Grow(1.0F)` + `CrossAlign(Stretch)`。
6. 内核 REST 全部走 `store::coreStore().api()`；UI 不直接持有 curl。
7. **岛屿风**（对齐 apitab）：极简黑白主题（app.cpp MinimalDark/Light）；
   一级岛 16pt / 二级岛 8pt 圆角，表面色经 `ResolveIslandTheme(theme)` 语义
   层级取，不直接用 surface_container_*。订阅卡交互范式：右键
   `ViewEvents::ContextMenuRequested` + `UseMenu().ShowAt`，双击
   `MultiTapGesture{.count=2}` + `MultiTapEvents::Recognized`，矢量图标着色
   用 `Image::Tint`（IconButton/Foreground 不着色 SVG）。
8. **响应式**：`UseViewportClass()` Compact(<600) 收窄侧栏(44pt)/一级岛内边距
   （PageScaffold）/首页卡片 2×2/订阅卡整宽列表；窗口最小 560×480。

## mihomo 交互要点

- external-controller：`127.0.0.1:9097`（避开常见 9090），secret 首启生成存
  settings 表（`core.secret`），REST 走 `Authorization: Bearer`，WS 走 `?token=`。
- 运行时配置 = `generateConfig(订阅YAML, ...)` 合成：文本级剔除订阅里的托管
  顶层键（连同其缩进值块；键表必须与注入块一一对应：mixed-port/mode/secret/
  unified-delay/tcp-concurrent/find-process-mode/global-client-fingerprint/
  profile 等），注入块放文件头，写 `core/config.yaml`，
  `mihomo -d core/ -f core/config.yaml` 启动。控制器就绪轮询 ≤30s（订阅带
  规则 provider 的冷启动要拉 geodata/规则集，5s 会误判）。
- 默认混合端口 **7899**（避开 Clash 7890 / Verge 7897 常见占用）。
- 测速：单节点 `GET /proxies/{name}/delay`；整组 `GET /group/{name}/delay`
  （返回节点→延迟 map，超时项值 ≤0）。测速超时要比 curl 传输超时窄。

## 里程碑状态（2026-09-05）

- ✅ M1：脚手架 + 内核生命周期 + REST/WS + 六页骨架（订阅 CRUD/代理组切换测速/
  规则列表/连接快照/日志流/设置）。
- ⬜ 待做：系统代理（gsettings）/ TUN 开关、流量图表（Canvas 自绘或
  HuxerUI::Charts 官方库）、订阅合并策略增强（规则覆写）、deep link
  （clash://install-config）、单实例、开机自启、规则 provider 管理、连接详情。
