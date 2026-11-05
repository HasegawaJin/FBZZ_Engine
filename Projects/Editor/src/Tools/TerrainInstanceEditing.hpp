/// @file    TerrainInstanceEditing.hpp
/// @brief   配置の個体編集・再接地・密度調整を同じ保存履歴へ確定する。
/// @author  Hasegawa Jin
/// @date    2026-11-05
#pragma once
#include "TerrainScatter.hpp"
#include <Editor/Op/EditorOperator.hpp>
namespace fbzz::editor {
struct TerrainInstanceEdit {
    std::string owner, definition, provenance, newName;
    std::vector<renderer::InstancePlacement> placements;
};
[[nodiscard]] OpResult CommitTerrainInstanceEdits(EditorContext& context, std::vector<TerrainInstanceEdit> edits, size_t affected);
[[nodiscard]] bool DecomposeTerrainPlacement(const math::Matrix4& matrix, renderer::InstancePlacement& placement);
void RegisterTerrainInstanceEditingOperators(OperatorRegistry& registry);
} /// @note namespace fbzz::editor
