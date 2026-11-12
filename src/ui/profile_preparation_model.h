#pragma once
#include <huxerui/huxerui.h>
#include <cstdint>
#include <map>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace clashflux::ui {
enum class ProfilePreparationPhase { Preparing, Ready, Failed };
struct ProfilePreparationView {
    std::uint64_t ticket = 0;
    std::string file, error;
    ProfilePreparationPhase phase = ProfilePreparationPhase::Preparing;
    bool operator==(const ProfilePreparationView&) const = default;
};
struct ProfilePreparationModel {
    huxerui::State<std::map<std::int64_t, ProfilePreparationView>> preparations{
        std::map<std::int64_t, ProfilePreparationView>{}};
    std::map<std::int64_t, std::stop_source> preparationCancels;
    std::uint64_t preparationSerial = 0;
    std::optional<ProfilePreparationView> Preparation(std::int64_t id) const {
        const auto& states = preparations.Get();
        const auto found = states.find(id);
        return found == states.end() ? std::nullopt : std::optional{found->second};
    }
    std::optional<std::pair<std::uint64_t, std::stop_token>> BeginPreparation(std::int64_t id, const std::string& file) {
        const auto previous = Preparation(id);
        if (previous && previous->file == file && previous->phase == ProfilePreparationPhase::Preparing) return {};
        if (const auto found = preparationCancels.find(id); found != preparationCancels.end()) found->second.request_stop();
        auto& cancel = preparationCancels[id] = std::stop_source{};
        const auto ticket = ++preparationSerial;
        preparations.Update([&](auto& states) { states[id] = {ticket, file, {}, ProfilePreparationPhase::Preparing}; });
        return std::pair{ticket, cancel.get_token()};
    }
    bool FinishPreparation(std::int64_t id, std::uint64_t ticket, bool ready, std::string error) {
        const auto current = Preparation(id);
        if (!current || current->ticket != ticket || current->phase != ProfilePreparationPhase::Preparing) return false;
        preparationCancels.erase(id);
        preparations.Update([&](auto& states) {
            auto& state = states.at(id);
            state.phase = ready ? ProfilePreparationPhase::Ready : ProfilePreparationPhase::Failed;
            state.error = std::move(error);
        });
        return true;
    }
    void ForgetPreparation(std::int64_t id, std::uint64_t ticket) {
        const auto current = Preparation(id);
        if (!current || current->ticket != ticket) return;
        if (const auto found = preparationCancels.find(id); found != preparationCancels.end()) {
            found->second.request_stop(); preparationCancels.erase(found);
        }
        preparations.Update([&](auto& states) { states.erase(id); });
    }
    template<class Profile> void PrunePreparations(const std::vector<Profile>& profiles) {
        const auto states = preparations.Get();
        for (const auto& [id, state] : states) {
            bool exists = false;
            for (const auto& p : profiles) if (p.id == id && p.file == state.file) { exists = true; break; }
            if (!exists) ForgetPreparation(id, state.ticket);
        }
    }
};
} // namespace clashflux::ui
