    // The tray belongs to the application. Its menu is rebuilt only when a
    // menu-visible value changes, and remains usable after the window retires.
    if (trayAvailable) {
        const huxerui::Locale trayLocale = huxerui::UseEnvironment<huxerui::Locale>();
        static_cast<void>(trayLocale);
        const TrayMenuText menuText{
            .showWindow = huxerui::UseString(Localized("显示主窗口")),
            .noProfiles = huxerui::UseString(Localized("暂无可用订阅")),
            .selectProfile = huxerui::UseString(Localized("选择订阅")),
            .selectLine = huxerui::UseString(Localized("切换当前订阅线路")),
            .noLines = huxerui::UseString(Localized("暂无可切换线路")),
            .autoLineSuffix = huxerui::UseString(Localized("（自动测速，不支持手动切换）")),
            .startCore = huxerui::UseString(Localized("启动内核")),
            .stopCore = huxerui::UseString(Localized("停止内核")),
            .connect = huxerui::UseString(Localized("连接代理")),
            .disconnect = huxerui::UseString(Localized("断开连接")),
            .systemProxy = huxerui::UseString(Localized("系统代理")),
            .tun = huxerui::UseString(Localized("TUN 模式")),
            .lightweight = huxerui::UseString(Localized("轻量级托盘模式")),
            .quit = huxerui::UseString(Localized("退出")),
        };
        trayRuntime->refresh = [weakTrayRuntime, tray, settingsModel, coreModel,
                                profilesModel, proxiesModel, startupTasks,
                                toast, menuText, finishExit, startupTrayEnabled] {
            const auto trayRuntime = weakTrayRuntime.lock();
            if (!trayRuntime) return;
            const SettingsView settings = settingsModel->view.Get();
            const bool enabled = settings.ready ? settings.trayEnabled : startupTrayEnabled;
            if (!enabled) {
                if (trayRuntime->presentationKey != "hidden") {
                    trayRuntime->HideTray();
                    trayRuntime->presentationKey = "hidden";
                }
                return;
            }
            if (!trayRuntime->control) return;
            const CoreControl& control = *trayRuntime->control;
            const CoreView core = coreModel->view.Get();
            const bool running = core.core.state == core::CoreState::Running;
            const bool menuRunning = control.Running();
            const bool proxyEnabled = control.SystemProxyEnabled();
            const bool tunEnabled = control.TunEnabled();
            const bool busy = control.Busy();
            const auto& profiles = profilesModel->list.Get();
            const auto& groups = proxiesModel->snapshot.Get().groups;

            std::string key;
            const auto append = [&key](std::string_view value) {
                key += std::to_string(value.size());
                key += ':';
                key += value;
            };
            for (const std::string* label : {
                     &menuText.showWindow, &menuText.noProfiles,
                     &menuText.selectProfile, &menuText.selectLine,
                     &menuText.noLines, &menuText.autoLineSuffix,
                     &menuText.startCore, &menuText.stopCore,
                     &menuText.connect, &menuText.disconnect,
                     &menuText.systemProxy, &menuText.tun,
                     &menuText.lightweight, &menuText.quit}) append(*label);
            key += std::format("{}{}{}{}{}{}{}{}",
                running, menuRunning, proxyEnabled, tunEnabled, busy,
                core.systemProxyActive, core.TunActive(), settings.lightweightTray);
            key += core.ProxyActive() ? '1' : '0';
            std::size_t profileCount = 0;
            for (const db::Profile& profile : profiles) {
                if (profile.type == "pptp") continue;
                ++profileCount;
                key += std::to_string(profile.id);
                append(profile.name);
                key += profile.selected ? '1' : '0';
            }
            std::string structureKey = std::format("p{};g{};", profileCount, groups.size());
            for (const ProxyGroupSnapshot& group : groups) {
                structureKey += std::format("{}:{};", group.selectable,
                    group.selectable ? group.nodes.size() : 0);
                append(group.name);
                append(group.displayName);
                append(group.current);
                key += group.selectable ? '1' : '0';
                for (const std::string& node : group.nodes) {
                    append(node);
                    if (const auto found = group.nodeLabels.find(node);
                        found != group.nodeLabels.end()) append(found->second);
                }
            }
            if (key == trayRuntime->presentationKey) return;
            // Linux assigns D-Bus IDs in tree order. When the tree changes,
            // Plasma can retain a previous checkbox or submenu at a reused ID.
            // Publish an empty tree for one poll before replacing its structure.
            if (!trayRuntime->menuStructureKey.empty() &&
                trayRuntime->menuStructureKey != structureKey) {
                trayRuntime->HideTray();
                return;
            }
            const auto windowGeneration = trayRuntime->windowGeneration;

            std::vector<huxerui::MenuEntry> entries;
            entries.push_back(huxerui::MenuItem(menuText.showWindow,
                [trayRuntime] { trayRuntime->ShowWindow(); }));
            entries.push_back(huxerui::MenuSection{});
            std::vector<huxerui::MenuEntry> profileEntries;
            for (const db::Profile& profile : profiles) {
                if (profile.type == "pptp") continue;
                profileEntries.push_back(huxerui::MenuItem(profile.name,
                    [startupTasks, trayRuntime, toast, profilesModel, proxiesModel,
                     windowGeneration,
                     id = profile.id] {
                        LaunchProfileSelection<std::string>(startupTasks, profilesModel, id,
                            [id](std::stop_token cancel) -> huxerui::Task<std::string> {
                            co_return co_await RunOnTaskThread([id, cancel] {
                                auto& store = store::profilesStore();
                                std::string error;
                                store.activate(id, cancel, &error);
                                return error;
                            });
                        }, [trayRuntime, toast, profilesModel, proxiesModel, windowGeneration](
                            std::optional<std::string> result, std::exception_ptr failure, std::uint64_t) {
                            std::string error = result.value_or(std::string{});
                            if (failure) {
                                try { std::rethrow_exception(failure); }
                                catch (const std::exception& exception) { error = exception.what(); }
                                catch (...) { error = "订阅切换失败"; }
                            }
                            profilesModel->PublishCached();
                            if (!error.empty()) {
                                if (trayRuntime->retired ||
                                    trayRuntime->windowGeneration != windowGeneration)
                                    stream::logApplication("error", error);
                                else toast.Show(error);
                            }
                            if (error.empty()) proxiesModel->RequestRefresh();
                        });
                    }).Checked(profile.selected));
            }
            if (profileEntries.empty()) profileEntries.push_back(
                huxerui::MenuItem(menuText.noProfiles, [] {}).Enabled(false));
            entries.push_back(huxerui::MenuItem(menuText.selectProfile,
                                                std::move(profileEntries)));

            std::vector<huxerui::MenuEntry> lineEntries;
            for (const ProxyGroupSnapshot& group : groups) {
                if (!group.selectable) {
                    lineEntries.push_back(huxerui::MenuItem(
                        group.displayName + menuText.autoLineSuffix, [] {}).Enabled(false));
                    continue;
                }
                std::vector<huxerui::MenuEntry> nodes;
                for (const std::string& node : group.nodes) {
                    const auto found = group.nodeLabels.find(node);
                    nodes.push_back(huxerui::MenuItem(
                        found == group.nodeLabels.end() ? node : found->second,
                        [startupTasks, trayRuntime, toast, proxiesModel,
                         windowGeneration,
                         groupName = group.name, node] {
                            startupTasks.Launch([trayRuntime, toast, proxiesModel,
                                                 groupName, node,
                                                 windowGeneration]() -> huxerui::Task<void> {
                                const bool ok = co_await RunOnTaskThread([groupName, node] {
                                    return SelectProxyLine(groupName, node);
                                });
                                if (!ok) {
                                    const std::string error =
                                        store::coreStore().snapshot().lastError;
                                    if (trayRuntime->retired ||
                                        trayRuntime->windowGeneration != windowGeneration)
                                        stream::logApplication("error", error.empty()
                                            ? "线路切换失败" : error);
                                    else toast.Show(error.empty() ? Localized("线路切换失败")
                                                                  : huxerui::StringVariant(error));
                                }
                                proxiesModel->RequestRefresh();
                            });
                        }).Checked(node == group.current));
                }
                if (nodes.empty()) nodes.push_back(
                    huxerui::MenuItem(menuText.noLines, [] {}).Enabled(false));
                lineEntries.push_back(huxerui::MenuItem(group.displayName, std::move(nodes)));
            }
            if (lineEntries.empty()) lineEntries.push_back(
                huxerui::MenuItem(menuText.noLines, [] {}).Enabled(false));
            entries.push_back(huxerui::MenuItem(menuText.selectLine,
                                                std::move(lineEntries)));
            entries.push_back(huxerui::MenuItem(
                control.ProxyActive() ? menuText.disconnect : menuText.connect,
                [trayRuntime] {
                    if (trayRuntime->control) {
                        const CoreControl control = trayRuntime->MenuControl();
                        control.Run({.action = control.ProxyActive()
                            ? CoreAction::Disconnect : CoreAction::Connect});
                    }
                }).Checked(control.ProxyActive()).Enabled(!busy));
            entries.push_back(huxerui::MenuItem(
                menuRunning ? menuText.stopCore : menuText.startCore,
                [trayRuntime] {
                    if (trayRuntime->control) {
                        const CoreControl control = trayRuntime->MenuControl();
                        control.Run({.action = control.Running()
                            ? CoreAction::Stop : CoreAction::Start});
                    }
                }).Checked(menuRunning).Enabled(!busy));
            entries.push_back(huxerui::MenuItem(menuText.systemProxy,
                [trayRuntime] {
                    if (trayRuntime->control) {
                        const CoreControl control = trayRuntime->MenuControl();
                        control.Run({.action = CoreAction::SystemProxy,
                                     .enabled = !control.SystemProxyEnabled()});
                    }
                }).Checked(proxyEnabled).Enabled(!busy));
            entries.push_back(huxerui::MenuItem(menuText.tun,
                [trayRuntime] {
                    if (trayRuntime->control) {
                        const CoreControl control = trayRuntime->MenuControl();
                        control.Run({.action = CoreAction::Tun,
                                     .enabled = !control.TunEnabled()});
                    }
                }).Checked(tunEnabled).Enabled(!busy));
            entries.push_back(huxerui::MenuItem(menuText.lightweight,
                [settingsModel] {
                    const bool enabled = !settingsModel->view.Get().lightweightTray;
                    store::coreStore().setSetting("tray.lightweight",
                                                  enabled ? "true" : "false");
                    settingsModel->Update([enabled](SettingsView& view) {
                        view.lightweightTray = enabled;
                    });
                }).Checked(settings.lightweightTray));
            entries.push_back(huxerui::MenuSection{});
            entries.push_back(huxerui::MenuItem(menuText.quit, finishExit));

            huxerui::ImageVariant icon = app::images::tray_default;
            if (running) {
                if (core.TunActive()) icon = app::images::tray_tun;
                else if (core.systemProxyActive) icon = app::images::tray_system_proxy;
            }
            tray.Show(std::move(icon), huxerui::SystemTrayOptions{
                .tooltip = "Clash-Flux", .menu = std::move(entries)});
            trayRuntime->presentationKey = std::move(key);
            trayRuntime->menuStructureKey = std::move(structureKey);
        };
    }
