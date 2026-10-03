/// @file    PixOperators.cpp
/// @brief   PIX UI opening and honest capture-readiness queries in the shared registry.
/// @author  Hasegawa Jin
/// @date    2026-10-02
#include "PixOperators.hpp"
#include "Util/PixLauncher.hpp"

#include <Editor/Op/OperatorGroups.hpp>
#include <Engine/Util/StringUtils.hpp>

#include <utility>

namespace fbzz::editor {
namespace {

struct PixOperatorState {
    core::PixCaptureStatus capture;
    std::filesystem::path directory;
    std::filesystem::path executable;
    std::string installationReason;
    bool installed = false;
};

PixOperatorState ReadPixOperatorState(const PixOperatorServices& services)
{
    PixOperatorState state;
    if (services.readCaptureStatus) state.capture = services.readCaptureStatus();
    else state.capture.reason = "PIX startup state is unavailable.";
    state.directory = state.capture.installedDirectory.empty()
        ? services.defaultInstalledDirectory : state.capture.installedDirectory;
    state.installed = ResolvePixUiExecutable(state.directory, state.executable, state.installationReason);
    return state;
}

OpResult MakePixStatusResult(const PixOperatorState& state)
{
    auto data = OpData::MakeObject();
    data.Set("requested", OpData(state.capture.requested));
    data.Set("captureReady", OpData(state.capture.ready));
    data.Set("installed", OpData(state.installed));
    data.Set("requiresRestart", OpData(!state.capture.ready));
    data.Set("captureDirectory", OpData(util::StringUtils::PathToUtf8(state.capture.installedDirectory)));
    data.Set("installedDirectory", OpData(util::StringUtils::PathToUtf8(state.directory)));
    data.Set("pixExecutable", OpData(util::StringUtils::PathToUtf8(state.executable)));
    data.Set("installationReason", OpData(state.installationReason));
    data.Set("launchArgument", OpData("--pix-capture"));
    std::string reason = state.capture.reason;
    if (reason.empty() && !state.capture.ready)
        reason = "GPU capture is not enabled for this session. Start the next DX12 session with --pix-capture.";
    data.Set("reason", OpData(reason));
    auto result = OpResult::Data(std::move(data));
    result.message = state.capture.ready ? "GPU capture ready." : reason;
    if (!state.installed) result.message += " " + state.installationReason;
    return result;
}

} /// @note namespace

void RegisterPixOperators(OperatorRegistry& registry, PixOperatorServices services)
{
    EditorOperator status;
    status.id = "tools.pix_status";
    status.label = "PIX Status";
    status.category = "Tools";
    status.desc = "Read PIX startup capture readiness without loading or launching anything.";
    status.kind = OpKind::Query;
    status.exec = [services](OpContext&, const OpArgs&) {
        return MakePixStatusResult(ReadPixOperatorState(services));
    };
    registry.Register(std::move(status));

    EditorOperator open;
    open.id = "tools.pix_open";
    open.label = "Open PIX...";
    open.category = "Tools";
    open.desc = "Open installed PIX UI. GPU capture requires --pix-capture at application startup.";
    open.caution = "Does not capture, restart the Editor, save edits or attach to a process.";
    open.kind = OpKind::Action;
    open.poll = [services](const OpContext&, const OpArgs&) {
        return services.launchUi && ReadPixOperatorState(services).installed;
    };
    open.exec = [services](OpContext&, const OpArgs&) {
        const auto state = ReadPixOperatorState(services);
        if (!state.installed) return OpResult::Err("PIX_NOT_FOUND", state.installationReason);
        if (!services.launchUi) return OpResult::Err("PIX_LAUNCH_UNAVAILABLE", "PIX UI launch is unavailable.");
        std::string error;
        if (!services.launchUi(state.executable, error)) {
            auto result = OpResult::Err("PIX_LAUNCH_FAILED", error);
            result.data = MakePixStatusResult(state).data;
            return result;
        }
        auto result = MakePixStatusResult(state);
        result.data.Set("opened", OpData(true));
        result.message = "PIX opened. " + result.message;
        return result;
    };
    registry.Register(std::move(open));
}

void RegisterPixOperators(OperatorRegistry& registry)
{
    PixOperatorServices services;
    services.readCaptureStatus = [] { return core::GetPixCaptureStatus(); };
    services.defaultInstalledDirectory = DefaultPixInstalledDirectory();
    services.launchUi = LaunchPixUi;
    RegisterPixOperators(registry, std::move(services));
}

} /// @note namespace fbzz::editor
