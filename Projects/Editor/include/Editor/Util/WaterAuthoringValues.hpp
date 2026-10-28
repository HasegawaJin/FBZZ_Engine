/// @file    WaterAuthoringValues.hpp
/// @brief   InspectorのWater値操作から所有者の実行状態を分離する。
/// @author  Hasegawa Jin
/// @date    2026-10-09
#pragma once

namespace fbzz::scene { struct WaterComponent; }
namespace fbzz::editor {
/// @note 作者設定だけを既存領域へコピーし、runtime・dirty・設定の正規化は変更しない。
void CopyWaterAuthoringValues(scene::WaterComponent& target, const scene::WaterComponent& source);
/// @return 保存するWater設定だけを持つ値。Scene、粒子、波紋、物理の実行状態を共有しない。
[[nodiscard]] scene::WaterComponent CaptureWaterAuthoringValues(const scene::WaterComponent& source);
/// @note 対象の接触演出・波紋・実効波・物理handleを保持し、形状cacheと描画dirtyだけを無効化する。
void ApplyWaterAuthoringValues(scene::WaterComponent& target, const scene::WaterComponent& values);
}
