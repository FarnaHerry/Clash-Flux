// core_store_helpers.inc — 私有编排辅助函数
// 由 src/store/core_store.cppm 在 CoreStore 类体内包含。

    struct PreparedStart {
        singbox::CompileOptions options;
        singbox::CompileResult compiled;
        nlohmann::json config;
        std::string proxyGroups;
        std::filesystem::path directory, file;
        ~PreparedStart() {
            std::error_code ignored;
            if (!file.empty()) std::filesystem::remove(file, ignored);
            if (!directory.empty()) std::filesystem::remove(directory, ignored);
        }
    };


    bool restartPreparedCore(const std::string& profileYaml, std::unique_ptr<PreparedStart> candidate,
                             const CoreSnapshot& before, bool systemProxyEnabled, bool skipUnchanged,
                             std::stop_token cancel, std::chrono::steady_clock::time_point preparationBegan) {
        const bool wasActive = before.state == core::CoreState::Running || before.state == core::CoreState::Starting;
        if (skipUnchanged && wasActive && candidate->compiled.json == lastAppliedJson_) return true;
        // Once handoff starts, finish/rollback under the lifecycle lock. A newer
        // request will apply afterwards; never abandon a stopped data plane.
        if (cancel.stop_requested()) return false;
        const auto previous = lastAppliedOptions_;
        stream::logApplication("info", std::format("主计划准备完成，用时 {} ms（原内核保持运行）",
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - preparationBegan).count()));
        const auto handoffBegan = std::chrono::steady_clock::now();
        if (!stopCore()) return false;
        preparedStart_ = std::move(candidate);
        startCore(profileYaml, false, before.tunEnabled,
                  systemProxyEnabled ? std::optional<bool>(true) : std::nullopt);
#if defined(__ANDROID__)
        if (wasActive && snapshot().state != core::CoreState::Failed) {
            if (before.tunEnabled) clashflux_android_start_vpn(); else clashflux_android_start_core();
        }
        return snapshot().state != core::CoreState::Failed;
#else
        if (snapshot().state == core::CoreState::Running) {
            if (before.proxyListening && snapshot().proxyListening) {
                std::lock_guard lock(mutex_);
                snap_.connectedSince = before.connectedSince;
            }
            stream::logApplication("info", std::format("主计划交接完成，用时 {} ms（控制器就绪）",
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - handoffBegan).count()));
            return true;
        }
        const auto failure = snapshot().lastError;
        // 只有上一份计划仍拥有同一原生接口时才允许恢复，不能复活已释放的接口。
        const bool safe = previous && std::ranges::all_of(previous->nativeConnections, [&](const auto& old) {
            if (!old.connected) return true;
            return std::ranges::any_of(nativeSessions_, [&](const auto& current) {
                return current.id == old.id && current.connected && current.interfaceName == old.interfaceName &&
                       current.nativeConfig == old.nativeConfig && current.transportAddress == old.transportAddress;
            });
        });
        if (wasActive && safe && stopCore()) {
            preparedOptions_ = *previous;
            startCore(previous->profileYaml, false, previous->tunInbound,
                      systemProxyEnabled ? std::optional<bool>(true) : std::nullopt);
        }
        const bool restored = snapshot().state == core::CoreState::Running;
        std::lock_guard lock(mutex_);
        if (restored && before.proxyListening) snap_.connectedSince = before.connectedSince;
        snap_.lastError = failure + (restored ? "；已恢复上一份运行计划" : "；未能恢复上一份运行计划");
        return false;
