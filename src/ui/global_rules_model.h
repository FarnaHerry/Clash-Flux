#pragma once

#include <huxerui/huxerui.h>
#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace clashflux::ui {

// One application-owned policy shared by the read-only list and every editor.
// Failed drafts survive navigation; readers only consume accepted rules.
// The caller owns worker tasks and publishes on the application thread.
template<class Rule> struct GlobalRulesModel {
    huxerui::State<std::vector<Rule>> rules{std::vector<Rule>{}};
    huxerui::State<std::vector<Rule>> accepted{std::vector<Rule>{}};
    huxerui::State<bool> loaded{false};
    huxerui::State<bool> saving{false};
    huxerui::State<bool> pending{false};
    huxerui::State<bool> dirty{false};
    huxerui::State<std::uint64_t> revision{0};
    huxerui::State<std::uint64_t> loadGeneration{0};
    huxerui::State<std::uint64_t> refresh{0};

    static void Replace(huxerui::State<std::vector<Rule>> destination, const std::vector<Rule>& source) {
        destination = source;
    }
    void Edited() {
        dirty = true;
        ++revision;
        ++loadGeneration; // reject even an ABA edit while a load was awaiting IO
    }
    bool PublishLoaded(std::uint64_t ticket, const std::vector<Rule>& source) {
        if (ticket != loadGeneration.Get() || saving.Get()) return false;
        bool changed = !std::ranges::equal(accepted.Get(), source);
        if (changed) Replace(accepted, source);
        if (!dirty.Get() && !std::ranges::equal(rules.Get(), source)) {
            Replace(rules, source);
            changed = true;
        }
        loaded = true;
        if (changed) ++revision;
        return true;
    }
    void PublishSaved(const std::vector<Rule>& source) {
        Replace(accepted, source);
        if (!pending.Get()) {
            Replace(rules, source);
            dirty = false;
        }
        loaded = true;
        ++revision;
        ++loadGeneration;
    }
};

} // namespace clashflux::ui
