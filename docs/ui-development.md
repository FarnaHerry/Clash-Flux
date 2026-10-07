# UI 开发约定

领域模块与普通 UI 头文件的 inline 规则、优化配置和实际验证案例见
[C++ 模块开发与内联性能](cpp-modules-development.md)。UI composable 不因该规则添加 inline。

## 响应式与平台边界

- 页面结构由视口等级决定，不由操作系统决定。Compact 使用移动式页面导航，Medium/Expanded 使用桌面布局或多字段弹窗。
- 平台判断只用于能力和内容差异，例如 Android VPN、桌面系统代理、PPTP/OpenVPN 支持情况。
- 多订阅编排是桌面能力，手机保留单活动代理订阅；桌面窄窗口与手机宽布局不改变
  该限制。开发目标见[桌面订阅编排](desktop-subscription-orchestration.md)，状态、
  诊断与能力门控交互参考[官方 GUI 研究](singbox-official-gui-review.md)。
- 多字段编辑在 Compact 视口进入独立页面；简单确认操作（例如删除确认）继续使用对话框。
- Android 在 `AppOptions::window.content_mode` 启用 `EdgeToEdge`，由 `AndroidAppContent`
  用 `IslandTheme::base` 绘制完整视口并统一消费 `SafeAreaPadding`。系统栏的
  `SystemBarsAppearance` 使用同一底色，图标明暗自动跟随；一级页、二级页和底部导航
  不重复增加安全区留白，不在 Java 或各页硬编码状态栏高度与颜色。

## 操作控件

- 文件选择器的筛选名称等本地化字符串在组合期解析，再通过拥有型 DTO 传给点击回调和异步任务。
  不在协程中调用 `UseString()`；选取取消或失败保留原表单与订阅，恢复导入状态，异常用 toast 反馈。

- 日志标题栏只提供竖向三点菜单：级别子菜单标记当前选择，导出子菜单提供剪贴板与系统文件导出，
  清空沿用两个日志来源及其待消费队列的清理。导出使用当前来源、当前级别的可见日志快照，
  保留时间戳与顺序；文件准备在任务线程完成，独占临时文件由 RAII 清理。日志专用弹出菜单通过
  `ViewEvents::Hover` 的 Enter 事件展开子菜单，保留点击展开与取消关闭，不修改框架。
  所有面板使用 `ClipChildren` 按面板圆角裁剪菜单项绘制及命中，避免 hover 填充越界；
  每个菜单项也使用菜单主题的圆角，使各项 hover 高亮各自圆角化；
  面板四边在主题原有内边距上增加 `spacing.extra_small`（默认 4pt），项间增加其一半（默认 2pt）；
  清空菜单项使用项目统一的 `theme.colors.error` 危险文字色。

- 语言使用下拉菜单，具体语言显示固定原名（简体中文、English），不随当前界面语言翻译；
  自动选项跟随界面语言。主题使用自动、深色、浅色三个图标选项，并保留无障碍标签。
- 桌面“启用托盘”统一控制关闭按钮：启用且系统托盘可用时隐藏到托盘，否则询问是否退出；
  确认框只有关闭与取消。旧 `tray.close_behavior` 设置不再读取，不删除用户持久化数据。

- 桌面 Medium/Expanded 使用平面布局和分层底色，左侧 Logo 与导航组成完整一栏，右侧为标题与内容一栏；竖线贯穿两栏，
  横线只分隔右侧标题栏与内容。页内区块、订阅和代理条目恢复卡片底色、圆角与
  选中填色，不增加条目底部或分区标题分隔线。手机保留现有卡片；按钮、输入框和浮层仍使用各自控件样式。

- 桌面 Medium/Expanded 的 `PageScaffold` 直接承载本页的 `WindowTitleBar`：页名在左、动作按钮在右，
  动作紧邻原生最小化按钮左侧，保留平台窗口控件预留区。内容不再保留重复标题或动作行；
  页内动作仍在原页面的组合状态作用域中挂载。左侧 Logo 栏与标题栏共用
  `kDesktopTitleBarHeight`，右侧标题与内容之间保留横线。手机标题与二级页返回栏不变。
- 顶部内容留白与一级侧栏在 Logo 栏下方的留白共用 `kDesktopTopContentGap`；
  标题水平留白共用 `kDesktopPageHorizontalInset`。修改共用量，不给各页单独加偏移。

