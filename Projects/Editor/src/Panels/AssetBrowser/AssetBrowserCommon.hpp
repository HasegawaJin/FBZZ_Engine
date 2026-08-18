// FBZZ Engine
// AssetBrowserCommon.hpp | fbzz::editor
// AssetBrowserPanel の分割ファイルで共有するファイル操作・描画ヘルパー
#pragma once

#include <Editor/Panels/AssetBrowserPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/EditorTaskOverlay.hpp>
#include <Editor/Import/FbxImportTool.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/ModalDialog.hpp>
#include <Editor/Util/PrefabSerializer.hpp>
#include <Editor/Util/SceneIO.hpp>
#include <Editor/Util/ScriptCodeGen.hpp>
#include <Editor/Util/SelectionVisuals.hpp>
#include <Editor/Util/Toast.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>

namespace fbzz::editor {

// ファイルアイコンのポリゴン + ドッグイアを描画する共通ヘルパー。
// WHY: DrawFileIconAt (AssetBrowserItems) と drawSubIcon (AssetBrowserCreate) で
//      同一コードが重複していたため、ここに集約する。ラベルは呼び出し側で描画する。
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

std::string SanitizeEntityName(const std::string& name);
std::string UniquePrefabPathInDir(const std::string& dir, const std::string& objectName);
bool SaveHierarchyPayloadAsPrefab(const ImGuiPayload* payload, EditorContext& ctx, const std::string& targetDir);
ImVec4 Lighten(ImVec4 c);
std::string ToProjectAssetPath(const std::string& path, const EditorContext& ctx);
// ドラッグ中のパスを、プロジェクト内は Assets 起点、外部マウントは絶対パスで保持する。
// WHY: 単純な文字列変換では外部プロジェクトの ".../Assets/..." まで自プロジェクトの
//      Assets と誤認し、移動元を壊れたパスへ解決してしまう。
std::string ToAssetDragPayloadPath(const std::string& path, const EditorContext& ctx);
// ImGui payload の終端を検証して、安全に ASSET_PATH 文字列を取り出す。
bool ReadAssetDragPayload(const ImGuiPayload* payload, std::string& outPath);

} // namespace fbzz::editor
