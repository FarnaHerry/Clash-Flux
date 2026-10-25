---
name: clashflux-branding
description: 修改 Clash-Flux Logo、托盘或原生应用图标、Android 启动图标和快捷磁贴资源时使用。
---

# 品牌与图标

先读 [品牌资源说明](../../docs/branding/README.md)。母版为 assets/branding/logo.svg，采用猫咪出箱 Demo 的完整运行态造型；修改母版后统一生成，不单独覆盖派生 PNG：

```bash
python3 scripts/generate-branding.py
python3 scripts/generate-branding.py --check
```

Windows 可使用实际可用的 Python 命令。保留主题 SVG、托盘及原生应用图标的统一生成关系。
正式资源可用 demos/box-cat-demo 的 --brand-preview 实际挂载；无需启动主程序或影响代理。

Android 图标职责固定：

- 应用图标 mipmap-*/ic_launcher.png 与 adaptive drawable/ic_launcher_foreground.xml 使用黑猫探出盒子的应用 Logo；前景 inset @drawable/ic_launcher_mascot，背景 @color/ic_launcher_background。
- 快捷开关磁贴 drawable/ic_qs_clash_flux.xml 与 drawable-night/ic_qs_clash_flux.xml 使用深浅两版猫爪。
- 透明应用源图为 resources/images/clash_flux_logo.png；只修改对应资源，不把猫爪铺到启动图标，也不反向替换。

涉及 UI 挂载/主题路径还须读 [UI 技能](../clashflux-ui/SKILL.md)：PNG 不得 Tint，首帧图像改动实际启动 GUI。
生成及 --check 不能替代 [测试技能](../clashflux-test/SKILL.md) 的主目标构建和版本检查。
