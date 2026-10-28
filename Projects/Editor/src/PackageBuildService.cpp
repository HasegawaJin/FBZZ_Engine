/// @file    PackageBuildService.cpp
/// @brief   共有配布ビルドの安全な開始とパネル非依存の進行。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#include <Editor/PackageBuildService.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/ManagedScriptEditor.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/ToolchainLocator.hpp>
#include <Editor/Util/AppIconWriter.hpp>
#include <Editor/Util/BuildConsole.hpp>
#include <Editor/Util/StandaloneLauncher.hpp>
#include <Engine/Scene/Managed/ManagedGamePackage.hpp>
#include <Engine/Util/FileSystem.hpp>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>

namespace fbzz::editor {
namespace {
/// @note 配布先へ結合するsceneは実体がproject内にある相対パスだけに正規化する。
/// @see https://learn.microsoft.com/windows/win32/api/stringapiset/nf-stringapiset-comparestringordinal Windows ordinal case-insensitive path comparison。
bool NormalizeScene(const std::string& projectRoot, const std::string& source, std::string& result)
{
    if (projectRoot.empty() || source.empty()) return false;
    const auto raw = util::FileSystem::PathFromUtf8(source);
    for (const auto& part : raw) if (part == L"..") return false;
    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(util::FileSystem::PathFromUtf8(projectRoot), error);
    if (error) return false;
    const auto scene = std::filesystem::weakly_canonical(raw.is_absolute() ? raw : root / raw, error);
    if (error || !std::filesystem::is_regular_file(scene, error) || error) return false;
    auto scenePart = scene.begin();
    for (const auto& rootPart : root) {
        if (scenePart == scene.end()) return false;
        const std::wstring left = rootPart.wstring();
        const std::wstring right = scenePart->wstring();
        if (CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()), right.c_str(), static_cast<int>(right.size()), TRUE) != CSTR_EQUAL) return false;
        ++scenePart;
    }
    std::filesystem::path relative;
    for (; scenePart != scene.end(); ++scenePart) relative /= *scenePart;
    if (relative.empty() || relative.is_absolute()) return false;
    result = util::FileSystem::PathToUtf8(relative);
    return true;
}

bool SafeProductName(const std::string& name)
{
    if (name.empty() || name == "." || name == ".." || name.back() == '.' || name.back() == ' ') return false;
    for (const unsigned char ch : name) {
        if (ch < 32 || ch == 127 || std::string_view("\\/:*?\"<>|").find(static_cast<char>(ch)) != std::string_view::npos) return false;
    }
    std::string base = name.substr(0, name.find('.'));
    for (char& ch : base) if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
    if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL") return false;
    return !(base.size() == 4 && (base.starts_with("COM") || base.starts_with("LPT")) && base[3] >= '1' && base[3] <= '9');
}
} /// @note namespace