#endif
    }

    static std::vector<clashflux::network_conflict::Tun> networkTuns(const nlohmann::json& config) {
        std::vector<clashflux::network_conflict::Tun> result;
        if (!config.contains("inbounds") || !config["inbounds"].is_array()) return result;
        for (const auto& inbound : config["inbounds"]) {
            if (!inbound.is_object() || inbound.value("type", "") != "tun") continue;
            clashflux::network_conflict::Tun tun{inbound.value("interface_name", "")};
            for (const char* key : {"address", "inet4_address", "inet6_address"}) {
                if (!inbound.contains(key)) continue;
                const auto& values = inbound[key];
                if (values.is_string()) tun.addresses.push_back(values.get<std::string>());
                else if (values.is_array()) for (const auto& value : values)
                    if (value.is_string()) tun.addresses.push_back(value.get<std::string>());
            }
            result.push_back(std::move(tun));
        }
        return result;
    }

    bool checkNetworkConflicts(const singbox::CompileOptions& options,
                               const nlohmann::json& config, std::string& error) {
#if !defined(__ANDROID__) && !defined(CLASHFLUX_IOS)
        namespace conflicts = clashflux::network_conflict;
        const auto observed = conflicts::Inspect();
        conflicts::Request request;
        request.tuns = networkTuns(config);
        for (const auto& connection : options.nativeConnections)
            if (connection.connected && !connection.interfaceName.empty())
                request.nativeInterfaces.push_back(connection.interfaceName);
        // Interface names, stale JSON and pidfiles alone never grant ownership.
        // A new GUI may adopt its own detached/service core. In that case require
        // both authenticated native control and independent live-owner evidence.
        const auto expected = snapshot().nativeStartedAt;
        const auto serviceAlive = service::coreRunningEvidence();
        const auto resident = expected ? 0 : readPidFile();
        const auto image = resident > 0 ? core::pidImageName(resident) : std::string{};
        const bool liveOwner = serviceAlive.value_or(false) || process_.running() ||
            (resident > 0 && core::pidAlive(resident) &&
             (image.empty() || image.find("sing-box") != std::string::npos));
        const bool hasActiveTunnel = std::ranges::any_of(observed.interfaces,
            [](const auto& link) { return link.up && link.tunnel; });
        if ((expected || liveOwner) && hasActiveTunnel) {
            const auto native = nativeApi_->runtime();
            if (native.ok && (expected ? native.startedAt == expected : liveOwner)) {
                auto previousJson = lastAppliedJson_;
                if (previousJson.empty()) {
                    const auto path = cfg::coreWorkDir() / "config.json";
                    std::error_code ec;
                    const auto size = std::filesystem::file_size(path, ec);
                    if (!ec && size <= 32 * 1024 * 1024) {
                        std::ifstream input(path, std::ios::binary);
                        previousJson.resize(static_cast<std::size_t>(size));
                        input.read(previousJson.data(), static_cast<std::streamsize>(previousJson.size()));
                        if (!input) previousJson.clear();
                    }
                }
                const auto previous = nlohmann::json::parse(previousJson, nullptr, false);
                if (previous.is_object()) request.owned = conflicts::OwnedInterfaces(observed, networkTuns(previous));
            }
        }
        error = conflicts::Check(observed, request);
        if (!error.empty()) return false;
        if (!observed.interfacesKnown || !observed.routesKnown)
            stream::logApplication("debug", "网络冲突检查部分证据不可用；未将未知状态当作无冲突");
#else
        // Android checks ConnectivityManager/VPN ownership before consent; iOS
        // remains deferred and must not claim desktop route inspection support.
        static_cast<void>(options); static_cast<void>(config); static_cast<void>(error);
#endif
        return true;
    }

    // Complete downloads, compilation, selector projection and kernel check
    // while the previous data plane is still alive. The exact staged bytes
    // are consumed by startCore; the stopped interval performs no downloads.
    std::unique_ptr<PreparedStart> prepareStartPlan(singbox::CompileOptions options,
                                                   std::string& error, std::stop_token cancel = {},
                                                   bool inspectNetwork = true) {
        if (cancel.stop_requested()) return {};
        auto plan = std::make_unique<PreparedStart>();
        const auto began = std::chrono::steady_clock::now();
        auto checkpoint = began;
        const auto trace = [&](std::string_view stage) {
            const auto now = std::chrono::steady_clock::now();
            stream::logApplication("info", std::format("profile-prepare stage={} elapsed_ms={}", stage,
                std::chrono::duration_cast<std::chrono::milliseconds>(now - checkpoint).count()));
            checkpoint = now;
        };
        const auto workDir = cfg::coreWorkDir();
        const auto publishFailure = [&] {
            std::lock_guard lock(mutex_);
            snap_.warnings = plan->compiled.warnings;
            snap_.fidelity = plan->compiled.fidelity;
        };
        // Catch another client's global interception before downloads can fail
        // through that tunnel; address/name checks follow actual compilation.
        if (inspectNetwork && !checkNetworkConflicts(options, nlohmann::json::object(), error)) return {};
        trace("network-preflight");
        bool geoPrepared = false;
        if (!prepareHttpRuleProviders(options, plan->compiled, error, nullptr, cancel, &geoPrepared)) {
            if (!cancel.stop_requested()) publishFailure();
            return {};
        }
        if (cancel.stop_requested()) return {};
        trace("providers-and-compile");
        if (!geoPrepared) prefetchRuleSets(plan->compiled.ruleSetResources, workDir);
        const bool directoryChanged = options.ruleSetDir != workDir.string();
        const bool geoInputsChanged = (!geoPrepared || directoryChanged) && clashflux::runtime_compile::GeoInputsChanged(
            !plan->compiled.ruleSetResources.empty(), directoryChanged);
        options.ruleSetDir = workDir.string();
        if (geoInputsChanged) plan->compiled = singbox::compileConfig(options, cancel);
        trace(geoInputsChanged ? "geo-prefetch-and-recompile" : "geo-prefetch-reuse");
        if (cancel.stop_requested()) return {};
        if (!plan->compiled.error.empty() || plan->compiled.json.empty()) {
            error = "订阅编译失败：" + plan->compiled.error; publishFailure(); return {};
        }
        plan->config = nlohmann::json::parse(plan->compiled.json, nullptr, false);
        auto& config = plan->config;
        if (!config.is_object()) { error = "订阅编译结果不是合法 JSON 对象"; return {}; }
        if (inspectNetwork && !checkNetworkConflicts(options, config, error)) return {};
        bool selectorChanged = false;
        plan->proxyGroups = proxyGroupsFromConfig(config, plan->compiled.sourceObjects, options.mainConnectionId, &selectorChanged);
        if (selectorChanged) plan->compiled.json = config.dump();
        if (cancel.stop_requested()) return {};
        trace("projection-and-json");
        if (!checkCompiledPlan(plan->compiled, error)) return {};
#if defined(__ANDROID__) || defined(CLASHFLUX_IOS)
        trace("check-deferred-to-libbox");
#else
        trace("kernel-check");
#endif
        const auto revisionText = setting("routing.applied_revision", "0");
        std::uint64_t revision = 0;
        const auto parsed = std::from_chars(revisionText.data(), revisionText.data() + revisionText.size(), revision);
        if (parsed.ec != std::errc() || parsed.ptr != revisionText.data() + revisionText.size() ||
            revision == std::numeric_limits<std::uint64_t>::max()) {
            error = "已应用计划修订号无效；保留原设置"; return {};
        }
        options.planRevision = revision + 1;
        plan->options = std::move(options);
        std::random_device random;
        for (int attempt = 0; attempt < 8; ++attempt) {
            auto directory = workDir / ("prepared-run-" + std::to_string(random()) + "-" + std::to_string(random()));
            std::error_code ec;
            if (std::filesystem::create_directory(directory, ec)) {
                plan->directory = std::move(directory); break;
            }
            if (ec && ec != std::errc::file_exists) break;
        }
        if (plan->directory.empty()) { error = "无法创建候选配置目录"; return {}; }
        plan->file = plan->directory / "config.json";
        std::ofstream output(plan->file, std::ios::binary | std::ios::trunc);
        output << plan->compiled.json; output.close();
        if (!output) { error = "候选配置写入失败"; return {}; }
        trace("stage-write");
        stream::logApplication("info", std::format("profile-prepare total_ms={} config_bytes={}",
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count(),
            plan->compiled.json.size()));
        return plan;
    }

    void cacheProxyGroups(const singbox::CompileOptions& options, singbox::CompileResult& compiled,
                          std::string snapshot) {
        // 记下这份快照对应的订阅文件指纹（mtime + 大小）：文件被重新下载或
        // 替换后缓存自动失效，不会一直返回旧的空快照。
        const std::filesystem::path file = selectedProfileFile();
        std::error_code ec;
        const auto stamp = std::filesystem::last_write_time(file, ec);
        const auto size = ec ? 0 : std::filesystem::file_size(file, ec);
        std::lock_guard lock(mutex_);
        compiledProxyGroups_ = std::move(snapshot);
        compiledProxyGroupsStamp_ = ec ? std::filesystem::file_time_type{}
                                       : stamp;
        compiledProxyGroupsSize_ = ec ? 0 : size;
        compiledSourceFiles_.clear();
        for (const auto& profile : db_->listProfiles()) {
            const auto id = routing::ConnectionId(profile.id);
            if (profile.file.empty() || (!profile.selected &&
                !std::ranges::any_of(options.auxiliarySources, [&](const auto& source) { return source.id == id; }))) continue;
            const auto sourceFile = cfg::profilesDir() / profile.file;
            std::error_code error;
            const auto sourceStamp = std::filesystem::last_write_time(sourceFile, error);
            const auto bytes = error ? 0 : std::filesystem::file_size(sourceFile, error);
            if (!error) compiledSourceFiles_[sourceFile] = {sourceStamp, bytes};
        }
        // 预览编译同样产出保真度账本：让「配置保真度」报告在内核没跑时也
        // 有内容（startCore 那条路径看到的是同一份订阅）。
        previewSourceObjects_ = std::make_shared<const std::vector<singbox::SourceObject>>(std::move(compiled.sourceObjects));
        if (snap_.state != core::CoreState::Running && snap_.state != core::CoreState::Starting) {
            snap_.fidelity = std::move(compiled.fidelity);
            snap_.sourceObjects = previewSourceObjects_;
        }
        compiledProfilesRevision_ = clashflux::persistence::persistence().profilesRevision();
        compiledPolicyText_ = setting("vpn.global_policy", "");
    }

    // 代理页快照（组 + 每节点 type/udp）由 clashflux.singbox 的纯函数生成，
    // 便于单测；这里只把「持久化的组选中项」从 settings 里喂进去。
    std::string proxyGroupsFromConfig(nlohmann::json& config,
        const std::vector<singbox::SourceObject>& objects = {}, std::string main = {}, bool* configChanged = nullptr) {
        if (main.empty()) for (const auto& profile : db_->listProfiles())
            if (profile.selected) main = routing::ConnectionId(profile.id);
        return singbox::BuildProxySnapshot(config, [this, &objects, &main](const std::string& group) {
            const auto key = "proxy.selection." + group;
            auto saved = setting(key, "");
            if (!saved.empty()) return saved;
            // 仅当前 main 迁移历史无来源键；次来源不继承其它订阅的同名选择。
            for (const auto& object : objects) {
                if (object.tag != group || object.sourceId != main || object.kind != vpn::TargetKind::Group) continue;
                const auto legacy = setting("proxy.selection." + object.objectId, "");
                for (const auto& member : objects) {
                    if (member.sourceId == main && member.objectId == legacy) {
                        setSetting(key, member.tag); return member.tag;
                    }
                }
            }
            return std::string{};
        }, configChanged);
    }

    inline singbox::CompileOptions routingOptions(
        const std::string& profileYaml, bool tunEnabled,
        bool speedTestOnly = false, const vpn::VpnPolicy* overridePolicy = nullptr,
        const std::optional<std::string>& mainOverride = std::nullopt,
        const singbox::ProfileSource* updated = nullptr, bool includeAuxiliary = true) {
        singbox::CompileOptions options;
        options.profileYaml = profileYaml;
        options.controller = cfg::controllerAddress();
#if !defined(__ANDROID__) && !defined(CLASHFLUX_IOS)
        options.nativeControllerPort = cfg::kNativeControllerPort;
#endif
        options.secret = secret_;
        options.mixedPort = mixedPort();
        options.httpPort = httpPort();
        options.socksPort = socksPort();
        options.mode = mode();
        options.allowLan = allowLan();
        options.ipv6 = ipv6Enabled();
        options.logLevel = logLevel();
        options.tunInbound = tunEnabled;
        options.speedTestOnly = speedTestOnly;
        options.ruleSetDir = cfg::coreWorkDir().string();
        options.ruleProviderDir = cfg::dataDir().string();
        options.ruleProviderCacheDir = (cfg::coreWorkDir() / "rule-providers").string();
        options.ruleSetLibrary = routing::DecodeRuleSetLibrary(setting("rules.resources", ""));
        auto profiles = db_->listProfiles();
        if (mainOverride) {
            for (auto& profile : profiles)
                if (profile.type != "pptp")
                    profile.selected = routing::ConnectionId(profile.id) == *mainOverride;
        }
        const auto policy = !includeAuxiliary ? vpn::VpnPolicy{} : overridePolicy ? *overridePolicy
            : routing::DecodePolicy(setting("vpn.global_policy", ""));
        std::string policyError;
        if (!routing::ValidatePlatformPolicy(policy, profiles, policyError)) throw std::runtime_error(policyError);
        routing::PopulateOptions(options, profiles, policy, nativeSessions_);
        for (const auto& profile : profiles) if (profile.selected)
            options.profileRuleEdits = routing::DecodeProfileRules(setting(routing::ProfileRulesKey(profile.id), ""));
        if (updated && updated->id == options.mainConnectionId) options.profileYaml = updated->content;
#if !defined(__ANDROID__) && !defined(CLASHFLUX_IOS)
        if (includeAuxiliary) for (const auto& profile : profiles) {
            if (profile.selected || profile.type == "pptp") continue;
            const auto id = routing::ConnectionId(profile.id);
            if (!std::ranges::any_of(policy.rules, [&](const auto& rule) {
                    return rule.enabled && rule.connectionId == id;
                })) continue;
            if (updated && updated->id == id) {
                options.auxiliarySources.push_back({id, profile.name, updated->content}); continue;
            }
            std::ifstream input(cfg::profilesDir() / profile.file, std::ios::binary);
            if (!input || profile.file.empty()) {
                options.auxiliarySources.push_back({id, profile.name, "", false}); continue;
            }
            options.auxiliarySources.push_back({id, profile.name,
                std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>())});
        }
