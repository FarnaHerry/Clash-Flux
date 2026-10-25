# Clash-Flux AI 开发规范

阅读、修改或评审代码前先读本文件，再按任务读取下面的项目技能。
项目技能放在普通、受版本控制的 `project-skills/`；不依赖工具自动发现。
只加载当前任务需要的 SKILL.md，以及它明确要求的相关引用/章节，不一次性读取整个目录。

## 按任务必读

开始工作和任务范围变化时重新匹配；同时涉及多个领域则读取各相关技能。路径均相对仓库根。

| 任务/触及范围 | 必须先读 |
| --- | --- |
| 任何修改后的构建/验证，测试、CTest、CI 门禁 | [clashflux-test](project-skills/clashflux-test/SKILL.md) |
| 发版、版本号、发布说明、打包、签名、发布流水线 | [clashflux-release](project-skills/clashflux-release/SKILL.md) |
| UI、布局、分页、菜单、主题、文案、首页动画、应用级模型、导入交互 | [clashflux-ui](project-skills/clashflux-ui/SKILL.md) |
| 数据库、迁移、订阅增删改/导入刷新、文件提交、flush、CLI 单实例数据路径 | [clashflux-data](project-skills/clashflux-data/SKILL.md) |
| 内核控制、托盘、无窗/首帧启动、PPTP、OpenVPN 入口、订阅唤醒链接 | [clashflux-runtime](project-skills/clashflux-runtime/SKILL.md) |
| Android 后台、Windows/Linux 服务、系统网络/权限、iOS | [clashflux-platform](project-skills/clashflux-platform/SKILL.md)，只读相关平台及引用 |
| CMake/CPS 包、HuxerUI/扩展 fork、固定依赖/内核升级 | [clashflux-dependencies](project-skills/clashflux-dependencies/SKILL.md) |
| 订阅转换、协议/DNS/规则/provider、保真度、来源编排与候选应用 | [clashflux-compiler](project-skills/clashflux-compiler/SKILL.md) |
| C++ 模块接口、inline/性能、JSON codec 边界 | [clashflux-cpp](project-skills/clashflux-cpp/SKILL.md) |
| Logo、托盘/应用图标、Android 启动图标/磁贴 | [clashflux-branding](project-skills/clashflux-branding/SKILL.md) |

## 全局底线

- 修改前检查 `git status`，保留已有改动；不擅自 reset、restore 或清理无关文件。未经用户明确要求，不提交、打标签、推送或发布；读取发版技能不等于发布授权。
- 每次修改任何文件后都重新编译主目标、运行版本检查和 `git diff --check`。脚本不会代替编译，失败不得宣称完成；最终说明实际命令、结果及未覆盖范围。
- 数据库记录与 profile 文件共同构成用户订阅；启动、升级和失败路径不删库重建、不降级成空缓存、不“修复性”清理订阅。hydrate 前不写回空数据；迁移保留全部行，失败明确终止并留原库。
- 刷新/编辑先独占暂存并完成解析/候选检查，再原子替换；失败保留原文件、引用与活动配置。删除只响应明确 ID，事务后核对全部引用和文件根；异步 flush 按 key/ID 版本确认，失败保留待写版本。
- 只暴露真实内核能力；转换不能静默丢条目或改语义，按 exact/approx/unsupported 记账。默认 TLS 校验不得为“能运行”而关闭，例外只能按明确配置作用于指定请求/端点。
- 平台能力与视口布局分开：手机不增加桌面多订阅编排，桌面窄窗不失去桌面能力。Windows 数据面归 SCM 服务，Android libbox 归后台、数据库归 UI，打开 Android 应用不自动启动代理；iOS 当前暂缓，不纳入 Release。
- 阻塞内核、网络、路由、系统设置操作放任务线程。组合期资源不在回调/协程中读取；一次性注册有生命周期；共享状态和运行控制走应用级模型，意图与实际状态分开，结果发布先查版本。
- 本进程 OS 资源用已有 RAII 包装；跨进程/服务资源按所有权协议管理。GUI/CLI 保持单实例、单 Runtime/托盘/缓存，数据库 CLI 在 Runtime 内执行；version/help/service 保留直接命令边界。
- 依赖固定来源/摘要/版本并跨平台一致；框架和扩展修复归各自 fork，不在应用重复打补丁或消费可变维护工作树。UI 改动经过 HuxerUI codegen。

## 最低验证命令

Linux/macOS：

```bash
cmake --build build --target clash-flux
./run.sh --version
git diff --check
```

Windows（已加载 MSVC 开发环境的 PowerShell）：

```powershell
cmake --build build --target clash-flux
./run.ps1 --version
git diff --check
```

其它构建目录用 `CLASHFLUX_BIN` 显式选择产物；完整测试/平台前提、GUI 检查和发布门禁见测试技能。
构建或配置检查不等于实机、VPN、签名或发布验收。

## 规范维护

- 根 AGENTS.md 最多 **150 行、6,000 字符**，两项必须同时满足；行数包含空行，字符按换行统一为 LF 的全文统计（含空白与换行）。
- `project-skills/` 中每个 SKILL.md 最多 **100 行**（含 frontmatter 和空行）；长细节放入该技能的 `references/` 或已有专题文档，并提供按需读取入口。
- 修改规范时核对上述限额；超限必须先合并重复内容或外移细节，不能靠压成长行、删除必要行为约束来规避。

新增约束先归入对应项目技能或已有专题文档；根文件只保留全局底线和任务索引，不堆版本、支持矩阵、UI 尺寸、事故经过或设备性能数字。
技能写执行步骤和非显然约束，详细设计/能力基线继续由它链接的 `docs/` 专题维护；同一规则只保留一个详细来源，相关改动同步入口与权威文档。
构建、运行、发布或开发流程变化时同步 README.md、README.en.md、本文件的相关入口、对应技能和专题。
映射/内核升级同步保真度基线；完成度/验收证据记录日期、代码快照、实际检查和未完成边界。
技能迁移不撤销已有行为限制；只有用户明确改变产品要求或有代码/回归证据证明过时，才删除对应约束。
