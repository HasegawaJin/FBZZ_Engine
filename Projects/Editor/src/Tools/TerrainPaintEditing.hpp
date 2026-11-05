/// @file    TerrainPaintEditing.hpp
/// @brief   Paint 素材の作成・編集を Operator と共有する引数変換。
/// @author  Hasegawa Jin
/// @date    2026-11-05
#pragma once
#include <Engine/Asset/TerrainPaintAsset.hpp>
#include <Editor/Op/EditorOperator.hpp>

namespace fbzz::editor {
[[nodiscard]] OpArgs TerrainPaintAssetArguments(const std::string& path, const asset::TerrainPaintAsset& asset);
[[nodiscard]] std::string TerrainPaintAssetFingerprint(const asset::TerrainPaintAsset& asset);
void RegisterTerrainPaintAssetOperators(OperatorRegistry& registry);
} /// @note namespace fbzz::editor
