# 2026-11-16 R2：应用服务与目标视图预生成

## 本轮范围

在 [R2 共享视图](2026-11-16-target-r2.md) 上接入来源接受后的后台预生成。只读页、显式编辑目录和预生成共用同一应用服务、缓存与发布检查；预览、目标编辑/保存和运行贯通尚未完成，不宣称 R2/R3 全部退出条件达成。

基线为 `f08cde6a5aee3211ac9c99dc8ecefe60c3fbcee2` 加已有 Android HTTP 修正、R0–R2 工作区；保留已有修改，未提交、打标、推送或发布。Linux / Clang 21 / libc++ / Debug；HuxerUI 固定源码 `89359f43ef34357fabcf1c994f4b5fa1062c91a9`，桌面 UI 经过 codegen，Android Java/native 使用同一源码。

转换源码没有修改，按 R2 清单的路径＋NUL＋字节＋NUL 算法，SHA256 仍为 `fd7533a653ced4a94dd4316e77b6a4c2dd049ca141e8932c401d9aea5f30e40b`。R2 应用清单加入 `CMakeLists.txt`、`src/ui/profile_rules_snapshot.{h,cpp}`、`src/ui/profile_rules_prewarm.h` 后整体排序，应用指纹为 `6e3185967f617508bd687077bb10f2ee48fbf7d4442a4cfba0d742a3c26f5fc8`。清单保存在 `/tmp/clashflux-target-r2-prewarm/source-files.json`。

## 生产接入

- `profile_rules_snapshot.cpp` 管理来源读取、目标获取、旧修改投影、目录、发布和输入同步；DTO 移到 `profile_rules_snapshot.h`。`rules_page.cpp` 只申请/引用快照，没有另一份加载器。转换核心不新增 UI、数据库或 Runtime 单例依赖。
- 沿既有 `DriveProfilesModel` 的修订检查推进预生成，不新建页面/后台轮询。已接受的导入、刷新、原文编辑和同文件记录提交进入来源代次；首次 hydrate 不把整个订阅库入队，已有格式目录就绪后只预生成默认可用来源。
- 导入资源准备期间先等待，完成后直接推进对应来源；刷新中的 ID 同样等待。页面请求与后台队列都遵守这个门控，编辑等待准备完成，失败后允许来源错误显示及显式重试。不会先用缺少依赖的中间状态填充成功视图。
- 队列只保留最小来源身份、接受版本及 ID；同一 ID 合并，最多一份后台预生成读取，忙来源不阻塞其他就绪来源。前台同 ID 请求仍走同一读取单飞；新版本、删除、ABA 和旧任务晚到在发布前核对。新版本抵达在途任务期间，只补做最新一次。
- DataSession 的多行统一选择版本不会推进来源代次；顺序/无关设置不让规则重新解析。单行记录版本无法区分用途时保守重验证，不能为了少做一次任务漏掉同文件接受刷新或已更新 provider。目标基线仍按完整内容及既有资源边界复用，记录修订不是候选批准。
- 不增加第二份强持有目标缓存。预生成共享已有 32 MiB 闲置预算，活动页按原契约固定数据；超预算结果可直接淘汰，淘汰/错误后队列结束，不自动反复重建。来源身份元数据随现存订阅数量保留、删除即清理，不包含订阅正文或目标载荷；原有候选/provider/每线程 YAML 预算与在用峰值仍按 R2 说明单独核算，不承诺进程 RSS 硬上限。
- 缺失、目录或空版本文件明确失败，不再将空读取交给编译器生成最小配置成功。只显示错误并保留记录/文件引用，不修复性清理数据库或订阅。

`rules-load` 增加来源 ID/代次；`rules-prewarm begin/finish` 记录 ID、队列数、就绪及应用累计 `reads_started`。不记录原文、规则值、URL 或凭据。日志和计数反映视图准备，不是已应用配置证明。

## 实际应用入口验证

