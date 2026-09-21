/// @file    CloudNoiseBake.hpp
/// @brief   起動時に CPU で焼く「タイラブルな 3D ノイズボリューム」生成ヘルパー。
/// @author  Hasegawa Jin
/// @date    2026-07-01
/// @note /// @note ボリューメトリック雲はレイマーチの各サンプル (+ ライトマーチ) で 3D ノイズを引くため、
/// @note 手続き計算だと ALU ネックになる。Nubis/Horizon 同様に事前ベイクした 3D テクスチャ
/// @note (Shape+Detail) を HW トライリニアでサンプルし、外部資産を持たず自己完結させるため
/// @note 周期 (タイラブル) ノイズを実行時生成する。生成物は RGBA8, WRAP サンプル前提:
/// @note Shape(128³) R=Perlin-Worley 基本形状/G-B-A=低周波 Worley FBM、Detail(32³) R/G/B=
/// @note 高周波 Worley FBM (雲縁の侵食用)。
#pragma once
#include <vector>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <thread>

namespace fbzz::renderer::cloudnoise {

/// @note 3D 整数セル座標 → [0,1) の決定的ハッシュ。呼び出し側で period でラップ済み (非負) の座標を渡す。
/// @note 乗算は uint32_t で行い符号付きオーバーフロー (UB) を避ける。
inline float Hash(int x, int y, int z)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)z * 1442695040u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (h & 0x00FFFFFFu) / float(0x01000000);
}

/// @note セルごとの特徴点オフセット [0,1)³。period でラップしてタイラブルにする。
inline void HashPoint(int x, int y, int z, int period, float& ox, float& oy, float& oz)
{
    int wx = ((x % period) + period) % period;
    int wy = ((y % period) + period) % period;
    int wz = ((z % period) + period) % period;
    ox = Hash(wx,        wy,        wz);
    oy = Hash(wx + 31,   wy + 57,   wz + 13);
    oz = Hash(wx + 101,  wy + 71,   wz + 167);
}

/// @note タイラブル Worley (cellular)。戻り値 = 1 - minDist（セル中心が明るい blob になる反転版）。
inline float Worley(float px, float py, float pz, int cells)
{
    px *= cells; py *= cells; pz *= cells;
    int bx = (int)std::floor(px), by = (int)std::floor(py), bz = (int)std::floor(pz);
    float fx = px - bx, fy = py - by, fz = pz - bz;
    float minD = 1e9f;
    for (int dz = -1; dz <= 1; ++dz)
    for (int dy = -1; dy <= 1; ++dy)
    for (int dx = -1; dx <= 1; ++dx)
    {
        float ox, oy, oz;
        HashPoint(bx + dx, by + dy, bz + dz, cells, ox, oy, oz);
        float rx = dx + ox - fx, ry = dy + oy - fy, rz = dz + oz - fz;
        float d = rx * rx + ry * ry + rz * rz;
        if (d < minD) minD = d;
    }
    return 1.0f - (std::min)(std::sqrt(minD), 1.0f);
}

/// @note タイラブル Worley FBM。各オクターブの cells を 2 倍にしても [0,1)³ で周期が保たれる。
inline float WorleyFbm(float x, float y, float z, int baseCells, int octaves)
{
    float v = 0.0f, a = 0.625f, sum = 0.0f;
    int cells = baseCells;
    for (int i = 0; i < octaves; ++i) { v += Worley(x, y, z, cells) * a; sum += a; a *= 0.5f; cells *= 2; }
    return v / (std::max)(sum, 1e-5f);
}

/// @note タイラブル value noise（簡易 Perlin 近似）。
inline float ValueNoise(float x, float y, float z, int cells)
{
    x *= cells; y *= cells; z *= cells;
    int x0 = (int)std::floor(x), y0 = (int)std::floor(y), z0 = (int)std::floor(z);
    float fx = x - x0, fy = y - y0, fz = z - z0;
    auto sm = [](float t){ return t * t * (3.0f - 2.0f * t); };
    float ux = sm(fx), uy = sm(fy), uz = sm(fz);
    auto H = [&](int dx, int dy, int dz) {
        int wx = ((x0 + dx) % cells + cells) % cells;
        int wy = ((y0 + dy) % cells + cells) % cells;
        int wz = ((z0 + dz) % cells + cells) % cells;
        return Hash(wx, wy + 13, wz + 57);
    };
    float c000 = H(0,0,0), c100 = H(1,0,0), c010 = H(0,1,0), c110 = H(1,1,0);
    float c001 = H(0,0,1), c101 = H(1,0,1), c011 = H(0,1,1), c111 = H(1,1,1);
    float x00 = c000 + (c100 - c000) * ux, x10 = c010 + (c110 - c010) * ux;
    float x01 = c001 + (c101 - c001) * ux, x11 = c011 + (c111 - c011) * ux;
    float y0v = x00 + (x10 - x00) * uy, y1v = x01 + (x11 - x01) * uy;
    return y0v + (y1v - y0v) * uz;
}

