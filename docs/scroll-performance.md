# 通用滚动区的性能检查

## 2026-11-25 卡片选中描边流光

基于 `eff37870` 工作区与 HuxerUI `b581978c`。Fedora / XWayland / GTK `GskGLRenderer` 的 1070 × 660pt 原生窗口运行独立测量夹具。夹具调用生产 `CardHoverOutline`，由 1,600 项虚拟网格提供滚动上下文，同时切换四张可见卡片的选中态。两组使用相同的 3.2 秒滚动和每 0.8 秒切换输入，静态和流光交替各跑五轮。流光只在新选中时持续 640ms。

| 指标 | 静态四色线 | 选中时扫一圈 |
| --- | ---: | ---: |
| 每轮 GTK 绘制区间 P95 的中位数 | 5.16 ms | 5.53 ms |
| 每轮绘制间隔 P95 的中位数 | 18.43 ms | 18.33 ms |
| 绘制间隔超过 25 ms | 0 / 955 | 0 / 955 |
| 每轮进程 CPU 时间中位数 | 1,100 ms | 1,175 ms |

GTK 区间取 `before-paint` 到 `after-paint`，间隔取连续 `after-paint`。各组 960 个绘制帧，剔除每轮首帧后各有 955 个间隔样本。停止输入后均无持续空闲绘制。减弱动态效果下，同样的单次选中样本由 41 次绘制回调降到 3 次。保留一次流光，静态边框继续随保留内容绘制。测量覆盖该 Linux 图形会话与固定输入，未测 GPU 时间、移动端功耗或其它系统帧率。

## 2026-11-13 快照与范围

应用 `e6f400093a4f` 加本轮及之前未提交的工作区，v0.3.105；固定 HUI 为
`e720ae5f48690e43a5899127ace1b61645c120f4`，未修改框架。Linux / Clang 21 /
libc++ / Debug / 无窗口 UiTest。优化前后保留相同默认样式、120ms 过渡和输入距离。
不能用原始 HEAD 的二进制替代优化前基线，因为基线包含此前未提交的滚轮/箭头实现。