#endif
        return options;
    }

    bool checkCompiledPlan(const singbox::CompileResult& compiled, std::string& error) {
        if (!compiled.error.empty() || compiled.json.empty()) {
            error = compiled.error.empty() ? "编排没有有效配置" : compiled.error; return false;
        }
#if !defined(__ANDROID__) && !defined(CLASHFLUX_IOS)
        const auto file = cfg::coreWorkDir() / ("candidate-" + cfg::randomSecret() + ".json");
        struct CandidateFile {
            std::filesystem::path path;
            ~CandidateFile() { std::error_code ec; std::filesystem::remove(path, ec); }
        } cleanup{file};
        std::ofstream output(file, std::ios::binary | std::ios::trunc);
        output << compiled.json; output.close();
        if (!output) { error = "无法写入候选编排"; return false; }
        return core::checkConfig(cfg::singboxBinary(), cfg::coreWorkDir(), file, error);
#else
        // libbox 在后台启动时核验；没有谎报成桌面的 CLI 预检查。
        error.clear(); return true;
#endif
    }

    bool validatePlanTargets(const singbox::CompileOptions& options,
                             const singbox::CompileResult& compiled,
                             const vpn::VpnPolicy* policy, std::string& error) {
        if (!policy) return true;
        for (const auto& rule : policy->rules) {
            if (!rule.enabled) continue;
            if (rule.targetKind != vpn::TargetKind::Default) {
                if (!std::ranges::any_of(compiled.sourceObjects, [&](const auto& object) {
                    return object.sourceId == rule.connectionId && object.kind == rule.targetKind &&
                           object.objectId == rule.targetObject;
                })) { error = "规则目标对象不存在：" + rule.connectionId + "/" + rule.targetObject; return false; }
            } else if (rule.connectionId != options.mainConnectionId &&
                !std::ranges::any_of(options.auxiliarySources, [&](const auto& source) { return source.id == rule.connectionId; }) &&
                !std::ranges::any_of(options.nativeConnections, [&](const auto& source) { return source.id == rule.connectionId; })) {
                error = "规则目标来源不存在：" + rule.connectionId; return false;
            }
        }
        return true;
    }

    bool prepareHttpRuleProviders(singbox::CompileOptions& options,
                                 singbox::CompileResult& compiled, std::string& error,
                                 const vpn::VpnPolicy* policy = nullptr, std::stop_token cancel = {},
                                 bool* geoPrepared = nullptr) {
        bool preparedGeo = false;
        if (geoPrepared) *geoPrepared = false;
        clashflux::runtime_compile::Normalize(options);
        rule_provider_cache::PreparationStats stats;
        const auto began = std::chrono::steady_clock::now();
        rule_provider_cache::Operations operations;
        operations.maxConcurrentFetches = 3;
        operations.revalidate = [this](const singbox::HttpRuleProviderResource& resource,
                                 const std::filesystem::path& file, const singbox::HttpCacheValidators& validators) {
          const auto request = [&](const std::map<std::string, std::string>& headers, bool conditional) {
            rule_provider_cache::FetchResult fetched;
#if defined(__ANDROID__)
            // curl has no TLS here. Reuse the platform bridge on the task
            // thread, with system trust, a bounded body, and explicit direct IO.
            constexpr int kDownloadTimeoutSecs = 20;
            constexpr int kDownloadAttempts = 2;
            constexpr auto kRetryDelay = std::chrono::milliseconds{250};
#if !defined(NDEBUG)
            const auto started = std::chrono::steady_clock::now();
#endif
            clashflux::android::ProfileHttpResponse response;
            for (int attempt = 1; attempt <= kDownloadAttempts; ++attempt) {
#if !defined(NDEBUG)
                __android_log_print(ANDROID_LOG_INFO, "ClashFluxImport",
                                    "rule-set begin name=%s attempt=%d max_bytes=%zu",
                                    resource.name.c_str(), attempt, resource.maxBytes);
#endif
                response = clashflux::android::DownloadProfile(
                    resource.url, kDownloadTimeoutSecs, false,
                    resource.maxBytes, true, headers);
#if !defined(NDEBUG)
                const auto elapsed = std::chrono::duration_cast<
                    std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started).count();
                __android_log_print(ANDROID_LOG_INFO, "ClashFluxImport",
                                    "rule-set end name=%s attempt=%d status=%ld bytes=%zu error=%d retryable=%d elapsed_ms=%lld",
                                    resource.name.c_str(), attempt, response.status,
                                    response.body.size(), response.error.empty() ? 0 : 1,
                                    response.retryable ? 1 : 0,
                                    static_cast<long long>(elapsed));
#endif
                const bool completeHttpResponse = ((response.status >= 200 &&
                    response.status < 300) || (conditional && response.status == 304)) && response.error.empty();
                if (completeHttpResponse || !response.retryable ||
                    attempt == kDownloadAttempts) break;
                std::this_thread::sleep_for(kRetryDelay);
            }
            fetched.ok = response.error.empty() && ((response.status >= 200 && response.status < 300) ||
                (conditional && response.status == 304));
            if (fetched.ok && response.status != 304) {
                std::ofstream output(file, std::ios::binary | std::ios::trunc);
                output << response.body;
                output.close();
                if (!output) { fetched.error = "无法完成平台规则集临时文件写入"; fetched.ok = false; return fetched; }
            }
#else
            api::ClashApi::DownloadOptions download;
            download.timeoutSecs = 20;
            download.maxBytes = resource.maxBytes;
            download.headers = headers;
            download.acceptNotModified = conditional;
            download.validate = [&resource](const auto& temp) {
                std::string failure;
                const auto text = singbox::ReadRuleProviderText(temp, resource.maxBytes, failure);
                if (text) singbox::ValidateHttpRuleProvider(resource, *text, failure);
                return failure;
            };
            const auto response = api_->downloadToFile(resource.url, file, download);
            fetched.ok = response.ok;
#endif
            fetched.status = response.status;
            fetched.notModified = response.status == 304;
            fetched.error = response.error;
            if (!fetched.ok && fetched.error.empty()) fetched.error = "HTTP " + std::to_string(response.status);
            if (const auto entry = response.headers.find("etag"); entry != response.headers.end())
                fetched.validators.etag = entry->second;
            if (const auto entry = response.headers.find("last-modified"); entry != response.headers.end())
                fetched.validators.lastModified = entry->second;
            return fetched;
          };
          auto headers = resource.headers;
          std::size_t bytes = 0;
          for (const auto& [name, value] : headers) bytes += name.size() + value.size() + 4;
          const bool conditional = (!validators.etag.empty() || !validators.lastModified.empty()) &&
              bytes + validators.etag.size() + validators.lastModified.size() + 40 <= 16384;
          if (conditional) {
              if (!validators.etag.empty()) headers.emplace("if-none-match", validators.etag);
              if (!validators.lastModified.empty()) headers.emplace("if-modified-since", validators.lastModified);
          }
          auto fetched = request(headers, conditional);
          // Conditional headers never follow redirects. Retry using the original
          // request policy so an originally header-free URL can still redirect.
          if (conditional && fetched.status >= 300 && fetched.status < 400 && fetched.status != 304)
              fetched = request(resource.headers, false);
          return fetched;
        };
        operations.check = [this, policy, cancel, &preparedGeo](const auto& candidate, auto& result, std::string& reason) {
            if (cancel.stop_requested()) return false;
            if (!validatePlanTargets(candidate, result, policy, reason)) return false;
            prefetchRuleSets(result.ruleSetResources, cfg::coreWorkDir());
            preparedGeo = true;
            // Only GEO prefetch can change compiler inputs here. HTTP/ASN
            // snapshots are already fully lowered and must still pass check.
            if (clashflux::runtime_compile::GeoInputsChanged(!result.ruleSetResources.empty(), false))
                result = singbox::compileConfig(candidate, cancel);
            return !cancel.stop_requested() && checkCompiledPlan(result, reason);
        };
        operations.commit = [](const auto& temp, const auto& dest, std::string& reason) {
            return api::CommitFile(temp, dest, reason);
        };
        const bool ready = rule_provider_cache::Prepare(options, operations, compiled, error, &stats, cancel);
        if (geoPrepared) *geoPrepared = ready && preparedGeo;
        stream::logApplication("info", std::format(
            "profile-providers ready={} total_ms={} discovery_compile_calls={} discovery_compile_us={} validated_snapshot_reuses={} conversion_cache_hits={}", ready,
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count(),
            stats.compileCount, stats.compileMicros, stats.validatedSnapshotReuses, stats.conversionCacheHits));
        if (ready) for (const auto& warning : compiled.warnings)
            if (warning.starts_with("HTTP 规则集「")) stream::logApplication("warning", warning);
        return ready;
    }

    // 三种托管形态的存活判定：服务托管问服务、接管的外部实例看 pidfile、
    // 直接 spawn 看进程句柄。接管形态没有 pidfile（对端不是本应用拉的）
    // 时只好信任——WS 断流会在 UI 层表现为无数据。
    bool coreAlive() {
        if (managedByService_) {
            // Controller responsiveness is independent of the service-owned
            // child. An explicit STOPPED must win even if a port still answers.
            // Unknown IPC evidence cannot turn the instance into Failed.
            return service::coreRunningEvidence().value_or(true);
        }
        if (adopted_) {
            const long pid = readPidFile();
            if (pid <= 0) return true;  // 无 pidfile：信任
#if defined(_WIN32)
            // Windows 没有 /proc：旧实现用 /proc/<pid> 判定，接管/驻留形态
            // 恒为 false，正在运行的内核会被误报成「启动后立即退出」。
            return core::pidAlive(pid);
#else
            // POSIX：僵尸进程同样有 /proc 条目（子进程退出码尚未被回收时），
            // 不能算存活——否则 detached 启动的内核已经 FATAL 退出，这里还会
            // 白等 30s 并报「控制器未就绪」。
            std::ifstream stat(std::format("/proc/{}/stat", pid));
            if (!stat) return false;
            const std::string content((std::istreambuf_iterator<char>(stat)),
                                      std::istreambuf_iterator<char>());
            const auto nameEnd = content.rfind(')');
            if (nameEnd == std::string::npos || nameEnd + 2 >= content.size())
                return false;
            return content[nameEnd + 2] != 'Z';
#endif
        }
        return process_.running();
    }

    // detached spawn（CLI/接管形态）没有本进程输出管道：诊断尾部只能读
    // <workDir>/core.log 中本次启动之后追加的内容（旧的启动记录不算）。
    static std::string detachedTail(const std::filesystem::path& workDir,
                                    std::uintmax_t offset) {
        std::ifstream in(workDir / "core.log", std::ios::binary);
        if (!in) return {};
        in.seekg(static_cast<std::streamoff>(offset));
        std::string line;
        std::string last;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) last = line;
        }
        if (last.size() > 300) last.resize(300);
        return core::stripAnsi(std::move(last));
    }

    // 规则集缓存清理：坏文件（0 字节、被写入错误内容、被中间设备截断）会让
    // 内核每次启动都 FATAL（parse rule-set: read rule: unexpected EOF），
    // 而按周刷新的 mtime 判断永远认为它"新鲜"。只清理应用自有 GEO 与
    // 默认内核 cache.db；用户提供的 .srs 不属于此缓存。
    void purgeRuleSetCache(const std::filesystem::path& workDir,
                          const std::vector<singbox::RuleSetResource>& resources) {
        if (resources.empty()) return;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(workDir, ec)) {
            if (ec) break;
            std::error_code typeError;
            if (!entry.is_regular_file(typeError) || typeError) continue;
            const std::string name = entry.path().filename().string();
            bool ruleSetCache = false;
#if !defined(__ANDROID__)
            const auto suffix = name.ends_with(".srs.part") ? std::string_view{".srs.part"}
                              : name.ends_with(".srs") ? std::string_view{".srs"}
                                                       : std::string_view{};
            if (!suffix.empty()) {
                const auto tag = std::string_view(name).substr(0, name.size() - suffix.size());
                const auto url = singbox::BuiltinRuleSetUrl(tag);
                ruleSetCache = !url.empty() && std::ranges::any_of(resources,
                    [&](const auto& resource) {
                        return resource.tag == tag && resource.url == url;
                    });
            }
#endif
            // The native database also contains subscription/personal resources
            // and selector state. It is never owned by a GEO repair request.
            if (!ruleSetCache) continue;
            std::error_code removeError;
            std::filesystem::remove(entry.path(), removeError);
        }
    }

    // 编译器清单包含未命中与已命中的自有 GEO，才能真正按周刷新。
    // 不扫描原生 JSON 的远程来源，不改下载 detour/format/更新策略。
    void prefetchRuleSets(const std::vector<singbox::RuleSetResource>& resources,
                          const std::filesystem::path& workDir) {
#if defined(__ANDROID__)
        // Android GEO 使用 Gradle 固定来源资产；不能改为运行时刷新。
        return;
#endif
        for (const auto& resource : resources) {
            const auto& tag = resource.tag;
            const auto& url = resource.url;
            const auto ownedUrl = singbox::BuiltinRuleSetUrl(tag);
            if (ownedUrl.empty() || url != ownedUrl) continue;
            const auto dest = workDir / (tag + ".srs");
            std::error_code ec;
            bool stale = true;
            if (std::filesystem::exists(dest, ec) && !ec) {
                if (!singbox::RuleSetCacheValid(dest)) {
                    // 坏缓存必须重下，否则内核会一直读同一个坏文件。
                    std::error_code removeError;
                    std::filesystem::remove(dest, removeError);
                } else {
                    const auto mtime = std::filesystem::last_write_time(dest, ec);
                    if (!ec) {
                        stale = decltype(mtime)::clock::now() - mtime >
                                std::chrono::hours{24 * 7};
                    }
                }
            }
            if (!stale) continue;
            api::ClashApi::DownloadOptions download;
            download.timeoutSecs = 8;  // 直连快速失败，不拖慢启动
            download.validate = [](const std::filesystem::path& temp) {
                return singbox::RuleSetCacheValid(temp) ? std::string{}
                    : std::string{"文件不是兼容的 SRS 规则集（文件头校验失败）"};
            };
            const auto result = api_->downloadToFile(url, dest, download);
            // 校验临时文件后才提交，HTTP 200 错误页也不能毁掉旧缓存。
            // 校验仅筛文件头，完整解压与规则校验仍由内核负责。
            const bool cacheUsable = singbox::RuleSetCacheValid(dest);
            if (!result.ok || !cacheUsable) {
                if (!cacheUsable) {
                    std::error_code removeError;
                    std::filesystem::remove(dest, removeError);
                }
                // 用拼接而非 std::format：clang-21/libc++ 对含 std::string 实参的
                // 多参数 std::format 会实例化宽字符候选并硬错误（见 src/api.cpp）。
                const std::string reason = result.ok
                                               ? std::string{"文件内容不是规则集"}
                                               : result.error;
                stream::logApplication(
                    "warning",
                    cacheUsable ? "规则集 " + tag + " 刷新失败，沿用本地缓存：" +
                                      reason
                                : "规则集 " + tag + " 预取失败，改由内核在线拉取：" +
                                      reason);
            }
        }
    }

    long readPidFile() {
        std::ifstream in(cfg::coreWorkDir() / "core.pid");
        long pid = 0;
        in >> pid;
        return pid;
    }

    // 系统代理所有权：当前桌面代理是否指向本应用写入的端口（开启时记录
    // proxy.system_port）。只有指向本应用时才允许撤销/重指，避免误关或
    // 抢占其他代理软件（clash-verge/FlClash 等）接管中的设置。
    bool systemProxyOurs() {
#if defined(__ANDROID__)
        return false;
#else
        const std::string current = sysproxy::currentProxy();
        return !current.empty() &&
               current == std::format("127.0.0.1:{}",
                                      setting("proxy.system_port",
                                              std::to_string(mixedPort())));
#endif
    }

    void ensureOpen() {
        // 数据库由启动流程的 Persistence::open 负责（异步、带 0→1 迁移）；
        // 这里只构建同步门面与内核 API 端点。Persistence 未就绪时先建空 secret
        // 的端点，启动任务随后调用 ensureSecret() 刷新。
        std::call_once(initFlag_, [this] {
            db_ = std::make_unique<db::Db>(cfg::databaseFile());
            if (clashflux::persistence::persistence().ready()) {
                ensureSecret();
            } else {
                api_ = std::make_unique<api::ClashApi>(cfg::controllerBaseUrl(),
                                                       secret_);
            }
            stream::logApplication("debug", "持久化门面与内核 API 端点已初始化");
        });
    }

    void fail(std::string error) {
        stream::logApplication("error", "内核操作失败：" + error);
        {
            std::lock_guard lock(mutex_);
            snap_.state = core::CoreState::Failed;
            snap_.runningSince = {};
            snap_.lastError = std::move(error);
        }
        captureHealthEvidence(false);
    }
