/// @file    TerrainWaterDefaults.hpp
/// @brief   Terrain / Water の Editor 既定アセットパスを集約する。
/// @author  Hasegawa Jin
/// @date    2026-06-07
#pragma once

#include <span>

namespace fbzz::editor {

// Terrain レイヤー [0-3] の既定 Material パスを返す。
// WHY: InspectorPanel にパス文字列を散らすと、アセット構成変更時に見落としやすいため。
[[nodiscard]] const char* DefaultTerrainLayerMaterialPath(int layerIndex);

// WaterComponent を追加・初期化するときに使う標準 Material パスを返す。
// WHY: Water の .mat の場所を UI 実装から分離し、テンプレート/生成処理と共有しやすくするため。
[[nodiscard]] const char* DefaultWaterMaterialPath();

/// エンジンが同梱する水の種類 (.mat 1 枚 = 1 種類)。
struct WaterMaterialPreset {
    const char* label;
    const char* path;
    const char* tooltip;
};

/// 同梱の水の種類。先頭は DefaultWaterMaterialPath() と同じ。
[[nodiscard]] std::span<const WaterMaterialPreset> WaterMaterialPresets();

} // namespace fbzz::editor
