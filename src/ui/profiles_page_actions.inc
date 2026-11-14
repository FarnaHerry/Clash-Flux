// profiles_page_actions.inc — ProfilesPage 的状态操作、菜单和弹窗动作。
// 此文件由 profiles_page.cpp 在 ProfilesPage 函数体内包含。

    auto reload = [profilesModel] { profilesModel->PublishCached(); };

    auto findProfile = [profiles](std::int64_t id) -> std::optional<model::Profile> {
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
            } else {
                // 写透权威模型：reorderPending 解除时镜像直接同步新顺序，
                // 不被旧列表覆盖回弹；顺序不完整时由下面的 RequestSync 重读。
                profilesModel->PublishOrder(ids);
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

    auto openConnections = [applicationTasks, toast,
                            vpnModel, coreControl](std::vector<std::int64_t> ids) {
        if (vpnModel->operation.Get().Busy()) return;
        if (ids.empty()) return;
        if (!vpnModel->Begin(VpnAction::Open, ids)) return;
        applicationTasks.Launch([ids, toast, vpnModel, coreControl]() -> huxerui::Task<void> {
            int opened = 0;
            int failed = 0;
            std::string firstError;
            for (const std::int64_t id : ids) {
                try {
                    const auto result = co_await RunOnTaskThread([id] {
                        const auto all = store::profilesStore().list();
                        const auto it = std::ranges::find_if(
                            all, [id](const model::Profile& profile) { return profile.id == id; });
                        if (it == all.end() || !isNativeVpnType(it->type)) {
                            return std::tuple{false, std::string{"订阅不存在或不是原生连接"},
                                              store::vpnStore().snapshot()};
                        }
                        std::string error;
                        const bool connected = store::vpnStore().connectPptp(*it, error);
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
                            coreControl.Run(CoreCommand{.action = CoreAction::Connect});
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

    auto closeConnections = [applicationTasks, toast,
                             vpnModel](std::vector<std::int64_t> ids) {
        if (vpnModel->operation.Get().Busy()) return;
        if (ids.empty()) return;
        if (!vpnModel->Begin(VpnAction::Close, ids)) return;
        applicationTasks.Launch([ids, toast, vpnModel]() -> huxerui::Task<void> {
            int closed = 0;
            int failed = 0;
            std::string firstError;
            for (const std::int64_t id : ids) {
                try {
                    auto next = co_await RunOnTaskThread([id] {
                        const auto all = store::profilesStore().list();
                        const auto it = std::ranges::find_if(
                            all, [id](const model::Profile& profile) { return profile.id == id; });
                        if (it == all.end() || !isNativeVpnType(it->type))
                            throw std::runtime_error("订阅不存在或不是原生连接");
                        store::vpnStore().disconnectPptp(id);
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

    auto togglePptpConnection = [vpnModel, toast, findProfile,
                                 openConnections, closeConnections](std::int64_t id) {
        if (vpnModel->operation.Get().Busy()) return;
        const auto profile = findProfile(id);
        if (!profile || profile->type != "pptp") return;
        // Read the latest shared snapshot at activation, rather than the card's
        // captured state. An external disconnect must turn the next click into Open.
        const auto& view = vpnModel->view.Get();
        for (const auto& state : view.pptp) {
            if (state.profileId != id) continue;
            if (state.state == vpn::ConnectionState::Connecting) return;
            if (state.state == vpn::ConnectionState::Connected) {
                closeConnections({id});
                return;
            }
        }
        if (vpn::PptpSingleSession()) {
            for (const auto& state : view.pptp) {
                if (state.profileId != id &&
                    (state.state == vpn::ConnectionState::Connected ||
                     state.state == vpn::ConnectionState::Connecting)) {
                    toast.Show(Localized(
                        "Windows 系统拨号同一时间只支持一条 PPTP，请先断开当前连接"));
                    return;
                }
            }
        }
        if (vpn::MaxConcurrentAuxiliaryConnections() == 1) {
            const auto otherActive = [id](const auto& states) {
                return std::ranges::any_of(states, [id](const auto& state) {
                    return state.profileId != id &&
                        (state.state == vpn::ConnectionState::Connected ||
                         state.state == vpn::ConnectionState::Connecting);
                });
            };
            if (otherActive(view.pptp)) {
                toast.Show(Localized("请先断开当前内网连接，再连接其他配置"));
                return;
            }
        }
        openConnections({id});
    };

    // ---- 编辑信息（按订阅类型显示对应表单；仅落库，下次更新/连接生效）----
    auto loadEditInfo = [tasks, findProfile, editName, editUrl, editType, editDesc,
                         editTimeout, editInterval, editSys, editCore,
                         editPptpServer, editPptpUsername,
                         editPptpPassword, editPptpTimeout, editPptpRoutes,
                         editPptpMppe](
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
                huxerui::TextEditingValue{std::format("{}", ProfileUpdateIntervalForEdit(
                    profile->autoUpdate, profile->intervalMins))};
            editSys = profile->useSystemProxy;
            editCore = profile->useCoreProxy;
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
            next();
        });
    };

    auto showEditInfo = [=](std::int64_t id) {
        loadEditInfo(id, [=] {
            ShowEditorDialog(dialog, [=](huxerui::DialogContext context) {
                return ProfileEditDialogContent(id, ProfileEditFields{editName, editUrl, editType, editDesc, editTimeout,
                    editInterval, editSys, editCore, editPptpServer,
                    editPptpUsername, editPptpPassword, editPptpTimeout, editPptpRoutes,
                    editPptpMppe}, tasks, toast, [context] { context.Dismiss(); });
            });
        });
    };

    const NativeProfileSupport profileSupport = profile_detail::profileSupport();
    const bool pptpSupported = profileSupport.pptp;
    auto openEditInfo = [compact, navigation, navigation_enabled, tasks, toast,
                         loadEditInfo, showEditInfo, editPageId,
                         editName, editUrl, editType, editDesc, editTimeout,
                         editInterval, editSys, editCore,
                         editPptpServer, editPptpUsername, editPptpPassword,
                         editPptpTimeout, editPptpRoutes, editPptpMppe](std::int64_t id) {
        if (!compact) {
            // Medium/Expanded 用弹窗；Compact 继续进入独立二级页。
            showEditInfo(id);
            return;
        }
        if (navigation_enabled) {
            loadEditInfo(id, [=] {
                const ProfileEditFields fields{
                    editName, editUrl, editType, editDesc, editTimeout,
                    editInterval, editSys, editCore,
                    editPptpServer, editPptpUsername, editPptpPassword,
                    editPptpTimeout, editPptpRoutes, editPptpMppe};
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

    // 订阅规则编辑只从卡片菜单进入；规则页本身只读。宽屏用弹窗，窄屏
    // 保留独立二级页与返回栈。
    auto showEditRules = [compact, dialog, profilesCache, profilesModel,
                          navigation, desktopNavigation](std::int64_t id) {
        if (!profilesModel->IsRuleSource(id)) return;
        if (!compact) {
            dialog.Show([profilesCache, id](huxerui::DialogContext context) {
                return SubscriptionRulesDialog(profilesCache, id,
                    [context] { context.Dismiss(); });
            });
            return;
        }
        OpenRuleEditor(navigation, desktopNavigation, [profilesCache, navigation, id] {
            return SubscriptionRulesEditorPage(profilesCache, id,
                [navigation] { static_cast<void>(navigation.Pop()); });
        });
    };

    // 外部资源属于该订阅自身的解析产物：只从卡片菜单进入，页面不跨订阅汇总。
    auto showExternalResources = [navigation, desktopNavigation, findProfile](std::int64_t id) {
        const auto profile = findProfile(id);
        if (!profile) return;
        const std::string name = profile->name;
        OpenRuleEditor(navigation, desktopNavigation, [id, name, navigation] {
            return ExternalResourcesPage(id, name,
                [navigation] { static_cast<void>(navigation.Pop()); });
        });
    };

    // 文件 I/O 在工作线程；只有最后一次打开请求可以发布文档。
    auto showEditFile = [compact, navigation, navigation_enabled, dialog, tasks,
                         toast, fileContent, fileLoading, filePageId](std::int64_t id) {
        const auto request = std::make_shared<TextDocument>();
        fileContent = request;
        fileLoading = true;
        if (compact) {
            if (navigation_enabled) {
                navigation.Push([=] {
                    return ProfileFileRoutePage(id, fileContent, fileLoading, tasks, toast,
                        [navigation] { static_cast<void>(navigation.Pop()); });
                });
            } else {
                filePageId = id;
            }
        } else {
            ShowEditorDialog(dialog, [=](huxerui::DialogContext context) {
                return ProfileFileDialog(id, fileContent, fileLoading, toast, context);
            });
        }
        tasks.Launch([=]() -> huxerui::Task<void> {
            TextDocumentHandle result;
            std::string error;
            try {
                auto text = co_await RunOnTaskThread([id] { return store::profilesStore().yamlOf(id); });
                result = std::make_shared<TextDocument>(std::move(text));
            } catch (const std::exception& exception) { error = exception.what(); }
            if (fileContent.Get() != request) co_return;
            fileContent = std::move(result);
            fileLoading = false;
            if (!error.empty()) toast.Show(error);
        });
    };

    auto showCompiledPreview = [compact, dialog, navigation, desktopNavigation,
                                tasks, toast](std::int64_t id) {
        tasks.Launch([=]() -> huxerui::Task<void> {
            std::string text;
            try {
                const auto content = co_await RunOnTaskThread(
                    [id] { return store::profilesStore().yamlOf(id); });
                const auto result = co_await RunOnTaskThread([id, content] {
                    return store::coreStore().profileRulesPreview(id, content);
                });
                text = result.error.empty() ? result.json : result.error;
            } catch (const std::exception& exception) { text = exception.what(); }
            if (text.empty()) { toast.Show(Localized("无法生成编译配置")); co_return; }
            auto document = std::make_shared<TextDocument>(std::move(text));
            if (!compact) {
                dialog.Show([document](huxerui::DialogContext context) {
                    return CompiledRulesPreviewDialog(document,
                        [context] { context.Dismiss(); });
                });
                co_return;
            }
            OpenRuleEditor(navigation, desktopNavigation, [document] {
                return CompiledRulesPreviewPage(document);
            });
        });
    };

    // ---- 分享二维码 ----
    auto showQr = [dialog, tasks, findProfile, toast,
                      error_text = huxerui::UseString(Localized("无法生成分享二维码"))](std::int64_t id) {
        tasks.Launch([=]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            const auto profile = findProfile(id);
            if (!profile || profile->url.empty()) co_return;
            try {
                auto symbol = co_await RunOnTaskThread([url = profile->url] {
                    return huxerui::qrcode::Encode(url);
                });
                dialog.Show(
                    [symbol = std::move(symbol)](huxerui::DialogContext) {
                        return DialogCard(ShareQrView(symbol));
                    },
                    huxerui::DialogOptions{});
            } catch (const std::exception&) {
                toast.Show(error_text);
            }
        });
    };

    // ---- 通过 QRS 多帧码分享完整配置 ----
    auto showQrs = [dialog, tasks, findProfile, toast,
                       read_error = huxerui::UseString(Localized("无法读取订阅配置")),
                       encode_error = huxerui::UseString(Localized("无法生成 QRS 分享码"))](std::int64_t id) {
        tasks.Launch([=]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            const auto profile = findProfile(id);
            if (!profile) co_return;
            auto config = co_await RunOnTaskThread([id] {
                return store::profilesStore().yamlOf(id);
            });
            if (config.empty()) {
                toast.Show(read_error);
                co_return;
            }
            try {
                auto [frames, first] = co_await RunOnTaskThread([config = std::move(config)] {
                    auto sequence = qrs::Encode(config);
                    if (sequence.frames.empty())
                        throw std::invalid_argument("QRS frame sequence is empty");
                    auto first = huxerui::qrcode::Encode(sequence.frames.front());
                    return std::pair{
                        std::make_shared<const std::vector<std::string>>(std::move(sequence.frames)),
                        std::move(first)};
                });
                dialog.Show(
                    [frames, first, toast](huxerui::DialogContext) {
                        return DialogCard(QrsSequenceView(frames, first, toast));
                    },
                    huxerui::DialogOptions{});
            } catch (const std::exception&) {
                toast.Show(encode_error);
            }
        });
    };

    // ---- 新建订阅弹窗 ----