- 页面命令优先使用带语义标签的 `IconButton`；桌面悬停必须提供说明操作含义的 `Tooltip`。
- 表达状态或模式的选项（例如“规则 / 全局 / 直连”）保留文字。
- 所有 Switch 设置项统一由三部分组成：主名称、小字描述、Switch。主名称和小字描述放在左侧列，Switch 固定在右侧并与文字列垂直居中对齐。设置页统一使用 `SettingSwitchRow`，不得在 Compact 视口改成上下堆叠。
- 小字描述允许自然换行；左侧文字列使用 Grow 保留 Switch 的固定右侧位置，不得因长描述把 Switch 挤到下一行。

## 页内标签与导航图标

- 应用 Logo 使用 `resources/images/clash_flux_logo.png` 中的黑猫探出盒子图形。桌面标题栏和 Windows 安装器将透明图放在白色底托上，以适配深色主题；Linux/Windows 任务栏应用图标使用透明背景和细白描边，Android 启动图标仍使用白底版本。桌面托盘图标由 `scripts/generate-tray-icons.sh` 从同一透明源图生成，背景保持透明，默认、TUN 和系统代理状态分别使用深蓝、青绿和紫色图形，并加细白描边以适配深色系统托盘。Android 快捷开关磁贴仍使用独立的猫爪图标。
- 桌面与移动端共用设置页主题选项，三个标签统一为“自动 / 深色 / 浅色”；“自动”按系统深浅模式选择主题。

