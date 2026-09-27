// profiles_page_layout.inc — ProfilesPage 的列表布局与页面装配。
// 此文件由 profiles_page.cpp 在 ProfilesPage 函数体内包含。

    // 订阅按类型分区：与代理页/规则页共用 SectionTabBar（统一样式）；每个
    // 类型一页，IndexedPages 保留各页滚动位置、只测量当前页。不再把分组头
    // 混排在同一张网格里。
    std::vector<std::string> profileTypes{"remote", "local", "pptp", "openvpn"};
    for (const db::Profile& profile : profiles) {
        if (std::find(profileTypes.begin(), profileTypes.end(), profile.type) ==
            profileTypes.end()) {
            profileTypes.push_back(profile.type);
        }
    }
    // 只给有订阅的类型出标签。
    std::vector<std::string> visibleTypes;
    for (const std::string& type : profileTypes) {
        const auto hasType = [&type](const db::Profile& profile) {
            return profile.type == type;
        };
        if (std::any_of(profiles.begin(), profiles.end(), hasType)) {
            visibleTypes.push_back(type);
        }
    }
    std::string currentType = activeType.Get();
    if (std::find(visibleTypes.begin(), visibleTypes.end(), currentType) ==
        visibleTypes.end()) {
        currentType = visibleTypes.empty() ? std::string{} : visibleTypes.front();
    }
    std::size_t selectedType = 0;
    for (std::size_t i = 0; i < visibleTypes.size(); ++i) {
        if (visibleTypes[i] == currentType) selectedType = i;
    }

    std::vector<SectionTab> typeTabs;
    typeTabs.reserve(visibleTypes.size());
    for (const std::string& type : visibleTypes) {
        // Compact 视口用短标签（PPTP / OpenVPN），否则第 4 个标签会被屏幕右缘
        // 截断半个字；桌面宽度充足，保留「PPTP 内网连接」这样的完整名称。
        typeTabs.push_back(
            SectionTab{type, compact ? profileTypeTabLabel(type)
                                     : profileTypeLabel(type)});
    }

    // 一个类型一页：只生成轻量的索引列表，真实 Profile 仍由共享缓存的
    // StateList 持有，卡片由 VirtualGrid 懒加载。
    std::vector<huxerui::View> typePages;
    typePages.reserve(visibleTypes.size());
    for (std::size_t page = 0; page < visibleTypes.size(); ++page) {
        const std::string& type = visibleTypes[page];
        std::vector<std::size_t> typeIndices;
        for (std::size_t index = 0; index < profiles.Size(); ++index) {
            if (profiles[index].type == type) typeIndices.push_back(index);
        }
        const bool selectedPage = page == selectedType;
        const std::size_t typeCount = typeIndices.size();
        huxerui::View grid = huxerui::VirtualGrid(
                                 typeCount + (compact ? 1U : 0U),
                                 [profiles, typeIndices = std::move(typeIndices),
                                  typeCount, compact, tasks, toast, http, reload,
                                  pptpStates, openVpnStates, isConnectionSelected,
                                  toggleConnection, openEditInfo, showEditRules,
                                  showEditFile, showQr, selectionPending, theme,
                                  dialog](std::size_t index) -> huxerui::View {
                                     if (index >= typeCount) {
                                         return CompactFloatingNavigationFooter()
                                             .Key("compact-floating-footer");
                                     }
                                     const db::Profile& profile =
                                         profiles[typeIndices[index]];
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
                                             ? store::vpnStore().openVpnState(
                                                   profile.id)
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
                                                showEditRules, showEditFile,
                                                showQr, dialog)
                                         .Key(profile.id);
                                 })
                                 .Columns(compact
                                              ? huxerui::GridColumns::Fixed(1)
                                              : huxerui::GridColumns::Adaptive(
                                                    kCardWidth))
                                 .EstimatedRowExtent(kCardHeight)
                                 .RowSpacing(kCardGap)
                                 .ColumnSpacing(kCardGap)
                                 .With(huxerui::Grow(1.0F), huxerui::ScrollBar());
        // 分区组件默认支持滑动（见 ui.h）；与代理页/规则页共用 SectionTabSwipeHandler。
        // 手机端「订阅」是 Pager 的一级页，处理器会在 6pt 内抢先认领指针会话，因此
        // 页内滑动与整页翻不冲突；两端没有相邻分区时不认领，让 Pager 整页翻
        // （见 section_swipe.h）。
        if (kSectionTabsSwipeDefault) {
            std::function<void()> swipePrev;
            if (page > 0) {
                swipePrev = [activeType, visibleTypes, page] {
                    activeType = visibleTypes[page - 1];
                };
            }
            std::function<void()> swipeNext;
            if (page + 1 < visibleTypes.size()) {
                swipeNext = [activeType, visibleTypes, page] {
                    activeType = visibleTypes[page + 1];
                };
            }
            grid = std::move(grid).On<huxerui::ViewEvents::PointerIntercept>(
                SectionTabSwipeHandler(swipeOrigin, swipeOwned,
                                       std::move(swipePrev),
                                       std::move(swipeNext)));
        }
        // 与代理页相同的换页过渡：透明度 + 横向轻移。
        typePages.push_back(
            huxerui::Column {std::move(grid)}
                .With(huxerui::Grow(1.0F),
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
                      huxerui::Transition{
                          huxerui::AnimateTo(
                              selectedPage ? 1.0F : 0.0F,
                              huxerui::TweenSpec{
                                  .duration = theme.motion.reduced_motion
                                                  ? 0.0
                                                  : theme.motion.normal,
                                  .easing = huxerui::Easing::EaseOut})}
                          .Opacity(0.82F, 1.0F)
                          .Offset({12.0F, 0.0F}, {}))
                .Key("type-page-" + type));
    }

    // 无订阅时（首帧镜像尚未同步、或库里确实为空）不能构造 IndexedPages：
    // 它要求至少一页，否则抛 invalid_argument 直接终止进程。下面的
    // page 已经在 profiles.Empty() 时用空状态视图替换掉这里的内容。
    huxerui::View profileBody = huxerui::Column{};
    if (!typePages.empty()) {
        profileBody = huxerui::IndexedPages(std::move(typePages), selectedType)
                          .With(huxerui::Grow(1.0F));
    }
    if (!typeTabs.empty()) {
        profileBody = huxerui::Column {
            SectionTabBar(typeTabs, currentType,
                          [activeType](const std::string& key) {
                              activeType = key;
                          }),
            std::move(profileBody),
        }.With(huxerui::Spacing(theme.spacing.small), huxerui::Grow(1.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    }

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
            : std::move(profileBody),
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
