// profiles_page_layout.inc — ProfilesPage 的列表布局与页面装配。
// 此文件由 profiles_page.cpp 在 ProfilesPage 函数体内包含。

    // 只生成轻量级的分组索引，真实 Profile 仍由 StateList 持有，卡片仍由
    // VirtualGrid 懒加载；这样分组不会退化成预先组合整个 View 列表。
    std::vector<std::string> profileTypes{"remote", "local", "pptp", "openvpn"};
    for (const db::Profile& profile : profiles) {
        if (std::find(profileTypes.begin(), profileTypes.end(), profile.type) ==
            profileTypes.end()) {
            profileTypes.push_back(profile.type);
        }
    }

    std::vector<ProfileGridItem> profileItems;
    std::vector<std::size_t> profileSpans;
    constexpr std::size_t kFullRowSpan = std::numeric_limits<std::size_t>::max();
    for (const std::string& type : profileTypes) {
        std::size_t count = 0;
        for (const db::Profile& profile : profiles) {
            if (profile.type == type) ++count;
        }
        if (count == 0) continue;

        profileItems.push_back(ProfileGridItem{
            .kind = ProfileGridItemKind::GroupHeader,
            .type = type,
            .profileCount = count,
        });
        profileSpans.push_back(kFullRowSpan);
        for (std::size_t index = 0; index < profiles.Size(); ++index) {
            if (profiles[index].type != type) continue;
            profileItems.push_back(ProfileGridItem{
                .kind = ProfileGridItemKind::Profile,
                .type = type,
                .profileIndex = index,
            });
            profileSpans.push_back(1);
        }
    }
    if (compact) {
        profileItems.push_back(ProfileGridItem{
            .kind = ProfileGridItemKind::Footer,
        });
        profileSpans.push_back(kFullRowSpan);
    }

    huxerui::View profileGrid = huxerui::VirtualGrid(
                                    profileItems.size(),
                                    [profiles, profileItems, compact, tasks, toast, http,
                                     reload,
                                     pptpStates, openVpnStates, isConnectionSelected,
                                     toggleConnection, openEditInfo, showEditRules,
                                     showEditFile, showQr, selectionPending, theme,
                                     dialog](
                                        std::size_t index)
                                         -> huxerui::View {
                                        const ProfileGridItem& item = profileItems[index];
                                        if (item.kind == ProfileGridItemKind::Footer) {
                                            return CompactFloatingNavigationFooter()
                                                .Key("compact-floating-footer");
                                        }
                                        if (item.kind == ProfileGridItemKind::GroupHeader) {
                                            return huxerui::Row{
                                                huxerui::Text(
                                                    profileTypeLabel(item.type),
                                                    huxerui::TextRole::Title),
                                                huxerui::Spacer{},
                                                huxerui::Text(std::format(
                                                    "{} 个", item.profileCount)),
                                            }
                                                .With(
                                                    huxerui::Padding(
                                                        huxerui::EdgeInsets::Symmetric(
                                                            theme.spacing.small,
                                                            theme.spacing.extra_small)),
                                                    huxerui::Background(
                                                        theme.colors.surface_container_low),
                                                    huxerui::CornerRadius(12.0F),
                                                    huxerui::CrossAlign(
                                                        huxerui::CrossAxisAlignment::Center))
                                                .Key("profile-group-" + item.type);
                                        }

                                        const db::Profile& profile =
                                            profiles[item.profileIndex];
                                        store::PptpState pptpState =
                                            profile.type == "pptp"
                                                ? store::vpnStore().state(profile.id)
                                                : store::PptpState{};
                                        for (const auto& state : pptpStates) {
                                            if (state.profileId == profile.id) {
                                                pptpState = state;
                                                break;
                                            }
                                        }
                                        store::OpenVpnState openVpnState =
                                            profile.type == "openvpn"
                                                ? store::vpnStore().openVpnState(profile.id)
                                                : store::OpenVpnState{};
                                        for (const auto& state : openVpnStates) {
                                            if (state.profileId == profile.id) {
                                                openVpnState = state;
                                                break;
                                            }
                                        }
                                        return ProfileCard(
                                                   profile, compact, tasks,
                                                   toast, http, reload,
                                                   profiles, selectionPending,
                                                   pptpState, openVpnState,
                                                   isConnectionSelected(profile.id),
                                                   toggleConnection, openEditInfo,
                                                   showEditRules, showEditFile, showQr,
                                                   dialog)
                                            .Key(profile.id);
                                    })
                                    .Columns(compact
                                                 ? huxerui::GridColumns::Fixed(1)
                                                 : huxerui::GridColumns::Adaptive(
                                                       kCardWidth))
                                    .EstimatedRowExtent(kCardHeight)
                                    .ItemSpans(std::move(profileSpans))
                                    .RowSpacing(kCardGap)
                                    .ColumnSpacing(kCardGap)
                                    .With(huxerui::Grow(1.0F), huxerui::ScrollBar());

    huxerui::View batchActions = huxerui::Row{};
    if (connectionSelection.Size() > 0) {
        batchActions = huxerui::Row {
            huxerui::Text(std::format("已选 {} 个连接", connectionSelection.Size())),
            huxerui::Button("打开").OnClick(openSelectedConnections),
            huxerui::Button("关闭").OnClick(closeSelectedConnections),
        }.With(huxerui::Spacing(8.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    }

    const auto resetCreateFields = [=] {
        newName = huxerui::TextEditingValue{""};
        newUrl = huxerui::TextEditingValue{""};
        newTypeIdx = 0;
        newDesc = huxerui::TextEditingValue{""};
        newTimeout = huxerui::TextEditingValue{""};
        newInterval = huxerui::TextEditingValue{"1440"};
        newAuto = true;
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
        newQrContent = huxerui::TextEditingValue{""};
        newConfigContent = huxerui::TextEditingValue{""};
        pickedPath = "";
    };
    const ProfileCreateFields createFields{
        newName, newUrl, newQrContent, newConfigContent, newTypeIdx, newDesc,
        newTimeout, newInterval, newAuto, newSys, newCore, newCert,
        newPptpServer, newPptpUsername, newPptpPassword, newPptpTimeout,
        newPptpRoutes, newPptpMppe, newOpenVpnConfig, newOpenVpnRoutes,
        pickedPath, importing};
    const auto openCreate = [=] {
        resetCreateFields();
        if (navigation_enabled) {
            navigation.Push([=] {
                return ProfileAddMethodPage(createFields, tasks, toast,
                                            picker, http, pptpSupported,
                                            openvpnSupported, navigation);
            });
            return;
        }
        OpenProfileCreate(compact, createPageOpen, tasks, showCreateDialog);
    };

    huxerui::View profileListPage = PageScaffold(
        "订阅",
        huxerui::Row {
            std::move(batchActions),
            huxerui::IconButton(app::images::add, "新建订阅")
                .With(huxerui::Tooltip("新建订阅"))
                .OnClick(openCreate),
        },
        profiles.Empty()
            ? huxerui::View{
                  huxerui::Column {
                      huxerui::Text("还没有订阅。点击右上角「新建订阅」导入。")
                          .Style(huxerui::TextStyle{
                              huxerui::Font::System(font_size::kBody),
                              theme.colors.on_surface_variant}),
                  }.With(huxerui::Padding(32.0F),
                         huxerui::Grow(1.0F),
                         huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
                         huxerui::CrossAlign(
                             huxerui::CrossAxisAlignment::Center))}
            : std::move(profileGrid),
        true);

    // IndexedPages 要求每一页都是挂载节点；平台函数负责 Android 编辑页是否
    // 存在，ProfilesPage 只提交同一份表单状态。
    huxerui::View mobileEditPage = ResponsiveProfileEditSurface(
        editPageId.Get(),
        ProfileEditFields{editName, editUrl, editType, editDesc, editTimeout,
                           editInterval, editAuto, editSys, editCore, editCert,
                           editPptpServer, editPptpUsername, editPptpPassword,
                           editPptpTimeout, editPptpRoutes, editPptpMppe,
                           editOpenVpnConfig, editOpenVpnRoutes},
        tasks, toast, [editPageId] { editPageId = 0; });

    huxerui::View mobileCreatePage = ResponsiveProfileCreateSurface(
        createPageOpen.Get(), createFields,
        tasks, toast, picker, http, pptpSupported, openvpnSupported,
        [createPageOpen] { createPageOpen = false; });

    huxerui::View mobileFilePage = ProfileFilePage(
        filePageId.Get(), fileContent, fileLoading, tasks, toast,
        [filePageId] { filePageId = 0; });
    huxerui::View mobileRulesPage = ProfileRulesPage(
        rulesPageId.Get(), editYamlText, editRules, editRuleInput,
        editRulesDirty, tasks, toast, [rulesPageId] { rulesPageId = 0; });

    return huxerui::IndexedPages(
               std::vector<huxerui::View>{std::move(profileListPage),
                                          std::move(mobileEditPage),
                                          std::move(mobileCreatePage),
                                          std::move(mobileFilePage),
                                          std::move(mobileRulesPage)},
               ResponsiveProfilePageIndex(compact, editPageId, createPageOpen,
                                          filePageId, rulesPageId))
        .With(huxerui::Grow(1.0F));
