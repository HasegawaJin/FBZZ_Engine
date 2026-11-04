/// @file    FlipbookMotionVectorEncoding.hpp
/// @brief   モーションベクター付きフリップブックの «保存値の規約» と Atlas 配置の計算。
/// @author  Hasegawa Jin
/// @date    2026-09-11

/// @note 規約 (Particle.hlsl は変更せず、そちらが実際に行う計算から逆算したもの):
/// @note d      = コマ N → N+1 で内容が動いた量。**Atlas 全体の UV** で軸ごと (+U 右 / +V 下)
/// @note 保存値 = clamp(-d / S, -1, 1) * 0.5 + 0.5 を RG へ
/// @note S      = 全画素の max(|d.x|, |d.y|)。マテリアルの motionVectorStrength にそのまま入れる
/// @note シェーダーは cur += m·b·S / next -= m·(1-b)·S なので、m·S = -d のとき
/// @note cur = p - b·d, next = p + (1-b)·d となり、2 コマとも «コマ N+b で内容がある位置» を指す。
/// @note d を軸ごとの Atlas UV で持つため、非正方の Atlas でもスカラー 1 つで足りる。

/// @note GPU 無しの純関数に切り出す理由: 符号を 1 つ間違えても補間が濁るだけで例外もログも
/// @note 出ない (旧生成器は符号・単位とも逆だった)。シェーダーの計算を C++ に写しテストで固定する。
#pragma once
#include <Engine/Api.hpp>

#include <Math/Vector2.hpp>
#include <Math/Vector4.hpp>

#include <cstdint>
#include <span>

namespace fbzz::asset {

/// @note Atlas の列・行。配置は左上から行優先 (ParticlePass の SpriteRectForFrame と同じ)。
struct FlipbookGrid {
    int columns = 1;
    int rows = 1;
    int frameCount = 1;
};

/// @note requestedColumns <= 0 で ceil(sqrt(frameCount)) 列に自動配置する。
[[nodiscard]] FBZZ_ENGINE_API FlipbookGrid ComputeFlipbookGrid(int frameCount, int requestedColumns);

struct FlipbookTileOrigin {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
};

[[nodiscard]] FBZZ_ENGINE_API FlipbookTileOrigin TileOriginPx(int frame, const FlipbookGrid& grid,
                                              std::uint32_t tileWidth, std::uint32_t tileHeight);

/// @note タイル 1 枚ぶりの UV で表した移動量を、Atlas 全体の UV へ直す。
[[nodiscard]] FBZZ_ENGINE_API math::Vector2 TileUvToAtlasUv(math::Vector2 tileUv, const FlipbookGrid& grid);

[[nodiscard]] FBZZ_ENGINE_API math::Vector2 PixelToAtlasUv(float dxPixels, float dyPixels,
                                           std::uint32_t atlasWidth, std::uint32_t atlasHeight);

/// @note 規約の S。全要素が 0 のときは minimumStrength を返す (0 除算と «効かない MV» を避ける)。
[[nodiscard]] FBZZ_ENGINE_API float ComputeRecommendedStrength(std::span<const math::Vector2> displacementsUv,
                                               float minimumStrength);

struct EncodedMotionVector {
    std::uint8_t r = 128;
    std::uint8_t g = 128;
};

/// @note 8bit ではゼロが 127.5 を表せず、デコード後に最大 S/255 の偏りが残る。
/// @note Atlas UV ではサブピクセルなので許容している。
[[nodiscard]] FBZZ_ENGINE_API EncodedMotionVector EncodeMotionVector(math::Vector2 displacementUv, float strength);

/// @note シェーダーと同じ `rg * 2 - 1`。
[[nodiscard]] FBZZ_ENGINE_API math::Vector2 DecodeMotionVector(std::uint8_t r, std::uint8_t g);

/// @note Particle.hlsl の currentUv / nextUv の計算の写し。シェーダー側を変えたらここも変えること。
[[nodiscard]] FBZZ_ENGINE_API math::Vector2 WarpCurrentUv(math::Vector2 uv, math::Vector2 decodedMotion,
                                          float blend, float strength);
[[nodiscard]] FBZZ_ENGINE_API math::Vector2 WarpNextUv(math::Vector2 uv, math::Vector2 decodedMotion,
                                       float blend, float strength);

/// @note coverage が threshold 以下の画素へ、覆われた 4 近傍の平均を iterations 回だけ広げる。
/// @note タイル境界は越えない (隣のコマの速度を持ち込まないため)。
/// @note 煙の縁は Bilinear で速度 0 の空白と混ざり、縁だけ動きが鈍って見える。
FBZZ_ENGINE_API void DilateMotion(std::span<math::Vector2> motion, std::span<const float> coverage,
                  std::uint32_t atlasWidth, std::uint32_t atlasHeight,
                  const FlipbookGrid& grid, std::uint32_t tileWidth, std::uint32_t tileHeight,
                  int iterations, float threshold = 1.0e-4f);

struct EncodedColorTexel {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 0;
    bool clipped = false;   ///< @note exposure 後の RGB が 1 を超えて切り詰められた
};

/// @note 事前乗算・リニア HDR の画素を、sRGB の 8bit へ落とす。α はリニアのまま。
/// @note 変換式は ParticleLinearToSrgb (シェーダーの SRGBToLinear の逆) と一致する。
[[nodiscard]] FBZZ_ENGINE_API EncodedColorTexel EncodeColorTexel(const math::Vector4& linearPremultiplied,
                                                 float exposure);

} /// @note namespace fbzz::asset
