    // Called on a worker. Tracing uses a fresh compatibility snapshot between
    // authenticated instance checks; a retained WebSocket body has no instance ID.
    clashflux::connection_trace::Frame tracedConnections(std::uint64_t expectedEpoch) {
        using namespace clashflux::connection_trace;
#if defined(__ANDROID__) || defined(CLASHFLUX_IOS)
        return {};
#else
        std::unique_lock lifecycle(lifecycleMutex_, std::try_to_lock);
        if (!lifecycle.owns_lock()) return {};
        const auto before = snapshot();
        if (before.state != core::CoreState::Running || before.runtimeEpoch != expectedEpoch ||
            !before.nativeStartedAt || !before.appliedRulePlan || before.appliedRulePlan->origins.empty()) return {};
        api::ClashApi controller(cfg::controllerBaseUrl(), secret_);
        api::NativeApi native(cfg::nativeControllerBaseUrl(), secret_);
        lifecycle.unlock();
        std::shared_ptr<const Binding> binding;
        { std::lock_guard lock(mutex_); binding = connectionRuleBinding_; }
        auto response = Read(before.appliedRulePlan, before.runtimeEpoch, before.planRevision,
            before.nativeStartedAt, binding, {
                .instance = [&] { return native.startedAt(); },
                .rules = [&]() -> std::optional<std::string> {
                    auto result = controller.rules();
                    return result.ok ? std::optional{std::move(result.body)} : std::nullopt;
                },
                .connections = [&]() -> std::optional<std::string> {
                    auto result = controller.traceConnections();
                    return result.ok ? std::optional{std::move(result.body)} : std::nullopt;
                },
                .current = [&](const Binding& candidate) { return connectionTraceCurrent(candidate); },
            });
        if (!response.binding) return {};
        {
            std::lock_guard lock(mutex_);
            if (snap_.state != core::CoreState::Running || snap_.runtimeEpoch != before.runtimeEpoch ||
                snap_.planRevision != before.planRevision || snap_.nativeStartedAt != before.nativeStartedAt ||
                snap_.appliedRulePlan != before.appliedRulePlan) return {};
            // Concurrent list/detail readers share the first published baseline.
            // A later reader must not reclassify already observed new IDs.
            if (connectionRuleBinding_ && connectionRuleBinding_->epoch == before.runtimeEpoch &&
                connectionRuleBinding_->revision == before.planRevision &&
                connectionRuleBinding_->instance == before.nativeStartedAt &&
                connectionRuleBinding_->plan == before.appliedRulePlan) response.binding = connectionRuleBinding_;
            else connectionRuleBinding_ = response.binding;
        }
        return response;
#endif
    }

    bool connectionTraceCurrent(const clashflux::connection_trace::Binding& binding) {
        std::lock_guard lock(mutex_);
        return snap_.state == core::CoreState::Running && snap_.runtimeEpoch == binding.epoch &&
            snap_.planRevision == binding.revision && snap_.nativeStartedAt == binding.instance &&
            snap_.appliedRulePlan == binding.plan;
    }
