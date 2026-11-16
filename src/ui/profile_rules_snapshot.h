#pragma once
// Include after importing clashflux.singbox. Owned application view data only.
#include <cstdint>
#include <memory>
#include <string>

namespace clashflux::ui {
struct ProfileRulesData {
    std::string content, error;
    singbox::ProfileRuleEdits edits;
    singbox::RuleSetLibrary library;
    singbox::ProfileRules effective; // Read-only target projection.
    std::shared_ptr<const singbox::TargetSourceModel> target;
    bool supported = false;
    std::size_t retained = 0; // Precomputed on the worker before publication.
};
struct ProfileRulesSnapshot {
    std::int64_t id = 0;
    std::shared_ptr<const ProfileRulesData> data = std::make_shared<const ProfileRulesData>();
    std::shared_ptr<const singbox::ProfileRules> editor; // Lazy legacy editor contract, until R3.
    std::size_t editorBytes = 0;
    std::size_t retainedBytes() const { return sizeof(*this) + data->retained + editorBytes; }
};
} // namespace clashflux::ui
