---
name: clashflux-ui
description: 修改或评审 Clash-Flux UI、布局、分页、共享模型、主题、字符串、菜单、首页动画或导入交互时使用。
---

# UI 开发

所有 UI 改动先读 [生命周期与模型](references/lifecycle.md)；再按实际改动读取 [UI 开发约定](../../docs/ui-development.md) 的对应章节，不加载无关平台文档。
composable 体内不能使用条件编译，沿用现有 DSL；UI 经过 HuxerUI codegen 和目标构建，不直接改生成文件。

## 按改动查阅

| 改动 | 必读位置与行为 |
| --- | --- |
| 响应式 / 搜索 / 标题栏 / 一级导航 | UI 文档“响应式与平台边界”“操作控件”。桌面样式默认不改手机；桌面非 Compact 才限宽居中，手机（含宽视口）和桌面 Compact 保留原结构，手机搜索高 48pt；回归实际高度、下方内容和输入/清空/添加。 |
| 二级页面 / 标签 / 横滑 | UI 文档“页内标签与导航图标”及手机二级页约定：手机二级页覆盖底栏，返回箭头/系统返回/手势共用返回直接父级的动作，push/pop 独立于一级动画，保留父页状态。 |
| 卡片 / 主题 / 阴影 / 拖放 | UI 文档“圆角阶梯与密集卡片”“主题颜色审查”“可拖动仪表盘卡片”；下文补充不可丢失的联动约束。 |
| 空状态 / 菜单 / 抽屉 | UI 文档“空状态占位”“通用操作菜单”“代理分组抽屉”；复用 EmptyState、action_menu、section_tab_picker，不在页面复制实现。 |
| 大文本查看 / 编辑 | UI 文档“大文本查看与编辑”；统一 TextDocument / DocumentTextView，工作线程预构建，文档独占 UI 挂载，按需快照，不逐键复制全文。 |
| 文案 / 图像 | UI 文档“界面文案与字符串资源”“正式 Logo”；使用下面的资源规则，改 Logo 另读 [品牌技能](../clashflux-branding/SKILL.md)。 |
| 首页猫咪 / 运行标题 / Demo | [首页与 Demo](references/home.md)，及 UI 文档的生产动画、VpnUptime 验收要求。 |
| 托盘 / 首帧 / 系统操作 | [运行控制技能](../clashflux-runtime/SKILL.md)；涉及服务再选 [平台技能](../clashflux-platform/SKILL.md)。 |

## 布局与交互联动

- 桌面 Medium/Expanded 平面两栏，标题在自定义标题栏，PageScaffold 承载本页动作；分隔线统一 kDesktopChromeDividerThickness=0.5pt。Compact 底部四项导航，桌面窄窗保留独立系统标题栏；规则/连接/日志从设置“更多”进入并可返回。宽窄变化保留页面身份、状态和平台能力。
- 外框/标题/侧栏用 background，内容用 IslandTheme::base（surface_container_low），卡片 raised；深浅色均为外框较亮、内容较暗、卡片提亮。阴影统一 ResolveFluxShadows，不在页面自写参数；标题投影最后绘制、不占布局/输入，抽屉投影裁在标题下。
- 首页真实固定卡片无骨架，缺省中性占位；其它页面仅首份真实数据未就绪时挂 page_loading，缓存就绪直接显示。流光只绘制，退出加载/切页/后台/reduced motion 停止；桌面宽屏一级切换不横滑、不翻页，不能关闭页内动画。
- 首页每张卡左上用 HomeCardHeading(icon,title) 或开关行：语义图标 20pt、primary 色、间距 8pt、居中对齐 SemiBold 标题。HomeGrid 固定目录/跨度、88pt 行高、12pt 卡距、两行 188pt；不借缩窄合并系统代理/TUN 卡或放大其它卡。
- DenseCard 的 dense_radius（当前 4pt）、每卡底距（当前 4pt）和 VirtualList::EstimatedItemExtent 一起调整；估算包括减半内边距×2、单行文本及底距。连接条目保留虚拟化与稳定 ID。
- 拖动预览跟随原始抓取点；拖放目标注册在滚动视口，间隙/边缘仍可拖并触发自动滚动。
- 二级标签用 SectionTabPages 的框架 Pager，标签在外、点击/菜单/滑动共用受控索引，稳定 Key、有界 Column+Grow 和虚拟列表；不额外 PointerIntercept 或逐页 Offset/Opacity。嵌套边缘交给外层，取消回弹、反向不崩，首次无入场、切回保留滚动。
- 分页用 fullWidthSections、SectionTabContentInsets，将 kSectionCardSpacing 的一半放每页根；Pager 外不套水平 Padding，步长为完整视口。相邻两页留白合为一份卡距。
- 标签/内容共用保留 UseSectionTabMotion handle；下划线与居中目标根据实际几何和呈现进度，先限滚动边界再插值，拖动/取消/反向都跟随，不逐帧写 State 或失效布局。手动浏览不持续拉回，溢出菜单变化不重挂载 ScrollView。
- 不加“内核未运行/请到设置启动”常驻横幅；由首页控件表达状态，只保留需要处置的操作失败等瞬时反馈。

## 资源与验证

- 新增/修改 Localized/LocalizedFormat 文案，同步 resources/strings/{default,en,zh}.properties，default/en 英文、zh 中文，按 FNV-1a-64 的 msg id 排序。漏资源在懒组合时也会终止进程；核对三元、数组、相邻字面量拼接后的完整字符串，ui_resources 回归不能只查显眼页面。
- Image.Tint 只支持矢量；PNG 按主题选资源。颜色归 theme_colors.h / ThemeSpec，深浅用 ThemeSpec::brightness，跟随系统在组合期读 UseSystemThemeBrightness（详见 UI 文档“主题颜色审查”）；阴影用 ThemeShadowColor，危险按钮继承样式并用 OnErrorColor；页面不写 RGB/固定黑白（二维码编码模块与固定图标资源例外）。
- 普通弹出菜单用 UseActionMenu + 描述，hover 子菜单复用公共项/表面；Danger 用 error 色，独立入口各有 anchor。事件只建拥有型描述，Scope 组合期读主题。托盘用框架原生 MenuItem，框架选择器不改成自制 Popup。
- 抽屉不叠遮罩色：右侧从 SectionTabPickerInsets 的桌面标题下沿开始，底部只局部将 BottomSheetStyle.scrim 设透明；保留其它模态样式、关闭和系统返回。
- 按 [测试技能](../clashflux-test/SKILL.md) 执行基础验证、生产 page_transition / ui_resources 及相关 GUI 检查。共享搜索、宽窄切换、分页取消/快速反向、实际滚动保留不能只凭截图或编译判断。
