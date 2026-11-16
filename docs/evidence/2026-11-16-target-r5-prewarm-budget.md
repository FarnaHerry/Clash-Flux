# R5 预生成预算复测

2026-11-16，`6b00957` 加 R3–R5 和预生成预算改动，应用 v0.3.111，HuxerUI `89359f43`。Fedora 45 / WSL2、Clang 21、Fedora 44 的 libc++ 22、Linux Release。改动前后的应用在同一台机器用同一隔离 Xvfb、软件 OpenGL 和运行库测量。每个应用、规模和点击时机各启动 5 次，共 60 次。订阅由生产导入入口保存，应用重启后点击真实规则页。运行方法沿用 [首次打开测量](2026-11-16-target-r5-first-open.md#复现与验证)，运行器参数为 `--rows 10000 50000 --samples 5`。

原文为 352,549 / 1,805,879 字节，完成的规则快照记账为 14.78 / 66.85 MiB。预生成在任务线程查询文件大小，以默认 1 MiB 为经验门槛。完整读取和转换的次数来自 `rules-load` 日志，来源列表的短前缀识别另走既有入口。

| 源规则数 | 首页出现后点击 ms | 点击到首批规则中位数 ms，改动前 → 后 | 完整读取次数，改动前 → 后 | 峰值 RSS 中位数 MiB，改动前 → 后 |
| ---: | ---: | ---: | ---: | ---: |
| 10,000 | 0 | 98 → 97 | 1 → 1 | 235 → 242 |
| 10,000 | 250 | 44 → 46 | 1 → 1 | 241 → 244 |
| 10,000 | 1,000 | 36 → 44 | 1 → 1 | 248 → 248 |
| 50,000 | 0 | 445 → 501 | 1 → 1 | 320 → 315 |
| 50,000 | 250 | 152 → 417 | 1 → 1 | 329 → 326 |
| 50,000 | 1,000 | 408 → 415 | 2 → 1 | 415 → 330 |

- 1 万条样本在 250 ms 和 1 秒点击的 10 次中均复用已发布的预生成快照。
- 5 万条样本在改动后 15 次均等待页面请求才开始完整读取。等 1 秒打开的 5 次均省去一次完整读取和转换。旧版在 250 ms 点击时会复用已进行一段时间的预生成，延迟加载使转换等待移到了点击后。
- 数据来自合成订阅和隔离窗口。文件大小仅估算预生成成本，无法预测所有来源及 provider 展开后的内存。大型来源的按需加载仍重建内存快照。真实订阅、冷磁盘、其他平台性能和生产磁盘缓存仍待验证。

历史二进制 SHA-256 为 `9972ab5a4d7ce941a5426b8cd1836f4040eb991bd0e426dd6a1d92b494b7c674`，新二进制为 `895b2918a1bb5eb77f275b40649603e591c96ac6ee3f3f1becb163543fb65f16`。[原始 CSV](2026-11-16-target-r5-prewarm-budget.csv) 的 SHA-256 为 `c432911c940780a36174ab89146ad19c1f794c666f79fd3492b4a6c0f3520099`。

## 回归检查

- Windows MSVC Debug 和 Linux Release 的 `cmake --build <构建目录> --target clash-flux`、设置 `CLASHFLUX_BIN` 后的 `run.ps1 --version` 或 `run.sh --version` 均通过，输出 v0.3.111。`git diff --check` 通过。
- Windows 的 `test_singbox --target-project`、`test_page_transition --rules-cache` 及 `ctest --output-on-failure --no-tests=error -R '^(core_api_lifetime|persistence|ui_resources)$'` 对应的三项用例通过。Windows 完整 `page_transition` 在 657 秒时结束等待并停止进程。
- Linux Release 通过 `test_page_transition --rules-cache` 和 `ctest --output-on-failure --no-tests=error -R '^(page_transition|ui_resources|core_api_lifetime)$'` 的全部三项用例。`core_api_lifetime` 的真文件/数据库回归覆盖延后读取、前台请求与在途大小查询、版本竞争、实际闲置预算及旧快照保留。
