/// @file    RenderPasses/Geometry/WaterNoiseBake.hpp
/// @brief   水面のさざ波タイル (タイラブルな勾配ノイズ + ミップ連鎖) を CPU で焼く。
/// @author  Hasegawa Jin
/// @date    2026-09-16
///
/// WHY 手続き計算をやめてテクスチャにするか:
///   1. 精度。旧実装は `frac(sin(dot(p,k)))` をワールド由来の格子添字で引いていた。
///      原点から数百 m 離れると sin の引数が 10^5 rad を超え、fp32 の刻みがハッシュを
///      量子化する。海サイズの水面ではさざ波が縞や繰り返しへ崩れていた。
///   2. エイリアシング。勾配をミップへ落とすと «平均すると平坦» が自動的に成り立ち、
///      遠景のさざ波が総毛立つのをハードウェアのフィルタが引き受ける。
///   3. 速度。5 オクターブ × 4 ハッシュ = 20 回の sin が消え、浮いたぶんを
///      異方フィルタと領域ワープに回せる。
///
/// WHY オーサリング資産にしないか: Water.hlsl の «アセット 0 個で成立する» 前提を崩さない。
/// タイルは起動時に 1 枚だけ焼いて全水面で共有するため、水面ごとのタイリング調整も要らない。
///
/// 生成物 (RGBA8, WRAP サンプル前提で完全タイラブル):
///   R/G = ∂h/∂q (q はノイズセル単位)。0.5 中心で `derivativeScale` 倍に正規化済み
///   B   = 高さ h を [0,1] へ写したもの (泡のムラに使う)
///   A   = 255 (予約)
/// @see Docs/design/water-waves.md
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fbzz::scene::waternoise {

/// タイル 1 辺 [texel]。
inline constexpr std::uint32_t kTileSize = 512u;
/// 勾配 1 成分あたりの目標 RMS。
/// WHY 固定値へ正規化するか: 旧実装の値ノイズ勾配の RMS がこの値だった。揃えておかないと、
///      ノイズの種類を差し替えただけで既存 .mat の detailStrength が別の強さを意味してしまう。
inline constexpr float kGradientRms = 0.5f;
/// タイル 1 辺あたりのノイズセル数。
/// WHY 64 か: 1 セル 8 texel を確保しつつ、さざ波の «最も粗いオクターブ» の繰り返しを
///      detailScale=0.37 で 173 m まで伸ばす。セルを増やすと texel/cell が減って
///      勾配が階段状になり、減らすと繰り返しが目に付く。
inline constexpr std::uint32_t kTileCells = 64u;

/// 焼き上がったタイル。mips[0] が最大解像度。
struct Tile {
    std::vector<std::vector<std::uint8_t>> mips;   ///< 各段の RGBA8
    std::vector<std::uint32_t>             sizes;  ///< 各段の 1 辺 [texel]
    /// R/G を [-1,1] へ戻したあとに掛ける係数。シェーダーへは CB 経由で渡す。
    float derivativeScale = 1.0f;
};

namespace detail {

/// 整数セル座標 → [0,1) の決定的ハッシュ。乗算は uint32_t で行い符号付きオーバーフロー (UB) を避ける。
inline float Hash(std::int32_t x, std::int32_t y, std::uint32_t salt)
{
    std::uint32_t h = static_cast<std::uint32_t>(x) * 374761393u
                    + static_cast<std::uint32_t>(y) * 668265263u + salt;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<float>(h & 0x00FFFFFFu) / static_cast<float>(0x01000000);
}

/// 格子点の単位勾配ベクトル。period でラップしてタイラブルにする。
inline void CellGradient(std::int32_t x, std::int32_t y, std::int32_t period, float& gx, float& gy)
{
    const std::int32_t wx = ((x % period) + period) % period;
    const std::int32_t wy = ((y % period) + period) % period;
    const float angle = Hash(wx, wy, 0x9E3779B9u) * 6.283185307f;
    gx = std::cos(angle);
    gy = std::sin(angle);
}

/// タイラブルな 2D 勾配ノイズ (Perlin)。値と解析勾配を同時に返す。
///
/// WHY 値ノイズではないか: 値ノイズの勾配は格子線上でゼロになるため、«平らな筋» が
///     碁盤目に残ってさざ波が泡状に見える。勾配ノイズは格子上でも傾きを持つ。
/// @param px,py セル単位の座標。
/// @param period 何セルで折り返すか。
/// @param[out] dx,dy ∂h/∂px, ∂h/∂py。
/// @return 高さ h。おおよそ [-0.7, 0.7]。
inline float GradientNoise(float px, float py, std::int32_t period, float& dx, float& dy)
{
    const std::int32_t ix = static_cast<std::int32_t>(std::floor(px));
    const std::int32_t iy = static_cast<std::int32_t>(std::floor(py));
    const float fx = px - static_cast<float>(ix);
    const float fy = py - static_cast<float>(iy);

    float g00x, g00y, g10x, g10y, g01x, g01y, g11x, g11y;
    CellGradient(ix,     iy,     period, g00x, g00y);
    CellGradient(ix + 1, iy,     period, g10x, g10y);
    CellGradient(ix,     iy + 1, period, g01x, g01y);
    CellGradient(ix + 1, iy + 1, period, g11x, g11y);

    const float n00 = g00x * fx         + g00y * fy;
    const float n10 = g10x * (fx - 1.f) + g10y * fy;
    const float n01 = g01x * fx         + g01y * (fy - 1.f);
    const float n11 = g11x * (fx - 1.f) + g11y * (fy - 1.f);

    /// @note 5 次のフェード。3 次 (smoothstep) だと 2 階微分が格子で折れ、
    ///       勾配テクスチャにしたとき «筋» として見えてしまう。
    const float ux  = fx * fx * fx * (fx * (fx * 6.f - 15.f) + 10.f);
    const float uy  = fy * fy * fy * (fy * (fy * 6.f - 15.f) + 10.f);
    const float dux = 30.f * fx * fx * (fx * (fx - 2.f) + 1.f);
    const float duy = 30.f * fy * fy * (fy * (fy - 2.f) + 1.f);

    const float k1 = n10 - n00;
    const float k2 = n01 - n00;
    const float k3 = n00 - n10 - n01 + n11;

    dx = dux * (k1 + k3 * uy)
       + (1.f - ux) * (1.f - uy) * g00x + ux * (1.f - uy) * g10x
       + (1.f - ux) * uy * g01x + ux * uy * g11x;
    dy = duy * (k2 + k3 * ux)
       + (1.f - ux) * (1.f - uy) * g00y + ux * (1.f - uy) * g10y
       + (1.f - ux) * uy * g01y + ux * uy * g11y;

    return n00 + k1 * ux + k2 * uy + k3 * ux * uy;
}

} // namespace detail

/// さざ波タイルを焼く。起動時に 1 度だけ呼ぶ想定。
///
/// @note ミップは 2x2 のボックス平均。符号付きの勾配を 0.5 中心で格納しているため、
///       符号化したまま平均しても «平均した勾配を符号化したもの» と一致する (アフィン変換)。
///       これが «遠くの水面ほど平坦 = 鏡へ近づく» を自動的に成立させる。
[[nodiscard]] inline Tile BakeDetailTile()
{
    Tile tile;
    const std::uint32_t size = kTileSize;
    const std::int32_t  period = static_cast<std::int32_t>(kTileCells);
    const float cellPerTexel = static_cast<float>(kTileCells) / static_cast<float>(size);

    std::vector<float> height(static_cast<std::size_t>(size) * size);
    std::vector<float> gradX(height.size());
    std::vector<float> gradY(height.size());

    double squareSum = 0.0;
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            /// @note テクセル «中心» で標本化する。角で焼くとバイリニア補間が
            ///       半テクセルぶんずれ、勾配と値の位相が食い違う。
            const float px = (static_cast<float>(x) + 0.5f) * cellPerTexel;
            const float py = (static_cast<float>(y) + 0.5f) * cellPerTexel;
            float dx = 0.0f, dy = 0.0f;
            const float h = detail::GradientNoise(px, py, period, dx, dy);
            const std::size_t i = static_cast<std::size_t>(y) * size + x;
            height[i] = h;
            gradX[i] = dx;
            gradY[i] = dy;
            squareSum += static_cast<double>(dx) * dx + static_cast<double>(dy) * dy;
        }
    }

    const float rms = (std::max)(
        static_cast<float>(std::sqrt(squareSum / (2.0 * static_cast<double>(height.size())))), 1.0e-6f);
    const float toTarget = kGradientRms / rms;
    float maxAbsGrad = 1.0e-4f;
    for (std::size_t i = 0; i < height.size(); ++i) {
        gradX[i] *= toTarget;
        gradY[i] *= toTarget;
        maxAbsGrad = (std::max)(maxAbsGrad, (std::max)(std::abs(gradX[i]), std::abs(gradY[i])));
    }
    tile.derivativeScale = maxAbsGrad;
    const float invGrad = 1.0f / maxAbsGrad;

    std::vector<std::uint8_t> level(static_cast<std::size_t>(size) * size * 4u);
    for (std::size_t i = 0; i < height.size(); ++i) {
        const auto encode = [](float v) {
            const float t = v * 0.5f + 0.5f;
            return static_cast<std::uint8_t>((std::min)((std::max)(t, 0.0f), 1.0f) * 255.0f + 0.5f);
        };
        level[i * 4u + 0u] = encode(gradX[i] * invGrad);
        level[i * 4u + 1u] = encode(gradY[i] * invGrad);
        /// @note 高さは泡のムラ用。振幅 0.7 前後なので 1/1.4 で [0,1] へ伸ばす。
        level[i * 4u + 2u] = encode(height[i] * (1.0f / 0.7f));
        level[i * 4u + 3u] = 255u;
    }
    tile.mips.push_back(std::move(level));
    tile.sizes.push_back(size);

    for (std::uint32_t w = size / 2u; w >= 1u; w /= 2u) {
        const std::vector<std::uint8_t>& src = tile.mips.back();
        const std::uint32_t srcW = tile.sizes.back();
        std::vector<std::uint8_t> dst(static_cast<std::size_t>(w) * w * 4u);
        for (std::uint32_t y = 0; y < w; ++y) {
            for (std::uint32_t x = 0; x < w; ++x) {
                for (std::uint32_t c = 0; c < 4u; ++c) {
                    const std::size_t s0 = ((static_cast<std::size_t>(y) * 2u) * srcW + x * 2u) * 4u + c;
                    const std::size_t s1 = s0 + 4u;
                    const std::size_t s2 = s0 + static_cast<std::size_t>(srcW) * 4u;
                    const std::size_t s3 = s2 + 4u;
                    const std::uint32_t sum = src[s0] + src[s1] + src[s2] + src[s3];
                    dst[(static_cast<std::size_t>(y) * w + x) * 4u + c] =
                        static_cast<std::uint8_t>((sum + 2u) / 4u);
                }
            }
        }
        tile.mips.push_back(std::move(dst));
        tile.sizes.push_back(w);
        if (w == 1u) break;
    }
    return tile;
}

} // namespace fbzz::scene::waternoise
