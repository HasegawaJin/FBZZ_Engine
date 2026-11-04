/// @file    WorkspaceOperators.cpp
/// @brief   エディター設定とドッキング配置を直接APIへ公開する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include "SettingsDocument.hpp"
#include <Editor/EditorApp.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Panels/IPanel.hpp>
#include <Editor/Util/EditorSettings.hpp>
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_set>

namespace fbzz::editor {
namespace {
bool ValidSettings(const EditorSettings& s)
{
    const auto range = [](float value, float lo, float hi) {
        return std::isfinite(value) && value >= lo && value <= hi;
    };
    for (const auto& panel : s.assetBrowserPanels)
        if (!range(panel.iconSize, 56.0f, 132.0f) || !range(panel.treeWidth, 140.0f, 420.0f)
            || panel.viewMode < 0 || panel.viewMode > 1 || panel.sortMode < 0 || panel.sortMode > 3) return false;
    const auto& import = s.defaultImportOptions;
    return static_cast<int>(import.sourceDcc) >= 0 && static_cast<int>(import.sourceDcc) <= 2
        && static_cast<int>(import.upAxis) >= 0 && static_cast<int>(import.upAxis) <= 2
        && static_cast<int>(import.normalMapConvention) >= 0 && static_cast<int>(import.normalMapConvention) <= 1
        && static_cast<int>(import.defaultCompression) >= 0 && static_cast<int>(import.defaultCompression) <= 7
        && range(s.editorUiScale, 0.7f, 2.0f) && (s.language == "en" || s.language == "ja")
        && range(s.cameraSpeed, 0.001f, 10000.0f) && range(s.cameraSensitivity, 0.001f, 100.0f)
        && range(s.cameraOrthoHeight, 0.01f, 100000.0f) && range(s.gridSize, 0.001f, 10000.0f)
        && range(s.snapPos, 0.001f, 10000.0f) && range(s.snapRot, 0.001f, 360.0f)
        && range(s.snapScale, 0.001f, 10000.0f) && s.gizmoMode >= 0 && s.gizmoMode <= 2
        && s.gizmoSpace >= 0 && s.gizmoSpace <= 1 && s.gizmoPivot >= 0 && s.gizmoPivot <= 1
        && s.gameViewportAspect >= 0 && s.gameViewportAspect <= static_cast<int>(EditorContext::GameViewportAspect::iPhoneLandscape)
        && s.playExecutionMode >= 0 && s.playExecutionMode <= static_cast<int>(PlayExecutionMode::STANDALONE)
        && s.playFocusMode >= 0 && s.playFocusMode <= static_cast<int>(EditorContext::PlayFocusMode::Unfocused)
        && s.mapActiveTool >= 0 && s.mapActiveTool <= static_cast<int>(EditorContext::MapTool::TerrainHole)
        && s.navMeshDrawMode >= 0 && s.navMeshDrawMode <= 4 && s.viewMode >= 0 && s.viewMode <= 3
        && range(s.navMeshDrawDistance, 0.0f, 100000.0f)
        && s.autoSaveIntervalSec >= 10 && s.autoSaveIntervalSec <= 86400
        && range(s.terrainBrushRadius, 0.01f, 10000.0f) && range(s.terrainBrushStrength, 0.0f, 10000.0f)
        && s.terrainBrushFalloff >= 0 && s.terrainBrushFalloff <= 2
        && s.terrainSculptMode >= 0 && s.terrainSculptMode <= 8
        && s.animPreviewLabelMode >= 0 && s.animPreviewLabelMode <= 2
        && s.animPreviewBackground >= 0 && s.animPreviewBackground <= 3
        && range(s.animPreviewGhostOffset, 0.0f, 0.5f)
        && range(s.animPreviewCameraPitch, -1.35f, 1.35f)
        && range(s.animPreviewSpeed, 0.05f, 4.0f) && range(s.animPreviewFov, 12.0f, 90.0f)
        && range(s.consoleDetailRatio, 0.1f, 0.8f)
        && range(s.defaultImportOptions.unitScaleMultiplier, 0.000001f, 10000.0f);
}
} /// @note namespace

void EditorApp::RegisterWorkspaceOperators()
{
    for (const char* action : {"inspect", "configure", "reload", "save"}) {
        EditorOperator op;
        const std::string name = action;
        op.id = "workspace.settings." + name; op.label = op.id; op.category = "Workspace";
        op.kind = name == "inspect" ? OpKind::Query : OpKind::Action;
        op.desc = "エディター設定・ホットキー・パネル設定を直接照会/編集する。settingsはinspectの部分オブジェクト。配列は全置換。saveで共有値と個人状態を通常の保存先へ保存する。";
        if (name == "configure") {
            OpParam patch; patch.name = "settings"; patch.type = OpParamType::Data;
            op.params.push_back(std::move(patch));
        }
        op.poll = [](const OpContext& context, const OpArgs&) { return !context.ctx.projectRoot.empty(); };
        op.exec = [this, name](OpContext&, const OpArgs& args) {
            CaptureEditorSettingsFromContext();
            CaptureNormalPanelVisibility();
            if (name == "configure" || name == "reload") {
                EditorSettings after = m_settings;
                const std::string path = m_ctx.projectRoot + "/Assets/EditorConfig/editor_settings.toml";
                if (name == "reload") {
                    if (!after.Load(path, m_ctx.projectRoot)) return OpResult::Err("SETTINGS_READ_FAILED", "エディター設定を読み取れません。");
                } else {
                    std::string text;
                    const OpData* patch = args.GetData("settings");
                    if (!patch || !settingsdocument::Patch(m_settings.ToToml(m_ctx.projectRoot), *patch, text)
                        || !after.LoadFromToml(text, m_ctx.projectRoot)
                        || !settingsdocument::Preserves(after.ToToml(m_ctx.projectRoot), *patch))
                        return OpResult::Err("BAD_ARG", "未知の設定キー、型不一致または不正な値です。");
                }
                if (!ValidSettings(after)) return OpResult::Err("BAD_ARG", "設定値が許容範囲外です。");
                std::unordered_set<std::string> names;
                for (const auto& binding : after.hotkeyOverrides) {
                    const auto& hotkeys = m_hotkeys.GetHotkeys();
                    const auto found = std::find_if(hotkeys.begin(), hotkeys.end(), [&binding](const auto& hotkey) {
                        return !hotkey.infoOnly && (hotkey.operatorId.empty() ? hotkey.name : hotkey.operatorId) == binding.name;
                    });
                    if (found == hotkeys.end() || !names.insert(binding.name).second
                        || (binding.key != ImGuiKey_None && (binding.key < ImGuiKey_NamedKey_BEGIN || binding.key >= ImGuiKey_NamedKey_END)))
                        return OpResult::Err("BAD_ARG", "ホットキー名、重複またはキーコードが不正です。");
                }
                for (const auto& [panel, open] : after.panelVisibility) {
                    static_cast<void>(open);
                    if (!FindPanelByName(panel)) return OpResult::Err("UNKNOWN_PANEL", panel);
                }
                m_settings = std::move(after);
                ApplyEditorSettingsToContext(true);
                ImGuiIO& io = ImGui::GetIO();
                if (m_settings.multiViewportEnabled) io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
                else io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;
            }
            if (name == "save" && !m_settings.Save(m_ctx.projectRoot + "/Assets/EditorConfig/editor_settings.toml", m_ctx.projectRoot))
                return OpResult::Err("SETTINGS_SAVE_FAILED", "エディター設定を保存できません。");
            OpData data = OpData::MakeObject();
            data.Set("settings", settingsdocument::Inspect(m_settings.ToToml(m_ctx.projectRoot)));
            data.Set("sharedPath", OpData(m_ctx.projectRoot + "/Assets/EditorConfig/editor_settings.toml"));
            data.Set("localPath", OpData(m_ctx.projectRoot + "/Library/EditorLocalState.toml"));
            return OpResult::Data(std::move(data));
        };
        m_operators.Register(std::move(op));
    }
    for (const char* action : {"inspect", "configure", "save"}) {
        EditorOperator op;
        const std::string name = action;
        op.id = "workspace.layout." + name; op.label = op.id; op.category = "Workspace";
        op.kind = name == "inspect" ? OpKind::Query : OpKind::Action;
        op.desc = "ドッキング配置のINI本文を直接取得/適用/保存する。configureはinspectのiniを受け取る。";
        if (name == "configure") {
            OpParam ini; ini.name = "ini"; ini.type = OpParamType::String; op.params.push_back(std::move(ini));
        }
        op.poll = [this](const OpContext&, const OpArgs&) { return !m_imguiIniPath.empty() && !m_ctx.mapEditingMode && !m_playViewportLayoutActive; };
        op.exec = [this, name](OpContext&, const OpArgs& args) {
            if (name == "configure") {
                const std::string ini = args.GetString("ini");
                if (ini.size() > 1024 * 1024 || ini.find('\0') != std::string::npos)
                    return OpResult::Err("BAD_ARG", "配置データが不正です。");
                ImGui::LoadIniSettingsFromMemory(ini.c_str(), ini.size());
            }
            if (name == "save") ImGui::SaveIniSettingsToDisk(m_imguiIniPath.c_str());
            std::size_t size = 0;
            const char* ini = ImGui::SaveIniSettingsToMemory(&size);
            OpData data = OpData::MakeObject(); data.Set("ini", OpData(std::string(ini, size)));
            data.Set("path", OpData(m_imguiIniPath));
            return OpResult::Data(std::move(data));
        };
        m_operators.Register(std::move(op));
    }
}
} /// @note namespace fbzz::editor
