# UI 开发约定

## 响应式与平台边界

- 页面结构由视口等级决定，不由操作系统决定。Compact 使用移动式页面导航，Medium/Expanded 使用桌面布局或多字段弹窗。
- 平台判断只用于能力和内容差异，例如 Android VPN、桌面系统代理、PPTP/OpenVPN 支持情况。
- 多订阅编排是桌面能力，手机保留单活动代理订阅；桌面窄窗口与手机宽布局不改变
  该限制。开发目标见[桌面订阅编排](desktop-subscription-orchestration.md)，状态、
  诊断与能力门控交互参考[官方 GUI 研究](singbox-official-gui-review.md)。
- 多字段编辑在 Compact 视口进入独立页面；简单确认操作（例如删除确认）继续使用对话框。

## 操作控件

- 页面命令优先使用带语义标签的 `IconButton`；桌面悬停必须提供说明操作含义的 `Tooltip`。
- 表达状态或模式的选项（例如“规则 / 全局 / 直连”）保留文字。
- 所有 Switch 设置项统一由三部分组成：主名称、小字描述、Switch。主名称和小字描述放在左侧列，Switch 固定在右侧并与文字列垂直居中对齐。设置页统一使用 `SettingSwitchRow`，不得在 Compact 视口改成上下堆叠。
- 小字描述允许自然换行；左侧文字列使用 Grow 保留 Switch 的固定右侧位置，不得因长描述把 Switch 挤到下一行。

## 页内标签与导航图标

- 应用 Logo 使用 `resources/images/clash_flux_logo.png` 中的黑猫探出盒子图形。桌面标题栏和 Windows 安装器将透明图放在白色底托上，以适配深色主题；Linux/Windows 任务栏应用图标使用透明背景和细白描边，Android 启动图标仍使用白底版本。桌面托盘图标由 `scripts/generate-tray-icons.sh` 从同一透明源图生成，背景保持透明，默认、TUN 和系统代理状态分别使用深蓝、青绿和紫色图形，并加细白描边以适配深色系统托盘。Android 快捷开关磁贴仍使用独立的猫爪图标。
- 桌面与移动端共用设置页主题选项，三个标签统一为“自动 / 深色 / 浅色”；“自动”按系统深浅模式选择主题。