void PackageBuildService::RefreshChecks(const EditorContext& ctx, const BuildSettings& settings)
{
    m_checks.clear();
    m_standaloneExeDir.clear();

    if (ctx.projectRoot.empty()) {
        m_checks.push_back({ Check::Level::FAILED, "Project", "No project is open." });
        return;
    }

    if (!SafeProductName(settings.productName))
        m_checks.push_back({ Check::Level::FAILED, "Product name", "Specify a safe Windows file name without path separators." });
    if (settings.version.find_first_of("\"\\\r\n") != std::string::npos)
        m_checks.push_back({ Check::Level::FAILED, "Version", "Version must not contain quotes, backslashes or line breaks." });

    const std::filesystem::path root = util::FileSystem::PathFromUtf8(ctx.projectRoot);
    std::filesystem::path managedProject;
    std::string managedReason;
    if (!scene::FindManagedGameProject(root, managedProject, managedReason))
        m_checks.push_back({ Check::Level::FAILED, "C# project", managedReason });
    else if (!managedProject.empty())
        m_checks.push_back({ Check::Level::OK, "C# project", util::FileSystem::PathToUtf8(managedProject) });
    for (const SceneEntry& entry : settings.scenes) {
        std::string normalized;
        if (entry.enabled && !NormalizeScene(ctx.projectRoot, entry.path, normalized))
            m_checks.push_back({ Check::Level::FAILED, "Scene", entry.path + " must be an existing project-local scene without traversal" });
    }

    /// @note 出力先
    /// @note Error にする: コミットは出力先を remove_all するため、空欄やデスクトップを指したまま
    /// @note ビルドを押すとそのフォルダが中身ごと消える。
    {
        std::string reason;
        if (settings.ValidateOutputPath(ctx.projectRoot, reason)) {
            m_checks.push_back({ Check::Level::OK, "Output",
                util::FileSystem::PathToUtf8(settings.ResolveOutputPath(ctx.projectRoot)) });
        } else {
            m_checks.push_back({ Check::Level::FAILED, "Output", reason });
        }
    }

    /// @note アイコン
    /// @note 事前に読む: 差し替えはコンパイル後にしか走らないため、読めない画像を指したままだと
    /// @note 数分かけたビルドがアイコンのためだけに失敗する。
    if (!settings.iconPath.empty()) {
        const std::filesystem::path icon = settings.ResolveIconPath(ctx.projectRoot);
        AppIconWriter::SourceInfo info;
        std::string reason;
        if (!AppIconWriter::Inspect(icon, info, reason)) {
            m_checks.push_back({ Check::Level::FAILED, "Icon", settings.iconPath + " — " + reason });
        } else if (info.isIco) {
            m_checks.push_back({ Check::Level::OK, "Icon",
                                 settings.iconPath + " (embedded as-is)" });
        } else {
            const std::string size = std::to_string(info.width) + "x" + std::to_string(info.height);
            if (info.width != info.height) {
                m_checks.push_back({ Check::Level::WARNING, "Icon",
                                     size + " is not square — it will be centered on a "
                                     "transparent square" });
            } else if (info.width < 256) {
                m_checks.push_back({ Check::Level::WARNING, "Icon",
                                     size + " is smaller than 256x256 — large icon views "
                                     "will look soft" });
            } else {
                m_checks.push_back({ Check::Level::OK, "Icon", settings.iconPath + " (" + size + ")" });
            }
        }
    }

    /// @note ツールチェーン (cmake / build.config)
    std::string runtimeConfiguration = settings.developmentBuild ? "Development" : "Release";
    bool dx12Enabled = false;
    const ToolchainLocator::Result toolchain =
        ToolchainLocator::Locate(util::FileSystem::PathFromUtf8(ctx.projectBuildRoot));
    if (!toolchain.found) {
        m_checks.push_back({ Check::Level::FAILED, "Toolchain", toolchain.error });
    } else {
        m_checks.push_back({ Check::Level::OK, "Toolchain",
                             util::FileSystem::PathToUtf8(toolchain.buildDir) });

        const std::filesystem::path exe = settings.developmentBuild
            ? (!toolchain.exeDevelopment.empty() ? toolchain.exeDevelopment : toolchain.exeDebug)
            : toolchain.exeRelease;
        m_standaloneExeDir = util::FileSystem::PathToUtf8(exe.parent_path());
        std::string runtimeReason;
        runtimeConfiguration = settings.developmentBuild
            ? (!toolchain.exeDevelopment.empty() ? "Development" : "Debug") : "Release";
        if (util::FileSystem::Exists(exe)) {
            if (BuildPipeline::ValidateRuntimePackage(exe, util::FileSystem::PathFromUtf8(ctx.engineRoot),
                                                      runtimeConfiguration, false, runtimeReason, &dx12Enabled)) {
                m_checks.push_back({ Check::Level::OK, "Graphics runtime",
                    dx12Enabled ? "Agility SDK, DXC and runtime notices" : "Graphics backend disabled" });
            } else {
                m_checks.push_back({ Check::Level::WARNING, "Graphics runtime", runtimeReason + " (the build re-stages the runtime)" });
            }
        }
    }

    /// @note 初回ゲームビルド前も SDK 版 Editor で選択構成の公開契約と必須配置を確認する。
    if (!ctx.engineRoot.empty()) {
        const auto sdkRoot = util::FileSystem::PathFromUtf8(ctx.engineRoot);
        const auto sdkEditor = sdkRoot / "tools" / runtimeConfiguration / "Editor/FBZZEditor.exe";
        std::string sdkReason;
        if (util::FileSystem::Exists(sdkRoot / "bin" / runtimeConfiguration)
            && util::FileSystem::Exists(sdkRoot / "lib" / runtimeConfiguration)
            && BuildPipeline::ValidateRuntimePackage(sdkEditor, sdkRoot, runtimeConfiguration, false, sdkReason, &dx12Enabled)) {
            m_checks.push_back({ Check::Level::OK, "SDK", runtimeConfiguration + " runtime contract validated" });
        } else {
            if (sdkReason.empty()) sdkReason = "SDK bin/lib configuration is missing: " + runtimeConfiguration;
            m_checks.push_back({ Check::Level::FAILED, "SDK", sdkReason + " — republish this SDK configuration" });
        }
    }

    /// @note ランタイム DLL / EngineAssets
    /// @note 未ビルドは警告止まりにする: DLL も EngineAssets も standalone ターゲットの POST_BUILD が
    /// @note 置くため、1 度もビルドしていない状態でエラーにすると「DLL が無いからビルドできない /
    /// @note ビルドしないと DLL が来ない」で詰む。
    if (!m_standaloneExeDir.empty()) {
        const std::filesystem::path exeDir = util::FileSystem::PathFromUtf8(m_standaloneExeDir);
        const bool builtOnce = util::FileSystem::Exists(exeDir);

        if (!builtOnce) {
            m_checks.push_back({ Check::Level::WARNING, "Runtime DLLs",
                                 "not staged yet — the build produces them next to the exe" });
        } else {
            /// @note DXC と PIX の必須判定は ProjectSettings でなく runtime の DX12 有効状態に従う。
            std::vector<std::wstring> required = {
                L"imgui.dll", L"FBZZMath.dll", L"FBZZPhysics.dll", L"FBZZFluid.dll", L"FBZZCore.dll", L"FBZZGraphics.dll", L"FBZZEngine.dll",
                runtimeConfiguration == "Debug" ? L"assimp-vc145-mtd.dll" : L"assimp-vc145-mt.dll",
            };
            if (dx12Enabled) {
                required.push_back(L"dxcompiler.dll");
                required.push_back(L"dxil.dll");
                required.push_back(L"WinPixEventRuntime.dll");
            }

            std::string missing;
            for (const std::wstring& dll : required) {
                if (util::FileSystem::Exists(exeDir / dll)) continue;
                if (!missing.empty()) missing += ", ";
                missing += util::FileSystem::PathToUtf8(std::filesystem::path(dll));
            }

            if (missing.empty()) {
                m_checks.push_back({ Check::Level::OK, "Runtime DLLs",
                                     dx12Enabled ? "including DXC (dx12)" : "Graphics backend disabled" });
            } else {
                m_checks.push_back({ Check::Level::WARNING, "Runtime DLLs",
                                     "missing next to the exe: " + missing +
                                     " (the build re-stages them; still missing afterwards means "
                                     "the SDK was not published for this configuration)" });
            }

            if (util::FileSystem::Exists(exeDir / L"EngineAssets")) {
                m_checks.push_back({ Check::Level::OK, "EngineAssets", "will be packaged" });
            } else {
                m_checks.push_back({ Check::Level::WARNING, "EngineAssets",
                                     "not staged next to the exe — assets that only exist in the SDK "
                                     "(default font, etc.) will be missing" });
            }
        }
    }

    /// @note Library/Baked (FBX 由来の実体)
    if (util::FileSystem::Exists(root / L"Library" / L"Baked")) {
        m_checks.push_back({ Check::Level::OK, "Library/Baked", "will be packaged" });
    } else {
        m_checks.push_back({ Check::Level::WARNING, "Library/Baked",
                             "not found — imported clips and model materials will be "
                             "re-imported from the source FBX at runtime" });
    }

    /// @note 開始シーン
    const auto& game = ctx.projectSettings.game;
    const std::string startScene = !game.runtime.startScene.empty() ? game.runtime.startScene : game.project.defaultScene;
    if (startScene.empty()) {
        m_checks.push_back({ Check::Level::FAILED, "Start scene",
                             "Project Settings names neither runtime.start_scene "
                             "nor project.default_scene" });
    } else if (std::string normalized; !NormalizeScene(ctx.projectRoot, startScene, normalized)) {
        m_checks.push_back({ Check::Level::FAILED, "Start scene", startScene + " must be an existing project-local scene without traversal" });
    } else {
        m_checks.push_back({ Check::Level::OK, "Start scene", startScene });
    }
}