`core_api_lifetime` 新增 `--rules-prewarm` 进程模式，通过相同应用服务、真实隔离数据库提交、不可变版本文件、Runtime 和任务线程执行；Python 驱动设置独占 HOME/APPDATA/XDG 路径。未链接完整规则页面，不将它称为真实 GUI 人工验收。

| 场景 | 实际结果 |
| --- | --- |
| 两份已保存来源 hydrate，目录就绪 | 仅默认来源启动 1 次读取，另一份不预载 |
| 预生成在途和完成后分别申请同一规则视图 | 读取总数仍为 1；首次完成后的页面请求拿到相同快照指针，不再 IO/转换 |
| 新来源已提交但资源准备中，前台同时申请 | 均等待；准备完成后只增加 1 次读取，页面打开前已有目标 |
| 刷新忙态中连续提交中间/最终版本，前台同时申请 | 队列只有 1 个 ID，旧可见快照保留；忙态结束只读最终版本 1 次 |
| 同文件、同正文、同 updatedAt 再次接受提交 | 重新准备视图，复用同一个无外部依赖目标基线；没有把同路径当成完整接受身份 |
| 运行模式与多来源选择变化 | 不增加来源读取；已有目标快照指针保留 |
| 512 字节预算下新增来源 | 预生成一次后淘汰，预算内；再次推进不循环重建 |
| 缺失/空文件 | 有错误且无目标模型，不出现空成功；补齐文件后显式重试可恢复 |
| 真实 worker 被生命周期锁阻塞时提交 A→B→A | 第一票拒绝，累计只读旧票和最新票两次，最终发布版本大于旧票；不发布中间版本 |
| 删除等待资源的排队 ID | 清除身份/待办，不启动旧来源读取 |

最后核对其余 7 条记录、原版本文件、setting 修订及已应用 state/runtimeEpoch/planRevision；除测试主动的提交、选择与模式设置外，预生成没有额外写入或运行操作。上述同文件用真实接受提交构造，未冒充新的 HTTP 304 网络回归；已有下载/资源回归继续独立验证网络和候选提交。

原 `test_page_transition --rules-cache` 继续验证应用级单飞、卸载/重挂、错误、版本/ABA、目录、固定/淘汰和输入预算。R0 冻结文件仍为 SHA256 `23946d4eb4525b832c9f31301343e0b377caa5c8cedd20f7d1c1f99bc13b346f`，独立 R0 二进制与当前消费者对 42 输入的完整观察逐字节相同，没有重录预期。

## 验证命令与边界

已执行主目标与全目标构建、`./run.sh --version`（v0.3.111）、`git diff --check`，定向 `core_api_lifetime`、`test_page_transition --rules-cache` 通过。最终完整必跑门禁 **37/37 通过，225.16 秒**，包括新应用服务回归、真实内核/资源、persistence 和 UI 组件；Android NDK 29 arm64-v8a native/Java 构建 **36 秒通过**，Java 已为最新。未打 APK、签名或发布，也未进行 Windows/macOS 本机、完整页面 GUI 人工或 Android 真机/VPN 验收。

```bash
cmake --build build
./run.sh --version
git diff --check
ctest --test-dir build --output-on-failure --no-tests=error -L clashflux-required
./build/test_page_transition --rules-cache
```

Android 从 platform/android 运行 `HUXERUI_HOME=/home/farna/.local/share/HuxerUI ./gradlew :app:externalNativeBuildDebug :app:compileDebugJavaWithJavac`。新增实现和头文件已登记 AndroidLegacy，桌面测试链接同一生产服务。没有改依赖/内核 pin，也没有把目标视图命中当成候选/内核检查通过。

预生成尚未完成、资源仍在准备或快照已淘汰时，首次打开仍可能等待或重建；特别是超预算大来源不能靠预生成承诺常驻。没有新增手机/Release 墙钟测量，没有据此宣称真机首开耗时、FPS、VPN、签名、发布或 Windows/macOS 实机通过。旧编辑投影和显式预览/运行仍需后续统一。
