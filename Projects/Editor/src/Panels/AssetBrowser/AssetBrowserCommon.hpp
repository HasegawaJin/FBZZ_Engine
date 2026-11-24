/// @file    AssetBrowserCommon.hpp
/// @brief   AssetBrowserPanel の分割ファイルで共有するファイル操作・描画ヘルパー。
/// @author  Hasegawa Jin
/// @date    2026-06-07
#pragma once

#include <Editor/Panels/AssetBrowser/AssetBrowserPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/UI/EditorTaskOverlay.hpp>
#include <Editor/Import/Fbx/FbxImportTool.hpp>
#include <Editor/Asset/Storage/AssetDirtyRegistry.hpp>
#include <Editor/Asset/Storage/AssetPath.hpp>
#include <Editor/UI/Widgets/ModalDialog.hpp>
#include <Editor/Prefab/PrefabSerializer.hpp>
#include <Editor/Scene/Persistence/SceneIO.hpp>
#include <Editor/Script/Authoring/ScriptCodeGen.hpp>
#include <Editor/Scene/Editing/Selection.hpp>
#include <Editor/Viewport/SelectionVisuals.hpp>
#include <Editor/UI/Widgets/Toast.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Core/Diagnostics/Logger.hpp>
#include <Graphics/Renderer/Resources/ResourceManager.hpp>
#include <Engine/Util/Files/FileSystem.hpp>
#include <Engine/Util/Text/StringUtils.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>

namespace fbzz::editor {

/// @note ファイルアイコンのポリゴン + ドッグイアを描画する共通ヘルパー。
/// @note DrawFileIconAt (AssetBrowserItems) と drawSubIcon (AssetBrowserCreate) の重複コードをここに集約した。ラベルは呼び出し側で描画する。
inline void DrawFileIconPolygon(ImDrawList* dl, ImVec2 origin, float sz, ImU32 cFill, ImU32 cDark)
{
    const float bodyH = sz * 0.85f;
    const float dog   = sz * 0.22f;
    ImVec2 pts[5] = {
        { origin.x,        origin.y       },
        { origin.x+sz-dog, origin.y       },
        { origin.x+sz,     origin.y+dog   },
        { origin.x+sz,     origin.y+bodyH },
        { origin.x,        origin.y+bodyH },
    };
    dl->AddConvexPolyFilled(pts, 5, cFill);
    dl->AddPolyline(pts, 5, cDark, ImDrawFlags_Closed, 1.0f);
    ImVec2 tri[3] = {
        { origin.x+sz-dog, origin.y     },
        { origin.x+sz,     origin.y+dog },
        { origin.x+sz-dog, origin.y+dog },
    };
    dl->AddConvexPolyFilled(tri, 3, cDark);
}

std::string UniquePrefabPathInDir(const std::string& dir, const std::string& objectName);
bool SaveHierarchyPayloadAsPrefab(const ImGuiPayload* payload, EditorContext& ctx, const std::string& targetDir);
ImVec4 Lighten(ImVec4 c);
std::string ToProjectAssetPath(const std::string& path, const EditorContext& ctx);
/// @note ドラッグ中のパスを、プロジェクト内は Assets 起点、外部マウントは絶対パスで保持する。
/// @note 単純な文字列変換では外部プロジェクトの ".../Assets/..." まで自プロジェクトの Assets と誤認し、移動元を壊れたパスへ解決してしまう。
std::string ToAssetDragPayloadPath(const std::string& path, const EditorContext& ctx);
/// @note ImGui payload の終端を検証して、安全に ASSET_PATH 文字列を取り出す。
bool ReadAssetDragPayload(const ImGuiPayload* payload, std::string& outPath);

} /// @note namespace fbzz::editor
