# 规则页首次加载性能

2026-11-16：只读页和编辑器共享目标规则投影。阶段日志为 `wait_us` / `read_us` / `target_us` / `target_reused` / `bytes` / `rows`。`target_us` 包含索引等待、冷转换的资源提交与回滚等待、转换和目标视图。新成本与边界见 [R2 证据](evidence/2026-11-16-target-r2.md)。[预生成接入](evidence/2026-11-16-target-r2-prewarm.md)验证了已就绪来源首次打开复用既有读取。历史投影快路径仅由测试目标保留。下文记录旧快路径的历史测量。

R5 补充进程重启后的原文重建和磁盘产物测量，包含分阶段耗时与独立进程峰值 RSS。当前应用重启沿用原文重建。见 [R5 证据](evidence/2026-11-16-target-r5.md)。

后续通过真实 Linux 窗口量到首次打开。中等规模快照的准备可由启动预生成覆盖。超出闲置预算的快照可能在打开前淘汰，产生第二次读取和转换。应用加入后台原文大小筛选后，5 万条规则的测试样本恢复单次完整读取。启用筛选前后的首屏时间及内存见 [预生成预算复测](evidence/2026-11-16-target-r5-prewarm-budget.md)。

## 2026-11-15 快照与范围

Clash-Flux `894a808` 加规则页共享缓存及本轮工作区，Linux / Clang 21 / libc++ /
Debug（`-g`，未改优化选项），实际 HuxerUI 为 `5c84b4ab`。
对照程序在修改编译器前后分别导入真实 `clashflux.singbox` 模块，使用同一工具链和固定
CPS yaml-cpp 0.9.0；优化前的编译器源码与 `894a808` 相同。
优化后四个编译器文件（`singbox.cpp`、`singbox.cppm`、`singbox_profile_rules.inc`、
`singbox_rule_projection.inc`，按该顺序以路径/NUL/内容/NUL 连接）的 SHA-256 为
`c338897c59e115930e6c03fb5be22e47a303f6316914a802b49f5dae098bb4fd`。

这是同步规则投影的墙钟耗时，**不包含文件读取、配置锁等待、任务调度、UI 挂载或 GPU 呈现**。
不能把 Debug 合成负载换算成 Release、用户订阅或 Android 真机首次进入时间。
界面加载日志 `rules-load` 单独记录 `wait_us`、`read_us`、`project_us` 和行数，
不记录订阅内容、地址、凭据或规则值；可据此判断真实加载是否还在等待配置操作。

## 实现与兼容边界

本节记录已实现的投影优化。后续架构工作以[目标工作模型规划](rule-document-plan.md)为准，优先复用转换产物并让交互消费目标结果；已有快路径是否收敛由迁移等价与实际成本决定，当前测量不作为新模型的性能证据。

普通单行规则块（plain 或无转义引号）直接投影，yaml-cpp 仍读取并校验整个临时文档：
规则序列在私有解析副本中表示为 literal scalar，不写回文件或送入内核编译。
检查缩进、标量字符、节点源位置及校验后的原始字节，避免把多行文本中的 `rules:` 当根键。
其它标量序列使用 yaml-cpp 事件接口；锚点、别名、复杂节点等不满足条件时走原 DOM 路径。
回归把同一文档加根锚点强制走独立 DOM 路径，核对规则、原文锚点、隐式默认出口和个人修改。
覆盖引号/转义、注释、Unicode/非法字节、换行、重复键、别名、复杂规则和错误文档。

展示调用不保留未消费的原规则副本；默认 API 仍保留它，编辑与编译语义不变。
规则类型字典只初始化一次；启用的替换/禁用操作只建立一次原规则锚点索引，
继续拒绝重复或丢失的锚点，保持原顺序、禁用和兜底语义。
共享缓存与候选提交约束见[订阅个人规则](subscription-personal-rules.md#规则页加载与缓存)。

## 固定负载与结果

合成 YAML 为 `rules:`、N 条 `DOMAIN-SUFFIX,rule<i>.test,DIRECT` 和末尾 `MATCH,DIRECT`。
N 为 1,000 / 10,000 / 100,000，文件分别为 37,914 / 388,914 / 3,988,914 字节。
每个进程按该顺序先测无个人修改，再测禁用前 1,000 条规则，不清理进程内已有缓存。
优化前与后调用分别匹配当时的页面路径（保留/省略未使用的原规则副本）。
前后程序交替运行三轮，下面为中位数；不设置 CI 墙钟阈值。
[原始记录](evidence/2026-11-15-rule-projection.csv)包含全部 36 项测量。

| 源规则数（不含 MATCH） | 个人修改数 | 优化前 ms | 优化后 ms |
| ---: | ---: | ---: | ---: |
| 1,000 | 0 | 31.274 | 12.549 |
| 1,000 | 1,000 | 32.431 | 14.101 |
| 10,000 | 0 | 241.501 | 107.299 |
| 10,000 | 1,000 | 358.824 | 110.185 |
| 100,000 | 0 | 2443.758 | 1070.706 |
| 100,000 | 1,000 | 3968.383 | 1089.769 |

## 重复检查

```bash
cmake --build build --target clash-flux test_singbox test_page_transition test_persistence
./run.sh --version
git diff --check
./build/test_singbox --profile-rules
ctest --test-dir build --output-on-failure --no-tests=error -R '^(singbox|page_transition|ui_resources|persistence|personal_rule_sets_runtime)$'
```

`--profile-rules` 核对 DOM 等价性及 10 万条规则的完整性、禁用数量和兜底，并输出各档耗时。
它不启动代理或修改用户数据；真实首次进入还需结合上述阶段日志和对应平台实测。