bool PackageBuildService::CanStart(const EditorContext& ctx, std::string& code, std::string& reason) const
{
    code.clear();
    reason.clear();
    if (ctx.projectRoot.empty()) { code = "NO_PROJECT"; reason = "No project is open."; }
    else if (ctx.playMode && (!ctx.playMode->IsInEditor() || ctx.playMode->HasPendingRestore())) { code = "PLAYING"; reason = "Cannot package while playing, paused or restoring the edit scene."; }
    else if (IsRunning()
        || ctx.managedBuildBusy
        || (ctx.buildConsole && ctx.buildConsole->IsBuilding())
        || (ctx.managedScripts && ctx.managedScripts->IsBusy())
        || (ctx.packageOtherBuildBusy && ctx.packageOtherBuildBusy())) {
        code = "BUILD_BUSY";
        reason = "A package, game, shader or C# build is already queued or running.";
    }
    return code.empty();
}

bool PackageBuildService::Start(EditorContext& ctx, const BuildSettings& settings, bool runAfterBuild,
    std::string& code, std::string& reason)
{
    if (!CanStart(ctx, code, reason)) return false;
    RefreshChecks(ctx, settings);
    for (const Check& check : m_checks) {
        if (check.level != Check::Level::FAILED) continue;
        code = "PACKAGE_PREFLIGHT_FAILED";
        reason = check.label + ": " + check.detail;
        return false;
    }
    std::filesystem::path managedProject;
    if (!scene::FindManagedGameProject(util::FileSystem::PathFromUtf8(ctx.projectRoot), managedProject, reason)) {
        code = "PACKAGE_PREFLIGHT_FAILED";
        return false;
    }
    BuildSettings normalized = settings;
    for (SceneEntry& entry : normalized.scenes) {
        if (!entry.enabled) continue;
        std::string relative;
        if (!NormalizeScene(ctx.projectRoot, entry.path, relative)) {
            code = "PACKAGE_PREFLIGHT_FAILED";
            reason = "Scene changed during package preflight: " + entry.path;
            return false;
        }
        entry.path = std::move(relative);
    }
    m_settings = normalized;
    m_requestId = "package:" + std::to_string(++m_nextRequest);
    m_cancelled = false;
    m_launchAttempted = false;
    m_launched = false;
    m_managedRequired = !managedProject.empty();
    m_lastOutputDir = util::FileSystem::PathToUtf8(settings.ResolveOutputPath(ctx.projectRoot));
    ctx.requestEditorSettingsSave = true;
    m_pipeline.Start(normalized, ctx.projectRoot, ctx.projectBuildRoot,
        ctx.standaloneTargetName, ctx.engineRoot, runAfterBuild);
    return true;
}