- 二级标签共用 `src/ui/section_tabs.cpp` 的 `SectionTabBar` 与 `SectionTabPages`，代理、订阅、规则、日志页都接入。标签栏固定在 Pager 外，点击、溢出菜单与内容横滑写回同一受控选择。
- 对照 ACGU 首页 `FeedTabs` + `SwipePages`：内容使用框架 Pager 的完整出入场、跟手拖动与取消回弹；内层首尾向外滑由框架滚动事务交给外层 Pager。不要再添加 PointerIntercept 或逐页入场过渡。
- 标签保留独立警示色角标和溢出菜单，因此继续使用现有标签结构；一条由实际布局驱动的保留指示线连续移动并调整宽度。每对标签栏/内容页在所有 hook 区域调用一次 `UseSectionTabMotion()`，将同一 handle 传给 `SectionTabBar` 和 `SectionTabPages`；它从 Pager 与参与显示的页的最终呈现几何读取进度，拖动、提交与取消回弹时下划线同步跟随。两端分别使用正弦的加速/减速插值，形成前端延伸、后端收拢的效果；动画只更新保留几何与指示线绘制，不逐帧写 State 或使布局失效。首次显示直接定位，reduced motion 保留线性跟手但关闭伸缩。
- 页面根用稳定语义 Key 与有界 Column，Pager 只测量显示轨道中的页。代理组仅声明轻量 VirtualGrid，卡片按视口构建，避免无界 IndexedPages 让大组节点全量构建；切回分区保留各页滚动位置。
- 标签栏用实际标签几何计算各项居中偏移，先限制到滚动范围，再按 Pager 呈现进度在两端偏移间插值（与 Flutter TabBar 相同）。内容拖动期间提前揭示目标，松手收敛、反向和取消回弹时标签条同步跟随；选择变化、标签尺寸变化与窗口缩放重新尽量居中。首尾标签按滚动边界对齐，手动滚动保持浏览位置。滚动请求在保留扩展的帧更新中执行，不逐帧写页面 State；溢出菜单的显隐不能重挂载 ScrollView。
- `cmake/patches/huxerui-pager-retarget.patch` 修复固定 HuxerUI 的过期 drag target 与跨标签动画反向时的离场页 slot。CMake 在加入源码前幂等应用，Linux、Windows、macOS、Android 和 iOS Simulator 源码检出步骤都应用；升级时校验并移除上游已修复的补丁。已有预编译 SDK 不会由 CMake 原地打补丁，此修复需使用已修复的 SDK 或源码构建。
- `cmake/patches/huxerui-hidden-virtual-layout.patch` 阻止隐藏虚拟页的 viewport dirty 标记每帧使可见祖先失效，同时在隐藏子树保留测量失效状态供重新显示时使用。未应用时保留多个分区会持续重复测量当前网格，动画帧尤为明显；CMake 与所有 CI 源码检出步骤应用此补丁，预编译 SDK 需包含同一修复。
- `tests/test_page_transition.cpp` 直接调用生产组件，验证方向、菜单、跟手与取消、嵌套边界、自动揭示、窗口缩放、滚动保留、快速反向、reduced motion 和万节点虚拟化。
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
- HuxerUI 通用拖动预览行为通过 `cmake/patches/huxerui-drag-preview-follows-pointer.patch` 维护。改动该补丁或更新 HuxerUI 固定版本时，确认 Linux、Windows、macOS、Android 和 iOS Simulator 的源码构建步骤都应用它。
- Linux GTK 窗口显示、隐藏和关闭时的帧生命周期由 `cmake/patches/huxerui-linux-close-frame.patch` 维护，只应用于 Linux 源码构建。
- macOS 与 iOS Simulator CI 通过 `cmake/patches/huxerui-window-p0960.patch` 修复固定 HuxerUI revision 在 Objective-C++ 中的聚合初始化兼容问题；升级 HuxerUI 固定版本时确认补丁仍可应用。
- CMake 在加入固定的 HuxerUI/Lib-Camera 依赖前，会按 `cmake/patches/huxerui-lib-camera-application-context.patch` 适配 Lib-Camera 的安装钩子和各平台 factory，使其兼容 HuxerUI `ApplicationContext` API。该补丁保持在 CMake 外部依赖源码中，不改写上游 checkout。
- 当前固定的 HuxerUI Lib-Charts 没有饼图组件；Clash-Flux 通过 `cmake/patches/huxerui-lib-charts-pie-chart.patch` 给该扩展库补充 `PieChartData` / `PieChart`，并在 `huxerui_use_library()` 加入源码前应用。升级 Lib-Charts revision 时先确认补丁仍可 `git apply --check --unidiff-zero`，若上游新增等价 API 则删除补丁和对应 CMake 接入。
- 首页流量曲线卡片占 2 行。`cmake/patches/huxerui-lib-charts-compact-plot.patch` 将 Lib-Charts 绘图面的最小高度降至 96pt，以容纳卡片内边距和标题；CMake 在加入依赖前应用此补丁，升级固定 revision 时需重新校验。
- 饼图通过 `PieChartData` 保存稳定 key、标签和值，`PieChart` 使用显式帧约束；标签与数值放在普通 HuxerUI Views 中，图形本身提供图像语义摘要。
- iOS CI 使用固定 HuxerUI revision 构建未签名 Simulator app/Packet Tunnel，并用与 Android 相同的固定 sing-box revision 生成 iOS device/Simulator `Libbox.xcframework`。它不生成 IPA 或 Release 资产；订阅下载由 Clash-Flux iOS `URLSession` 桥接，默认校验证书，按订阅选项显式允许无效证书。签名设备包和真机 VPN 生命周期验证完成前，iOS Release 仍不纳入门禁。

HuxerUI revision 更新到上游最新后，先对照每个维护补丁的变更；上游已包含的改动要移除补丁和 CI 应用步骤。保留的补丁必须在最新源码 checkout 上通过 `git apply --check --unidiff-zero`，并让所有适用平台继续使用同一个固定 SHA。

## 改动后的构建验证

每次修改完成后，无论改动大小、涉及何种文件（代码、UI、脚本还是配置），先重新
编译，再运行版本命令验证：

```bash
cmake --build build --target clash-flux
./run.sh --version
```

不得跳过本地编译，也不得只依赖旧的可执行文件判断改动有效。使用其他构建目录时，
用 `CLASHFLUX_BIN` 显式指定对应产物；需要确认 GUI 启动或交互时，再执行 `./run.sh`。
