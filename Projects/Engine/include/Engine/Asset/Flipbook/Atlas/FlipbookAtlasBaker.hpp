/// @file    FlipbookAtlasBaker.hpp
/// @brief   順序付き画像列を等間隔グリッドのFlipbook Atlasへ結合するAPI。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#pragma once
#include <Engine/Api.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::asset {

/// @brief Atlasの列数と出力先を指定する。
/// @note 行数はフレーム数から一意に決まるため保存値を二重化せず、columns=0を自動配置とする。
struct FlipbookAtlasBakeSettings {
    std::vector<std::string> framePaths; ///< フリップブックへ配置する入力画像のパス列。並び順が再生順。
    std::string outputPath; ///< 焼いたアトラス画像のファイルパス。
    int columns = 0; ///< アトラスに横方向へ並べるコマ数。
    bool overwrite = false; ///< 同名の出力画像を上書きできるか。
    bool generateTextureMeta = true; ///< 生成したアトラスにテクスチャのインポート設定を添えるか。
};

/// @note 生成物の参照とParticleEmitterへ設定するColumns/Rowsを呼び出し側へ返す。
struct FlipbookAtlasBakeResult {
    bool success = false; ///< アトラスの生成・出力に成功したか。
    std::string message; ///< 画像入力の不一致や書き込み失敗などの診断。
    std::string outputPath; ///< 焼いたアトラス画像のファイルパス。
    int frameCount = 0; ///< アトラスに格納した有効なコマ数。
    int columns = 0; ///< アトラスに横方向へ並べるコマ数。
    int rows = 0; ///< アトラスに縦方向へ並べたコマ数。
    std::uint32_t frameWidth = 0; ///< 元画像 1 コマの幅 [px]。
    std::uint32_t frameHeight = 0; ///< 元画像 1 コマの高さ [px]。
    std::uint32_t atlasWidth = 0; ///< 出力アトラス全体の幅 [px]。
    std::uint32_t atlasHeight = 0; ///< 出力アトラス全体の高さ [px]。
};

/// @note framePathsの順序を再生順として、左上から右方向・次に下方向へ配置する。
/// @note 異なるフレーム寸法を暗黙に拡縮すると時間方向に画質が揺れるため、寸法不一致はエラーにする。
[[nodiscard]] FBZZ_ENGINE_API FlipbookAtlasBakeResult BakeFlipbookAtlas(
    const FlipbookAtlasBakeSettings& settings);

} // namespace fbzz::asset