void PackageBuildService::Tick()
{
    m_pipeline.Tick();
    if (m_pipeline.GetState() == BuildPipeline::State::Done && m_pipeline.WantsRunAfter() && !m_launchAttempted) {
        m_launchAttempted = true;
        m_launched = StandaloneLauncher::LaunchExe(m_pipeline.GetOutputExePath(), "");
    }
}

void PackageBuildService::Cancel()
{
    if (!IsRunning()) return;
    m_cancelled = true;
    m_pipeline.Cancel();
}

void PackageBuildService::Reset()
{
    if (IsRunning()) return;
    m_pipeline.Reset();
    m_requestId.clear();
    m_cancelled = false;
    m_launchAttempted = false;
    m_launched = false;
    m_managedRequired = false;
}

OpData PackageBuildService::Status() const
{
    const BuildPipeline::State state = m_pipeline.GetState();
    const char* name = "idle";
    if (state == BuildPipeline::State::Running) name = "running";
    else if (state == BuildPipeline::State::Done) name = "done";
    else if (state == BuildPipeline::State::Failed) name = m_cancelled ? "cancelled" : "failed";
    const std::string exe = m_requestId.empty() ? std::string{} : m_pipeline.GetOutputExePath();
    const std::filesystem::path managedManifest = m_requestId.empty() ? std::filesystem::path{}
        : util::FileSystem::PathFromUtf8(m_pipeline.GetOutputDir()) / L"Managed" / L"game.managed.toml";
    std::error_code error;
    const bool exeExists = !exe.empty() && std::filesystem::is_regular_file(util::FileSystem::PathFromUtf8(exe), error) && !error;
    error.clear();
    const bool manifestExists = !managedManifest.empty() && std::filesystem::is_regular_file(managedManifest, error) && !error;
    OpData data = OpData::MakeObject();
    data.Set("available", true);
    data.Set("packageRequestId", m_requestId);
    data.Set("state", name);
    data.Set("terminal", !IsRunning());
    data.Set("building", IsRunning());
    data.Set("progress", m_pipeline.GetProgress());
    data.Set("status", m_pipeline.GetStatus());
    data.Set("error", m_pipeline.GetError());
    data.Set("errorCode", state == BuildPipeline::State::Failed ? (m_cancelled ? "PACKAGE_CANCELLED" : "PACKAGE_BUILD_FAILED") : "");
    data.Set("configuration", m_pipeline.GetRuntimeConfiguration());
    data.Set("outputDirectory", m_requestId.empty() ? std::string{} : m_pipeline.GetOutputDir());
    data.Set("outputExe", exe);
    data.Set("outputExeExists", exeExists);
    data.Set("managedRequired", m_managedRequired);
    data.Set("managedManifest", util::FileSystem::PathToUtf8(managedManifest));
    data.Set("managedManifestExists", manifestExists);
    /// @note 失敗時に残った前回の成果物を今回の完了と誤認させない。
    data.Set("outputReady", state == BuildPipeline::State::Done && exeExists && (!m_managedRequired || manifestExists));
    data.Set("launchAttempted", m_launchAttempted);
    data.Set("launched", m_launched);
    data.Set("launchError", m_launchAttempted && !m_launched ? "Completed package executable could not be launched." : "");
    const std::string& log = m_pipeline.GetBuildLog();
    constexpr std::size_t MAX_LOG_BYTES = 65536;
    data.Set("log", log.substr(log.size() > MAX_LOG_BYTES ? log.size() - MAX_LOG_BYTES : 0));
    data.Set("logTruncated", log.size() > MAX_LOG_BYTES);
    return data;
}

OpData PackageBuildService::CheckStatus() const
{
    OpData checks = OpData::MakeArray();
    bool valid = true;
    for (const Check& check : m_checks) {
        OpData item = OpData::MakeObject();
        item.Set("level", check.level == Check::Level::FAILED ? "error" : check.level == Check::Level::WARNING ? "warning" : "ok");
        item.Set("label", check.label);
        item.Set("detail", check.detail);
        checks.Push(std::move(item));
        if (check.level == Check::Level::FAILED) valid = false;
    }
    OpData data = OpData::MakeObject();
    data.Set("valid", valid);
    data.Set("checks", std::move(checks));
    return data;
}

} /// @note namespace fbzz::editor