inline float PerlinFbm(float x, float y, float z, int baseCells, int octaves)
{
    float v = 0.0f, a = 0.5f, sum = 0.0f;
    int cells = baseCells;
    for (int i = 0; i < octaves; ++i) { v += ValueNoise(x, y, z, cells) * a; sum += a; a *= 0.5f; cells *= 2; }
    return v / (std::max)(sum, 1e-5f);
}

inline float Remap(float v, float lo, float hi, float nlo, float nhi)
{
    return nlo + (v - lo) / (std::max)(hi - lo, 1e-5f) * (nhi - nlo);
}

/// @note z スライス単位で並列実行（ハッシュは純粋関数・出力は z ごとに排他なのでスレッド安全）。
template <class Fn>
inline void ParallelZ(int depth, Fn&& fn)
{
    unsigned n = (std::max)(1u, std::thread::hardware_concurrency());
    n = (std::min)(n, (unsigned)depth);
    std::vector<std::thread> pool;
    int per = (depth + (int)n - 1) / (int)n;
    for (unsigned t = 0; t < n; ++t) {
        int z0 = (int)t * per, z1 = (std::min)(depth, z0 + per);
        if (z0 >= z1) break;
        pool.emplace_back([z0, z1, &fn]{ for (int z = z0; z < z1; ++z) fn(z); });
    }
    for (auto& th : pool) th.join();
}

/// @note Shape ボリューム: R=Perlin-Worley, G/B/A=Worley FBM 帯。
inline std::vector<uint8_t> BakeShape(int size)
{
    std::vector<uint8_t> data((size_t)size * size * size * 4u);
    ParallelZ(size, [&](int z) {
        for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
        {
            float u = (x + 0.5f) / size, v = (y + 0.5f) / size, w = (z + 0.5f) / size;
            float perlin = PerlinFbm(u, v, w, 4, 3);
            float wlow   = WorleyFbm(u, v, w, 4, 3);
            /// @note Perlin-Worley: perlin を worley でリマップして雲塊らしい連結感を出す。
            float pw = std::clamp(Remap(perlin, wlow - 1.0f, 1.0f, 0.0f, 1.0f), 0.0f, 1.0f);
            float g = WorleyFbm(u, v, w, 8,  3);
            float b = WorleyFbm(u, v, w, 14, 3);
            float a = WorleyFbm(u, v, w, 22, 2);
            size_t idx = ((size_t)z * size * size + (size_t)y * size + x) * 4u;
            data[idx + 0] = (uint8_t)(std::clamp(pw, 0.0f, 1.0f) * 255.0f);
            data[idx + 1] = (uint8_t)(std::clamp(g,  0.0f, 1.0f) * 255.0f);
            data[idx + 2] = (uint8_t)(std::clamp(b,  0.0f, 1.0f) * 255.0f);
            data[idx + 3] = (uint8_t)(std::clamp(a,  0.0f, 1.0f) * 255.0f);
        }
    });
    return data;
}

/// @note Detail ボリューム: 高周波 Worley FBM（縁の侵食用）。
inline std::vector<uint8_t> BakeDetail(int size)
{
    std::vector<uint8_t> data((size_t)size * size * size * 4u);
    ParallelZ(size, [&](int z) {
        for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
        {
            float u = (x + 0.5f) / size, v = (y + 0.5f) / size, w = (z + 0.5f) / size;
            float r = WorleyFbm(u, v, w, 4,  3);
            float g = WorleyFbm(u, v, w, 8,  3);
            float b = WorleyFbm(u, v, w, 16, 2);
            size_t idx = ((size_t)z * size * size + (size_t)y * size + x) * 4u;
            data[idx + 0] = (uint8_t)(std::clamp(r, 0.0f, 1.0f) * 255.0f);
            data[idx + 1] = (uint8_t)(std::clamp(g, 0.0f, 1.0f) * 255.0f);
            data[idx + 2] = (uint8_t)(std::clamp(b, 0.0f, 1.0f) * 255.0f);
            data[idx + 3] = 255;
        }
    });
    return data;
}

} // namespace fbzz::renderer::cloudnoise