实现与交互约束见 [UI 开发约定](ui-development.md#滚动与留白的归属)。本轮依次移除
组合期的偏移订阅、Compact 的滚动条/hover 扩展、未改变区域状态的 hover 扫描，
再将输入观察合并到已有视口 Scope。滚动仍使用同一控制器，在下次布局前提交偏移。
宽屏的主题、语言、配置和 hover 目标变化仍可更新小范围声明；偏移变化只更新保留几何和绘制。

## 固定工作负载

- 普通正文、VirtualList、VirtualGrid 各 40 项，行高 40pt，视口为 400/800 × 300pt。
- 预热 hover 和一次 20pt 滚轮并 settle；测量 96 帧，每帧推进虚拟时间 16ms，鼠标在
  x=100/101、y=150 之间移动，每 4 帧输入一次 10pt 纵向滚轮。全程检查页面工厂未重新组合。
- 独立 nested-hover 场景只在两层嵌套滚动区内移动鼠标，测量同样的 96 帧。
- 同一次构建环境保存优化前后二进制，前后交替运行 5 轮，表中为各轮结果的中位数。
  [原始记录](evidence/2026-11-13-scroll-work.csv) 包含每轮分配调用数和帧耗时 P50/P95。

分配统计只计测量阶段当前 UI 线程的 glibc malloc/calloc/realloc 调用，包含框架和测试工具；
不代表字节数、净存活对象、所有对齐分配或其它线程。每帧耗时包含输入、BuildFrame、布局、
绘制记录、语义和 UiTest 结构捕获，**没有单独分离布局/绘制耗时，也没有实际 GPU 呈现**。
这些 Debug 结果不能推算 Release、原生 FPS、Android 真机功耗或内存峰值。

## 结果

| 场景 | 96 帧分配调用：前 → 后 | 每帧 P95 中位数（ms）：前 → 后 |
| --- | ---: | ---: |
| wheel-w400-mode0 | 277,944 → 273,912 | 4.108 → 3.988 |
| wheel-w400-mode2 | 148,796 → 144,789 | 2.404 → 2.315 |
| wheel-w400-mode3 | 245,981 → 241,927 | 4.046 → 3.893 |
| wheel-w800-mode0 | 290,808 → 287,448 | 4.333 → 4.218 |
| wheel-w800-mode2 | 161,680 → 158,330 | 2.632 → 2.519 |
| wheel-w800-mode3 | 258,877 → 255,511 | 4.236 → 4.130 |
| nested-hover | 101,952 → 98,112 | 1.487 → 1.422 |

普通/虚拟滚轮场景的分配调用减少约 1.2%–2.7%，nested-hover 减少约 3.8%；
无窗口帧处理 P95 中位数下降约 2.5%–4.4%。计时受本机负载影响，不设为 CI 耗时阈值。

单独用 GDB 计数，不采用调试器下的耗时：

| 96 帧内的工作 | 优化前 | 优化后 |
| --- | ---: | ---: |
| 宽屏单滚动区的声明更新 | 48 | 0 |
| 窄屏单滚动区的声明更新 | 48 | 0 |
| 窄屏滚动条帧回调 / 几何计算 | 96 / 96 | 0 / 0 |
| 宽屏正文内鼠标 Move 引起的 hover Refresh | 95 | 0 |
| 嵌套正文内鼠标 Move 引起的 hover Refresh | 190 | 0 |

声明更新以 ScrollRail::Extension::Update 调用计，不等同于整页组合次数；它直接覆盖
原先随偏移变化的滚动条声明协调。宽屏保留必要的滚动条帧/几何工作；进入/离开、
启用状态、溢出变化、卸载和遮挡仍会重新选择高亮目标。

## 重复检查

先完成主目标与生产 UI 测试构建：

```bash
cmake --build build --target clash-flux test_page_transition
./run.sh --version
git diff --check
./build/test_page_transition --scrollbars
./build/test_page_transition --scroll-profile
./build/test_page_transition --proxy-grid-profile
```

Linux/glibc 可选分配探针只通过 LD_PRELOAD 注入测试进程，不链接进产品，不扩大到 GUI：

```bash
cc -shared -fPIC -ftls-model=initial-exec tests/scroll_work_alloc_probe.c -o /tmp/clashflux-scroll-alloc-probe.so
LD_PRELOAD=/tmp/clashflux-scroll-alloc-probe.so ./build/test_page_transition --scroll-profile
```

没有分配探针时输出 alloc_probe=0，allocations=0 不代表没有分配。函数计数需要带调试
符号的测试二进制及 GDB；脚本每个测量区间单独计数，不将构造/预热/销毁混进计数。

```bash
gdb -q -batch -x tests/scroll_work_counts.gdb ./build/test_page_transition
```

完整 UI 回归仍运行 page_transition / ui_resources，包含中间帧同步、宽窄切换、拖动、
箭头长按与隐藏时取消、嵌套边界、输入接管、主题/语言、hover 每帧颜色及虚拟化。
原生性能优化应另在 Release 和实际设备测量，必要时再定位框架布局或绘制热点。

## 2026-11-18 代理节点网格宽屏复核

15 列代理卡片网格在框架默认半视口虚拟缓存下，滚动 96 帧产生 68,595 次卡片工厂
调用（同距离约 3.9 千），p50 1,150.6 ms；节点网格固定 `CacheExtent(200pt)` 后
降为 3,855 次、p50 401.0 ms。3 列时同样由 13,719 次降至 771 次。夹具已补齐生产
`ColumnSpacing(8pt)`，旧证据中缺参数的 15 列数字不再引用。方法、数字与边界见
[证据](evidence/2026-11-16-proxy-grid-scroll.md)。

同日继续收紧 `ProxyNodeTile` 与 `ProxiesPage`：静态卡片去掉无条件 `Lifecycle` 二次重组、外层冗余 `SelectableTile`/`Column` 包装、每卡 `ProvideEnvironment`/`ResolveFluxIndication` 分配，以及未激活测速按钮（未悬停/未获焦/未测速）的 `Tooltip`/`Indication`/`PointerCursor` 与 `Rotation` 扩展；内层文本列隐藏重复无障碍子节点，滚轮平滑过渡期间静止指针掠过卡片抑制悬停重组，节点标题改为 `SingleLineEllipsisText`（复用 `MeasureRun`/`DrawTextRun` 缓存与每线程字宽省略表，避免超长 CJK 节点名跨越虚拟网格边界时反复触发无缓存 DirectWrite `FitParagraph` 二分排版）；页面将 `CoreModel` 1Hz 轮询和悬浮测速进度隔离到独立子组件，避免流量刷新清空 `VirtualGrid` 条目声明。Windows x64 / MSVC 19.52 / Release / 固定 HuxerUI `d2c9781` 无窗口 `UiTest` 同输入对比（`./build/test_page_transition.exe --proxy-grid-profile`，含语义与快照捕获，非原生 GPU/真机帧率）：

- `w=800, cache=c200`：挂载 `672.82 → 538.38 ms`（-20.0%），每帧 P50 `34.51 → 27.54 ms`（-20.2%），P95 `38.68 → 30.62 ms`（-20.8%）。
- `w=3200, cache=c200`：挂载 `3,317.63 → 2,563.95 ms`（-22.7%），每帧 P50 `177.55 → 135.19 ms`（-23.9%），P95 `186.30 → 141.18 ms`（-24.2%）。

## 2026-11-18 日志页宽屏虚拟化与段落缓存

`LogsPage` 与 `src/ui/log_row_view.h` 使用以下设计优化宽屏日志滚动与排版稳定性：
- **流合并与稳定标识**：日志条目分配单调递增 `LogEntry::id` 作为 `VirtualList` 行键；流增量按 120ms 批处理合入。
- **平台段落排版缓存（方案 A）**：首帧未获知真实行内容宽度（`widthPx == 0`）时不按猜测宽度入缓存，直接由框架 `huxerui::Text` 排版并挂载每行共享去重（`postedPx`）的 `ObserveLogTextWidth`；获知真实宽度后通过 `TextMeasurer::CreateTextLayout` 生成 `LogParagraph`，仅当 `line_count > 0` 且测量高度不小于 `font.Size() * 0.8F`（有效高度，排除无窗口或异常 0 度量）时按 `(entryId, widthPx, fontSize, resolvedLineHeight)` 写入每 UI 线程最多 2,048 条缓存，由 `LogParagraphPaint` 调用 `PaintContext::DrawText(Point, const TextLayout&)` 绘制；无头测试或度量异常时自动回落到框架 `Text` 且不缓存无效高度。
- **结构一致与预渲染边界**：缓存命中与未命中行共用同一 `Column -> Row` 节点骨架（不包裹额外的 `huxerui::Scope`），避免后续批次刷新撕裂重建可见行；`EstimatedItemExtent` 按减半卡片内边距 ×2 + 实测单行字高 + 底部间距估算，`CacheExtent(180pt)` 控制预创建范围。

## 2026-11-24 纵向代理分组投影

基于 `3ed4df5` 加本轮前的工作区，Linux / Clang 21 / `-O2`，分别从优化前后源码
提取分组投影函数。数据为 36 组、每组 1,000 个成员、4 列。无搜索时展开 24 组，
搜索时每组命中 200 项。输入快照预先构建，前后版本交替执行 5 次，每次取 51 次投影
的耗时中位数，表中再取 5 次的中位数。分配统计为单次投影中的 `operator new` 调用。

| 场景 | 分配调用 前 → 后 | 投影耗时 前 → 后 |
| --- | ---: | ---: |
| 展开分组 | 72,999 → 1 | 2.28 ms → 约 0.001 ms |
| 跨组搜索 | 108,301 → 325 | 8.48 ms → 3.91 ms |

优化后展开只保留每组的首行位置，搜索只保存实际命中的成员索引。另用空组、尾部不满一行、
无匹配和 1/2/4/7/11 列对照组头与节点顺序，结果一致。测量范围仅为列表投影，
未包括网络、快照解码、卡片挂载、布局或原生绘制。

## 2026-11-25 纵向代理分组短滚动

基于 `3ed4df5` 工作区与 HuxerUI `b581978c`。Linux / Clang 21 / Debug 的 `build/test_page_transition --proxy-group-panel` 在 960 × 360pt 无窗口视口测试 128 行展开组。外层 Column 使正文先按宽松约束测量，再为滚动条让位后重新测量。两个版本从相同的 `ScrollTo(1650)` 请求出发，再依次发出 36pt 滚轮和 20pt 的触控板 Begin/End。用 GDB 在 `ProxyGroupRowMetrics::Measure` 和行高缓存重建处计数。

| 检查 | 修复前 | 修复后 |
| --- | ---: | ---: |
| 单次 36pt 滚轮位移 | 24.57pt | 36pt |
| 单次 20pt 触控板位移 | 36pt | 20pt |
| 单次触控板行高测量调用 | 110 | 39 |
| 单次触控板行高缓存重建 | 2 | 0 |

统计为此输入的应用层调用次数。原生帧率和真机手感未测量。

## 2026-11-25 纵向代理分组连续滚动

Linux / Clang 21 / Debug，基于 `eff37870` 和固定 HuxerUI `b581978c`。用同一份
`build/test_page_transition --proxy-group-profile` 夹具，在一个展开组内放 5,000 张节点卡，
预热鼠标悬停后每帧滚动 36pt，共 96 帧，两版最终偏移均为 3,456pt。每个宽度各运行一次。

| 视口（pt） | 节点卡工厂调用（前 → 后） | 每帧 P95（ms，前 → 后） |
| --- | ---: | ---: |
| 800 × 1,400 | 12,399 → 234 | 116.10 → 45.08 |
| 3,200 × 1,400 | 61,995 → 1,170 | 540.85 → 203.72 |

固定正文约束使分组列表按扣除边距后的宽度测量，虚拟行缓存沿用节点网格的 200pt。
滚动区悬停在几何提交时缓存溢出状态，协调视口声明时不再读取可观察的滚动指标。
回归包含悬停后的列表刷新和连续滚动，同时检查实际偏移与冻结组头。耗时包含无窗口
UiTest 的布局、绘制记录和快照处理，未测量原生 GPU 帧率或设备手感。