- 二级标签共用 `src/ui/section_tabs.cpp` 的 `SectionTabBar` 与 `SectionTabPages`，代理、订阅、规则、日志页都接入。标签栏固定在 Pager 外，点击、溢出菜单与内容横滑写回同一受控选择。
- 对照 ACGU 首页 `FeedTabs` + `SwipePages`：内容使用框架 Pager 的完整出入场、跟手拖动与取消回弹；内层首尾向外滑由框架滚动事务交给外层 Pager。不要再添加 PointerIntercept 或逐页入场过渡。
- 标签保留独立警示色角标和溢出菜单，因此继续使用现有标签结构；一条由实际布局驱动的保留指示线连续移动并调整宽度。每对标签栏/内容页在所有 hook 区域调用一次 `UseSectionTabMotion()`，将同一 handle 传给 `SectionTabBar` 和 `SectionTabPages`；它从 Pager 与参与显示的页的最终呈现几何读取进度，拖动、提交与取消回弹时下划线同步跟随。两端分别使用正弦的加速/减速插值，形成前端延伸、后端收拢的效果；标签文字的普通色与主色也按同一实际进度线性混合：离场标签逐渐淡出，目标标签逐渐增强，取消或反向拖动沿原进度恢复；警示角标保持语义警示色。文字仍由普通 Text 测量并提供语义，保留扩展只绘制渐变颜色。动画只更新保留几何与标签前景绘制，不逐帧写 State 或使布局失效。首次显示直接定位，reduced motion 保留线性跟手但关闭伸缩。
- 页面根用稳定语义 Key 与有界 Column，Pager 只测量显示轨道中的页。代理组仅声明轻量 VirtualGrid，卡片按视口构建，避免无界 IndexedPages 让大组节点全量构建；切回分区保留各页滚动位置。
- 分页视口占满页面骨架可用宽度，水平内容边距放进每个分页，与内容一起移动。对照 [FlClash 的代理分组实现](https://github.com/chen08209/FlClash/blob/main/lib/views/proxies/tab.dart)：`TabBarView` 占满宽度，每页网格设置左右 16 的 padding，卡片间距另由网格设置。我们的四种二级标签页启用骨架的 `fullWidthSections`：骨架通过 `SectionTabContentInsets` 提供边距，标题和标签条也用相同值；`SectionTabPages` 将其一半应用在每个稳定页根上。代理/订阅网格的行列间距与分页边距均由 `kSectionCardSpacing`（8pt）计算，页面每侧留 4pt，滑动中的两页边距合成 8pt，恰好等于卡片间距。Pager 外侧不加水平 Padding，翻页步长始终是完整视口宽度，形成连续的卡片节奏。
- 标签栏用实际标签几何计算各项居中偏移，先限制到滚动范围，再按 Pager 呈现进度在两端偏移间插值（与 Flutter TabBar 相同）。内容拖动期间提前揭示目标，松手收敛、反向和取消回弹时标签条同步跟随；选择变化、标签尺寸变化与窗口缩放重新尽量居中。首尾标签按滚动边界对齐，手动滚动保持浏览位置。滚动请求在保留扩展的帧更新中执行，不逐帧写页面 State；溢出菜单的显隐不能重挂载 ScrollView。
- Pager 反向切换修复已合入 HuxerUI fork 的 `farna/main`：清除过期 drag target，保留跨标签反向动画的离场页 slot。应用使用固定集成 SHA，不再在 CMake 或 CI 打框架补丁。
- 隐藏虚拟页布局修复同样由 `farna/main` 维护：viewport dirty 留在隐藏子树，重新显示时恢复测量，避免逐帧使可见祖先失效。
- `tests/test_page_transition.cpp` 直接调用生产组件，验证分页全宽、拖动中的相邻内容间距、方向、跟手与取消、嵌套边界、自动揭示、窗口缩放、滚动保留、快速反向、reduced motion 和万节点虚拟化。
- 所有一级导航共用 `src/ui/app.cpp` 的 `kNavigationEntries`，每个条目只定义一个线条图标。桌面侧栏与手机底栏选中时保持同一轮廓与线宽，仅改变内容颜色和选中指示器，不配置填充版选中图标。
- 代理导航使用地球网络图标 `proxies.svg`，连接导航使用链环图标 `connections.svg`；日志使用纸张图标 `logs.svg`，规则使用分流路径图标 `route.svg`。

## Windows 后台操作与托盘菜单

- Windows CI 使用 runner 预装的 OpenSSL（`Program Files/OpenSSL`），配置 CMake 前验证可执行文件与开发头文件；打包时从相同安装目录复制运行时 DLL。
- 启动、轮询和退出清理中的系统操作不能闪出命令行窗口。优先使用 Win32 API；确实需要子进程时，用 `CreateProcessW` 的 `CREATE_NO_WINDOW` 并重定向标准句柄。不要在后台 Windows 路径使用 `std::system` 或 `_popen`。
- 只有需要用户明确授权的流程可以显示系统交互提示，例如 UAC 提权。普通后台操作、失败清理和状态探测都保持安静执行。
- HuxerUI 的 Windows 系统托盘菜单是原生菜单，`SystemTrayOptions` 不提供 `MenuStyle` 参数。桌面托盘生命周期按当前应用 `ThemeSpec` 同步 Windows 原生菜单主题；Windows 10 build 17763 及以上支持此同步，旧系统保留系统原生菜单主题。修改 HuxerUI 或 Windows 平台适配时，保持托盘菜单与应用深浅模式一致。

## 可拖动仪表盘卡片

- 拖动反馈层保持卡片原尺寸，并持续跟随按下时的抓取点；不要复用会在视口边缘翻转或夹位的弹出层放置规则。
- 排序由卡片布局几何决定，原位置保留半透明占位；滚动视口注册宽域拖放目标，让卡片间隙和视口边缘仍能接收拖动并触发边缘自动滚动。
- HuxerUI 通用拖动预览、Linux 窗口帧生命周期和 Objective-C++ 聚合初始化兼容修复已合入 `farna/main`，由同一固定 SHA 覆盖所有平台。维护与升级见 [HuxerUI fork](huxerui-fork.md)。
- Camera 的 ApplicationContext/UiWindow 兼容、Charts 的 `PieChartData` / `PieChart` 与 96pt 绘图面高度、SQLite 的启动只读查询均由各自 `FarnaHerry` fork 的 `farna/main` 维护。应用原生构建与 Android 库图共用 CMake 固定的 URL/COMMIT，不再在配置时修改依赖源码。升级记录与固定版本见 [HuxerUI fork](huxerui-fork.md)。
- 饼图通过 `PieChartData` 保存稳定 key、标签和值，`PieChart` 使用显式帧约束；标签与数值放在普通 HuxerUI Views 中，图形本身提供图像语义摘要。
- iOS CI 使用固定 HuxerUI revision 构建未签名 Simulator app/Packet Tunnel，并用与 Android 相同的固定 sing-box revision 生成 iOS device/Simulator `Libbox.xcframework`。它不生成 IPA 或 Release 资产；订阅下载由 Clash-Flux iOS `URLSession` 桥接，默认校验证书，按订阅选项显式允许无效证书。签名设备包和真机 VPN 生命周期验证完成前，iOS Release 仍不纳入门禁。

HuxerUI 更新先在独立维护仓库 merge 官方 main 到 `farna/main`，审查差异并验证框架与应用后再更新应用固定 SHA；官方 main 保留为对照基线。Lib-Charts、Lib-SQLite、Lib-Camera 在各自 fork 中采用同一流程，各平台和 Android Java/native 共用固定 SHA。

## 改动后的构建验证

每次修改完成后，无论改动大小、涉及何种文件（代码、UI、脚本还是配置），先重新
编译，再运行版本命令验证：

```bash
cmake --build build --target clash-flux
./run.sh --version
```

不得跳过本地编译，也不得只依赖旧的可执行文件判断改动有效。使用其他构建目录时，
用 `CLASHFLUX_BIN` 显式指定对应产物；需要确认 GUI 启动或交互时，再执行 `./run.sh`。

## 启动设置快照

`AppRoot` 在首帧构建前通过 `persistence::readStartupSettings` 同步读取必须先于设置 hydrate 确定的
`ui.theme_mode`、`ui.theme_color`、`ui.language`、`tray.enabled` 与 `tray.start_minimized`。
它们控制首帧外观、初始语言、托盘显示和初始窗口可见性，避免加载设置前短暂呈现错误状态。
快照只读取一次，重组不重复磁盘 IO；SettingsModel 就绪后以模型值接管运行期行为。
该 API 使用临时只读 SQLite 查询，兼容迁移前旧库；不存在的数据库不创建，损坏或无法读取的数据库明确
报告失败并停止启动。其它设置仍按原异步流程 hydrate；启动内核等操作等待 SettingsModel ready，
不为它们扩大同步读取范围。
Lib-SQLite 的同步只读 API 已合入 `FarnaHerry/Lib-SQLite` 的 `farna/main`，
各平台通过 CMake 获取同一固定 SHA，不能只改本地第三方源码。回归覆盖三种模式、首次启动、旧库、
损坏库保留与原有多订阅迁移/重开保护；构建和自动化回归不代表真机首帧验收。

主题页「模式」使用横向排列的三个独立图标加文字卡片（自动/深色/浅色），「主题色」使用整卡色块选择，
不显示颜色名称或色值，保留居中的对钩与无障碍名称。
模式、色块和添加入口共用 `theme_color_card.h` 的正方形表面、圆角、居中图标和交互样式；
Compact 使用较小的共用边长，三种模式仍保持横向一行；模式卡片保留图标与文字。
间距、圆角、图标尺寸、选中描边与切换时长取 ThemeSpec，页面特有尺寸集中为命名常量。`ui.theme_color` 使用稳定的 blue/purple/green/orange/pink/teal ID，
自定义颜色使用规范化的 `#RRGGBB` 作为 ID，`ui.custom_theme_colors` 按换行保存完整列表；
输入校验、去重和 HSB/HEX 转换集中在 `theme_colors.h`，缺省或未知值显示品牌蓝色。
主题色卡片用 Flow 自动换行，添加弹窗提供 HSB（色相/饱和度/亮度）滑块、HEX 输入与实时预览；保存后即时选中。
自定义颜色按深浅模式调整亮度，并选择对比度足够的按钮文字色。预设配对色集中在 `theme_colors.h`，切换通过 SettingsModel
即时写透；主题色更新 primary/容器前景与焦点颜色，深浅模式共用选择且保留中性背景层级。

## 通用操作菜单

应用内同类弹出操作菜单统一使用 [action_menu.h](../src/ui/action_menu.h)，当前日志菜单、
二级标签溢出菜单、订阅卡片操作菜单、主题色块右键菜单已接入。菜单项圆角、hover/press、禁用、勾选、图标、
危险色、面板裁剪和留白都在该控件维护。长菜单使用框架 ScrollView，在浮层约束内滚动。

```cpp
auto menu = UseActionMenu(); // 每个独立入口各持有一个 handle
Button("操作").With(menu.Anchor()).OnClick([menu] {
    menu.Show({
        ActionMenuItem("更新", [] { /* 更新 */ }),
        ActionMenuSection{},
        ActionMenuItem("删除", [] { /* 进入确认流程 */ }).Danger(),
    });
});
```

`Show()` 使用稳定入口锚点，右键坐标入口使用 `ShowAt(windowPoint, entries)`。点击菜单项
先关闭面板，再调用操作；危险标记仅控制视觉，删除确认由调用方处理。描述可在事件中构建，
所有样式及本地化解析在面板 Scope 内完成，不能在事件或任务中调用组合期 API。

自定义多级 hover 菜单（日志级别/导出）直接共用 `ActionMenuItemView` 和
`ActionMenuSurface`，子面板的生命周期和关闭动作由调用方管理；不要复制容器与菜单项样式。
系统托盘是平台原生菜单，继续使用框架 MenuItem；Select 等框架输入控件不替换为这个组件。
回归使用生产控件，覆盖日志 hover 子菜单、取消/外部关闭、标签选择，以及通用菜单禁用、
勾选、锚点/坐标打开和长菜单末项滚动执行。

## 代理分组抽屉

代理页传入 `SectionTabPickerMode::ResponsiveGroups`，标签溢出时的箭头打开分组抽屉。
方向使用框架响应式分类：Compact 从底部打开（框架 BottomSheet），Medium/Expanded 从右侧
打开（窗口级 LayerController，360pt 宽、内容区高度）；不以编译平台决定方向。其它页面的标签选择
仍使用通用操作菜单。窗口安装 `InstallSectionPickerLayers`，不要在组合期重复注册层服务。

交互参考 [FlClash 分组选项](https://github.com/chen08209/FlClash/blob/main/lib/views/proxies/tab.dart)
与 [响应式 sheet](https://github.com/chen08209/FlClash/blob/main/lib/widgets/sheet.dart)：分组使用可换行
卡片，选中高亮，选择后关闭并写回同一受控分页索引。我们的保真度角标保持警示色。长内容
由框架 ScrollView 滚动；关闭按钮、透明外部点击区域、Escape/系统返回都关闭当前抽屉，减少动态效果时
关闭进入/退出运动。右侧运动使用保留 Offset 补间，不逐帧更新 State 或内容布局。

自动化回归覆盖宽屏右侧几何、窄屏底部几何、窗口缩窄后改变方向、选择与标签同步、角标、
外部点击关闭、标题栏仍可点击、取消键、右侧进入中间帧及矮窗口滚动至末项；这些检查不代表手机真机返回键验收。

代理分组抽屉不叠加遮罩颜色；右侧抽屉通过 `SectionTabPickerInsets` 从桌面标题栏分割线下方开始，标题栏不被抽屉覆盖。底部抽屉仅局部覆盖 `BottomSheetStyle.scrim` 为透明，保留其它模态组件的样式。

桌面窗口最小尺寸为 320 × 480 逻辑像素（`src/app.cpp` 的 `minimum_size`），默认启动尺寸仍为 1080 × 720。允许缩到手机竖屏尺寸并进入 Compact 视口；平台能力仍由平台判定，不能通过窗口宽度改变订阅编排权限。

桌面 Compact（宽度小于 600）由 `responsive_shell.h` 切换为四项底部导航与页内标题；独立 40pt 窗口标题栏保留拖动和系统窗口控件。规则/连接/日志通过设置「更多」进入，返回设置时恢复底部导航。内容容器 Key 与树路径保持稳定，越过断点不重挂载页面。桌面订阅编排、系统代理等能力不因窗口缩窄改变。

订阅页在 Compact（手机与桌面窄窗）将「新建订阅」加号放在 `PillSearchField` 胶囊内部最右侧；有搜索内容时，清空按钮位于加号左侧。搜索框的外层 Scope 显式声明 Grow，Compact 搜索标题行不放 Spacer，避免两者平分剩余宽度；搜索表面占满标题行可用宽度，保留骨架统一的两侧留白。无批量选择时不再保留独立的动作行，批量连接操作仍在搜索栏下方；Medium/Expanded 的加号继续位于窗口标题栏。入口复用同一个新建操作，不改变平台订阅能力或导入流程。新建订阅与各添加方式的配置表单复用二级页面骨架，返回图标、居中的标题与右侧导入按钮同处顶行；新建/添加表单的左右动作使用等宽 40pt 槽位，导入中在右侧同一槽位显示 loading，不在表单底部重复导入按钮，返回键与图标使用同一返回动作。`test_page_transition` 的生产标题布局回归检查 320/420pt 搜索表面的实际宽度，以及普通/加载状态的标题中心。

Compact 胶囊导航共用 `CompactNavigationDock` 悬浮于页面内容之上，桌面窄窗与手机保持相同的最大宽度、圆角和边距。底色由 `CompactNavigationSurfaceColor` 从卡片角色 `surface_container`（`IslandTheme::raised`）派生，深浅主题都比页面 base 层更亮，Alpha 为 0.78；不对整个导航施加 Opacity，文字、图标及滑动选中指示仍不透明，不增加背景模糊。导航最大宽度为 352pt，四个槽位等分可用宽度；选中胶囊宽度为槽位宽度减 12pt、最多 76pt，高度 50pt，垂直居中于 64pt 导航行。窗口缩放直接重新定位，首帧不播放入场动画。图标与文字保留普通色底层，选中色覆盖层通过框架 Opacity 在 0.2 秒内渐变；点击导航时选中胶囊使用与 hui-test 相同的弹簧（stiffness 240、damping_ratio 0.78）轻微越过目标后回弹，横滑翻页提交后使用 0.2 秒缓出配合 Pager 收敛。反向改选由保留的 MotionController 从当前位移与速度继续，减少动态效果时直接切换。各导航项显式使用空 `Indication` 覆盖 `OnClick` 默认指示层，不显示悬停、按压或点击波纹，点击仍切页并移动选中胶囊。滚动内容保留 `CompactFloatingNavigationFooter`，末项可滚到胶囊上方；首页启动按钮在窄窗避开导航。

## 空状态占位

页面、分区与列表编辑区域无内容时统一使用 `src/ui/empty_state.h` 的
`EmptyState(message, icon)`，不自行拼接 Text/Column 或套空卡片。
组件在可用内容区域内水平、垂直居中：48pt 语义图标、12pt 图文间距、统一 Body 字号和
`on_surface_variant` 颜色；四边留白 24pt，文字内容最大宽度 360pt、居中且自然换行。
窄屏遵循实际可用宽度，有界区域使用 Grow，滚动内容或弹窗可由调用方指定高度。
现有提示文字及页面动作入口保留；首页卡片中的字段缺省值、选择器帮助说明和菜单禁用项
不属于空页面占位。组件用 Scope 延迟读取主题，供弹窗与懒加载工厂安全复用。

内容与外框统一使用主题层级：标题栏、一级侧栏与窗口外框用 `colors.background`，
`PageScaffold` / `SecondaryPageScaffold` 的内容区域用 `IslandTheme::base`
（`surface_container_low`），页内卡片沿用 `raised`（`surface_container`）。
深浅模式都遵循「外框较亮 → 内容区较暗 → 卡片再次提亮」；Compact、宽屏和二级页共用此规则，
不在页面分别硬编码颜色，不新增外层卡片或阴影。

浅色层级：外框 `#FAFBFC`、内容 `#EFF1F4`、普通卡片 `#FDFDFD`、未选中可选卡片 `#FAFBFC`。深色层级：外框 `#1A1B1C`、内容 `#141516`、普通卡片 `#1E1F20`、未选中可选卡片 `#242526`。选中卡片保留品牌色。

## 主题颜色审查（2026-10-04）

颜色集中配置入口为 `src/ui/theme_colors.h`：`FluxDarkColors()` / `FluxLightColors()`
定义 ColorScheme，成功/警告语义色也集中在该文件。`FluxThemed` 只将角色色映射到组件样式，
页面从 `UseTheme()` 读取角色色；阴影统一通过 `ThemeShadowColor(theme, opacity)`
读取当前主题 scrim 色相，删除按钮文字用 `OnErrorColor(theme)`，危险按钮继承当前 ButtonStyle。
Logo 容器底色使用 surface，不再固定白色。页面代码不再直接写 RGB/白色/黑色。

明确保留的例外是 `profiles_page_support.cpp` 的二维码绘制：固定白底、近黑模块用于扫描对比，
属于编码图像而非界面主题。PNG/SVG Logo、托盘及启动图标是资源素材，遵循各自平台图标约定。
布局尺寸、动画时长、交互透明度及文字角色仍是控件参数；本次没有将所有几何常量改成颜色变量。
主题切换仍由 SettingsModel 写透驱动，首帧同步主题读取保持原逻辑。

普通单色 SVG 由 Image.Tint 或 IconButton/NavigationPane 的主题前景色着色；源码中黑色路径不等于运行时固定黑色。回归 `page_transition` 增加已挂载 EmptyState 的浅色→深色→浅色点击切换，检查实际文字绘制颜色。

## 界面 Logo 深浅版本（方案 2 保留）

界面 Logo 按当前 ThemeSpec 切换：浅色继续使用原 `resources/images/clash_flux_logo.png`，
深色使用新增 `resources/images/clash_flux_logo_dark.png`（浅灰透明 PNG）。移除 Logo
后面的表面色底板，保留相同尺寸、布局与原图。深色图由 imagegen 基于原图生成，
提示为仅将黑猫/盒子形状改为浅灰、保留透明眼孔和轮廓；它是独立资源，不覆盖原图。
两张方案预览继续保留在生成目录；应用/启动图标与托盘资源不跟随此次 UI 切换修改。

当前界面切换为方案 3：浅色使用 `resources/images/clash_flux_logo_outline.png`，
黑猫/盒子保持暗色，边缘加浅灰描边、背景透明。该资源由 imagegen 基于原图生成，
提示为仅增加细浅灰描边、保留形状和透明间隙。原 `clash_flux_logo.png`、方案 2 的
`clash_flux_logo_dark.png` 和两个预览均保留，不覆盖应用图标、启动图标或托盘素材。

深色内部填色试验资源 `resources/images/clash_flux_logo_outline_dark.png` 保留，但不再用于
界面：实际小尺寸截图显示生成描边含杂点，细线缩小后难以辨认；白猫与白色圆形底托
也未采用。深色模式试用手工绘制的 `clash_flux_logo_vector_dark_refined.svg`：黑猫与盒子为
暗色实心路径、连续浅色轮廓与不透明浅色眼睛，没有底托。路径简化以适配 32pt 小尺寸，
不是原 PNG 的逐像素描摹；SVG 保留多色填充，不使用整体 Tint。浅色使用相同路径的
`clash_flux_logo_vector_light_refined.svg`：黑猫、盒子与接近浅色背景的轮廓及浅色眼睛。
深浅模式均为透明背景 SVG、32pt，相同 viewBox 和路径保证切换时形状与大小一致。
随 ThemeSpec 自动切换，原图、生成试验与方案 2 资源均保留。

矢量初版 `clash_flux_logo_vector_dark.svg` 保留用于对比。精细版调整耳尖、脸颊和尾巴的
贝塞尔曲线、盒子面板的真实折叠边缘与微圆角，描边从 3.2 缩至 2.6 个坐标单位，
减少中心接缝拥挤；各路径有独立 ID，便于在矢量编辑器内继续调整。

HuxerUI 的 `Image.Tint` 仅支持矢量图，PNG 调用会在启动时抛出异常。
位图 Logo 通过选择对应资源适配主题，不能套用单色 SVG 的 Tint 路径；必须实际启动 GUI 验证。

2026-10-04：订阅 URL 导入结果不能由表单所属的短生命周期 TaskScope 持有。
`profile_import_task.h` 的 `LaunchProfileImport` 使用应用任务域，让后台已完成的持久化
导入在页面重建/卸载后仍有结果回传；先统一清除 busy，再调用成功或失败回调，异常
转为失败结果，重复点击不会创建第二个任务。桌面导入弹窗、独立 URL 表单及 URL
快捷入口使用该生产封装。选文件/相机/导航动画保持原页面生命周期。
`test_page_transition` 增加视图卸载后的完成、异常结束 loading 和失败后重试回归。
本次根据正在运行的 GUI 成功订阅行与后台线程空闲定位完成阶段；没有删除或重导入
用户已有订阅。正在运行的旧 GUI 需要重启才会使用重编译后的回调。
