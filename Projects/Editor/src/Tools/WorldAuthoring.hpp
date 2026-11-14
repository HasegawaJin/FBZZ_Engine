/// @file    WorldAuthoring.hpp
/// @brief   Shared checked authoring services for world UI and AI operation projections.
/// @author  Hasegawa Jin
/// @date    2026-11-07
#pragma once
#include <Editor/Op/EditorOperator.hpp>
#include <Engine/Scene/WorldCell.hpp>
#include <span>

namespace fbzz::scene { class GameObject; struct TerrainComponent; struct TerrainHeightRegion; }

namespace fbzz::editor {
/// @note Appends conservative world XZ bounds for changes that affect terrain sampling by PCG.
void CollectTerrainPcgBounds(const scene::GameObject&, const scene::TerrainComponent& before,
    const scene::TerrainComponent& after, std::vector<scene::WorldCellBounds>&);
void CollectTerrainPcgBounds(const scene::GameObject&, const scene::TerrainComponent&,
    const scene::TerrainHeightRegion&, std::vector<scene::WorldCellBounds>&);
/// @note The terrain edit is already applied. Generated output drafts share its checked history.
[[nodiscard]] std::unique_ptr<ICommand> RefreshTerrainPcg(OpContext&, std::span<const scene::WorldCellBounds>,
    std::unique_ptr<ICommand> terrainCommand);
[[nodiscard]] OpResult InspectWorldCells(OpContext&, const OpArgs&);
[[nodiscard]] OpResult ConfigureWorldCells(OpContext&, const OpArgs&);
[[nodiscard]] OpResult InspectWorldPcgProfile(OpContext&, const OpArgs&);
[[nodiscard]] OpResult SaveWorldPcgProfile(OpContext&, const OpArgs&);
[[nodiscard]] OpResult ConfigureWorldPcgVolume(OpContext&, const OpArgs&);
[[nodiscard]] OpResult InspectWorldPcgVolume(OpContext&, const OpArgs&);
/// @note Resolves project-relative paths and GUID references, including the destination of a new profile draft.
[[nodiscard]] bool SameWorldPcgProfile(EditorContext&, const std::string& first, const std::string& second);
[[nodiscard]] OpResult PreviewWorldPcg(OpContext&, const OpArgs&);
[[nodiscard]] OpResult GenerateWorldPcg(OpContext&, const OpArgs&);
[[nodiscard]] OpResult DetachWorldPcg(OpContext&, const OpArgs&);
[[nodiscard]] OpResult BakeWorldCells(OpContext&, const OpArgs&);
[[nodiscard]] OpResult BuildWorldHlod(OpContext&, const OpArgs&);
[[nodiscard]] OpResult InspectWorldStreaming(OpContext&, const OpArgs&);
[[nodiscard]] OpResult ControlWorldStreaming(OpContext&, const OpArgs&);
} /// @note namespace fbzz::editor
