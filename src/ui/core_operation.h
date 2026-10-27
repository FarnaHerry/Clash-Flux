#pragma once

#include <huxerui/huxerui.h>

#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>

namespace clashflux::ui {

enum class CoreAction { Idle, Start, Stop, Restart, Mode, SystemProxy, Tun };

struct CoreCommand {
    CoreAction action = CoreAction::Idle;
    bool enabled = false;
    std::string mode;
    bool operator==(const CoreCommand&) const = default;
};

// One application-owned operation, shared by every entry point. A publication
// revision also rejects reads started before a command or a synchronous edit.
struct CoreOperations {
    huxerui::State<CoreCommand> command{CoreCommand{}};
    huxerui::State<std::uint64_t> revision{0};
    std::uint64_t serial = 0;

    bool Busy() const { return command.Get().action != CoreAction::Idle; }
    void Invalidate() { revision = revision.Get() + 1; }
    std::optional<std::uint64_t> Begin(CoreCommand next) {
        if (Busy() || next.action == CoreAction::Idle) return std::nullopt;
        Invalidate();
        command = std::move(next);
        return ++serial;
    }
    bool Finish(std::uint64_t ticket) {
        if (!Busy() || ticket != serial) return false;
        Invalidate();
        command = CoreCommand{};
        return true;
    }
    bool AcceptsPoll(std::uint64_t startedAt) const {
        return !Busy() && startedAt == revision.Get();
    }
};

// The caller retains its operation ticket throughout authorization/install.
// Retry exactly once after confirmed installation; cancellation must not apply
// the original intent or start another authorization attempt.
template <class Result, class Install, class Execute>
huxerui::Task<std::optional<Result>> ResumeCoreCommandAfterServiceInstall(
    CoreCommand command, Install install, Execute execute) {
    if (!co_await install()) co_return std::nullopt;
    co_return co_await execute(std::move(command));
}

// Own both the model and completion until application work settles. The caller
// passes its application TaskScope; page retirement cannot strand the command.
// Completion runs on the UI thread, after shared pending has been cleared.
template <class Result, class Owner, class Work, class Completed>
bool LaunchCoreOperation(huxerui::TaskScope tasks, std::shared_ptr<Owner> owner,
                         CoreCommand command, Work work, Completed completed) {
    const auto ticket = owner->operations.Begin(command);
    if (!ticket) return false;
    tasks.Launch([owner, ticket = *ticket, command = std::move(command),
                  work = std::move(work), completed = std::move(completed)]() mutable
                     -> huxerui::Task<void> {
        std::optional<Result> result;
        std::exception_ptr error;
        try { result.emplace(co_await work(std::move(command))); }
        catch (...) { error = std::current_exception(); }
        if (owner->operations.Finish(ticket)) completed(std::move(result), error);
    });
    return true;
}

} // namespace clashflux::ui
