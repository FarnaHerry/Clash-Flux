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

    auto reorderIds = [profiles] {
        std::vector<std::int64_t> ids;
        ids.reserve(profiles.Size());
        for (std::size_t i = 0; i < profiles.Size(); ++i) ids.push_back(profiles[i].id);
        return ids;
    };

    auto restoreOrder = [profiles](const std::vector<std::int64_t>& ids) {
        for (std::size_t target = 0; target < ids.size(); ++target) {
            for (std::size_t current = target; current < profiles.Size(); ++current) {
                if (profiles[current].id != ids[target]) continue;
                profiles.Move(current, target);
                break;
            }
        }
    };

    ProfileReorderActions reorderProfile;
    reorderProfile.begin = [reorderSession, reorderTarget, reorderIds, profilesModel](std::int64_t id) {
        auto& session = *reorderSession.Get();
        if (session.activeId != 0) return;
        session.activeId = id;
        session.previewTargetId = 0;
        reorderTarget = 0;
        session.originalOrder = reorderIds();
        profilesModel->reorderPending = true;
        // 让已经在途的旧列表快照失效；松手后再请求一次新快照。
        profilesModel->RequestSync();
    };
    reorderProfile.preview = [reorderSession, reorderTarget](std::int64_t fromId,
                                                              std::int64_t toId) {
        auto& session = *reorderSession.Get();
        if (session.activeId != fromId || session.previewTargetId == toId) return;
        session.previewTargetId = toId;
        // 只更新预览槽位；拖动源始终留在原始网格节点，避免指针会话随卡片换位失效。
        reorderTarget = toId;
    };
    reorderProfile.drop = [profiles, reorderSession, reorderTarget, reorderIds, restoreOrder,
                           profilesModel, toast](
                              std::int64_t fromId, std::int64_t toId) {
        auto& session = *reorderSession.Get();
        if (session.activeId != fromId) return;
        if (fromId != toId) {
            std::size_t fromIdx = profiles.Size();
            std::size_t toIdx = profiles.Size();
            for (std::size_t i = 0; i < profiles.Size(); ++i) {
                if (profiles[i].id == fromId) fromIdx = i;
                if (profiles[i].id == toId) toIdx = i;
            }
            if (fromIdx < profiles.Size() && toIdx < profiles.Size())
                profiles.Move(fromIdx, toIdx);
        }
        const auto ids = reorderIds();
        if (ids != session.originalOrder) {
            // setOrder 只排序内存缓存并标记异步落库；拖动过程中不写存储。
            // 在 UI 线程按投放顺序提交，避免连续 worker 任务反序覆盖。
            auto& profileStore = store::profilesStore();
            if (!profileStore.setOrder(ids)) {
                restoreOrder(session.originalOrder);
                toast.Show(profileStore.lastError());
            }
        }
        session.activeId = 0;
        session.previewTargetId = 0;
        reorderTarget = 0;
        session.originalOrder.clear();
        profilesModel->reorderPending = false;
        profilesModel->RequestSync();
    };
    reorderProfile.cancel = [reorderSession, reorderTarget,
                             profilesModel](std::int64_t id) {
        auto& session = *reorderSession.Get();
        if (session.activeId != id) return;
        session.activeId = 0;
        session.previewTargetId = 0;
        reorderTarget = 0;
        session.originalOrder.clear();
        profilesModel->reorderPending = false;
        profilesModel->RequestSync();
    };

    auto isConnectionSelected = [connectionSelection](std::int64_t id) {
        for (const std::int64_t selected : connectionSelection) {
            if (selected == id) return true;
        }
        return false;
    };

    auto toggleConnection = [connectionSelection, pptpStates, openVpnStates,
                             toast, findProfile, vpnModel](std::int64_t id, bool checked) {
        if (vpnModel->operation.Get().Busy()) return;
        for (std::size_t index = 0; index < connectionSelection.Size(); ++index) {
            if (connectionSelection[index] != id) continue;
            if (!checked) connectionSelection.Erase(index);
            return;
        }
        if (!checked) return;

        const std::size_t maximum = vpn::MaxConcurrentAuxiliaryConnections();
        if (connectionSelection.Size() >= maximum) {
            toast.Show(LocalizedFormat("同时最多选择 {} 条内网连接", maximum));
            return;
        }
        if (maximum == 1) {
            const auto hasOtherActive = [id](const auto& states) {
                for (const auto& state : states) {
                    if (state.profileId != id &&
                        state.state == vpn::ConnectionState::Connected) {
                        return true;
                    }
                }
                return false;
            };
            if (hasOtherActive(pptpStates) || hasOtherActive(openVpnStates)) {
                toast.Show(Localized("请先断开当前内网连接，再连接其他配置"));
                return;
            }
        }
        if (vpn::PptpSingleSession()) {
            // Windows 的 PPTP 是单连接槽位：选择前就给出平台原因，避免选中
            // 后在拨号阶段才失败（批量打开由存储层同一检查兜底）。
            const auto profile = findProfile(id);
            if (profile && profile->type == "pptp") {
                for (const auto& state : pptpStates) {
                    if (state.profileId != id &&
                        (state.state == vpn::ConnectionState::Connected ||
                         state.state == vpn::ConnectionState::Connecting)) {
                        toast.Show(Localized(
                            "Windows 系统拨号同一时间只支持一条 PPTP，请先断开当前连接"));
                        return;
                    }
                }
            }
        }
        connectionSelection.PushBack(id);
    };

    auto openSelectedConnections = [applicationTasks, toast, connectionSelection,
                                    vpnModel, coreControl] {
        if (vpnModel->operation.Get().Busy()) return;
        std::vector<std::int64_t> ids;
        for (const std::int64_t id : connectionSelection) ids.push_back(id);
        if (ids.empty()) {
            toast.Show(Localized("请先选择 PPTP 或 OpenVPN 订阅"));
            return;
        }
        if (!vpnModel->Begin(VpnAction::Open, ids)) return;
        connectionSelection.Clear();
        applicationTasks.Launch([ids, toast, vpnModel, coreControl]() -> huxerui::Task<void> {
            int opened = 0;
            int failed = 0;
            std::string firstError;
            for (const std::int64_t id : ids) {
                try {
                    const auto result = co_await RunOnTaskThread([id] {
                        const auto all = store::profilesStore().list();
                        const auto it = std::ranges::find_if(
                            all, [id](const db::Profile& profile) { return profile.id == id; });
                        if (it == all.end() || !isNativeVpnType(it->type)) {
                            return std::tuple{false, std::string{"订阅不存在或不是原生连接"},
                                              store::vpnStore().snapshot()};
                        }
                        std::string error;
                        const bool connected = it->type == "openvpn"
                                                   ? store::vpnStore().connectOpenVpn(*it, error)
                                                   : store::vpnStore().connectPptp(*it, error);
                        return std::tuple{connected, std::move(error),
                                          store::vpnStore().snapshot()};
                    });
                    // 每条连接完成就发布，不等待下一拍或整批拨号结束。
                    vpnModel->Publish(std::get<2>(result));
                    if (std::get<0>(result)) {
                        ++opened;
                    } else {
                        ++failed;
                        if (firstError.empty()) firstError = std::get<1>(result);
                    }
                } catch (const std::exception& exception) {
                    ++failed;
                    if (firstError.empty()) firstError = exception.what();
                } catch (...) {
                    ++failed;
                    if (firstError.empty()) firstError = "未知连接错误";
                }
                vpnModel->Complete(id);
            }
            if (opened > 0) {
                try {
                    const auto coreSnapshot = co_await RunOnTaskThread([] {
                        return store::coreStore().snapshot();
                    });
                    if (!coreControl.Busy()) {
                        coreControl.model->Update([&](CoreView& view) { view.core = coreSnapshot; });
                        if (coreSnapshot.state == core::CoreState::Stopped ||
                            coreSnapshot.state == core::CoreState::Failed) {
                            coreControl.Run(CoreCommand{.action = CoreAction::Start});
                        }
                    } else {
                        coreControl.model->RequestRefresh();
                        if (coreSnapshot.state != core::CoreState::Running)
                            toast.Show(Localized("连接已建立；主内核正在处理其他操作，请稍后启动"));
                    }
                } catch (const std::exception& exception) {
                    toast.Show(LocalizedFormat("连接已建立，读取主内核状态失败：{}", exception.what()));
                } catch (...) {
                    toast.Show(Localized("连接已建立，读取主内核状态失败"));
                }
            }
            vpnModel->Finish();
            if (failed == 0) {
                toast.Show(LocalizedFormat("已打开 {} 个连接", opened));
            } else {
                toast.Show(LocalizedFormat(
                    "已打开 {} 个，失败 {} 个{}", opened,
                    failed, firstError.empty()
                        ? std::string{}
                        : "：" + firstError));
            }
        });
    };

    auto closeSelectedConnections = [applicationTasks, toast, connectionSelection, vpnModel] {
        if (vpnModel->operation.Get().Busy()) return;
        std::vector<std::int64_t> ids;
        for (const std::int64_t id : connectionSelection) ids.push_back(id);
        if (ids.empty()) {
            toast.Show(Localized("请先选择 PPTP 或 OpenVPN 订阅"));
            return;
        }
        if (!vpnModel->Begin(VpnAction::Close, ids)) return;
        connectionSelection.Clear();
        applicationTasks.Launch([ids, toast, vpnModel]() -> huxerui::Task<void> {
            int closed = 0;
            int failed = 0;
            std::string firstError;
            for (const std::int64_t id : ids) {
                try {
                    auto next = co_await RunOnTaskThread([id] {
                        const auto all = store::profilesStore().list();
                        const auto it = std::ranges::find_if(
                            all, [id](const db::Profile& profile) { return profile.id == id; });
                        if (it == all.end() || !isNativeVpnType(it->type))
                            throw std::runtime_error("订阅不存在或不是原生连接");
                        if (it->type == "openvpn") store::vpnStore().disconnectOpenVpn(id);
                        else store::vpnStore().disconnectPptp(id);
                        return store::vpnStore().snapshot();
                    });
                    vpnModel->Publish(std::move(next));
                    ++closed;
                } catch (const std::exception& exception) {
                    ++failed;
                    if (firstError.empty()) firstError = exception.what();
                } catch (...) {
                    ++failed;
                    if (firstError.empty()) firstError = "未知断开错误";
                }
                vpnModel->Complete(id);
            }
            vpnModel->Finish();
            if (failed == 0) toast.Show(LocalizedFormat("已关闭 {} 个连接", closed));
            else toast.Show(LocalizedFormat("已关闭 {} 个，失败 {} 个：{}", closed, failed, firstError));
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
                        huxerui::Text(Localized("编辑订阅"), huxerui::TextRole::Title),
                        huxerui::ScrollView(huxerui::Column {
                            huxerui::Text(Localized(pptp ? "类型：PPTP 内网连接"
                                               : openvpn ? "类型：OpenVPN 内网连接"
                                               : editType.Get() == "local"
                                                     ? "类型：本地文件"
                                                     : "类型：远程订阅")),
                            huxerui::TextField(editName.Get())
                                .Label(Localized("名称"))
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
                                          .Label(Localized("订阅链接（留空 = 本地导入）"))
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
                                                    .Label(Localized("描述（可选）"))
                                                    .Variant(huxerui::TextFieldVariant::Outlined)
                                                    .OnChanged([editDesc](
                                                                   const huxerui::TextEditingValue& v) {
                                                        editDesc = v;
                                                    })}
                                : huxerui::View{huxerui::Row{}},
                        }
                                           .With(huxerui::Spacing(16.0F),
                                                 huxerui::CrossAlign(
                                                     huxerui::CrossAxisAlignment::
                                                         Stretch)))
                            .With(huxerui::Frame{.height = kDialogFormHeight}),
                        huxerui::Row {
                            huxerui::Button(Localized("取消")).OnClick(
                                [ctx] { ctx.Dismiss(); }),
                            huxerui::Button(Localized("保存")).OnClick([=] {
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
                                    toast.Show(err.empty() ? Localized("已保存")
                                                           : huxerui::StringVariant(err));
                                });
                            }),
                        }.With(huxerui::MainAlign(
                                   huxerui::MainAxisAlignment::SpaceBetween)),
                    }
                                      .With(huxerui::Spacing(16.0F),
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
    auto openEditInfo = [compact, navigation, navigation_enabled, tasks, toast,
                         loadEditInfo, showEditInfo, editPageId,
                         editName, editUrl, editType, editDesc, editTimeout,
                         editInterval, editAuto, editSys, editCore, editCert,
                         editPptpServer, editPptpUsername, editPptpPassword,
                         editPptpTimeout, editPptpRoutes, editPptpMppe,
                         editOpenVpnConfig, editOpenVpnRoutes](std::int64_t id) {
        if (navigation_enabled) {
            loadEditInfo(id, [=] {
                const ProfileEditFields fields{
                    editName, editUrl, editType, editDesc, editTimeout,
                    editInterval, editAuto, editSys, editCore, editCert,
                    editPptpServer, editPptpUsername, editPptpPassword,
                    editPptpTimeout, editPptpRoutes, editPptpMppe,
                    editOpenVpnConfig, editOpenVpnRoutes};
                navigation.Push([=] {
                    return ProfileEditRoutePage(
                        id, fields, tasks, toast,
                        [navigation] { static_cast<void>(navigation.Pop()); });
                });
            });
            return;
        }
        OpenProfileEditInfo(id, compact, loadEditInfo, showEditInfo, editPageId);
    };

    // ---- 编辑规则（rules 列表 + 前置/后置；工作副本，保存才落盘）----
    auto showEditRules = [compact, navigation, navigation_enabled, dialog, tasks,
                          toast, editYamlText, editRules,
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
            if (singbox::DetectProfileFormat(yaml) != singbox::ProfileFormat::ClashYaml) {
                toast.Show(Localized("此格式请使用全局路由或编辑配置文件"));
                co_return;
            }
            editYamlText = yaml;
            ReplaceStateList(editRules, store::parseRules(yaml));
            editRuleInput = huxerui::TextEditingValue{""};
            editRulesDirty = false;
            if (navigation_enabled) {
                navigation.Push([=] {
                    return ProfileRulesPage(
                        id, editYamlText, editRules, editRuleInput,
                        editRulesDirty, tasks, toast,
                        [navigation] { static_cast<void>(navigation.Pop()); });
                });
                co_return;
            }
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
                        ruleList = EmptyState(Localized("订阅没有 rules 规则"), app::images::route)
                            .With(huxerui::Frame{.height = 300.0F});
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
                        huxerui::Text(Localized("编辑规则"), huxerui::TextRole::Title),
                        huxerui::Text(Localized(
                            "前置插到列表头、后置追加到尾；点「保存」"
                            "写回订阅文件，启用中的订阅将重启内核生效。"))
                            .Style(huxerui::TextStyle{
                                huxerui::Font::System(font_size::kCaption),
                                hintColor}),
                        std::move(ruleList),
                        huxerui::Row {
                            huxerui::TextField(editRuleInput.Get())
                                .Label(Localized("规则，如 DOMAIN-SUFFIX,example.com,代理组"))
                                .Variant(huxerui::TextFieldVariant::Outlined)
                                .OnChanged(
                                    [editRuleInput](
                                        const huxerui::TextEditingValue& v) {
                                        editRuleInput = v;
                                    })
                                .With(huxerui::Grow(1.0F)),
                            huxerui::Button(Localized("前置")).OnClick([=] { addRule(true); }),
                            huxerui::Button(Localized("后置")).OnClick([=] { addRule(false); }),
                        }
                            .With(huxerui::Spacing(8.0F),
                                  huxerui::CrossAlign(
                                      huxerui::CrossAxisAlignment::Center)),
                        huxerui::Row {
                            huxerui::Button(Localized("取消")).OnClick(
                                [ctx] { ctx.Dismiss(); }),
                            huxerui::Button(Localized("保存")).OnClick([=] {
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
                                    toast.Show(err.empty() ? Localized("规则已保存")
                                                           : huxerui::StringVariant(err));
                                });
                            }),
                        }.With(huxerui::MainAlign(
                                   huxerui::MainAxisAlignment::SpaceBetween)),
                    }
                                      .With(huxerui::Spacing(16.0F),
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
    auto showEditFile = [compact, navigation, navigation_enabled, dialog, tasks,
                         toast, fileContent, fileLoading, filePageId, profilesModel,
                         hintColor = theme.colors.on_surface_variant](
                            std::int64_t id) {
        tasks.Launch([=]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            *fileContent.Get() = "";
            fileLoading = true;
            if (navigation_enabled) {
                navigation.Push([=] {
                    return ProfileFilePage(
                        id, fileContent, fileLoading, tasks, toast,
                        [navigation] { static_cast<void>(navigation.Pop()); });
                });
                const std::string yaml = co_await RunOnTaskThread([id] {
                    return store::profilesStore().yamlOf(id);
                });
                *fileContent.Get() = yaml;
                fileLoading = false;
                co_return;
            }
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
                [tasks, toast, fileContent, fileLoading, id, hintColor, profilesModel](
                    huxerui::DialogContext ctx) -> huxerui::View {
                    const huxerui::View body =
                        fileLoading.Get()
                            ? huxerui::View{
                                  huxerui::Column {
                                      huxerui::ProgressCircle()
                                          .With(huxerui::Frame{
                                              .width = 28.0F,
                                              .height = 28.0F}),
                                      huxerui::Text(Localized("正在加载订阅文件…"))
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
                        huxerui::Text(Localized("编辑文件"), huxerui::TextRole::Title),
                        huxerui::Text(Localized(
                            "直接编辑配置原文；保存后若该来源参与运行，内核将重启使改动生效。"))
                            .Style(huxerui::TextStyle{
                                huxerui::Font::System(font_size::kCaption),
                                hintColor}),
                        body,
                        huxerui::Row {
                            huxerui::Button(Localized("取消")).OnClick(
                                [ctx] { ctx.Dismiss(); }),
                            huxerui::Button(Localized("保存")).OnClick([=] {
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
                                    toast.Show(err.empty() ? Localized("已保存")
                                                           : huxerui::StringVariant(err));
                                    if (err.empty()) profilesModel->RequestSync();
                                });
                            }),
                        }.With(huxerui::MainAlign(
                                   huxerui::MainAxisAlignment::SpaceBetween)),
                    }
                                      .With(huxerui::Spacing(16.0F),
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
    auto showQr = [dialog, tasks, findProfile](std::int64_t id) {
        tasks.Launch([=]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            const auto profile = findProfile(id);
            if (!profile || profile->url.empty()) co_return;
            const std::string url = profile->url;
            dialog.Show(
                [url]() -> huxerui::View {
                    return DialogCard(huxerui::Canvas(QrPainter(url))
                        .With(huxerui::Frame{.width = 240.0F,
                                             .height = 240.0F}));
                },
                huxerui::DialogOptions{});
        });
    };

    // ---- 新建订阅弹窗 ----
