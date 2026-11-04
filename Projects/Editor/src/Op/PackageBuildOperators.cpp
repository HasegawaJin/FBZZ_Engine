/// @file    PackageBuildOperators.cpp
/// @brief   配布ビルドの開始・事前確認・状態・取消を共有登録簿へ載せる。
/// @author  Hasegawa Jin
/// @date    2026-10-05
#include <Editor/PackageBuildService.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Util/UndoStack.hpp>

#include <string>

namespace fbzz::editor {
namespace {

bool CandidateSettings(const PackageBuildService& service, const OpArgs& args, BuildSettings& settings, std::string& reason)
{
    settings = service.Settings();
    if (args.Has("outputDirectory")) settings.outputDirectory = args.GetString("outputDirectory");
    if (args.Has("productName")) settings.productName = args.GetString("productName");
    if (args.Has("version")) settings.version = args.GetString("version");
    if (args.Has("iconPath")) settings.iconPath = args.GetString("iconPath");
    if (args.Has("developmentBuild")) settings.developmentBuild = args.GetBool("developmentBuild");
    if (args.Has("stripEditorAssets")) settings.stripEditorAssets = args.GetBool("stripEditorAssets");
    if (const OpData* scenes = args.GetData("scenes")) {
        if (!scenes->IsArray()) { reason = "scenes must be an array of path/enabled objects."; return false; }
        settings.scenes.clear();
        for (const OpData& entry : scenes->AsArray()) {
            const OpData* path = entry.Find("path");
            const OpData* enabled = entry.Find("enabled");
            if (!entry.IsObject() || !path || !path->IsString() || path->AsString().empty()
                || (enabled && !enabled->IsBool())) { reason = "Every scene requires a nonempty path and optional boolean enabled."; return false; }
            for (const auto& member : entry.AsObject()) {
                if (member.first != "path" && member.first != "enabled") { reason = "Unknown scene setting: " + member.first; return false; }
            }
            settings.scenes.push_back({ path->AsString(), !enabled || enabled->AsBool() });
        }
    }
    return true;
}

std::vector<OpParam> SettingsParams()
{
    return {
        { "outputDirectory", OpParamType::String, "Safe project-relative or absolute output folder; replaces that folder only on success.", false, {} },
        { "productName", OpParamType::String, "Windows executable file name without path separators.", false, {} },
        { "version", OpParamType::String, "Product version written into the game manifest.", false, {} },
        { "iconPath", OpParamType::String, "Project-relative icon source; empty preserves the default icon.", false, {} },
        { "developmentBuild", OpParamType::Bool, "Development runtime rather than Release.", false, {} },
        { "stripEditorAssets", OpParamType::Bool, "Exclude editor-only source and configuration from the package.", false, {} },
        { "scenes", OpParamType::Data, "Scene entries [{path,enabled}] replacing the package scene selection.", false, {} }
    };
}

OpData DescribeSettings(const BuildSettings& settings)
{
    auto data = OpData::MakeObject();
    data.Set("outputDirectory", settings.outputDirectory);
    data.Set("productName", settings.productName);
    data.Set("version", settings.version);
    data.Set("iconPath", settings.iconPath);
    data.Set("developmentBuild", settings.developmentBuild);
    data.Set("stripEditorAssets", settings.stripEditorAssets);
    auto scenes = OpData::MakeArray();
    for (const auto& scene : settings.scenes) {
        auto entry = OpData::MakeObject();
        entry.Set("path", scene.path);
        entry.Set("enabled", scene.enabled);
        scenes.Push(std::move(entry));
    }
    data.Set("scenes", std::move(scenes));
    return data;
}

} /// @note namespace

void RegisterPackageBuildOperators(OperatorRegistry& registry)
{
    EditorOperator inspect;
    inspect.id = "build.settings.inspect";
    inspect.label = "Inspect Build Settings";
    inspect.category = "Build";
    inspect.kind = OpKind::Query;
    inspect.desc = "配布パネルの現在の設定とシーン一覧を返す。";
    inspect.poll = [](const OpContext& context, const OpArgs&) { return context.ctx.packageBuild != nullptr; };
    inspect.exec = [](OpContext& context, const OpArgs&) {
        return OpResult::Data(DescribeSettings(context.ctx.packageBuild->Settings()));
    };
    registry.Register(std::move(inspect));

    EditorOperator configure;
    configure.id = "build.settings.configure";
    configure.label = "Configure Build Settings";
    configure.category = "Build";
    configure.kind = OpKind::Mutation;
    configure.undoLabel = "Edit Build Settings";
    configure.desc = "配布設定を部分更新する。scenesは順序を含めた全置換。設定を保存し、ビルドは開始しない。";
    configure.params = SettingsParams();
    configure.poll = [](const OpContext& context, const OpArgs&) { return context.ctx.packageBuild != nullptr; };
    configure.exec = [](OpContext& context, const OpArgs& args) {
        auto& ctx = context.ctx;
        auto& service = *ctx.packageBuild;
        BuildSettings after;
        std::string reason;
        if (!CandidateSettings(service, args, after, reason)) return OpResult::Err("BAD_ARG", reason);
        const auto before = service.Settings();
        const auto project = ctx.projectRoot;
        const auto apply = [&ctx, project](const BuildSettings& value, std::string& error) {
            if (ctx.projectRoot != project || !ctx.packageBuild) {
                error = "Project changed.";
                return false;
            }
            ctx.packageBuild->Settings() = value;
            ctx.packageBuild->RefreshChecks(ctx, value);
            ctx.requestEditorSettingsSave = true;
            return true;
        };
        if (!apply(after, reason)) return OpResult::Err("BUILD_SETTINGS_FAILED", reason);
        auto result = OpResult::Data(DescribeSettings(after));
        result.command = std::make_unique<CheckedLambdaCommand>("Edit Build Settings",
            [apply, after](std::string& error) { return apply(after, error); },
            [apply, before](std::string& error) { return apply(before, error); });
        return result;
    };
    registry.Register(std::move(configure));

    EditorOperator check;
    check.id = "build.package_check";
    check.label = "Check Game Package";
    check.category = "Build";
    check.desc = "Read the same package preflight as Build Settings without changing settings or starting a process.";
    check.kind = OpKind::Query;
    check.params = SettingsParams();
    check.exec = [](OpContext& context, const OpArgs& args) {
        PackageBuildService* service = context.ctx.packageBuild;
        if (!service) return OpResult::Err("NO_PACKAGE_SERVICE", "Package build service is unavailable.");
        BuildSettings settings;
        std::string reason;
        if (!CandidateSettings(*service, args, settings, reason)) return OpResult::Err("BAD_ARG", reason);
        service->RefreshChecks(context.ctx, settings);
        OpData data = service->CheckStatus();
        std::string code;
        data.Set("canStart", service->CanStart(context.ctx, code, reason) && data.Find("valid")->AsBool());
        data.Set("blockedCode", code);
        data.Set("blockedReason", reason);
        return OpResult::Data(std::move(data));
    };
    registry.Register(std::move(check));

    EditorOperator start;
    start.id = "build.package";
    start.label = "Build Game Package";
    start.category = "Build";
    start.desc = "Start an asynchronous game export; poll build.package_status with the returned packageRequestId.";
    start.caution = "Successful commit replaces the selected output folder; package build is not undoable.";
    start.params = SettingsParams();
    start.params.push_back({ "runAfterBuild", OpParamType::Bool, "Launch the completed executable once.", false, false });
    start.kind = OpKind::Action;
    start.poll = [](const OpContext& context, const OpArgs&) {
        std::string code;
        std::string reason;
        return context.ctx.packageBuild && context.ctx.packageBuild->CanStart(context.ctx, code, reason);
    };
    start.exec = [](OpContext& context, const OpArgs& args) {
        PackageBuildService* service = context.ctx.packageBuild;
        if (!service) return OpResult::Err("NO_PACKAGE_SERVICE", "Package build service is unavailable.");
        std::string code;
        std::string reason;
        BuildSettings settings;
        if (!CandidateSettings(*service, args, settings, reason)) return OpResult::Err("BAD_ARG", reason);
        if (!service->Start(context.ctx, settings, args.GetBool("runAfterBuild"), code, reason)) {
            OpResult result = OpResult::Err(code, reason);
            result.data = service->CheckStatus();
            return result;
        }
        OpData data = service->Status();
        data.Set("async", true);
        return OpResult::Data(std::move(data));
    };
    registry.Register(std::move(start));

    EditorOperator status;
    status.id = "build.package_status";
    status.label = "Package Build Status";
    status.category = "Build";
    status.desc = "Read the current package request, diagnostics and output executable/managed manifest presence.";
    status.kind = OpKind::Query;
    status.params = { { "packageRequestId", OpParamType::String, "Optional exact current request ID; a replaced or reset request is not the newest request.", false, {} } };
    status.exec = [](OpContext& context, const OpArgs& args) {
        PackageBuildService* service = context.ctx.packageBuild;
        if (!service) return OpResult::Err("NO_PACKAGE_SERVICE", "Package build service is unavailable.");
        if (args.Has("packageRequestId") && args.GetString("packageRequestId") != service->RequestId())
            return OpResult::Err("PACKAGE_REQUEST_NOT_FOUND", "The package request is no longer the current request.");
        return OpResult::Data(service->Status());
    };
    registry.Register(std::move(status));

    EditorOperator cancel;
    cancel.id = "build.package_cancel";
    cancel.label = "Cancel Package Build";
    cancel.category = "Build";
    cancel.desc = "Cancel the current running export and retain its terminal diagnostics.";
    cancel.kind = OpKind::Action;
    cancel.params = { { "packageRequestId", OpParamType::String, "Optional exact current request ID to avoid cancelling a newer build.", false, {} } };
    cancel.poll = [](const OpContext& context, const OpArgs&) { return context.ctx.packageBuild && context.ctx.packageBuild->IsRunning(); };
    cancel.exec = [](OpContext& context, const OpArgs& args) {
        PackageBuildService* service = context.ctx.packageBuild;
        if (args.Has("packageRequestId") && args.GetString("packageRequestId") != service->RequestId())
            return OpResult::Err("PACKAGE_REQUEST_NOT_FOUND", "The package request is no longer the current request.");
        service->Cancel();
        return OpResult::Data(service->Status());
    };
    registry.Register(std::move(cancel));
}

} /// @note namespace fbzz::editor
