/// @file    TerrainBrush.cpp
/// @brief   Terrain ブラシカーネルの実装。TerrainTool (マウス) と EditorBusDispatcher (AI) の共通実体。
/// @author  Hasegawa Jin
/// @date    2026-08-14
/// @see Docs/design/terrain-layers.md
#include "TerrainBrush.hpp"

#include <Engine/Scene/TerrainSplat.hpp>
#include <Math/Matrix4.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Vector4.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace fbzz::editor {

namespace {

/// @brief 侵食系の «1 秒あたりの効き» を strength から作る倍率。
/// @note strength の既定値 0.05 は Raise 向け (1 秒で maxHeight の 5%)。侵食はその値でも数秒の長押しで形が変わる速さにする。
constexpr float kTerrainErosionRatePerSecond = 30.0f;

/// @brief Smooth で使う上下左右 4 近傍の平均 (正規化高さ)。
float TerrainBrushAvg4(const scene::TerrainComponent& terrain, int x, int z)
{
    auto h = [&](int xi, int zi) {
        xi = std::clamp(xi, 0, terrain.columns - 1);
        zi = std::clamp(zi, 0, terrain.rows    - 1);
        return terrain.heightData[static_cast<size_t>(zi) * static_cast<size_t>(terrain.columns)
                                + static_cast<size_t>(xi)];
    };
    return (h(x - 1, z) + h(x + 1, z) + h(x, z - 1) + h(x, z + 1)) * 0.25f;
}

/// @brief 32 bit 整数ハッシュ (lowbias32)。乱数とノイズの格子値を種から決定的に作る。
/// @see https://nullprogram.com/blog/2018/07/31/ (C. Wellons, "Prospecting for Hash Functions")
std::uint32_t TerrainBrushHash32(std::uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

/// @brief カウンター方式の決定的乱数。同じ種なら人と AI バスで同じ列になる。
struct TerrainBrushRng {
    std::uint32_t state = 0;

    /// @return [0, 1)
    float Next01()
    {
        state += 0x9e3779b9u;
        return static_cast<float>(TerrainBrushHash32(state) >> 8) * (1.0f / 16777216.0f);
    }
};

/// @brief 格子点 (ix, iz) の値 [0, 1]。
float TerrainBrushLatticeValue(int ix, int iz, std::uint32_t seed)
{
    const std::uint32_t h = TerrainBrushHash32(
        static_cast<std::uint32_t>(ix) * 0x8da6b343u
        ^ static_cast<std::uint32_t>(iz) * 0xd8163841u
        ^ TerrainBrushHash32(seed));
    return static_cast<float>(h >> 8) * (1.0f / 16777215.0f);
}

/// @brief 2D 値ノイズ [0, 1]。補間は 5 次の fade (勾配が格子で連続)。
/// @see https://iquilezles.org/articles/morenoise/ (I. Quilez, "Value Noise Derivatives")
float TerrainBrushValueNoise(float x, float z, std::uint32_t seed)
{
    const float fx0 = std::floor(x);
    const float fz0 = std::floor(z);
    const int   ix  = static_cast<int>(fx0);
    const int   iz  = static_cast<int>(fz0);
    const float fx  = x - fx0;
    const float fz  = z - fz0;
    const float ux  = fx * fx * fx * (fx * (fx * 6.0f - 15.0f) + 10.0f);
    const float uz  = fz * fz * fz * (fz * (fz * 6.0f - 15.0f) + 10.0f);

    const float a = TerrainBrushLatticeValue(ix,     iz,     seed);
    const float b = TerrainBrushLatticeValue(ix + 1, iz,     seed);
    const float c = TerrainBrushLatticeValue(ix,     iz + 1, seed);
    const float d = TerrainBrushLatticeValue(ix + 1, iz + 1, seed);
    return math::Lerp(math::Lerp(a, b, ux), math::Lerp(c, d, ux), uz);
}

/// @brief fBm (lacunarity 2 / gain 0.5)。振幅の総和で割って [0, 1] に収める。
/// @see https://iquilezles.org/articles/fbm/ (I. Quilez, "fractional Brownian Motion")
float TerrainBrushFbm(float x, float z, std::uint32_t seed, int octaves)
{
    float sum = 0.0f;
    float norm = 0.0f;
    float amplitude = 1.0f;
    float frequency = 1.0f;
    for (int o = 0; o < octaves; ++o) {
        sum  += amplitude * TerrainBrushValueNoise(x * frequency, z * frequency,
                                                   seed + static_cast<std::uint32_t>(o) * 0x632be5abu);
        norm += amplitude;
        frequency *= 2.0f;
        amplitude *= 0.5f;
    }
    return norm > 0.0f ? sum / norm : 0.5f;
}

/// @brief 頂点が有効な格子として揃っているか。揃っていなければ平坦化して揃える。
/// @return 書き込める格子が無ければ false。
bool TerrainBrushEnsureHeights(scene::TerrainComponent& terrain)
{
    if (terrain.columns <= 0 || terrain.rows <= 0 || terrain.cellSize <= 0.0f) return false;
    if (terrain.heightData.size() != terrain.VertexCount())
        terrain.InitFlat();
    return true;
}

/// @brief 熱侵食 (安息角を超えた斜面を崩す)。
/// @note 読み出しは領域のコピーから行い、差分を後でまとめて足す。走査順で片側へ崩れる偏りを出さないため。
/// @see https://history.siggraph.org/learning/the-synthesis-and-rendering-of-eroded-fractal-terrains-by-musgrave-kolb-and-mace/ (Musgrave, Kolb, Mace 1989, "Thermal weathering")
void TerrainBrushThermal(scene::TerrainComponent& terrain, const math::Vector3& hitLocal,
                         const TerrainBrush& brush, float dt)
{
    const float cell = terrain.cellSize;
    const float safeMaxHeight = std::max(terrain.maxHeight, 0.0001f);
    const float tanTalus = std::tan(math::Clamp(brush.talusDegrees, 0.0f, 89.0f) * math::DEG2RAD);

    const int ri = static_cast<int>(brush.radius / cell) + 1;
    const int cx = static_cast<int>(std::floor(hitLocal.x / cell));
    const int cz = static_cast<int>(std::floor(hitLocal.z / cell));
    const int x0 = std::max(cx - ri, 0);
    const int x1 = std::min(cx + ri, terrain.columns - 1);
    const int z0 = std::max(cz - ri, 0);
    const int z1 = std::min(cz + ri, terrain.rows - 1);
    if (x0 > x1 || z0 > z1) return;

    /// @note 崩れた土は 1 マス外まで届くので、コピーと差分は 1 マス広く取る。
    const int ex0 = std::max(x0 - 1, 0);
    const int ex1 = std::min(x1 + 1, terrain.columns - 1);
    const int ez0 = std::max(z0 - 1, 0);
    const int ez1 = std::min(z1 + 1, terrain.rows - 1);
    const int ew  = ex1 - ex0 + 1;
    const int eh  = ez1 - ez0 + 1;

    std::vector<float> src(static_cast<size_t>(ew) * static_cast<size_t>(eh));
    std::vector<float> delta(src.size(), 0.0f);
    auto local = [&](int x, int z) {
        return static_cast<size_t>(z - ez0) * static_cast<size_t>(ew) + static_cast<size_t>(x - ex0);
    };
    for (int z = ez0; z <= ez1; ++z)
        for (int x = ex0; x <= ex1; ++x)
            src[local(x, z)] = terrain.heightData[static_cast<size_t>(z) * static_cast<size_t>(terrain.columns)
                                                + static_cast<size_t>(x)];

    static constexpr std::array<std::array<int, 2>, 8> kNeighbours = { {
        { -1, -1 }, { 0, -1 }, { 1, -1 }, { -1, 0 }, { 1, 0 }, { -1, 1 }, { 0, 1 }, { 1, 1 },
    } };

    for (int z = z0; z <= z1; ++z) {
        for (int x = x0; x <= x1; ++x) {
            const float dx = static_cast<float>(x) * cell - hitLocal.x;
            const float dz = static_cast<float>(z) * cell - hitLocal.z;
            const float w  = TerrainBrushWeight(brush, std::sqrt(dx * dx + dz * dz));
            if (w <= 0.0f) continue;

            const float h = src[local(x, z)];
            std::array<float, 8> excess{};
            float excessSum = 0.0f;
            float excessMax = 0.0f;
            for (size_t n = 0; n < kNeighbours.size(); ++n) {
                const int nx = x + kNeighbours[n][0];
                const int nz = z + kNeighbours[n][1];
                if (nx < ex0 || nx > ex1 || nz < ez0 || nz > ez1) continue;
                const bool diagonal = kNeighbours[n][0] != 0 && kNeighbours[n][1] != 0;
                /// @note 安息角を正規化高さの差へ直す。斜め隣は水平距離が √2 倍なので許す差も √2 倍。
                const float talus = tanTalus * cell * (diagonal ? 1.41421356f : 1.0f) / safeMaxHeight;
                const float e = (h - src[local(nx, nz)]) - talus;
                if (e <= 0.0f) continue;
                excess[n] = e;
                excessSum += e;
                excessMax = std::max(excessMax, e);
            }
            if (excessSum <= 0.0f) continue;

            /// @note c ≤ 0.5 で «超過分の半分» を上限にする。それ以上動かすと高低が入れ替わって振動する。
            const float c = 0.5f * math::Clamp01(brush.strength * w * dt * kTerrainErosionRatePerSecond);
            const float moved = c * excessMax;
            delta[local(x, z)] -= moved;
            for (size_t n = 0; n < kNeighbours.size(); ++n) {
                if (excess[n] <= 0.0f) continue;
                delta[local(x + kNeighbours[n][0], z + kNeighbours[n][1])] += moved * excess[n] / excessSum;
            }
        }
    }

    for (int z = ez0; z <= ez1; ++z)
        for (int x = ex0; x <= ex1; ++x) {
            float& h = terrain.heightData[static_cast<size_t>(z) * static_cast<size_t>(terrain.columns)
                                        + static_cast<size_t>(x)];
            h = std::clamp(src[local(x, z)] + delta[local(x, z)], -1.0f, 1.0f);
        }
}

/// @brief 格子単位の位置 (px, pz) での高さと勾配。高さは «マス» 単位 (水平と同じ尺度)。
struct TerrainBrushHeightGradient {
    float height = 0.0f;
    float gradX  = 0.0f;
    float gradZ  = 0.0f;
};

/// @pre px ∈ [0, columns - 1)、pz ∈ [0, rows - 1)。
TerrainBrushHeightGradient TerrainBrushSampleGradient(const scene::TerrainComponent& terrain,
                                                      float px, float pz, float toCells)
{
    const int   x  = static_cast<int>(px);
    const int   z  = static_cast<int>(pz);
    const float fx = px - static_cast<float>(x);
    const float fz = pz - static_cast<float>(z);
    const size_t c = static_cast<size_t>(terrain.columns);
    const size_t i00 = static_cast<size_t>(z) * c + static_cast<size_t>(x);
    const float h00 = terrain.heightData[i00]            * toCells;
    const float h10 = terrain.heightData[i00 + 1]        * toCells;
    const float h01 = terrain.heightData[i00 + c]        * toCells;
    const float h11 = terrain.heightData[i00 + c + 1]    * toCells;

    TerrainBrushHeightGradient out;
    out.gradX  = (h10 - h00) * (1.0f - fz) + (h11 - h01) * fz;
    out.gradZ  = (h01 - h00) * (1.0f - fx) + (h11 - h10) * fx;
    out.height = h00 * (1.0f - fx) * (1.0f - fz) + h10 * fx * (1.0f - fz)
               + h01 * (1.0f - fx) * fz          + h11 * fx * fz;
    return out;
}

/// @brief 水滴による水力侵食。
/// @note 水滴はブラシ円の外に出たら消す。縁の外まで削り跡を伸ばさないため (運んでいた土も消える)。
/// @note 高さを «マス» 単位へ直して計算する。cellSize / maxHeight を変えても同じ見た目の削れ方になる。
/// @see https://www.firespark.de/resources/downloads/implementation%20of%20a%20methode%20for%20hydraulic%20erosion.pdf (H. T. Beyer 2015, §3 "Simulation")
void TerrainBrushHydraulic(scene::TerrainComponent& terrain, const math::Vector3& hitLocal,
                           const TerrainBrush& brush, float dt, std::uint32_t strokeStep)
{
    if (terrain.columns < 3 || terrain.rows < 3 || brush.radius <= 0.0f) return;

    /// @name Beyer 2015 の既定パラメーター
    /// @{
    constexpr float kInertia       = 0.05f;
    constexpr float kCapacity      = 4.0f;
    constexpr float kMinCapacity   = 0.01f;
    constexpr float kDeposition    = 0.3f;
    constexpr float kErosion       = 0.3f;
    constexpr float kEvaporation   = 0.01f;
    constexpr float kGravity       = 4.0f;
    constexpr int   kMaxLifetime   = 30;
    constexpr int   kErosionRadius = 2;
    /// @}

    const float cell     = terrain.cellSize;
    const float toCells  = std::max(terrain.maxHeight, 0.0001f) / cell;
    const float toNorm   = 1.0f / toCells;
    const float rate     = math::Clamp01(brush.strength * dt * kTerrainErosionRatePerSecond);
    if (rate <= 0.0f) return;
    const float maxX = static_cast<float>(terrain.columns - 1);
    const float maxZ = static_cast<float>(terrain.rows - 1);

    /// @note 削りの重み: 半径 2 マスの円錐を正規化する。1 点だけ削ると針状の穴が残る。
    struct KernelTap { int dx; int dz; float weight; };
    std::vector<KernelTap> kernel;
    float kernelSum = 0.0f;
    for (int dz = -kErosionRadius; dz <= kErosionRadius; ++dz)
        for (int dx = -kErosionRadius; dx <= kErosionRadius; ++dx) {
            const float w = static_cast<float>(kErosionRadius) - std::sqrt(static_cast<float>(dx * dx + dz * dz));
            if (w <= 0.0f) continue;
            kernel.push_back({ dx, dz, w });
            kernelSum += w;
        }
    for (KernelTap& tap : kernel) tap.weight /= kernelSum;

    auto heightAt = [&](int x, int z) -> float& {
        return terrain.heightData[static_cast<size_t>(z) * static_cast<size_t>(terrain.columns)
                                + static_cast<size_t>(x)];
    };
    auto brushWeightAtGrid = [&](float px, float pz) {
        const float dx = px * cell - hitLocal.x;
        const float dz = pz * cell - hitLocal.z;
        return TerrainBrushWeight(brush, std::sqrt(dx * dx + dz * dz));
    };

    TerrainBrushRng rng;
    rng.state = TerrainBrushHash32(brush.seed ^ TerrainBrushHash32(strokeStep + 0x68e31da4u));

    const int dropletCount = std::clamp(brush.erosionDroplets, 0, 4096);
    for (int d = 0; d < dropletCount; ++d) {
        /// @note 円内一様: 半径は √u で引く (u のままだと中心に偏る)。
        const float u = rng.Next01();
        const float v = rng.Next01();
        const float r = brush.radius * std::sqrt(u);
        float px = (hitLocal.x + r * std::cos(v * math::TWO_PI)) / cell;
        float pz = (hitLocal.z + r * std::sin(v * math::TWO_PI)) / cell;
        if (px < 0.0f || pz < 0.0f || px >= maxX || pz >= maxZ) continue;

        float dirX = 0.0f;
        float dirZ = 0.0f;
        float speed = 1.0f;
        float water = 1.0f;
        float sediment = 0.0f;

        for (int life = 0; life < kMaxLifetime; ++life) {
            const int   nodeX = static_cast<int>(px);
            const int   nodeZ = static_cast<int>(pz);
            const float offX  = px - static_cast<float>(nodeX);
            const float offZ  = pz - static_cast<float>(nodeZ);
            const TerrainBrushHeightGradient here = TerrainBrushSampleGradient(terrain, px, pz, toCells);

            dirX = dirX * kInertia - here.gradX * (1.0f - kInertia);
            dirZ = dirZ * kInertia - here.gradZ * (1.0f - kInertia);
            const float len = std::sqrt(dirX * dirX + dirZ * dirZ);
            if (len <= 1e-6f) break;
            dirX /= len;
            dirZ /= len;
            px += dirX;
            pz += dirZ;
            if (px < 0.0f || pz < 0.0f || px >= maxX || pz >= maxZ) break;

            const float w = brushWeightAtGrid(static_cast<float>(nodeX) + offX, static_cast<float>(nodeZ) + offZ);
            if (w <= 0.0f) break;
            const float scale = rate * w;

            const float newHeight = TerrainBrushSampleGradient(terrain, px, pz, toCells).height;
            const float deltaHeight = newHeight - here.height;
            const float capacity = std::max(-deltaHeight * speed * water * kCapacity, kMinCapacity);

            if (sediment > capacity || deltaHeight > 0.0f) {
                /// @note 登りなら窪みを埋める分だけ、下りで容量超過なら超過分の一部を置く。
                const float want = deltaHeight > 0.0f
                    ? std::min(deltaHeight, sediment)
                    : (sediment - capacity) * kDeposition;
                const float amount = want * scale;
                const std::array<float, 4> share = {
                    (1.0f - offX) * (1.0f - offZ), offX * (1.0f - offZ), (1.0f - offX) * offZ, offX * offZ,
                };
                const std::array<std::array<int, 2>, 4> corner = { {
                    { nodeX, nodeZ }, { nodeX + 1, nodeZ }, { nodeX, nodeZ + 1 }, { nodeX + 1, nodeZ + 1 },
                } };
                float deposited = 0.0f;
                for (size_t k = 0; k < 4; ++k) {
                    float& h = heightAt(corner[k][0], corner[k][1]);
                    const float before = h;
                    h = std::min(h + amount * share[k] * toNorm, 1.0f);
                    deposited += (h - before) * toCells;
                }
                sediment = std::max(sediment - deposited, 0.0f);
            } else {
                /// @note 下り坂の高低差より多くは削らない。削り過ぎると自分の通り道に穴を掘って止まる。
                const float amount = std::min((capacity - sediment) * kErosion, -deltaHeight) * scale;
                float eroded = 0.0f;
                for (const KernelTap& tap : kernel) {
                    const int x = nodeX + tap.dx;
                    const int z = nodeZ + tap.dz;
                    if (x < 0 || z < 0 || x >= terrain.columns || z >= terrain.rows) continue;
                    float& h = heightAt(x, z);
                    const float before = h;
                    h = std::max(h - amount * tap.weight * toNorm, -1.0f);
                    eroded += (before - h) * toCells;
                }
                sediment += eroded;
            }

            /// @note v' = √(v² + Δh·g)。下り (Δh < 0) で加速する向きに符号を取り、負の平方根を避ける。
            speed = std::sqrt(std::max(speed * speed - deltaHeight * kGravity, 0.0f));
            water *= (1.0f - kEvaporation);
        }
    }
}

} // namespace

math::Vector3 ToTerrainLocal(const scene::Transform& transform, const math::Vector3& worldPoint)
{
    const math::Vector4 local = math::Matrix4::Inverse(transform.GetWorldMatrix())
                              * math::Vector4{ worldPoint.x, worldPoint.y, worldPoint.z, 1.0f };
    return { local.x, local.y, local.z };
}

math::Vector3 ToTerrainWorld(const scene::Transform& transform, const math::Vector3& localPoint)
{
    const math::Vector4 world = transform.GetWorldMatrix()
                              * math::Vector4{ localPoint.x, localPoint.y, localPoint.z, 1.0f };
    return { world.x, world.y, world.z };
}

bool BrushOverlapsTerrainXZ(const scene::TerrainComponent& terrain,
                            const math::Vector3& localCenter, float radius)
{
    const float w = static_cast<float>(terrain.columns - 1) * terrain.cellSize;
    const float d = static_cast<float>(terrain.rows    - 1) * terrain.cellSize;
    const float nearestX = std::clamp(localCenter.x, 0.0f, w);
    const float nearestZ = std::clamp(localCenter.z, 0.0f, d);
    const float dx = localCenter.x - nearestX;
    const float dz = localCenter.z - nearestZ;
    return (dx * dx + dz * dz) <= radius * radius;
}

float TerrainBrushWeight(const TerrainBrush& brush, float dist)
{
    const float r = brush.radius;
    if (r <= 0.0f || dist >= r) return 0.0f;
    /// @note t ∈ [0, 1)
    const float t = dist / r;
    switch (brush.falloff) {
        case TerrainFalloff::Linear:
            return 1.0f - t;
        case TerrainFalloff::Smooth:
            /// @note smoothstep: t²(3 - 2t)
            return 1.0f - t * t * (3.0f - 2.0f * t);
        case TerrainFalloff::Gaussian:
            /// @note exp(-3 * t²) → t=0 で 1、t=1 で exp(-3) ≒ 0.05
            return std::exp(-3.0f * t * t);
    }
    return 0.0f;
}

int ResolvePaintLayerForTerrain(const scene::TerrainComponent& terrain,
                                const std::string&             sourceMaterial,
                                int                            preferredLayer)
{
    /// @note material path がある場合は各 Terrain 内で同じ path の層へ解決する。
    if (!sourceMaterial.empty()) {
        for (int li = 0; li < terrain.LayerCount(); ++li) {
            if (terrain.layerMaterials[static_cast<size_t>(li)] == sourceMaterial)
                return li;
        }
        return -1;
    }

    if (preferredLayer < 0 || preferredLayer >= terrain.LayerCount()) return -1;
    return terrain.layerMaterials[static_cast<size_t>(preferredLayer)].empty() ? preferredLayer : -1;
}

void ApplyTerrainSculpt(scene::TerrainComponent& terrain,
                        const math::Vector3&     hitLocal,
                        const TerrainBrush&      brush,
                        TerrainSculptOp          op,
                        float                    flattenTargetWorldHeight,
                        float                    dt,
                        std::uint32_t            strokeStep)
{
    /// @note heightData が未初期化のまま書くと添字が飛ぶ。ブラシ側で平坦化して整合を取る。
    if (!TerrainBrushEnsureHeights(terrain)) return;

    if (op == TerrainSculptOp::ThermalErosion) {
        TerrainBrushThermal(terrain, hitLocal, brush, dt);
        return;
    }
    if (op == TerrainSculptOp::HydraulicErosion) {
        TerrainBrushHydraulic(terrain, hitLocal, brush, dt, strokeStep);
        return;
    }

    const float safeMaxHeight = std::max(terrain.maxHeight, 0.0001f);
    const int   noiseOctaves  = std::clamp(brush.noiseOctaves, 1, 8);
    const float noiseInvScale = 1.0f / std::max(brush.noiseScale, 0.001f);

    const int cx = static_cast<int>(hitLocal.x / terrain.cellSize);
    const int cz = static_cast<int>(hitLocal.z / terrain.cellSize);
    const int ri = static_cast<int>(brush.radius / terrain.cellSize) + 1;

    for (int z = cz - ri; z <= cz + ri; ++z) {
        if (z < 0 || z >= terrain.rows) continue;
        for (int x = cx - ri; x <= cx + ri; ++x) {
            if (x < 0 || x >= terrain.columns) continue;

            const float wx   = static_cast<float>(x) * terrain.cellSize;
            const float wz   = static_cast<float>(z) * terrain.cellSize;
            const float dx   = wx - hitLocal.x;
            const float dz   = wz - hitLocal.z;
            const float dist = std::sqrt(dx * dx + dz * dz);
            const float w    = TerrainBrushWeight(brush, dist);
            if (w <= 0.0f) continue;

            const size_t idx = static_cast<size_t>(z) * static_cast<size_t>(terrain.columns)
                             + static_cast<size_t>(x);
            float& h = terrain.heightData[idx];

            switch (op) {
                case TerrainSculptOp::Raise:
                    h = std::clamp(h + brush.strength * w * dt, -1.0f, 1.0f);
                    break;
                case TerrainSculptOp::Lower:
                    /// @note heightData=0 はフラットな基準面。0 でクランプすると溝・川床・クレーターを作れないため負値を許す。
                    h = std::clamp(h - brush.strength * w * dt, -1.0f, 1.0f);
                    break;
                case TerrainSculptOp::Flatten: {
                    const float targetNorm = flattenTargetWorldHeight / safeMaxHeight;
                    h = math::Lerp(h, targetNorm, brush.strength * w * dt);
                    break;
                }
                case TerrainSculptOp::Smooth: {
                    const float avg = TerrainBrushAvg4(terrain, x, z);
                    h = math::Lerp(h, avg, brush.strength * w * dt);
                    break;
                }
                case TerrainSculptOp::Stamp:
                    /// @note ブラシ中心が最高点になるよう既存高さと weight の最大値を取る。押し付けなので既存の高い部分は下げない。
                    h = std::max(h, w);
                    break;
                case TerrainSculptOp::Noise: {
                    /// @note 地形ローカル XZ で引くので、同じ種なら隣接 Terrain の共有頂点でも同じ値になる (境界に段差を作らない)。
                    const float n = TerrainBrushFbm(wx * noiseInvScale, wz * noiseInvScale, brush.seed, noiseOctaves);
                    h = std::clamp(h + (n * 2.0f - 1.0f) * brush.strength * w * dt, -1.0f, 1.0f);
                    break;
                }
                case TerrainSculptOp::Terrace: {
                    const float step = std::max(brush.terraceStep, 0.001f);
                    const float s    = h * safeMaxHeight / step;
                    const float k    = std::floor(s);
                    const float f    = s - k;
                    const float sharp = math::Clamp01(brush.terraceSharpness);
                    /// @note target = (k + f^(1 + 8·sharpness))·step。sharpness 0 で恒等写像になる。
                    const float target = (k + std::pow(f, 1.0f + 8.0f * sharp)) * step / safeMaxHeight;
                    h = std::clamp(math::Lerp(h, target, math::Clamp01(brush.strength * w * dt)), -1.0f, 1.0f);
                    break;
                }
                case TerrainSculptOp::ThermalErosion:
                case TerrainSculptOp::HydraulicErosion:
                    break;
            }
        }
    }
}

void ApplyTerrainPaint(scene::TerrainComponent& terrain,
                       const math::Vector3&     hitLocal,
                       const TerrainBrush&      brush,
                       int                      layerIndex,
                       float                    dt)
{
    if (terrain.columns <= 0 || terrain.rows <= 0) return;
    /// @note 層 0 枚の地形も «白い既定層» 1 枚として描くので、層 0 だけは塗れる。
    if (layerIndex < 0 || layerIndex >= std::max(terrain.LayerCount(), 1)) return;
    terrain.EnsureSplat();

    const int cx = static_cast<int>(hitLocal.x / terrain.cellSize);
    const int cz = static_cast<int>(hitLocal.z / terrain.cellSize);
    const int ri = static_cast<int>(brush.radius / terrain.cellSize) + 1;

    for (int z = cz - ri; z <= cz + ri; ++z) {
        if (z < 0 || z >= terrain.rows) continue;
        for (int x = cx - ri; x <= cx + ri; ++x) {
            if (x < 0 || x >= terrain.columns) continue;

            const float dx = static_cast<float>(x) * terrain.cellSize - hitLocal.x;
            const float dz = static_cast<float>(z) * terrain.cellSize - hitLocal.z;
            const float w  = TerrainBrushWeight(brush, std::sqrt(dx * dx + dz * dz));
            const float t  = brush.strength * w * dt;
            if (t <= 0.0f) continue;

            const size_t base = (static_cast<size_t>(z) * static_cast<size_t>(terrain.columns)
                               + static_cast<size_t>(x)) * static_cast<size_t>(scene::TERRAIN_SPLAT_SLOTS);
            scene::terrain_splat::BlendToward(&terrain.splatIndices[base], &terrain.splatWeights[base],
                                              layerIndex, std::min(t, 1.0f));
        }
    }
}

void ApplyTerrainRamp(scene::TerrainComponent& terrain,
                      const math::Vector3&     startLocal,
                      const math::Vector3&     endLocal,
                      const TerrainBrush&      brush)
{
    if (!TerrainBrushEnsureHeights(terrain) || brush.radius <= 0.0f) return;

    const float safeMaxHeight = std::max(terrain.maxHeight, 0.0001f);
    const float strength = math::Clamp01(brush.strength);
    const float segX = endLocal.x - startLocal.x;
    const float segZ = endLocal.z - startLocal.z;
    const float segLenSq = segX * segX + segZ * segZ;
    /// @note 長さがほぼ 0 の線分は向きが決まらないので、始点の高さへの Flatten として扱う。
    const bool degenerate = segLenSq < 1e-8f;

    const float cell = terrain.cellSize;
    const int x0 = std::max(static_cast<int>(std::floor((std::min(startLocal.x, endLocal.x) - brush.radius) / cell)), 0);
    const int x1 = std::min(static_cast<int>(std::ceil ((std::max(startLocal.x, endLocal.x) + brush.radius) / cell)), terrain.columns - 1);
    const int z0 = std::max(static_cast<int>(std::floor((std::min(startLocal.z, endLocal.z) - brush.radius) / cell)), 0);
    const int z1 = std::min(static_cast<int>(std::ceil ((std::max(startLocal.z, endLocal.z) + brush.radius) / cell)), terrain.rows - 1);

    for (int z = z0; z <= z1; ++z) {
        for (int x = x0; x <= x1; ++x) {
            const float px = static_cast<float>(x) * cell;
            const float pz = static_cast<float>(z) * cell;
            float t = 0.0f;
            if (!degenerate)
                t = math::Clamp01(((px - startLocal.x) * segX + (pz - startLocal.z) * segZ) / segLenSq);
            const float qx = startLocal.x + segX * t;
            const float qz = startLocal.z + segZ * t;
            const float dist = std::sqrt((px - qx) * (px - qx) + (pz - qz) * (pz - qz));
            const float w = TerrainBrushWeight(brush, dist);
            if (w <= 0.0f) continue;

            const float target = math::Lerp(startLocal.y, endLocal.y, t) / safeMaxHeight;
            float& h = terrain.heightData[static_cast<size_t>(z) * static_cast<size_t>(terrain.columns)
                                        + static_cast<size_t>(x)];
            h = std::clamp(math::Lerp(h, target, strength * w), -1.0f, 1.0f);
        }
    }
}

bool ApplyTerrainHole(scene::TerrainComponent& terrain,
                      const math::Vector3&     hitLocal,
                      const TerrainBrush&      brush,
                      bool                     hole)
{
    if (terrain.columns < 2 || terrain.rows < 2 || terrain.cellSize <= 0.0f || brush.radius <= 0.0f)
        return false;

    const float cell = terrain.cellSize;
    const int cx0 = std::max(static_cast<int>(std::floor((hitLocal.x - brush.radius) / cell)), 0);
    const int cx1 = std::min(static_cast<int>(std::ceil ((hitLocal.x + brush.radius) / cell)), terrain.columns - 2);
    const int cz0 = std::max(static_cast<int>(std::floor((hitLocal.z - brush.radius) / cell)), 0);
    const int cz1 = std::min(static_cast<int>(std::ceil ((hitLocal.z + brush.radius) / cell)), terrain.rows - 2);
    const float radiusSq = brush.radius * brush.radius;

    bool changed = false;
    for (int cz = cz0; cz <= cz1; ++cz) {
        for (int cx = cx0; cx <= cx1; ++cx) {
            const float dx = (static_cast<float>(cx) + 0.5f) * cell - hitLocal.x;
            const float dz = (static_cast<float>(cz) + 0.5f) * cell - hitLocal.z;
            if (dx * dx + dz * dz > radiusSq) continue;
            if (terrain.IsHoleCell(cx, cz) == hole) continue;
            if (terrain.SetHoleCell(cx, cz, hole)) changed = true;
        }
    }
    return changed;
}

} // namespace fbzz::editor
