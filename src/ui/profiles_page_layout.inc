// profiles_page_layout.inc — ProfilesPage 的列表布局与页面装配。
// 此文件由 profiles_page.cpp 在 ProfilesPage 函数体内包含。

    // 订阅按类型分区：与代理页/规则页共用 SectionTabBar（统一样式）；每个
    // 类型一页，Pager 保留各页滚动位置并负责跟手切换。不再把分组头
    // 混排在同一张网格里。
    std::vector<std::string> profileTypes{"remote", "local", "pptp"};
    for (const db::Profile& profile : profiles) {
        if (std::find(profileTypes.begin(), profileTypes.end(), profile.type) ==
            profileTypes.end()) {
            profileTypes.emplace_back(profile.type);
        }
    }
    const std::string profileQuery = profileSearch.Get().text;
    const auto matchesProfile = [&profileQuery](const db::Profile& profile) {
        return SearchTextMatches(profile.name, profileQuery) ||
               SearchTextMatches(profile.description, profileQuery) ||
               SearchTextMatches(profile.url, profileQuery) ||
               SearchTextMatches(profile.type, profileQuery);
    };
    // 只给当前搜索结果覆盖到的类型出标签；清空搜索后恢复完整类型集合。
    std::vector<std::string> visibleTypes;
    for (const std::string& type : profileTypes) {
        const auto hasType = [&type, &matchesProfile](const db::Profile& profile) {
            return profile.type == type && matchesProfile(profile);
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
        // Compact 视口用短标签（PPTP），否则第 4 个标签会被屏幕右缘
        // 截断半个字；桌面宽度充足，保留「PPTP 内网连接」这样的完整名称。
        typeTabs.push_back(
            SectionTab{type, compact ? profileTypeTabLabel(type)
                                     : profileTypeLabel(type)});
    }

    // 一个类型一页：只生成轻量的索引列表，真实 Profile 仍由共享缓存的
    // StateList 持有，卡片由 VirtualGrid 懒加载。
    std::vector<huxerui::View> typePages;
    typePages.reserve(visibleTypes.size());
    // VirtualGrid 的 item factory 在布局阶段调用；在这里订阅 State，目标变化才会
    // 更新网格声明并重建可见卡片。只在 factory 内 Get() 不会触发页面重组。
    const std::int64_t previewActiveId = reorderSession.Get()->activeId;
    const std::int64_t previewTargetId = reorderTarget.Get();
    for (std::size_t page = 0; page < visibleTypes.size(); ++page) {
        const std::string& type = visibleTypes[page];
        std::vector<std::size_t> typeIndices;
        for (std::size_t index = 0; index < profiles.Size(); ++index) {
            if (profiles[index].type == type && matchesProfile(profiles[index])) {
                typeIndices.push_back(index);
            }
        }
        const std::size_t typeCount = typeIndices.size();
        std::vector<std::int64_t> typeIds;
        typeIds.reserve(typeCount);
        for (const std::size_t profileIndex : typeIndices)
            typeIds.push_back(profiles[profileIndex].id);
        ProfileReorderActions pageReorder = reorderProfile;
        pageReorder.movePointer = [ids = typeIds, geometry = gridGeometry.Get(),
                                   preview = reorderProfile.preview](
                                      std::int64_t fromId, huxerui::Point position) {
            const std::int64_t closestId = geometry->Closest(ids, position);
            if (closestId != 0 && preview) preview(fromId, closestId);
        };
        const auto dropAtPointer = [ids = typeIds, geometry = gridGeometry.Get(),
                                    drop = reorderProfile.drop](
                                       std::int64_t fromId, huxerui::Point position) {
            const std::int64_t closestId = geometry->Closest(ids, position);
            if (closestId != 0 && drop) drop(fromId, closestId);
        };
        huxerui::View grid = huxerui::VirtualGrid(
                                 typeCount + (compact ? 1U : 0U),
                                 [profiles, typeIndices = std::move(typeIndices), typeIds = std::move(typeIds),
                                  typeCount, compact, tasks, toast, http, reload,
                                  pptpStates,
                                  vpnOperation,
                                  pptpAvailable = vpnModel->view.Get().pptpAvailable,
                                  togglePptpConnection, openEditInfo, showEditRules,
                                  showEditFile, showQr, selectionPending, theme,
                                  dialog, pageReorder, dropAtPointer,
                                  previewActiveId, previewTargetId,
                                  geometry = gridGeometry.Get()](std::size_t index) -> huxerui::View {
                                     if (index >= typeCount) {
                                         return CompactFloatingNavigationFooter()
                                             .Key("compact-floating-footer");
                                     }
                                     const db::Profile& profile =
                                         profiles[typeIndices[index]];
                                     store::PptpState pptpState{
                                         .profileId = profile.id,
                                         .toolsAvailable = pptpAvailable};
                                     for (const auto& state : pptpStates) {
                                         if (state.profileId == profile.id) {
                                             pptpState = state;
                                             break;
                                         }
                                     }
                                     huxerui::Point cardOffset{};
                                     const auto from = std::find(typeIds.begin(), typeIds.end(), previewActiveId);
                                     const auto to = std::find(typeIds.begin(), typeIds.end(), previewTargetId);
                                     if (from != typeIds.end() && to != typeIds.end()) {
                                         const std::size_t fromIndex = static_cast<std::size_t>(from - typeIds.begin());
                                         const std::size_t toIndex = static_cast<std::size_t>(to - typeIds.begin());
                                         const std::size_t destination =
                                             ProfileReorderDestination(index, fromIndex, toIndex);
                                         if (destination != index) {
                                             const auto sourceBounds = geometry->slots.find(profile.id);
                                             const auto targetBounds = geometry->slots.find(typeIds[destination]);
                                             if (sourceBounds != geometry->slots.end() &&
                                                 targetBounds != geometry->slots.end()) {
                                                 cardOffset = {
                                                     targetBounds->second.x - sourceBounds->second.x,
                                                     targetBounds->second.y - sourceBounds->second.y};
                                             }
                                         }
                                     }
                                     huxerui::View card = ProfileCard(
                                                profile, compact, tasks,
                                                toast, http, reload,
                                                profiles, selectionPending,
                                                pptpState,
                                                vpnOperation.Pending(profile.id),
                                                vpnOperation.action == VpnAction::Open,
                                                vpnOperation.Busy(),
                                                togglePptpConnection, openEditInfo,
                                                showEditRules, showEditFile,
                                                showQr, dialog, pageReorder)
                                         .Key(profile.id)
                                         .With(huxerui::Offset(huxerui::AnimateTo(
                                             cardOffset,
                                             huxerui::TweenSpec{.duration = theme.motion.reduced_motion ? 0.0 : 0.18,
                                                                .easing = huxerui::Easing::EaseOut})));
                                     huxerui::View slot = huxerui::Column {card}
                                         .With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
                                               ProfileSlotBounds{geometry, profile.id},
                                               huxerui::DropTarget::Accepts<ProfileDragPayload>())
                                         .On<huxerui::DropEvents<ProfileDragPayload>::Dropped>(
                                             [dropAtPointer](
                                                 const ProfileDragPayload& payload, const huxerui::DropEvent& event) {
                                                 dropAtPointer(payload.id, event.window_position);
                                             });
                                     if (previewActiveId != 0 && previewTargetId == profile.id &&
                                         previewTargetId != previewActiveId) {
                                         slot = std::move(slot).With(
                                             huxerui::Border(theme.colors.primary, kCardStrongOutlineWidth),
                                             huxerui::CornerRadius(kSelectableTileRadius));
                                     }
                                     return std::move(slot).Key("profile-slot-" + std::to_string(index));
                                 })
                                 .Columns(compact
                                              ? huxerui::GridColumns::Fixed(1)
                                              : huxerui::GridColumns::Adaptive(
                                                    kCardWidth))
                                 .EstimatedRowExtent(kCardHeight)
                                 .RowSpacing(kSectionCardSpacing)
                                 .ColumnSpacing(kSectionCardSpacing)
                                 .With(huxerui::Grow(1.0F), huxerui::ScrollBar(),
                                       huxerui::DropTarget::Accepts<ProfileDragPayload>())
                                 .On<huxerui::DropEvents<ProfileDragPayload>::Dropped>(
                                     [dropAtPointer](
                                         const ProfileDragPayload& payload, const huxerui::DropEvent& event) {
                                         dropAtPointer(payload.id, event.window_position);
                                     });
        typePages.push_back(huxerui::Column {grid}.With(
            huxerui::Grow(1.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))
            .Key("type-page-" + type));
    }

    huxerui::View profileBody = visibleTypes.empty()
        ? EmptyState(Localized("没有匹配的订阅"), app::images::search)
        : SectionTabPages(typePages, selectedType,
            [activeType, visibleTypes](std::size_t index) {
                if (index < visibleTypes.size()) activeType = visibleTypes[index];
            }, sectionMotion);
    if (!typeTabs.empty()) {
        profileBody = huxerui::Column {
            SectionTabBar(typeTabs, currentType,
                          [activeType](const std::string& key) {
                              activeType = key;
                          }, sectionMotion),
            std::move(profileBody),
        }.With(huxerui::Spacing(theme.spacing.small), huxerui::Grow(1.0F),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
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
        newQrContent = huxerui::TextEditingValue{""};
        pickedPath = "";
    };
    const ProfileCreateFields createFields{
        newName, newUrl, newQrContent, newTypeIdx, newDesc,
        newTimeout, newInterval, newAuto, newSys, newCore, newCert,
        newPptpServer, newPptpUsername, newPptpPassword, newPptpTimeout,
        newPptpRoutes, newPptpMppe,
        pickedPath, importing};
    // Desktop dialog and compact page mount the same creation form.
    const auto showCreateDialog = [createFields, tasks, toast, picker, http,
                                    pptpSupported, dialog] {
        dialog.Show([=](huxerui::DialogContext ctx) {
            return DialogCard(huxerui::Column{
                // embedded：DialogCard 是唯一容器层，布局与编辑订阅弹窗一致。
                ProfileCreatePage(createFields, tasks, toast, picker, http,
                                  pptpSupported, [ctx] { ctx.Dismiss(); },
                                  /*embedded=*/true),
            }.With(huxerui::Frame{.width = 420.0F},
                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
        });
    };
    const auto openCreate = [=] {
        resetCreateFields();
        if (navigation_enabled) {
            navigation.Push([=] {
                return ProfileAddMethodPage(createFields, tasks, toast,
                                            picker, http, pptpSupported,
                                            navigation);
            });
            return;
        }
        OpenProfileCreate(compact, createPageOpen, tasks, showCreateDialog);
    };

    huxerui::View addAction =
        huxerui::IconButton(app::images::add, Localized("新建订阅"))
            .With(huxerui::Tooltip(Localized("新建订阅")))
            .OnClick(openCreate)
            .Key("profile-add-subscription");
    huxerui::View profileSearchField = PillSearchField(
        profileSearch, Localized("搜索订阅"), compact ? addAction : huxerui::View{});
    // composable 的外层 Scope 也要声明 Grow，标题行才会把剩余宽度交给搜索胶囊。
    if (compact) profileSearchField = std::move(profileSearchField).With(huxerui::Grow(1.0F));
    huxerui::View pageActions;
    if (!compact) pageActions = huxerui::Row{std::move(pageActions), std::move(addAction)};
    huxerui::View profileListPage = PageScaffold(
        Localized("订阅"),
        std::move(pageActions),
        profiles.Empty()
            ? EmptyState(Localized("还没有订阅。点击右上角「新建订阅」导入。"), app::images::request)
            : std::move(profileBody),
        compact, true, true,
        std::nullopt, {}, std::move(profileSearchField));

    // IndexedPages 要求每一页都是挂载节点；平台函数负责 Android 编辑页是否
    // 存在，ProfilesPage 只提交同一份表单状态。
    huxerui::View mobileEditPage = ResponsiveProfileEditSurface(
        editPageId.Get(),
        ProfileEditFields{editName, editUrl, editType, editDesc, editTimeout,
                           editInterval, editAuto, editSys, editCore, editCert,
                           editPptpServer, editPptpUsername, editPptpPassword,
                           editPptpTimeout, editPptpRoutes, editPptpMppe},
        tasks, toast, [editPageId] { editPageId = 0; });

    huxerui::View mobileCreatePage = ResponsiveProfileCreateSurface(
        createPageOpen.Get(), createFields,
        tasks, toast, picker, http, pptpSupported,
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
