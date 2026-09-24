# UI 开发约定

## 响应式与平台边界

- 页面结构由视口等级决定，不由操作系统决定。Compact 使用移动式页面导航，Medium/Expanded 使用桌面布局或多字段弹窗。
- 平台判断只用于能力和内容差异，例如 Android VPN、桌面系统代理、PPTP/OpenVPN 支持情况。
- 多字段编辑在 Compact 视口进入独立页面；简单确认操作（例如删除确认）继续使用对话框。

## 操作控件

- 页面命令优先使用带语义标签的 `IconButton`；桌面悬停必须提供说明操作含义的 `Tooltip`。
- 表达状态或模式的选项（例如“规则 / 全局 / 直连”）保留文字。
- 所有 Switch 设置项统一由三部分组成：主名称、小字描述、Switch。主名称和小字描述放在左侧列，Switch 固定在右侧并与文字列垂直居中对齐。设置页统一使用 `SettingSwitchRow`，不得在 Compact 视口改成上下堆叠。
- 小字描述允许自然换行；左侧文字列使用 Grow 保留 Switch 的固定右侧位置，不得因长描述把 Switch 挤到下一行。

## Windows 后台操作与托盘菜单

- 启动、轮询和退出清理中的系统操作不能闪出命令行窗口。优先使用 Win32 API；确实需要子进程时，用 `CreateProcessW` 的 `CREATE_NO_WINDOW` 并重定向标准句柄。不要在后台 Windows 路径使用 `std::system` 或 `_popen`。
- 只有需要用户明确授权的流程可以显示系统交互提示，例如 UAC 提权。普通后台操作、失败清理和状态探测都保持安静执行。
- HuxerUI 的 Windows 系统托盘菜单是原生菜单，`SystemTrayOptions` 不提供 `MenuStyle` 参数。桌面托盘生命周期按当前应用 `ThemeSpec` 同步 Windows 原生菜单主题；Windows 10 build 17763 及以上支持此同步，旧系统保留系统原生菜单主题。修改 HuxerUI 或 Windows 平台适配时，保持托盘菜单与应用深浅模式一致。

## 可拖动仪表盘卡片

- 拖动反馈层保持卡片原尺寸，并持续跟随按下时的抓取点；不要复用会在视口边缘翻转或夹位的弹出层放置规则。
- 排序由卡片布局几何决定，原位置保留半透明占位；滚动视口注册宽域拖放目标，让卡片间隙和视口边缘仍能接收拖动并触发边缘自动滚动。
- HuxerUI 通用拖动预览行为通过 `cmake/patches/huxerui-drag-preview-follows-pointer.patch` 维护。改动该补丁或更新 HuxerUI 固定版本时，确认 Linux、Windows、macOS、Android 和 iOS Simulator 的源码构建步骤都应用它。
- macOS 与 iOS Simulator CI 通过 `cmake/patches/huxerui-window-p0960.patch` 修复固定 HuxerUI revision 在 Objective-C++ 中的聚合初始化兼容问题；升级 HuxerUI 固定版本时确认补丁仍可应用。
- iOS Simulator CI 从仓库根 CMake 项目构建 `clash-flux_huxerui_ios_core`，同时应用 P0960 和拖动预览补丁。该目标只验证 Clash-Flux 源码与 HuxerUI 静态平台库可编译，不生成应用包；iOS 网络扩展、sing-box 接入和 curl TLS 尚未纳入。

## 改动后的构建验证

每次修改完成后，无论改动大小、涉及何种文件（代码、UI、脚本还是配置），先重新
编译，再运行版本命令验证：

```bash
cmake --build build --target clash-flux
./run.sh --version
```

不得跳过本地编译，也不得只依赖旧的可执行文件判断改动有效。使用其他构建目录时，
用 `CLASHFLUX_BIN` 显式指定对应产物；需要确认 GUI 启动或交互时，再执行 `./run.sh`。
