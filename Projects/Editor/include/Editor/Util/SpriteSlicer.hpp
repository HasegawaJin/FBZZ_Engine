/// @file    SpriteSlicer.hpp
/// @brief   Sprite シートの矩形生成 (グリッド / alpha 島) と既存矩形への畳み込み。
/// @author  Hasegawa Jin
/// @date    2026-09-07
///
/// Sprite Editor のパネルと AI バス (sprite.slice) が同じ結果を出すための共有実装。
/// WHY 切り出すか: 切り直しは «ID を引き継げるか» が参照の生死を決める操作で、
///      写経を 2 つ持つと片方だけが規則から外れたときに «AI で切ると参照が切れる»
///      という、画面からは原因の見えない差になる。規約は Docs/design/sprite-reference.md。
#pragma once
#include <Engine/Asset/TextureAsset.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::editor::spriteslice {

/// 既にある矩形をどう扱うか。
/// DeleteExisting だけが ID を作り直す = その Sprite への参照が全部切れる。
enum class ExistingMode { DeleteExisting, Smart, Safe };

struct GridParams {
    bool  byCellCount   = false;  ///< true: columns/rows で割る / false: cellWidth/Height で敷く
    int   columns       = 1;
    int   rows          = 1;
    int   cellWidth     = 64;
    int   cellHeight    = 64;
    int   offsetX       = 0;
    int   offsetY       = 0;
    int   paddingX      = 0;
    int   paddingY      = 0;
    float pivotX        = 0.5f;
    float pivotY        = 0.5f;
    bool  keepEmptyRects = true;  ///< false なら不透明ピクセルが 1 つも無いセルを捨てる
    std::string baseName;         ///< 生成名の接頭辞 (末尾に連番が付く)
};

struct AutoTrimParams {
    float pivotX = 0.5f;
    float pivotY = 0.5f;
    std::string baseName;
};

struct Result {
    std::vector<asset::SpriteRect> sprites;
    std::string error;  ///< 非空なら失敗。そのまま人へ出せる文言
};

/// 等間隔グリッドで矩形を作る。生成物には新しい ID が付く (畳み込みで引き継ぐ)。
[[nodiscard]] Result GenerateGrid(const std::string& imagePath,
                                  uint32_t textureWidth, uint32_t textureHeight,
                                  const GridParams& params);

/// alpha が連結した島ごとに外接矩形を作る (Unity の Automatic Slice 相当)。
[[nodiscard]] Result GenerateAutoTrim(const std::string& imagePath,
                                      const AutoTrimParams& params);

/// 生成結果を既存の一覧へ畳み込む。
/// Smart / Safe は重なった既存矩形の ID・名前・pivot・Border を残すため、保存済みの
/// 参照も手で詰めた pivot も生き残る。動くのは矩形だけ。
/// @param outReusedCount 既存を再利用した数 (= 切れずに済んだ参照の数)。不要なら nullptr。
[[nodiscard]] std::vector<asset::SpriteRect> MergeIntoExisting(
    const std::vector<asset::SpriteRect>& existing,
    std::vector<asset::SpriteRect> generated,
    ExistingMode mode, int* outReusedCount = nullptr);

} // namespace fbzz::editor::spriteslice
