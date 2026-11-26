/// @file    RenderTrailSampling.hpp
/// @brief   軌跡のサンプル履歴を更新する。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#pragma once

#include <Engine/Scene/Components/VFX/TrailComponent.hpp>
#include <cstdint>

namespace fbzz::scene { class Scene; class GameObject; }

namespace fbzz::scene::trail_extraction_detail {

void ResetTrailSampleStorage(TrailComponent& trail);
void AdvanceTrailSamples(Scene& scene, GameObject& owner, TrailComponent& trail, float currentTime, uint64_t frameStamp);
const TrailPoint& ReadTrailPoint(const TrailComponent& trail, int logicalIndex);
void ClearTrailSamples(TrailComponent& trail);

} // namespace fbzz::scene::trail_extraction_detail
