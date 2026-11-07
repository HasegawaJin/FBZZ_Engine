/// @file    WorldAuthoring.hpp
/// @brief   Shared checked authoring services for world UI and AI operation projections.
/// @author  Hasegawa Jin
/// @date    2026-11-07
#pragma once
#include <Editor/Op/EditorOperator.hpp>

namespace fbzz::editor {
[[nodiscard]] OpResult InspectWorldCells(OpContext&, const OpArgs&);
[[nodiscard]] OpResult ConfigureWorldCells(OpContext&, const OpArgs&);
[[nodiscard]] OpResult InspectWorldPcgProfile(OpContext&, const OpArgs&);
[[nodiscard]] OpResult SaveWorldPcgProfile(OpContext&, const OpArgs&);
[[nodiscard]] OpResult ConfigureWorldPcgVolume(OpContext&, const OpArgs&);
[[nodiscard]] OpResult PreviewWorldPcg(OpContext&, const OpArgs&);
[[nodiscard]] OpResult GenerateWorldPcg(OpContext&, const OpArgs&);
[[nodiscard]] OpResult DetachWorldPcg(OpContext&, const OpArgs&);
[[nodiscard]] OpResult BakeWorldCells(OpContext&, const OpArgs&);
[[nodiscard]] OpResult BuildWorldHlod(OpContext&, const OpArgs&);
[[nodiscard]] OpResult InspectWorldStreaming(OpContext&, const OpArgs&);
[[nodiscard]] OpResult ControlWorldStreaming(OpContext&, const OpArgs&);
} /// @note namespace fbzz::editor
