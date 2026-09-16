/// @file    CurlNoise.hpp
/// @brief   発散ゼロの乱流ベクトル場。式は ParticleNoise.hlsli と一対一で対応する。
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// WHY ヘッダーへ出すか:
///   同じ式を ParticlePass (粒子の乱流)・VectorFieldAsset (Curl レシピのベイク)・
///   HLSL の 3 か所が持つ。C++ 側が 2 か所に分かれた時点で «片方だけ直す» が起こり、
///   焼いた場と実行時の乱流がずれる。値が一致していることが前提の機能なので実体は 1 つにする。
#pragma once
#include <cmath>
#include <cstdint>
#include <Math/Vector3.hpp>

namespace fbzz::core {

/// 整数ハッシュ (PCG 系)。格子点から再現可能な擬似乱数を作る。
inline uint32_t PcgHash(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

/// 格子点 (整数座標) → [-1, 1] の擬似乱数値
inline float LatticeValue(int xi, int yi, int zi)
{
    const uint32_t h = PcgHash(static_cast<uint32_t>(xi) * 73856093u
                             ^ static_cast<uint32_t>(yi) * 19349663u
                             ^ static_cast<uint32_t>(zi) * 83492791u);
    return static_cast<float>(h) * (2.0f / 4294967295.0f) - 1.0f;
}

/// 3D 値ノイズ [-1, 1]。8 格子点を smoothstep 重みでトリリニア補間する。
inline float ValueNoise3D(const math::Vector3& p)
{
    const float fx = std::floor(p.x);
    const float fy = std::floor(p.y);
    const float fz = std::floor(p.z);
    const int xi = static_cast<int>(fx);
    const int yi = static_cast<int>(fy);
    const int zi = static_cast<int>(fz);
    float tx = p.x - fx;
    float ty = p.y - fy;
    float tz = p.z - fz;
    // smoothstep フェード: 格子境界で勾配を連続にする
    tx = tx * tx * (3.0f - 2.0f * tx);
    ty = ty * ty * (3.0f - 2.0f * ty);
    tz = tz * tz * (3.0f - 2.0f * tz);
    const float c000 = LatticeValue(xi,     yi,     zi);
    const float c100 = LatticeValue(xi + 1, yi,     zi);
    const float c010 = LatticeValue(xi,     yi + 1, zi);
    const float c110 = LatticeValue(xi + 1, yi + 1, zi);
    const float c001 = LatticeValue(xi,     yi,     zi + 1);
    const float c101 = LatticeValue(xi + 1, yi,     zi + 1);
    const float c011 = LatticeValue(xi,     yi + 1, zi + 1);
    const float c111 = LatticeValue(xi + 1, yi + 1, zi + 1);
    const float x00 = c000 + (c100 - c000) * tx;
    const float x10 = c010 + (c110 - c010) * tx;
    const float x01 = c001 + (c101 - c001) * tx;
    const float x11 = c011 + (c111 - c011) * tx;
    const float y0 = x00 + (x10 - x00) * ty;
    const float y1 = x01 + (x11 - x01) * ty;
    return y0 + (y1 - y0) * tz;
}

/// カールノイズ: 3 成分のベクトルポテンシャル ψ の回転 (∇×ψ) を中心差分で求める。
/// WHY: 回転場は発散ゼロのため粒子が一点に溜まらず、煙・炎らしい滑らかな渦を作れる。
inline math::Vector3 CurlNoise(const math::Vector3& p)
{
    // 各ポテンシャル成分は同じノイズを離れた位置からサンプリングして独立させる
    const math::Vector3 p1 = { p.x + 31.341f, p.y + 31.341f, p.z + 31.341f };
    const math::Vector3 p2 = { p.x - 47.853f, p.y - 47.853f, p.z - 47.853f };
    const math::Vector3 p3 = { p.x + 12.793f, p.y + 12.793f, p.z + 12.793f };
    constexpr float eps = 0.25f;
    constexpr float invTwoEps = 1.0f / (2.0f * eps);
    const math::Vector3 dx = { eps, 0.0f, 0.0f };
    const math::Vector3 dy = { 0.0f, eps, 0.0f };
    const math::Vector3 dz = { 0.0f, 0.0f, eps };
    const float dp1dy = (ValueNoise3D(p1 + dy) - ValueNoise3D(p1 - dy)) * invTwoEps;
    const float dp1dz = (ValueNoise3D(p1 + dz) - ValueNoise3D(p1 - dz)) * invTwoEps;
    const float dp2dx = (ValueNoise3D(p2 + dx) - ValueNoise3D(p2 - dx)) * invTwoEps;
    const float dp2dz = (ValueNoise3D(p2 + dz) - ValueNoise3D(p2 - dz)) * invTwoEps;
    const float dp3dx = (ValueNoise3D(p3 + dx) - ValueNoise3D(p3 - dx)) * invTwoEps;
    const float dp3dy = (ValueNoise3D(p3 + dy) - ValueNoise3D(p3 - dy)) * invTwoEps;
    return { dp3dy - dp2dz, dp1dz - dp3dx, dp2dx - dp1dy };
}

/// Turbulence / Noise モジュール共通のサンプル座標。時間スクロールは軸ごとに
/// 速度を変え、場全体が一方向へ流れて見えないようにする (HLSL 側と一致)。
inline math::Vector3 TurbulenceSamplePoint(const math::Vector3& position,
                                           float frequency, float speed, float time)
{
    const float scroll = time * speed;
    return { position.x * frequency + scroll,
             position.y * frequency + scroll * 0.35f,
             position.z * frequency + scroll * 0.7f };
}

} // namespace fbzz::core
