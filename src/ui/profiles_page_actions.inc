// profiles_page_actions.inc — ProfilesPage 的状态操作、菜单和弹窗动作。
// 此文件由 profiles_page.cpp 在 ProfilesPage 函数体内包含。

    auto reload = [tasks] {
        tasks.Launch([=]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            // 列表由 2s 泵刷新；这里仅给一拍推迟，保证任务内 State 写不与
            // 点击路径叠加。
        });
    };

    auto findProfile = [profiles](std::int64_t id) -> std::optional<db::Profile> {
        for (const auto& p : profiles) {
            if (p.id == id) return p;
        }
        return std::nullopt;
    };

    auto isConnectionSelected = [connectionSelection](std::int64_t id) {
        for (const std::int64_t selected : connectionSelection) {
            if (selected == id) return true;
        }
        return false;
    };

    auto toggleConnection = [connectionSelection](std::int64_t id, bool checked) {
        for (std::size_t index = 0; index < connectionSelection.Size(); ++index) {
            if (connectionSelection[index] != id) continue;
            if (!checked) connectionSelection.Erase(index);
            return;
        }
        if (checked) connectionSelection.PushBack(id);
    };

    auto openSelectedConnections = [tasks, toast, connectionSelection] {
        std::vector<std::int64_t> ids;
        for (const std::int64_t id : connectionSelection) ids.push_back(id);
        if (ids.empty()) {
            toast.Show("请先选择 PPTP 或 OpenVPN 订阅");
            return;
        }
        tasks.Launch([ids, toast, connectionSelection]() -> huxerui::Task<void> {
            const auto result = co_await RunOnTaskThread([ids] {
                int opened = 0;
                int failed = 0;
                std::string firstError;
                const auto all = store::profilesStore().list();
                for (const std::int64_t id : ids) {
                    const auto it = std::ranges::find_if(
                        all, [id](const db::Profile& profile) { return profile.id == id; });
                    if (it == all.end() || !isNativeVpnType(it->type)) continue;
                    std::string error;
                    const bool connected = it->type == "openvpn"
                                               ? store::vpnStore().connectOpenVpn(*it, error)
                                               : store::vpnStore().connectPptp(*it, error);
                    if (connected) {
                        ++opened;
                    } else {
                        ++failed;
                        if (firstError.empty()) firstError = error;
                    }
                }
                return std::tuple{opened, failed, firstError};
            });
            connectionSelection.Clear();
            if (std::get<1>(result) == 0) {
                toast.Show(std::format("已打开 {} 个连接", std::get<0>(result)));
            } else {
                toast.Show(std::format("已打开 {} 个，失败 {} 个{}", std::get<0>(result),
                                      std::get<1>(result),
                                      std::get<2>(result).empty()
                                          ? ""
                                          : "：" + std::get<2>(result)));
            }
        });
    };

    auto closeSelectedConnections = [tasks, toast, connectionSelection] {
        std::vector<std::int64_t> ids;
        for (const std::int64_t id : connectionSelection) ids.push_back(id);
        if (ids.empty()) {
            toast.Show("请先选择 PPTP 或 OpenVPN 订阅");
            return;
        }
        tasks.Launch([ids, toast, connectionSelection]() -> huxerui::Task<void> {
            co_await RunOnTaskThread([ids] {
                const auto all = store::profilesStore().list();
                for (const std::int64_t id : ids) {
                    const auto it = std::ranges::find_if(
                        all, [id](const db::Profile& profile) { return profile.id == id; });
                    if (it == all.end()) continue;
                    if (it->type == "openvpn") store::vpnStore().disconnectOpenVpn(id);
                    else if (it->type == "pptp") store::vpnStore().disconnectPptp(id);
                }
            });
            connectionSelection.Clear();
            toast.Show("已关闭选中的连接");
        });
    };

    // ---- 编辑信息（按订阅类型显示对应表单；仅落库，下次更新/连接生效）----
    auto loadEditInfo = [tasks, findProfile, editName, editUrl, editType, editDesc,
                         editTimeout, editInterval, editAuto, editSys, editCore,
                         editCert, editPptpServer, editPptpUsername,
                         editPptpPassword, editPptpTimeout, editPptpRoutes,
                         editPptpMppe, editOpenVpnConfig, editOpenVpnRoutes](
        std::int64_t id, std::function<void()> next) {
        tasks.Launch([=]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            const auto profile = findProfile(id);
            if (!profile) co_return;
            editName = huxerui::TextEditingValue{profile->name};
            editUrl = huxerui::TextEditingValue{profile->url};
            editType = profile->type;
            editDesc = huxerui::TextEditingValue{profile->description};
            editTimeout =
                huxerui::TextEditingValue{std::format("{}", profile->timeoutSecs)};
            editInterval =
                huxerui::TextEditingValue{std::format("{}", profile->intervalMins)};
            editAuto = profile->autoUpdate;
            editSys = profile->useSystemProxy;
            editCore = profile->useCoreProxy;
            editCert = profile->allowInvalidCert;
            std::string pptpError;
            const auto config = pptp::ParsePptpConfig(profile->nativeConfig, pptpError);
            editPptpServer = huxerui::TextEditingValue{
                config ? config->server : ""};
            editPptpUsername = huxerui::TextEditingValue{
                config ? config->username : ""};
            editPptpPassword = huxerui::TextEditingValue{
                config ? config->password : ""};
            editPptpTimeout = huxerui::TextEditingValue{
                config ? std::to_string(config->connectTimeoutSecs) : "30"};
            editPptpRoutes = huxerui::TextEditingValue{profile->nativeRoutes};
            editPptpMppe = config ? config->requireMppe : true;
            editOpenVpnConfig = huxerui::TextEditingValue{profile->nativeConfig};
            editOpenVpnRoutes = huxerui::TextEditingValue{profile->nativeRoutes};
            next();
        });
    };

    auto showEditInfo = [loadEditInfo, dialog, tasks, toast, editName, editUrl,
                         editType, editDesc, editTimeout, editInterval, editAuto,
                         editSys, editCore, editCert, editPptpServer,
                         editPptpUsername, editPptpPassword, editPptpTimeout,
                         editPptpRoutes, editPptpMppe, editOpenVpnConfig,
                         editOpenVpnRoutes,
                         textColor = theme.colors.on_surface,
                         hintColor = theme.colors.on_surface_variant](
                            std::int64_t id) {
        loadEditInfo(id, [dialog, tasks, toast, editName, editUrl, editType,
                          editDesc, editTimeout, editInterval, editAuto, editSys,
                          editCore, editCert, editPptpServer, editPptpUsername,
                          editPptpPassword, editPptpTimeout, editPptpRoutes,
                          editPptpMppe, editOpenVpnConfig, editOpenVpnRoutes,
                          id, textColor, hintColor] {
            dialog.Show(
                [tasks, toast, editName, editUrl, editType, editDesc, editTimeout,
                 editInterval, editAuto, editSys, editCore, editCert,
                 editPptpServer, editPptpUsername, editPptpPassword,
                 editPptpTimeout, editPptpRoutes, editPptpMppe, editOpenVpnConfig,
                 editOpenVpnRoutes, id,
                 textColor, hintColor](huxerui::DialogContext ctx) -> huxerui::View {
                    const bool pptp = editType.Get() == "pptp";
                    const bool openvpn = editType.Get() == "openvpn";
                    return DialogCard(huxerui::Column {
                        huxerui::Text("编辑订阅", huxerui::TextRole::Title),
                        huxerui::ScrollView(huxerui::Column {
                            huxerui::Text(pptp ? "类型：PPTP 内网连接"
                                               : openvpn ? "类型：OpenVPN 内网连接"
                                               : editType.Get() == "local"
                                                     ? "类型：本地文件"
                                                     : "类型：远程订阅"),
                            huxerui::TextField(editName.Get())
                                .Label("名称")
                                .Variant(huxerui::TextFieldVariant::Outlined)
                                .OnChanged(
                                    [editName](const huxerui::TextEditingValue& v) {
                                        editName = v;
                                    }),
                            pptp
                                ? huxerui::View{PptpOptionsForm(
                                      editPptpServer, editPptpUsername,
                                      editPptpPassword, editPptpTimeout,
                                      editPptpRoutes, editPptpMppe)}
                                : openvpn
                                ? huxerui::View{OpenVpnOptionsForm(
                                      editOpenVpnConfig, editOpenVpnRoutes)}
                                : huxerui::View{huxerui::Column {
                                      huxerui::TextField(editUrl.Get())
                                          .Label("订阅链接（留空 = 本地导入）")
                                          .Placeholder("https://...")
                                          .Variant(huxerui::TextFieldVariant::Outlined)
                                          .OnChanged([editUrl](
                                                         const huxerui::TextEditingValue& v) {
                                              editUrl = v;
                                          }),
                                      ProfileOptionsForm(
                                          editDesc, editTimeout, editInterval,
                                          editAuto, editSys, editCore, editCert),
                                  }},
                            pptp || openvpn
                                ? huxerui::View{huxerui::TextField(editDesc.Get())
                                                    .Label("描述（可选）")
                                                    .Variant(huxerui::TextFieldVariant::Outlined)
                                                    .OnChanged([editDesc](
                                                                   const huxerui::TextEditingValue& v) {
                                                        editDesc = v;
                                                    })}
                                : huxerui::View{huxerui::Row{}},
                        }
                                           .With(huxerui::Spacing(12.0F),
                                                 huxerui::CrossAlign(
                                                     huxerui::CrossAxisAlignment::
                                                         Stretch)))
                            .With(huxerui::Frame{.height = kDialogFormHeight}),
                        huxerui::Row {
                            huxerui::Button("取消").OnClick(
                                [ctx] { ctx.Dismiss(); }),
                            huxerui::Button("保存").OnClick([=] {
                                std::string configError;
                                std::optional<std::string> nativeConfig;
                                if (editType.Get() == "pptp") {
                                    nativeConfig = makePptpConfig(
                                        editPptpServer.Get(), editPptpUsername.Get(),
                                        editPptpPassword.Get(), editPptpTimeout.Get(),
                                        editPptpMppe.Get(), configError);
                                    if (!nativeConfig) {
                                        toast.Show(configError);
                                        return;
                                    }
                                } else if (editType.Get() == "openvpn") {
                                    if (!openvpn::ParseOpenVpnConfig(
                                            editOpenVpnConfig.Get().text, configError)) {
                                        toast.Show(configError);
                                        return;
                                    }
                                    nativeConfig = editOpenVpnConfig.Get().text;
                                }
                                ctx.Dismiss();
                                tasks.Launch([=]() -> huxerui::Task<void> {
                                    db::Profile fields;
                                    fields.name = editName.Get().text;
                                    fields.type = editType.Get();
                                    fields.url = isNativeVpnType(fields.type)
                                                     ? ""
                                                     : editUrl.Get().text;
                                    fields.description = editDesc.Get().text;
                                    fields.timeoutSecs =
                                        parseNumber(editTimeout.Get(), 60);
                                    fields.intervalMins =
                                        parseNumber(editInterval.Get(), 0);
                                    fields.autoUpdate = editAuto.Get();
                                    fields.useSystemProxy = editSys.Get();
                                    fields.useCoreProxy = editCore.Get();
                                    fields.allowInvalidCert = editCert.Get();
                                    if (nativeConfig) {
                                        fields.nativeConfig = *nativeConfig;
                                        fields.nativeRoutes = joinRoutes(
                                            parseRouteField(fields.type == "openvpn"
                                                                ? editOpenVpnRoutes.Get().text
                                                                : editPptpRoutes.Get().text));
                                    }
                                    const std::string err = co_await RunOnTaskThread(
                                        [id, fields] {
                                            auto& ps = store::profilesStore();
                                            if (!ps.updateProfile(id, fields))
                                                return ps.lastError();
                                            return std::string{};
                                        });
                                    toast.Show(err.empty() ? "已保存" : err);
                                });
                            }),
                        }.With(huxerui::MainAlign(
                                   huxerui::MainAxisAlignment::SpaceBetween)),
                    }
                                      .With(huxerui::Spacing(12.0F),
                                            huxerui::Frame{.width = 420.0F},
                                            huxerui::CrossAlign(
                                                huxerui::CrossAxisAlignment::
                                                    Stretch)));
                },
                huxerui::DialogOptions{});
        });
    };

    const NativeProfileSupport profileSupport = profile_detail::profileSupport();
    const bool pptpSupported = profileSupport.pptp;
    const bool openvpnSupported = profileSupport.openvpn;
    auto openEditInfo = [compact, loadEditInfo, showEditInfo, editPageId](
                            std::int64_t id) {
        OpenProfileEditInfo(id, compact, loadEditInfo, showEditInfo, editPageId);
    };

    // ---- 编辑规则（rules 列表 + 前置/后置；工作副本，保存才落盘）----
    auto showEditRules = [compact, dialog, tasks, toast, editYamlText, editRules,
                          editRuleInput, editRulesDirty,
                          rulesPageId,
                          textColor = theme.colors.on_surface,
                          hintColor = theme.colors.on_surface_variant](
                             std::int64_t id) {
        tasks.Launch([=]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            const std::string yaml =
                co_await RunOnTaskThread([id] {
                    return store::profilesStore().yamlOf(id);
                });
            editYamlText = yaml;
            ReplaceStateList(editRules, store::parseRules(yaml));
            editRuleInput = huxerui::TextEditingValue{""};
            editRulesDirty = false;
            if (compact) {
                rulesPageId = id;
                co_return;
            }
            dialog.Show(
                [tasks, toast, editYamlText, editRules, editRuleInput,
                 editRulesDirty, id, textColor, hintColor](
                    huxerui::DialogContext ctx) -> huxerui::View {
                    // 规则行（动态列表：键用行号；行内无状态）。
                    huxerui::View ruleList;
                    if (editRules.Empty()) {
                        ruleList = huxerui::Column {
                            huxerui::Text("订阅没有 rules 规则")
                                .Style(huxerui::TextStyle{
                                    huxerui::Font::System(font_size::kCaption),
                                    hintColor}),
                        }.With(huxerui::Padding(12.0F),
                               huxerui::Frame{.height = 300.0F});
                    } else {
                        ruleList = huxerui::VirtualList(
                                       editRules.Size(),
                                       [editRules, textColor,
                                        hintColor](std::size_t index) {
                                           return huxerui::Row {
                                               huxerui::Text(std::format("{}", index + 1))
                                                   .Style(huxerui::TextStyle{
                                                       huxerui::Font::System(
                                                           font_size::kCaption),
                                                       hintColor})
                                                   .With(huxerui::Frame{.width = 32.0F}),
                                               huxerui::Text(truncateOneLine(
                                                                 editRules[index], 72))
                                                   .Style(huxerui::TextStyle{
                                                       huxerui::Font::Monospace(
                                                           font_size::kMonoBody),
                                                       textColor}),
                                           }
                                               .With(
                                                   huxerui::Spacing(6.0F),
                                                   huxerui::CrossAlign(
                                                       huxerui::CrossAxisAlignment::Center))
                                               .Key(std::to_string(index));
                                       })
                                       .ItemExtent(28.0F)
                                       .With(huxerui::Frame{.height = 300.0F},
                                             huxerui::ScrollBar());
                    }
                    auto addRule = [=](bool prepend) {
                        const std::string text = editRuleInput.Get().text;
                        if (text.empty()) return;
                        const std::string yaml = store::insertRule(
                            editYamlText.Get(), text, prepend);
                        editYamlText = yaml;
                        ReplaceStateList(editRules, store::parseRules(yaml));
                        editRuleInput = huxerui::TextEditingValue{""};
                        editRulesDirty = true;
                    };
                    return DialogCard(huxerui::Column {
                        huxerui::Text("编辑规则", huxerui::TextRole::Title),
                        huxerui::Text("前置插到列表头、后置追加到尾；点「保存」"
                                      "写回订阅文件，启用中的订阅将重启内核生效。")
                            .Style(huxerui::TextStyle{
                                huxerui::Font::System(font_size::kCaption),
                                hintColor}),
                        std::move(ruleList),
                        huxerui::Row {
                            huxerui::TextField(editRuleInput.Get())
                                .Label("规则，如 DOMAIN-SUFFIX,example.com,代理组")
                                .Variant(huxerui::TextFieldVariant::Outlined)
                                .OnChanged(
                                    [editRuleInput](
                                        const huxerui::TextEditingValue& v) {
                                        editRuleInput = v;
                                    })
                                .With(huxerui::Grow(1.0F)),
                            huxerui::Button("前置").OnClick([=] { addRule(true); }),
                            huxerui::Button("后置").OnClick([=] { addRule(false); }),
                        }
                            .With(huxerui::Spacing(8.0F),
                                  huxerui::CrossAlign(
                                      huxerui::CrossAxisAlignment::Center)),
                        huxerui::Row {
                            huxerui::Button("取消").OnClick(
                                [ctx] { ctx.Dismiss(); }),
                            huxerui::Button("保存").OnClick([=] {
                                ctx.Dismiss();
                                if (!editRulesDirty.Get()) return;
                                const std::string content = editYamlText.Get();
                                tasks.Launch([=]() -> huxerui::Task<void> {
                                    const std::string err = co_await
                                        RunOnTaskThread([id, content] {
                                            auto& ps = store::profilesStore();
                                            if (!ps.saveYaml(id, content))
                                                return ps.lastError();
                                            return std::string{};
                                        });
                                    toast.Show(err.empty() ? "规则已保存" : err);
                                });
                            }),
                        }.With(huxerui::MainAlign(
                                   huxerui::MainAxisAlignment::SpaceBetween)),
                    }
                                      .With(huxerui::Spacing(12.0F),
                                            huxerui::Frame{.width = 620.0F},
                                            huxerui::CrossAlign(
                                                huxerui::CrossAxisAlignment::
                                                    Stretch)));
                },
                huxerui::DialogOptions{});
        });
    };

    // ---- 编辑文件（懒加载 YAML + 非受控多行编辑：OnChanged 只写 shared_ptr
    // 指向的内容，不触发 State 写 → 无逐键全文重排版；保存取现值）----
    auto showEditFile = [compact, dialog, tasks, toast, fileContent, fileLoading,
                         filePageId,
                         hintColor = theme.colors.on_surface_variant](
                            std::int64_t id) {
        tasks.Launch([=]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            *fileContent.Get() = "";
            fileLoading = true;
            if (compact) {
                filePageId = id;
                const std::string yaml = co_await RunOnTaskThread([id] {
                    return store::profilesStore().yamlOf(id);
                });
                *fileContent.Get() = yaml;
                fileLoading = false;
                co_return;
            }
            dialog.Show(
                [tasks, toast, fileContent, fileLoading, id, hintColor](
                    huxerui::DialogContext ctx) -> huxerui::View {
                    const huxerui::View body =
                        fileLoading.Get()
                            ? huxerui::View{
                                  huxerui::Column {
                                      huxerui::ProgressCircle()
                                          .With(huxerui::Frame{
                                              .width = 28.0F,
                                              .height = 28.0F}),
                                      huxerui::Text("正在加载订阅文件…")
                                          .Style(huxerui::TextStyle{
                                              huxerui::Font::System(
                                                  font_size::kCaption),
                                              hintColor}),
                                  }
                                      .With(huxerui::Spacing(10.0F),
                                            huxerui::Padding(24.0F),
                                            huxerui::MainAlign(
                                                huxerui::MainAxisAlignment::
                                                    Center),
                                            huxerui::CrossAlign(
                                                huxerui::CrossAxisAlignment::
                                                    Center))}
                              : huxerui::View{
                                    huxerui::TextField(huxerui::
                                                           TextEditingValue{
                                                               *fileContent
                                                                    .Get()})
                                        .Variant(
                                            huxerui::TextFieldVariant::Outlined)
                                        .LineLimits(huxerui::
                                                        TextFieldLineLimits::
                                                            MultiLine(14, 22))
                                        // 非受控：只落 shared_ptr，不回写
                                        // State（避免逐键全文重组）。
                                        .OnChanged(
                                            [fileContent](
                                                const huxerui::TextEditingValue&
                                                    v) {
                                                *fileContent.Get() = v.text;
                                            })};
                    return DialogCard(huxerui::Column {
                        huxerui::Text("编辑文件", huxerui::TextRole::Title),
                        huxerui::Text("直接编辑订阅 YAML 原文；保存后若该订阅"
                                      "启用中，内核将重启使改动生效。")
                            .Style(huxerui::TextStyle{
                                huxerui::Font::System(font_size::kCaption),
                                hintColor}),
                        body,
                        huxerui::Row {
                            huxerui::Button("取消").OnClick(
                                [ctx] { ctx.Dismiss(); }),
                            huxerui::Button("保存").OnClick([=] {
                                ctx.Dismiss();
                                if (fileLoading.Get()) return;
                                const std::string content = *fileContent.Get();
                                tasks.Launch([=]() -> huxerui::Task<void> {
                                    const std::string err = co_await
                                        RunOnTaskThread([id, content] {
                                            auto& ps = store::profilesStore();
                                            if (!ps.saveYaml(id, content))
                                                return ps.lastError();
                                            return std::string{};
                                        });
                                    toast.Show(err.empty() ? "已保存" : err);
                                });
                            }),
                        }.With(huxerui::MainAlign(
                                   huxerui::MainAxisAlignment::SpaceBetween)),
                    }
                                      .With(huxerui::Spacing(12.0F),
                                            huxerui::Frame{.width = 640.0F},
                                            huxerui::CrossAlign(
                                                huxerui::CrossAxisAlignment::
                                                    Stretch)));
                },
                huxerui::DialogOptions{});
            // 文件读取在任务线程；完成后经 fileLoading=false 触发重组展示。
            const std::string yaml =
                co_await RunOnTaskThread([id] {
                    return store::profilesStore().yamlOf(id);
                });
            *fileContent.Get() = yaml;
            fileLoading = false;
        });
    };

    // ---- 分享二维码 ----
    auto showQr = [dialog, tasks, toast, profiles, findProfile, clipboard,
                   hintColor = theme.colors.on_surface_variant](std::int64_t id) {
        tasks.Launch([=]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            const auto profile = findProfile(id);
            if (!profile || profile->url.empty()) co_return;
            const std::string url = profile->url;
            dialog.Show(
                [url, clipboard, toast,
                 hintColor](huxerui::DialogContext ctx) -> huxerui::View {
                    return DialogCard(huxerui::Column {
                        huxerui::Text("分享订阅", huxerui::TextRole::Title),
                        huxerui::Text(truncateOneLine(url, 60))
                            .Style(huxerui::TextStyle{
                                huxerui::Font::Monospace(font_size::kChip),
                                hintColor}),
                        huxerui::Canvas(QrPainter(url))
                            .With(huxerui::Frame{.width = 240.0F,
                                                 .height = 240.0F}),
                        huxerui::Row {
                            huxerui::Button("复制链接").OnClick(
                                [clipboard, toast, url] {
                                    if (clipboard->WriteText(url)) {
                                        toast.Show("已复制到剪贴板");
                                    } else {
                                        toast.Show("复制失败");
                                    }
                                }),
                            huxerui::Button("关闭").OnClick(
                                [ctx] { ctx.Dismiss(); }),
                        }.With(huxerui::MainAlign(
                                   huxerui::MainAxisAlignment::End),
                               huxerui::Spacing(8.0F)),
                    }
                                      .With(huxerui::Spacing(12.0F),
                                            huxerui::Frame{.width = 320.0F},
                                            huxerui::CrossAlign(
                                                huxerui::CrossAxisAlignment::
                                                    Stretch)));
                },
                huxerui::DialogOptions{});
        });
    };

    // ---- 新建订阅弹窗 ----
    // 类型（远程/本地/PPTP/OpenVPN）+ 名称 + 类型表单；远程导入是网络下载（阻塞），走任务
    // 线程，成功才关弹窗。打开前清空上一轮的输入。
    auto showCreateDialog = [dialog, tasks, toast, picker, http, newName,
                             newUrl,
                             newTypeIdx, newDesc, newTimeout, newInterval,
                             newAuto, newSys, newCore, newCert, newPptpServer,
                             newPptpUsername, newPptpPassword, newPptpTimeout,
                             newPptpRoutes, newPptpMppe, newOpenVpnConfig,
                             newOpenVpnRoutes, pickedPath, importing,
                             pptpSupported, openvpnSupported,
                             hintColor = theme.colors.on_surface_variant] {
        newName = huxerui::TextEditingValue{""};
        newUrl = huxerui::TextEditingValue{""};
        newTypeIdx = 0;
        newDesc = huxerui::TextEditingValue{""};
        newTimeout = huxerui::TextEditingValue{""};
        newInterval = huxerui::TextEditingValue{""};
        newAuto = false;
        newSys = false;
        newCore = false;
        newCert = false;
        newPptpServer = huxerui::TextEditingValue{""};
        newPptpUsername = huxerui::TextEditingValue{""};
        newPptpPassword = huxerui::TextEditingValue{""};
        newPptpTimeout = huxerui::TextEditingValue{"30"};
        newPptpRoutes = huxerui::TextEditingValue{""};
        newPptpMppe = true;
        newOpenVpnConfig = huxerui::TextEditingValue{""};
        newOpenVpnRoutes = huxerui::TextEditingValue{""};
        pickedPath = "";
        dialog.Show(
            [tasks, toast, picker, http, newName, newUrl, newTypeIdx, newDesc,
             newTimeout, newInterval, newAuto, newSys, newCore, newCert,
             newPptpServer, newPptpUsername, newPptpPassword, newPptpTimeout,
             newPptpRoutes, newPptpMppe, newOpenVpnConfig, newOpenVpnRoutes,
             pickedPath, importing, pptpSupported, openvpnSupported,
             hintColor](huxerui::DialogContext ctx) -> huxerui::View {
                const std::size_t typeIndex = newTypeIdx.Get();
                const bool remote = typeIndex == 0;
                const bool local = typeIndex == 1;
                const bool pptp = pptpSupported && typeIndex == 2;
                const bool openvpn = openvpnSupported &&
                                     typeIndex == (pptpSupported ? 3 : 2);
                huxerui::View typeFields;
                if (remote) {
                    typeFields = huxerui::TextField(newUrl.Get())
                                     .Label("订阅链接")
                                     .Placeholder("https://...")
                                     .Variant(huxerui::TextFieldVariant::Outlined)
                                     .OnChanged([newUrl](
                                                    const huxerui::TextEditingValue& v) {
                                         newUrl = v;
                                     });
                } else if (local) {
                    typeFields = huxerui::Row {
                        huxerui::Button("选择文件")
                            .OnClick([tasks, picker, pickedPath] {
                                tasks.Launch([=]() -> huxerui::Task<void> {
                                    const auto picked = co_await picker->OpenFileAsync(
                                        huxerui::FilePickerFilter{
                                            .name = "YAML 订阅",
                                            .extensions = {"yaml", "yml"}});
                                    if (!picked) co_return;
                                    if (const auto f = picked->AsFile()) {
                                        pickedPath = f->Path();
                                    } else {
                                        pickedPath = "";
                                    }
                                });
                            }),
                        huxerui::Text(pickedPath.Get())
                            .Style(huxerui::TextStyle{
                                huxerui::Font::Monospace(font_size::kChip),
                                hintColor}),
                    }.With(huxerui::Spacing(8.0F),
                          huxerui::CrossAlign(
                              huxerui::CrossAxisAlignment::Center));
                } else if (pptp) {
                    typeFields = PptpOptionsForm(
                        newPptpServer, newPptpUsername, newPptpPassword,
                        newPptpTimeout, newPptpRoutes, newPptpMppe);
                } else if (openvpn) {
                    typeFields = OpenVpnOptionsForm(newOpenVpnConfig,
                                                    newOpenVpnRoutes);
                } else {
                    typeFields = huxerui::TextField(newUrl.Get())
                                     .Label("订阅链接")
                                     .Placeholder("https://...")
                                     .Variant(huxerui::TextFieldVariant::Outlined)
                                     .OnChanged([newUrl](
                                                    const huxerui::TextEditingValue& v) {
                                         newUrl = v;
                                     });
                }
                huxerui::View commonFields =
                    (pptp || openvpn) ? huxerui::View{huxerui::TextField(newDesc.Get())
                                              .Label("描述（可选）")
                                              .Variant(huxerui::TextFieldVariant::Outlined)
                                              .OnChanged([newDesc](
                                                             const huxerui::TextEditingValue& v) {
                                                  newDesc = v;
                                              })}
                         : huxerui::View{ProfileOptionsForm(
                               newDesc, newTimeout, newInterval, newAuto, newSys,
                               newCore, newCert)};
                return DialogCard(huxerui::Column {
                    huxerui::Text("新建订阅", huxerui::TextRole::Title),
                    huxerui::ScrollView(huxerui::Column {
                        huxerui::SegmentedButton(
                            [&]() {
                                std::vector<huxerui::StringVariant> types{
                                    "远程订阅", "本地文件"};
                                if (pptpSupported) types.emplace_back("PPTP 内网");
                                if (openvpnSupported) types.emplace_back("OpenVPN 内网");
                                return types;
                            }(),
                            newTypeIdx.Get())
                            .OnChanged([newTypeIdx](std::size_t idx) {
                                newTypeIdx = idx;
                            }),
                        std::move(typeFields),
                        huxerui::TextField(newName.Get())
                            .Label("名称（可选）")
                            .Variant(huxerui::TextFieldVariant::Outlined)
                            .OnChanged([newName](const huxerui::TextEditingValue& v) {
                                newName = v;
                            }),
                        std::move(commonFields),
                    }
                                       .With(huxerui::Spacing(12.0F),
                                             huxerui::CrossAlign(
                                                 huxerui::CrossAxisAlignment::Stretch)))
                        .With(huxerui::Frame{.height = kDialogFormHeight}),
                    huxerui::Row {
                        huxerui::Button("取消").OnClick([ctx] { ctx.Dismiss(); }),
                        importing.Get()
                            ? huxerui::View{
                                  huxerui::ProgressCircle()
                                      .With(huxerui::Frame{.width = 20.0F,
                                                           .height = 20.0F})}
                            : huxerui::View{
                                  huxerui::Button("导入").OnClick([=] {
                                      const std::string url = newUrl.Get().text;
                                      if (remote && url.empty()) {
                                          toast.Show("订阅链接不能为空");
                                          return;
                                      }
                                      if (local && pickedPath.Get().empty()) {
                                          toast.Show("请先选择订阅文件");
                                          return;
                                      }
                                      std::string configError;
                                      std::optional<std::string> nativeConfig;
                                      if (pptp) {
                                          nativeConfig = makePptpConfig(
                                              newPptpServer.Get(), newPptpUsername.Get(),
                                              newPptpPassword.Get(), newPptpTimeout.Get(),
                                              newPptpMppe.Get(), configError);
                                          if (!nativeConfig) {
                                              toast.Show(configError);
                                              return;
                                          }
                                      } else if (openvpn) {
                                          if (!openvpn::ParseOpenVpnConfig(
                                                  newOpenVpnConfig.Get().text,
                                                  configError)) {
                                              toast.Show(configError);
                                              return;
                                          }
                                          nativeConfig = newOpenVpnConfig.Get().text;
                                      }
                                      importing = true;
                                      tasks.Launch([=]() -> huxerui::Task<void> {
                                          db::Profile options;
                                          options.description =
                                              newDesc.Get().text;
                                          if (!pptp && !openvpn) {
                                              options.timeoutSecs =
                                                  parseNumber(newTimeout.Get(), 60);
                                              options.intervalMins =
                                                  parseNumber(newInterval.Get(), 0);
                                              options.autoUpdate = newAuto.Get();
                                              options.useSystemProxy = newSys.Get();
                                              options.useCoreProxy = newCore.Get();
                                              options.allowInvalidCert = newCert.Get();
                                          } else {
                                              options.type = pptp ? "pptp" : "openvpn";
                                              options.nativeConfig = *nativeConfig;
                                              options.nativeRoutes = joinRoutes(
                                                  parseRouteField(pptp
                                                                      ? newPptpRoutes.Get().text
                                                                      : newOpenVpnRoutes.Get().text));
                                          }
                                          const std::string name =
                                              newName.Get().text;
                                          const auto [rid, err] =
                                              co_await ImportProfileForPlatform(
                                                  http,
                                                  ProfileImportRequest{
                                                      .remote = remote,
                                                      .local = local,
                                                      .pptp = pptp,
                                                      .openvpn = openvpn,
                                                      .name = name,
                                                      .url = url,
                                                      .picked_path = pickedPath.Get(),
                                                      .options = std::move(options)});
                                          importing = false;
                                          if (rid == 0) {
                                              toast.Show(
                                                  err.empty() ? "导入失败" : err);
                                          } else {
                                              ctx.Dismiss();
                                              toast.Show("订阅已导入");
                                          }
                                      });
                                  })},
                    }.With(huxerui::MainAlign(
                               huxerui::MainAxisAlignment::SpaceBetween),
                           huxerui::CrossAlign(
                               huxerui::CrossAxisAlignment::Center)),
                }
                                  .With(huxerui::Spacing(12.0F),
                                        huxerui::Frame{.width = 420.0F},
                                        huxerui::CrossAlign(
                                            huxerui::CrossAxisAlignment::Stretch)));
            },
            huxerui::DialogOptions{});
    };
