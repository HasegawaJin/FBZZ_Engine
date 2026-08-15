// FBZZ Engine
// FlipbookAtlasBaker.hpp | fbzz::asset
// 順序付き画像列を等間隔グリッドのFlipbook Atlasへ結合するAPI
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::asset {

// Atlasの列数と出力先を指定する。
// WHY: 行数はフレーム数から一意に決まるため保存値を二重化せず、columns=0を自動配置とする。
struct FlipbookAtlasBakeSettings {
    std::vector<std::string> framePaths;
    std::string outputPath;
    int columns = 0;
    bool overwrite = false;
    bool generateTextureMeta = true;
};

// 生成物の参照とParticleEmitterへ設定するColumns/Rowsを呼び出し側へ返す。
struct FlipbookAtlasBakeResult {
    bool success = false;
    std::string message;
    std::string outputPath;
    int frameCount = 0;
    int columns = 0;
    int rows = 0;
    std::uint32_t frameWidth = 0;
    std::uint32_t frameHeight = 0;
    std::uint32_t atlasWidth = 0;
    std::uint32_t atlasHeight = 0;
};

// framePathsの順序を再生順として、左上から右方向・次に下方向へ配置する。
// 異なるフレーム寸法を暗黙に拡縮すると時間方向に画質が揺れるため、寸法不一致はエラーにする。
[[nodiscard]] FlipbookAtlasBakeResult BakeFlipbookAtlas(
    const FlipbookAtlasBakeSettings& settings);

} // namespace fbzz::asset
