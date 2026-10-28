/// @file    ManagedDataAssetEditor.hpp
/// @brief   Managed DataAsset の共通 file transaction と Inspector/menu 投影。
/// @author  Hasegawa Jin
/// @date    2026-10-06
#pragma once
#include <Editor/Op/EditorOperator.hpp>
namespace fbzz::editor {
void RegisterManagedDataAssetOperators(OperatorRegistry& registry);
/// @return C# DataAsset の file なら true。schema/世代失効の診断も Inspector 内で表示する。
bool DrawManagedDataAssetInspector(EditorContext& context, const std::string& path);
void DrawManagedDataAssetCreateMenu(EditorContext& context, const std::string& directory);
void CloseManagedDataAssetEditorDocuments();
}
