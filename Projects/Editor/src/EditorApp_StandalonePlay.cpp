/// @file    EditorApp_StandalonePlay.cpp
/// @brief   Standalone Play uses the existing launcher with an explicit saved-scene handoff.
/// @author  Hasegawa Jin
/// @date    2026-10-23
#include <Editor/EditorApp.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/ModalDialog.hpp>
#include <Editor/Util/Toast.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <shellapi.h>
#include <array>

namespace fbzz::editor {

void EditorApp::StartStandalonePlayMode(bool allowPhysicalInput)
{
    CheckSceneDirtyForStandalone();
    if (m_ctx.sceneDirty || m_ctx.currentScenePath.empty() || AssetDirtyRegistry::HasAny()) {
        /// @note Scene-relative Terrain/sidecar paths prevent reuse of the old shared temporary snapshot file.
        const std::string expectedProject = m_projectRoot;
        const auto* expectedScene = m_scene.get();
        const uint64_t expectedGeneration = m_scene->GetRenderSceneGeneration();
        const std::string expectedPath = m_ctx.currentScenePath;
        ModalDialog::OpenConfirm("Save and Start Standalone Play",
            "Standalone Play reads the current scene and assets from disk.\n"
            "Save unsaved changes and start in a separate process?\n"
            "Cancel keeps all edits and does not start a game.",
            [this, expectedProject, expectedScene, expectedGeneration, expectedPath, allowPhysicalInput] {
                if (m_projectRoot != expectedProject || m_scene.get() != expectedScene
                    || m_scene->GetRenderSceneGeneration() != expectedGeneration || m_ctx.currentScenePath != expectedPath
                    || m_standalonePlay.IsActive() || !m_playMode.IsInEditor()) return;
                if (AssetDirtyRegistry::SaveAll() != 0 || AssetDirtyRegistry::HasAny()) {
                    Toast::Error("Standalone Play: an asset could not be saved. See Console."); return;
                }
                if (m_ctx.sceneDirty || m_ctx.currentScenePath.empty()) {
                    SaveScene(true, [this, allowPhysicalInput] { LaunchStandalonePlay(allowPhysicalInput); });
                    return;
                }
                LaunchStandalonePlay(allowPhysicalInput);
            });
        return;
    }
    LaunchStandalonePlay(allowPhysicalInput);
}

void EditorApp::LaunchStandalonePlay(bool allowPhysicalInput)
{
    if (m_standalonePlay.IsActive() || !m_playMode.IsInEditor() || m_ctx.InPrefabEditMode()
        || m_ctx.managedBuildBusy || m_packageBuild.IsRunning()) return;
    if (m_ctx.sceneDirty || m_ctx.currentScenePath.empty() || AssetDirtyRegistry::HasAny()) {
        Toast::Error("Standalone Play was not started: save the current scene and assets first."); return;
    }
    if (IsSceneStaleOnDisk()) {
        Toast::Error("Standalone Play: the scene changed on disk. Reload or save it before starting."); return;
    }
    if (!SaveProjectSettingsNow()) {
        Toast::Error("Standalone Play: ProjectSettings could not be saved. See Console."); return;
    }
    std::array<wchar_t, 32768> executableBuffer{};
    const DWORD count = GetModuleFileNameW(nullptr, executableBuffer.data(), static_cast<DWORD>(executableBuffer.size()));
    if (!count || count == executableBuffer.size()) {
        Toast::Error("Standalone Play could not resolve the current launcher path."); return;
    }
    std::filesystem::path executable(executableBuffer.data());
    if (executable.filename() != L"FBZZEditor.exe") {
        /// @note Local Sandbox hosts use the matching Editor launcher built beside the Sandbox directory.
        executable = executable.parent_path().parent_path() / L"Editor" / L"FBZZEditor.exe";
    }
    static uint64_t nextSession = 0;
    const auto project = util::FileSystem::MakeAbsolute(util::FileSystem::PathFromUtf8(m_projectRoot));
    const auto logName = std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())
        + L"-" + std::to_wstring(++nextSession) + L".log";
    StandalonePlayRequest request;
    request.allowPhysicalInput = allowPhysicalInput;
    request.executable = std::move(executable);
    request.project = project;
    request.scene = util::FileSystem::MakeAbsolute(util::FileSystem::PathFromUtf8(m_ctx.currentScenePath));
    request.log = project / L"Library" / L"StandalonePlay" / logName;
    if (!m_standalonePlay.Start(request)) {
        FBZZ_LOG_ERROR("%s", m_standalonePlay.Diagnostic().c_str());
        Toast::Error(m_standalonePlay.Diagnostic());
        return;
    }
    FBZZ_LOG_INFO("%s", m_standalonePlay.Diagnostic().c_str());
}

void EditorApp::TickStandalonePlay(float dt)
{
    if (!m_standalonePlay.Tick(dt)) return;
    if (m_standalonePlay.Failed()) {
        FBZZ_LOG_ERROR("%s", m_standalonePlay.Diagnostic().c_str());
        Toast::Error(m_standalonePlay.Diagnostic());
    } else {
        FBZZ_LOG_INFO("%s", m_standalonePlay.Diagnostic().c_str());
    }
}

void EditorApp::OpenStandalonePlayLog()
{
    const auto& path = m_standalonePlay.LogPath();
    if (path.empty()) return;
    ShellExecuteW(m_hwnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

} /// @note namespace fbzz::editor
