// FBZZ Engine
// AssetBrowserCommon.hpp | fbzz::editor
// AssetBrowserPanel の分割ファイルで共有するファイル操作・描画ヘルパー
#pragma once

#include <Editor/Panels/AssetBrowserPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/EditorTaskOverlay.hpp>
#include <Editor/Import/FbxImportTool.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/ModalDialog.hpp>
#include <Editor/Util/PrefabSerializer.hpp>
#include <Editor/Util/SceneIO.hpp>
#include <Editor/Util/ScriptCodeGen.hpp>
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

std::string SanitizeEntityName(const std::string& name);
std::string UniquePrefabPathInDir(const std::string& dir, const std::string& objectName);
bool SaveHierarchyPayloadAsPrefab(const ImGuiPayload* payload, EditorContext& ctx, const std::string& targetDir);
ImVec4 Lighten(ImVec4 c);
std::string ToProjectAssetPath(const std::string& path, const EditorContext& ctx);

} // namespace fbzz::editor
