---
name: clashflux-release
description: 准备 Clash-Flux 版本、发布说明、安装包或修改发布流水线时使用，约束版本一致性、测试门禁、Android 签名与发布动作。
---

# 准备与发布版本

准备发版可以修改版本和说明，但提交、打标签、推送、发布必须有用户明确授权；授权不因读取此技能而扩大。
以 [.github/workflows/build.yml](../../.github/workflows/build.yml) 的当前构建矩阵、publish-release 依赖和资产检查为准，不凭旧发布记录推断。
对外资产统一 `clash-flux-<version>-<platform>-<arch>.<format>`；改名同步 CI 暂存/收集/存在性检查、应用更新匹配和 README，保留历史名称的受限兼容，详见 [应用升级](../../docs/app-updates.md)。不改已发布资产或旧 Bundle 清理规则。

## 应用内升级

修改升级行为先读 [应用升级](../../docs/app-updates.md)，同时读取 UI、运行、数据与对应平台技能。
Linux 授权/安装期间保留主进程，成功才退出；取消/失败恢复原窗口与同一持久化会话，Polkit 无桌面代理时明确报错。
`clash-flux-updater` 必须随 Windows/Linux 运行目录和安装包交付；回归运行 `app_update`、`app_update_async`、`app_update_handoff` 及 persistence，真实安装另需隔离机器验收。
Android 使用正式签名 APK 和系统安装确认；构建运行 `:app:testAppUpdatePolicy`，发布验收另测正式签名跨版本覆盖、授权/取消和数据保留，不能以 debug 包或宿主 JVM 回归替代真机安装。

## 准备发布提交

1. 检查 git status、当前版本、上一发布的 Android versionCode 和现有发布说明；保留用户已有改动。
2. 同一次发布提交更新 `CMakeLists.txt` 的 project VERSION 与 `platform/android/app/build.gradle`：versionName 完全一致，versionCode 在上一发布基础上 +1。CI 不检查两侧一致性，必须人工核对。
3. 编写 `docs/releases/<标签>.md`，包含中英文和本次真实变化；版本、验证或流程变化同步 README.md、README.en.md 与相关技能/专题。
4. 使用 [测试技能](../clashflux-test/SKILL.md) 完成目标构建、版本及适用回归；正式桌面 CI 启用 REQUIRE_PROJECT_TESTS，不得靠缺失前提、禁用测试或删用户数据恢复流水线。
5. 获得提交/标签授权后，核对已提交内容：`git show <提交或标签>:CMakeLists.txt` 与 `git show <提交或标签>:platform/android/app/build.gradle`。标签存在时以该标签内容核验，不能只看工作区。

## Android 发布阻断项

先读 [Android 构建](../../docs/android-build.md) 的“发布 APK 签名”“发布版本号”和来源/缓存约束。
Release 必须使用长期发布密钥；缺少密钥必须失败，不得生成或上传 debug 签名替代品，私钥及口令不入库。
APK 必须同时启用 v1 和 v2；minSdk=24 时默认检查会跳过 v1，必须降低核验最低 API：

```bash
apksigner verify --verbose --min-sdk-version 23 --print-certs <apk>
```

分别断言 v1、v2 均为 true，证书 SHA-256 为：
`a4a0bc09a3dc76d7ac37f92043d2be6e153bda1347ce344013828e15472419f4`。
任一不匹配都阻断发布。不得跳过 stageBundledRuleSets；Java/native HuxerUI、libbox revision/摘要与 GEO 资产按 Android 文档核验。

## 发布动作与结果核对

- `v*` 标签触发 CI；仅在用户授权后推送标签，等待发布门禁与安装包收集完成。由 publish-release 汇总一次创建带产物的 Release，不提前创建空 Release。
- 存在 `docs/releases/<标签>.md` 时优先用它，缺省才自动生成说明；已有 Release 的更新按当前 workflow 和用户授权处理，不擅自移动已发布标签或覆盖资产。
- 以实际 workflow 要求核对 Windows 安装器/归档、Linux 包/归档、macOS 归档和 Android APK。服务载荷版本与应用匹配，打包内核符合锁定版本与摘要。
- iOS 是暂缓的非阻塞 Simulator 编译诊断，不生成 IPA、不进入 Release，不承诺真机支持；恢复支持需要明确项目决策。
- 最终报告提交/标签、CI 与安装包/签名的真实结果；没有执行的签名、安装、真机测试不能写成通过。
- Windows 包含 legacy-bundle-cleanup.dll；新 Burn 升级标识避开旧缓存 bootstrapper，MSI 标识保持不变。发布前按 [迁移边界](../../docs/huxerui-fork.md#windows-legacy-bundle-migration) 验证跨版本升级、回滚和卸载，不能以同版本重新打包替代。
