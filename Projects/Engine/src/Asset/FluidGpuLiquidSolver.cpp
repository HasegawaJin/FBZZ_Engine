/// @file    FluidGpuLiquidSolver.cpp
/// @brief   液体の粒子ソルバー (PBF) の GPU 版と、GPU へ上げる値を CPU で決める純関数 (FluidGpuLiquidPack.hpp)
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// 近傍探索は «(セル番号, 粒子番号) の bitonic sort + 並べた鍵の二分探索» で組む。
/// WHY 計数ソート (原子加算 → 累積和 → 振り分け) にしないか:
///   - セルの表が要らない。格子は h = 粒子半径 × 4 で切るので、半径 0.002 では 400 × 575 × 400 = 9200 万セルになり、
///     セルごとの開始位置の表とその累積和を持てない。並べる要素数は粒子数だけで、セル数に依らない
///   - 振り分けを原子加算で行うと同じセルの中の順番が実行ごとに変わり、近傍の和の丸めが揺れて焼くたびに絵が変わる。
///     (セル, 粒子番号) で並べれば全順序なので決定論的
///   - エンジンに bitonic sort の型が既にある (ParticleGpuSortStep / ParticleGpuSortLocal)。累積和の CS は無い
/// x 方向に隣り合う 3 セルは鍵が連続するので、27 セルは 9 本の連続した範囲になる。範囲は刻みの頭に 1 回だけ
/// 二分探索で求め (LiquidRanges)、密度拘束の反復と粘性で使い回す。
#include <Engine/Asset/FluidGpuLiquidSolver.hpp>

#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/FluidGpuLiquidPack.hpp>
#include <Engine/Asset/FluidOperatorEval.hpp>
#include <Engine/Asset/FluidSourceMask.hpp>
#include <Engine/Asset/FluidStepping.hpp>
#include <Engine/Core/CurlNoise.hpp>
#include <Engine/Renderer/ComputeCall.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/ResourceManager.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace fbzz::asset {
namespace {

constexpr float kPi = 3.14159265358979323846f;
// FluidLiquidSolver.cpp と同じ値。近傍格子が覆う範囲で、外へ出た粒子は捨てる。
constexpr float kBoundsMinX = -1.6f;
constexpr float kBoundsMaxX =  1.6f;
constexpr float kBoundsMinY = -1.6f;
constexpr float kBoundsMaxY =  3.0f;
constexpr float kBoundsMinZ = -1.6f;
constexpr float kBoundsMaxZ =  1.6f;
constexpr float kTensileStrength = 0.05f;
constexpr float kEmitPacking = 2.0f;
constexpr int kTextureEmitAttempts = 16;
// 湧かない枠 (粒子が 1 つも無いレシピでもバッファは 1 要素要る) の湧く時刻。
constexpr float kNeverSpawn = 3.0e38f;
// WHY 1 Tick の Dispatch 数に上限を置くか: DX12 は Dispatch 1 回ごとに SRV 32 + UAV 8 のディスクリプタを
//     フレームの動的ヒープ (65536) から取る。液体は 1 刻みで 90 回前後 Dispatch するので、1 Tick に 4 コマ × 8 刻みを
//     積むとヒープが尽き、同じフレームの描画まで束縛に失敗する。1024 回 (約 41000 枚) で打ち切り、残りは次の Tick へ回す。
constexpr int kDispatchBudgetPerTick = 1024;
constexpr std::uint32_t kParticleGroupSize = 64;
constexpr std::uint32_t kVoxelGroupSize = 4;
// ComputeCall::srvBuffers の添字 = HLSL のレジスタ番号 (LiquidCommon.hlsli の一覧と一致させること)。
constexpr std::size_t kSlotPositions = 14;
constexpr std::size_t kSlotSorted = 15;
constexpr std::size_t kSlotAux = 29;
constexpr std::size_t kSlotLambda = 30;

constexpr const char* kKernelPaths[] = {
    "Assets/Shaders/Bake/Fluid/LiquidClear.cs.hlsl",
    "Assets/Shaders/Bake/Fluid/LiquidPredict.cs.hlsl",
    "Assets/Shaders/Bake/Fluid/LiquidSortKeys.cs.hlsl",
    "Assets/Shaders/Bake/Fluid/LiquidSortStep.cs.hlsl",
    "Assets/Shaders/Bake/Fluid/LiquidSortLocal.cs.hlsl",
    "Assets/Shaders/Bake/Fluid/LiquidRanges.cs.hlsl",
    "Assets/Shaders/Bake/Fluid/LiquidLambda.cs.hlsl",
    "Assets/Shaders/Bake/Fluid/LiquidDelta.cs.hlsl",
    "Assets/Shaders/Bake/Fluid/LiquidVelocity.cs.hlsl",
    "Assets/Shaders/Bake/Fluid/LiquidViscosity.cs.hlsl",
    "Assets/Shaders/Bake/Fluid/LiquidSplat.cs.hlsl",
};

// LiquidCommon.hlsli の cbuffer LiquidStepConstants (b0) と 1:1。
struct alignas(16) LiquidStepConstants {
    float time;              float dt;             float gravity;       float cohesion;
    float viscosity;         float radius;         float h;             float h2;
    float poly6;             float spikyGradient;  float inverseRest;   float relaxation;
    float tensileReference;  float tensileScale;   float maxCorrection; float lifetime;
    std::uint32_t floorEnabled; float floorHeight; float floorKeep;     std::uint32_t particleCount;
    std::uint32_t cellsX;    std::uint32_t cellsY; std::uint32_t cellsZ; std::uint32_t sortCount;
    float boundsMin[3];                                                 float cellSize;
    float boundsMax[3];                                                 std::uint32_t forceCount;
    std::uint32_t colliderCount; std::uint32_t resolution; float splatRadius; float cellsPerUnit;
    FluidGpuForce forces[kMaxFluidGpuForces];
    FluidGpuCollider colliders[kMaxFluidGpuColliders];
};
static_assert(sizeof(LiquidStepConstants) == 144 + 48 * kMaxFluidGpuForces + 64 * kMaxFluidGpuColliders,
              "LiquidCommon.hlsli の LiquidStepConstants と一致させること");

// LiquidCommon.hlsli の cbuffer LiquidPassConstants (b1)。並べ替えの段 (k, j) と、鍵を作る位置の間隔。
struct alignas(16) LiquidPassConstants {
    std::uint32_t a;
    std::uint32_t b;
    std::uint32_t c;
    std::uint32_t d;
};

[[nodiscard]] float Saturate(float value) { return std::clamp(value, 0.0f, 1.0f); }

[[nodiscard]] math::Vector3 NormalizeOr(const math::Vector3& v, const math::Vector3& fallback)
{
    const float lengthSq = v.x * v.x + v.y * v.y + v.z * v.z;
    return lengthSq < 1.0e-12f ? fallback : v * (1.0f / std::sqrt(lengthSq));
}

// 以下 3 つは FluidLiquidSolver.cpp の同名関数の 3D の枝の写し。式を変えるときは両方を直す。
[[nodiscard]] float ShapeVolume(const FluidSource& source)
{
    const float sx = (std::max)(source.size.x, 0.0f);
    const float sy = (std::max)(source.size.y, 0.0f);
    const float sz = (std::max)(source.size.z, 0.0f);
    switch (source.shape) {
    case FluidSourceShape::Box:
    case FluidSourceShape::Texture: return 8.0f * sx * sy * sz;
    case FluidSourceShape::Cone:    return kPi * sx * sx * sy / 3.0f;
    case FluidSourceShape::Ring:    return 2.0f * kPi * kPi * sx * sy * sy;
    case FluidSourceShape::Cylinder: return 2.0f * kPi * sx * sx * sy;
    // 両端の半球を合わせると球 1 つ。
    case FluidSourceShape::Capsule: return 2.0f * kPi * sx * sx * sy + 4.0f / 3.0f * kPi * sx * sx * sx;
    case FluidSourceShape::Sphere:
    default:                        return 4.0f / 3.0f * kPi * sx * sx * sx;
    }
}

[[nodiscard]] float ShapeWidthAlong(const FluidSource& source, const math::Vector3& direction)
{
    const float sx = (std::max)(source.size.x, 0.0f);
    const float sy = (std::max)(source.size.y, 0.0f);
    const float sz = (std::max)(source.size.z, 0.0f);
    switch (source.shape) {
    case FluidSourceShape::Box:
        return 2.0f * (std::fabs(direction.x) * sx + std::fabs(direction.y) * sy + std::fabs(direction.z) * sz);
    case FluidSourceShape::Texture: {
        math::Vector3 right;
        math::Vector3 up;
        math::Vector3 normal;
        FluidTextureSourceBasis(source, right, up, normal);
        return 2.0f * (std::fabs(math::Vector3::Dot(direction, right)) * sx
                       + std::fabs(math::Vector3::Dot(direction, up)) * sy
                       + std::fabs(math::Vector3::Dot(direction, normal)) * sz);
    }
    case FluidSourceShape::Cone:
    case FluidSourceShape::Ring: {
        const math::Vector3 axis = NormalizeOr(source.direction, math::Vector3{ 0.0f, 1.0f, 0.0f });
        const float along = direction.x * axis.x + direction.y * axis.y + direction.z * axis.z;
        const float across = std::sqrt((std::max)(0.0f, 1.0f - along * along));
        if (source.shape == FluidSourceShape::Ring) return 2.0f * (sx * across + sy);
        return (std::max)(0.0f, sy * along + sx * across) + (std::max)(0.0f, -sy * along + sx * across);
    }
    case FluidSourceShape::Capsule:
    case FluidSourceShape::Cylinder: {
        const math::Vector3 axis = NormalizeOr(source.direction, math::Vector3{ 0.0f, 1.0f, 0.0f });
        const float along = direction.x * axis.x + direction.y * axis.y + direction.z * axis.z;
        if (source.shape == FluidSourceShape::Capsule) return 2.0f * (sy * std::fabs(along) + sx);
        const float across = std::sqrt((std::max)(0.0f, 1.0f - along * along));
        return 2.0f * (sy * std::fabs(along) + sx * across);
    }
    case FluidSourceShape::Sphere:
    default:
        return 2.0f * sx;
    }
}

// «同時に発生源の中に居る粒» が kEmitPacking まで詰めて収まるよう、形を中心から相似に広げる倍率。
[[nodiscard]] float FittedEmitScale(const FluidSource& source, const math::Vector3& launch, float particleRadius)
{
    const float count = static_cast<float>((std::max)(source.count, 0));
    const float speed = launch.Length();
    const math::Vector3 direction = speed > 1.0e-6f ? launch * (1.0f / speed) : math::Vector3{ 0.0f, 1.0f, 0.0f };
    const float width = ShapeWidthAlong(source, direction);
    const float travel = speed * source.duration;
    float inside = (source.duration <= 0.0f || travel <= width) ? count : count * width / travel;
    inside /= kEmitPacking;
    const float spacing = 2.0f * particleRadius;
    const float needed = inside * spacing * spacing * spacing;
    const float measure = ShapeVolume(source);
    if (measure <= 1.0e-12f || needed <= measure) return 1.0f;
    return std::cbrt(needed / measure);
}

// FluidLiquidSolver::Reset と同じ «有効な発生源を先頭から上限まで» の絞り込み。
[[nodiscard]] std::vector<FluidSource> PackedSources(const FluidRecipe& recipe)
{
    std::vector<FluidSource> sources;
    for (const FluidSource& source : recipe.sources)
        if (source.enabled && sources.size() < static_cast<std::size_t>(kMaxFluidSources)) sources.push_back(source);
    return sources;
}

} // namespace

// ─── FluidGpuLiquidPack.hpp ─────────────────────────────────────────────────

float GpuLiquidParticleRadius(const FluidLiquidSettings& settings)
{
    return std::clamp(settings.particleRadius, 0.002f, 0.1f);
}

GpuLiquidKernel MakeGpuLiquidKernel(float particleRadius)
{
    GpuLiquidKernel k;
    k.radius = std::clamp(particleRadius, 0.002f, 0.1f);
    k.h = k.radius * 4.0f;
    const float h2 = k.h * k.h;
    k.poly6 = 315.0f / (64.0f * kPi * std::pow(k.h, 9.0f));
    k.spikyGradient = -45.0f / (kPi * std::pow(k.h, 6.0f));
    const auto poly6 = [&](float distanceSquared) {
        if (distanceSquared >= h2) return 0.0f;
        const float d = h2 - distanceSquared;
        return k.poly6 * d * d * d;
    };
    const auto spikyScale = [&](float distance) {
        if (distance <= 1.0e-7f || distance >= k.h) return 0.0f;
        const float d = k.h - distance;
        return k.spikyGradient * d * d / distance;
    };

    // 静止密度は «粒子が直径の間隔で並んだ状態» で測る (FluidLiquidSolver::Reset と同じ演算順)。
    const float spacing = 2.0f * k.radius;
    const int reach = static_cast<int>(std::ceil(k.h / spacing));
    float density = 0.0f;
    float gradientSquared = 0.0f;
    for (int kz = -reach; kz <= reach; ++kz) {
        for (int j = -reach; j <= reach; ++j) {
            for (int i = -reach; i <= reach; ++i) {
                const float dx = static_cast<float>(i) * spacing;
                const float dy = static_cast<float>(j) * spacing;
                const float dz = static_cast<float>(kz) * spacing;
                const float r2 = dx * dx + dy * dy + dz * dz;
                density += poly6(r2);
                if (i == 0 && j == 0 && kz == 0) continue;
                const float scale = spikyScale(std::sqrt(r2));
                gradientSquared += (scale * dx) * (scale * dx) + (scale * dy) * (scale * dy)
                                 + (scale * dz) * (scale * dz);
            }
        }
    }
    k.restDensity = (std::max)(density, 1.0e-6f);
    const float restGradientSum = (std::max)(gradientSquared / (k.restDensity * k.restDensity), 1.0e-12f);
    k.relaxation = 0.01f * restGradientSum;
    k.tensileReference = (std::max)(poly6((0.2f * k.h) * (0.2f * k.h)), 1.0e-12f);
    k.tensileScale = kTensileStrength / restGradientSum;
    return k;
}

GpuLiquidGrid MakeGpuLiquidGrid(float particleRadius)
{
    GpuLiquidGrid grid;
    grid.cellSize = std::clamp(particleRadius, 0.002f, 0.1f) * 4.0f;
    grid.cellsX = (std::max)(1, static_cast<int>(std::ceil((kBoundsMaxX - kBoundsMinX) / grid.cellSize)));
    grid.cellsY = (std::max)(1, static_cast<int>(std::ceil((kBoundsMaxY - kBoundsMinY) / grid.cellSize)));
    grid.cellsZ = (std::max)(1, static_cast<int>(std::ceil((kBoundsMaxZ - kBoundsMinZ) / grid.cellSize)));
    return grid;
}

int GpuLiquidParticleCapacity(const FluidRecipe& recipe, int limit)
{
    long long total = 0;
    for (const FluidSource& source : PackedSources(recipe)) total += (std::max)(source.count, 0);
    const long long capacity = (std::min)({ static_cast<long long>((std::max)(recipe.liquid.maxParticles, 1)),
                                            static_cast<long long>((std::max)(limit, 0)), total });
    return static_cast<int>(capacity);
}

std::vector<GpuLiquidSpawn> BuildGpuLiquidEmission(const FluidRecipe& recipe, int limit)
{
    std::vector<GpuLiquidSpawn> spawns;
    const int capacity = GpuLiquidParticleCapacity(recipe, limit);
    if (capacity <= 0) return spawns;

    const float radius = GpuLiquidParticleRadius(recipe.liquid);
    std::vector<FluidSource> sources = PackedSources(recipe);
    // WHY 粒子半径まで広げるか: FluidLiquidSolver::Reset と同じ。0 の軸が残ると形の体積が 0 になり、
    //     FittedEmitScale が詰まりすぎを広げられずに全粒を 1 点へ出してしまう。
    for (FluidSource& source : sources) {
        source.size.x = (std::max)(source.size.x, radius);
        source.size.y = (std::max)(source.size.y, radius);
        source.size.z = (std::max)(source.size.z, radius);
    }
    std::vector<FluidSourceMask> masks(sources.size());
    for (std::size_t i = 0; i < sources.size(); ++i) {
        if (sources[i].shape != FluidSourceShape::Texture || sources[i].texture.empty()) continue;
        if (!LoadFluidSourceMask(sources[i].texture, masks[i])) masks[i].values.clear();
    }

    struct Event {
        float time;
        std::uint32_t source;
    };
    std::vector<Event> events;
    for (std::size_t e = 0; e < sources.size(); ++e) {
        const FluidSource& source = sources[e];
        const int count = (std::max)(source.count, 0);
        for (int k = 0; k < count; ++k) {
            const float time = source.duration <= 0.0f
                ? source.startTime
                : source.startTime + source.duration * (static_cast<float>(k + 1) / static_cast<float>(count));
            events.push_back({ time, static_cast<std::uint32_t>(e) });
        }
    }
    // CPU は枠が尽きたら後から湧く粒を出さない。先に湧く粒から枠を渡す (同時刻は発生源の順を保つ)。
    std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) { return a.time < b.time; });
    events.resize(static_cast<std::size_t>(capacity));

    std::uint32_t rng = core::PcgHash(recipe.seed * 747796405u + 2891336453u) | 1u;
    const auto next = [&rng]() {
        rng = rng * 1664525u + 1013904223u;
        return static_cast<float>(rng >> 8) * (1.0f / 16777216.0f);
    };

    spawns.reserve(events.size());
    for (const Event& event : events) {
        const FluidSource& source = sources[event.source];
        const FluidOperatorPose pose = PoseFluidSource(source, event.time);
        const math::Vector3 launch = source.velocity + pose.motionVelocity;
        const float speed = launch.Length();
        const float scale = FittedEmitScale(source, launch, radius);
        const float jitterScale = Saturate(source.spread) * speed;

        math::Vector3 point = pose.center;
        bool placed = false;
        if (source.shape == FluidSourceShape::Texture) {
            for (int attempt = 0; attempt < kTextureEmitAttempts && !placed; ++attempt) {
                const float u0 = next();
                const float u1 = next();
                const float u2 = next();
                const float accept = next();
                placed = SampleFluidTextureSourcePoint(source, pose.center, masks[event.source], u0, u1, u2, accept,
                                                       /*volumetric=*/true, point);
            }
        }
        if (!placed) {
            const float u0 = next();
            const float u1 = next();
            const float u2 = next();
            point = SampleFluidSourcePoint(source, pose.center, u0, u1, u2, /*volumetric=*/true);
        }
        // ばらつきの向きは球面上で一様にする (CPU と同じ引き方)。
        const float cz = next() * 2.0f - 1.0f;
        const float angle = next() * 2.0f * kPi;
        const float ring = std::sqrt((std::max)(0.0f, 1.0f - cz * cz));
        const float jitter = next() * jitterScale;

        GpuLiquidSpawn spawn{};
        spawn.positionTime[0] = pose.center.x + (point.x - pose.center.x) * scale;
        spawn.positionTime[1] = pose.center.y + (point.y - pose.center.y) * scale;
        spawn.positionTime[2] = pose.center.z + (point.z - pose.center.z) * scale;
        spawn.positionTime[3] = event.time;
        spawn.velocityKey[0] = launch.x + ring * std::cos(angle) * jitter;
        spawn.velocityKey[1] = launch.y + ring * std::sin(angle) * jitter;
        spawn.velocityKey[2] = launch.z + cz * jitter;
        spawn.velocityKey[3] = source.colorKey;
        spawns.push_back(spawn);
    }
    return spawns;
}

int PackGpuLiquidForces(const FluidRecipe& recipe, float time, FluidGpuForce (&out)[kMaxFluidGpuForces])
{
    int count = 0;
    for (const FluidForce& force : recipe.forces) {
        if (!force.enabled) continue;
        if (count >= kMaxFluidGpuForces) break;
        FluidGpuForce& f = out[count++];
        const FluidOperatorPose pose = PoseFluidForce(force, time);
        const math::Vector3 direction = NormalizeOr(force.direction, math::Vector3{ 1.0f, 0.0f, 0.0f });
        f.centerType[0] = pose.center.x;
        f.centerType[1] = pose.center.y;
        f.centerType[2] = pose.center.z;
        f.centerType[3] = static_cast<float>(force.type);
        f.directionStrength[0] = direction.x;
        f.directionStrength[1] = direction.y;
        f.directionStrength[2] = direction.z;
        // 量のエンベロープは強さへ畳む (CPU の FluidForceDelta と掛ける順を揃える)。
        f.directionStrength[3] =
            FluidForceActive(force, time) ? force.strength * FluidForceAmount(force, time) : 0.0f;
        f.params[0] = force.radius;
        f.params[1] = force.falloffPower;
        f.params[2] = force.noiseFrequency;
        f.params[3] = force.noiseSpeed;
    }
    for (int i = count; i < kMaxFluidGpuForces; ++i) out[i] = FluidGpuForce{};
    return count;
}

int PackGpuLiquidColliders(const FluidRecipe& recipe, float time, float dt,
                           FluidGpuCollider (&out)[kMaxFluidGpuColliders])
{
    int count = 0;
    for (const FluidCollider& collider : recipe.colliders) {
        if (!collider.enabled) continue;
        if (count >= kMaxFluidGpuColliders) break;
        FluidGpuCollider& c = out[count++];
        const FluidOperatorPose pose = PoseFluidCollider(collider, time);
        const bool sphere = collider.shape == FluidColliderShape::Sphere;
        const float sizeX = (std::max)(collider.size.x, 0.0f);
        c.centerShape[0] = pose.center.x;
        c.centerShape[1] = pose.center.y;
        c.centerShape[2] = pose.center.z;
        c.centerShape[3] = static_cast<float>(collider.shape);
        c.sizeActive[0] = sizeX;
        c.sizeActive[1] = sphere ? sizeX : (std::max)(collider.size.y, 0.0f);
        c.sizeActive[2] = sphere ? sizeX : (std::max)(collider.size.z, 0.0f);
        c.sizeActive[3] = FluidColliderActive(collider, time) ? 1.0f : 0.0f;
        const math::Vector3 normal = NormalizeOr(collider.direction, math::Vector3{ 0.0f, 1.0f, 0.0f });
        c.normal[0] = normal.x;
        c.normal[1] = normal.y;
        c.normal[2] = normal.z;
        c.normal[3] = 0.0f;
        c.velocity[0] = pose.motionVelocity.x;
        c.velocity[1] = pose.motionVelocity.y;
        c.velocity[2] = pose.motionVelocity.z;
        c.velocity[3] = std::exp(-(std::max)(collider.friction, 0.0f) * 10.0f * dt);
    }
    for (int i = count; i < kMaxFluidGpuColliders; ++i) out[i] = FluidGpuCollider{};
    return count;
}

std::uint32_t GpuLiquidSortCount(int particleCount)
{
    std::uint32_t count = kGpuLiquidSortBlock;
    while (count < static_cast<std::uint32_t>((std::max)(particleCount, 0))) count <<= 1;
    return count;
}

std::vector<GpuLiquidSortStage> BuildGpuLiquidSortStages(std::uint32_t sortCount)
{
    std::vector<GpuLiquidSortStage> stages;
    for (std::uint32_t k = 2u; k <= sortCount; k <<= 1) {
        for (std::uint32_t j = k >> 1; j > 0u; j >>= 1) {
            if (j <= kGpuLiquidSortBlock / 2u) {
                // 比較の相手が 1 グループ内に収まったら、残る段は全部グループ共有メモリで回す。
                stages.push_back({ k, j, true });
                break;
            }
            stages.push_back({ k, j, false });
        }
    }
    return stages;
}

// ─── FluidGpuLiquidSolver ─────────────────────────────────────────────────────

struct FluidGpuLiquidSolver::Impl {
    using TextureHandle = renderer::ResourceHandle<renderer::TextureTag>;
    using ConstantHandle = renderer::ResourceHandle<renderer::ConstantBufferTag>;
    using BufferHandle = renderer::ResourceHandle<renderer::StructuredBufferTag>;

    enum Kernel : std::size_t {
        Clear, Predict, SortKeys, SortStep, SortLocal, Ranges, Lambda, Delta, Velocity, Viscosity, Splat, KernelCount
    };
    std::array<renderer::ResourceHandle<renderer::ShaderTag>, KernelCount> kernels{};

    /// 湧かせ方 (float4 × 2 / 粒子)。Initialize で一度だけ上げる。
    BufferHandle spawn;
    /// 粒子の状態 (float4 × 2 / 粒子)。粘性は近傍の速度を読むので 2 本を交互に使う。
    std::array<BufferHandle, 2> state{};
    /// 予測位置 (float4 / 粒子。w = 1 なら生きている)。密度拘束の反復で交互に使う。
    std::array<BufferHandle, 2> predicted{};
    BufferHandle lambda;
    /// (セル番号, 粒子番号)。sortCount 要素。
    BufferHandle sortBuffer;
    /// 粒子ごとの近傍 9 行ぶんの並べ替え済み範囲 [lo, hi)。
    BufferHandle rangeBuffer;

    /// 刻みごとの定数 (FluidGpuSolver と同じ理由で刻みの数だけ別に持つ)。
    std::vector<ConstantHandle> stepConstants;
    std::size_t nextStepConstant = 0;
    ConstantHandle outputConstants;
    /// 鍵を作る位置の読み方 (予測位置 = 間隔 1 / 状態の位置 = 間隔 2)。中身は作ったときから変えない。
    ConstantHandle keysFromPredicted;
    ConstantHandle keysFromState;
    /// 並べ替えの段ごとの (k, j)。フレーム内で書き直さないよう段の数だけ持つ。
    std::vector<ConstantHandle> sortStageConstants;
    std::vector<GpuLiquidSortStage> sortStages;
    std::uint32_t sortStagesFor = 0;

    FluidRecipe recipe;
    GpuLiquidKernel kernel;
    GpuLiquidGrid grid;
    int resolution = 0;
    float frameDt = 1.0f / 24.0f;
    int substeps = 1;
    int iterations = 4;
    float splatRadius = 0.03f;
    int particleCount = 0;
    std::uint32_t sortCount = 0;
    float time = 0.0f;
    int warmupFrames = 0;
    int steppedFrames = 0;
    std::size_t currentState = 0;
    int dispatchesThisTick = 0;
    bool needsClear = true;
    bool ready = false;
    std::uint64_t resetVersion = 0;

    [[nodiscard]] bool Owned(const renderer::ResourceManager& resources) const
    {
        return resetVersion == resources.GetResetVersion();
    }

    // デバイスリセット後の古いハンドルは実体ごと消えている。返さずに忘れる。
    void ForgetHandles()
    {
        kernels = {};
        spawn = {};
        state = {};
        predicted = {};
        lambda = {};
        sortBuffer = {};
        rangeBuffer = {};
        stepConstants.clear();
        outputConstants = {};
        keysFromPredicted = {};
        keysFromState = {};
        sortStageConstants.clear();
        sortStagesFor = 0;
    }

    void ReleaseBuffers(renderer::ResourceManager& resources)
    {
        const bool owned = Owned(resources);
        const auto release = [&](BufferHandle& buffer) {
            if (owned && buffer.IsValid()) resources.Release(buffer);
            buffer = {};
        };
        release(spawn);
        for (BufferHandle& buffer : state) release(buffer);
        for (BufferHandle& buffer : predicted) release(buffer);
        release(lambda);
        release(sortBuffer);
        release(rangeBuffer);
    }

    void ReleaseSortStageConstants(renderer::ResourceManager& resources)
    {
        if (Owned(resources))
            for (const ConstantHandle& constants : sortStageConstants)
                if (constants.IsValid()) resources.Release(constants);
        sortStageConstants.clear();
        sortStagesFor = 0;
    }

    void ReleaseConstants(renderer::ResourceManager& resources)
    {
        if (Owned(resources)) {
            for (const ConstantHandle& constants : stepConstants)
                if (constants.IsValid()) resources.Release(constants);
            for (ConstantHandle* constants : { &outputConstants, &keysFromPredicted, &keysFromState })
                if (constants->IsValid()) resources.Release(*constants);
        }
        stepConstants.clear();
        outputConstants = {};
        keysFromPredicted = {};
        keysFromState = {};
        ReleaseSortStageConstants(resources);
    }

    [[nodiscard]] LiquidStepConstants Pack(float stepTime, float dt) const
    {
        const FluidLiquidSettings& liquid = recipe.liquid;
        LiquidStepConstants c{};
        c.time = stepTime;
        c.dt = dt;
        c.gravity = liquid.gravity;
        c.cohesion = Saturate(liquid.cohesion);
        c.viscosity = Saturate(liquid.viscosity);
        c.radius = kernel.radius;
        c.h = kernel.h;
        c.h2 = kernel.h * kernel.h;
        c.poly6 = kernel.poly6;
        c.spikyGradient = kernel.spikyGradient;
        c.inverseRest = 1.0f / kernel.restDensity;
        c.relaxation = kernel.relaxation;
        c.tensileReference = kernel.tensileReference;
        c.tensileScale = kernel.tensileScale;
        // 1 反復の補正を粒子半径の半分までに抑える (CPU と同じ)。
        c.maxCorrection = kernel.radius * 0.5f;
        c.lifetime = liquid.particleLifetime;
        c.floorEnabled = liquid.floor ? 1u : 0u;
        c.floorHeight = liquid.floorHeight;
        c.floorKeep = std::exp(-(std::max)(liquid.floorFriction, 0.0f) * 10.0f * dt);
        c.particleCount = static_cast<std::uint32_t>(particleCount);
        c.cellsX = static_cast<std::uint32_t>(grid.cellsX);
        c.cellsY = static_cast<std::uint32_t>(grid.cellsY);
        c.cellsZ = static_cast<std::uint32_t>(grid.cellsZ);
        c.sortCount = sortCount;
        for (int axis = 0; axis < 3; ++axis) {
            c.boundsMin[axis] = grid.boundsMin[axis];
            c.boundsMax[axis] = grid.boundsMax[axis];
        }
        c.cellSize = grid.cellSize;
        c.forceCount = static_cast<std::uint32_t>(PackGpuLiquidForces(recipe, stepTime, c.forces));
        c.colliderCount = static_cast<std::uint32_t>(PackGpuLiquidColliders(recipe, stepTime, dt, c.colliders));
        c.resolution = static_cast<std::uint32_t>(resolution);
        c.splatRadius = splatRadius;
        c.cellsPerUnit = static_cast<float>(resolution) * 0.5f;
        return c;
    }

    [[nodiscard]] int DispatchesPerFrame() const
    {
        const int perStep = 5 + static_cast<int>(sortStages.size()) + 2 * iterations;
        return substeps * perStep;
    }

    [[nodiscard]] renderer::ComputeCall Call(Kernel k, ConstantHandle b0, std::uint32_t groups,
                                             ConstantHandle b1 = {}) const
    {
        renderer::ComputeCall call;
        call.shader = kernels[k];
        call.constantBuffers[0] = b0;
        call.constantBuffers[1] = b1;
        call.dispatchX = (std::max)(groups, 1u);
        return call;
    }

    [[nodiscard]] std::uint32_t ParticleGroups() const
    {
        return (static_cast<std::uint32_t>(particleCount) + kParticleGroupSize - 1u) / kParticleGroupSize;
    }

    void Run(renderer::IRenderer& renderer, renderer::ResourceManager& resources, const renderer::ComputeCall& call)
    {
        renderer.Dispatch(call, resources);
        ++dispatchesThisTick;
    }

    void ClearState(renderer::IRenderer& renderer, renderer::ResourceManager& resources, ConstantHandle constants)
    {
        // 作ったばかりの RW バッファの中身は未定義。全粒子を «まだ湧いていない» (0) から始める。
        const std::uint32_t elements = static_cast<std::uint32_t>(particleCount) * 2u;
        renderer::ComputeCall clear = Call(Clear, constants, (elements + kParticleGroupSize - 1u) / kParticleGroupSize);
        clear.uavBuffers[0] = state[currentState];
        Run(renderer, resources, clear);
        needsClear = false;
    }

    void BuildSort(renderer::IRenderer& renderer, renderer::ResourceManager& resources, ConstantHandle constants,
                   ConstantHandle keyMode, BufferHandle positions)
    {
        const std::uint32_t groups = sortCount / kGpuLiquidSortBlock;
        renderer::ComputeCall keys = Call(SortKeys, constants, groups, keyMode);
        keys.srvBuffers[kSlotPositions] = positions;
        keys.uavBuffers[1] = sortBuffer; // u3
        Run(renderer, resources, keys);
        for (std::size_t i = 0; i < sortStages.size(); ++i) {
            renderer::ComputeCall stage =
                Call(sortStages[i].local ? SortLocal : SortStep, constants, groups, sortStageConstants[i]);
            stage.uavBuffers[1] = sortBuffer;
            Run(renderer, resources, stage);
        }
    }

    void Step(renderer::IRenderer& renderer, renderer::ResourceManager& resources, float dt)
    {
        const ConstantHandle constants = stepConstants[nextStepConstant++];
        const LiquidStepConstants packed = Pack(time, dt);
        resources.Update(constants, &packed, sizeof(packed));
        if (needsClear) ClearState(renderer, resources, constants);

        const BufferHandle current = state[currentState];
        const BufferHandle next = state[1 - currentState];
        const std::uint32_t groups = ParticleGroups();

        // 湧かせる → 寿命 → 重力・外力 → 位置の予測。
        renderer::ComputeCall predict = Call(Predict, constants, groups);
        predict.srvBuffers[kSlotAux] = spawn;
        predict.uavBuffers[0] = current;       // u2
        predict.uavBuffers[1] = predicted[0];  // u3
        Run(renderer, resources, predict);

        // 近傍は反復の前に 1 回だけ組む (CPU と同じ)。
        BuildSort(renderer, resources, constants, keysFromPredicted, predicted[0]);
        renderer::ComputeCall ranges = Call(Ranges, constants, groups);
        ranges.srvBuffers[kSlotPositions] = predicted[0];
        ranges.srvBuffers[kSlotSorted] = sortBuffer;
        ranges.uavBuffers[0] = rangeBuffer;
        Run(renderer, resources, ranges);

        std::size_t solved = 0;
        for (int iteration = 0; iteration < iterations; ++iteration) {
            renderer::ComputeCall lambdaCall = Call(Lambda, constants, groups);
            lambdaCall.srvBuffers[kSlotPositions] = predicted[solved];
            lambdaCall.srvBuffers[kSlotSorted] = sortBuffer;
            lambdaCall.srvBuffers[kSlotAux] = rangeBuffer;
            lambdaCall.uavBuffers[0] = lambda;
            Run(renderer, resources, lambdaCall);

            // 全粒子の補正を読んでから動かす (CPU の «全員分を求めてから足す» と同じ)。書き先は別の予測位置。
            renderer::ComputeCall delta = Call(Delta, constants, groups);
            delta.srvBuffers[kSlotPositions] = predicted[solved];
            delta.srvBuffers[kSlotSorted] = sortBuffer;
            delta.srvBuffers[kSlotAux] = rangeBuffer;
            delta.srvBuffers[kSlotLambda] = lambda;
            delta.uavBuffers[0] = predicted[1 - solved];
            Run(renderer, resources, delta);
            solved = 1 - solved;
        }

        renderer::ComputeCall velocity = Call(Velocity, constants, groups);
        velocity.srvBuffers[kSlotPositions] = predicted[solved];
        velocity.uavBuffers[0] = current;
        Run(renderer, resources, velocity);

        // 粘性 + 年齢 + 領域外。近傍の «更新後の» 速度を読むので書き先は別の状態バッファ。
        renderer::ComputeCall viscosity = Call(Viscosity, constants, groups);
        viscosity.srvBuffers[kSlotPositions] = current;
        viscosity.srvBuffers[kSlotSorted] = sortBuffer;
        viscosity.srvBuffers[kSlotAux] = rangeBuffer;
        viscosity.uavBuffers[0] = next;
        Run(renderer, resources, viscosity);

        currentState = 1 - currentState;
        time += dt;
    }
};

FluidGpuLiquidSolver::FluidGpuLiquidSolver()
    : m_impl(std::make_unique<Impl>())
{
}

FluidGpuLiquidSolver::~FluidGpuLiquidSolver() = default;

bool FluidGpuLiquidSolver::Initialize(renderer::ResourceManager& resources, const FluidRecipe& recipe, int resolution,
                                      float frameDt, float radiusScale, std::string& outError)
{
    Impl& s = *m_impl;
    if (recipe.kind != FluidKind::Liquid) {
        outError = "GPU の粒子ソルバーで解けるのは液体 (kind = liquid) のレシピだけです";
        return false;
    }
    if (s.resetVersion != resources.GetResetVersion()) {
        s.ForgetHandles();
        s.resetVersion = resources.GetResetVersion();
    }

    static_assert(sizeof(kKernelPaths) / sizeof(kKernelPaths[0]) == Impl::KernelCount,
                  "kKernelPaths は Kernel の並びと 1:1");
    for (std::size_t i = 0; i < Impl::KernelCount; ++i) {
        if (!s.kernels[i].IsValid())
            s.kernels[i] = resources.LoadShader(AssetManager::ResolveAssetPath(kKernelPaths[i]));
        if (!s.kernels[i].IsValid()) {
            outError = std::string("液体の Compute シェーダーを読み込めません: ") + kKernelPaths[i];
            s.ready = false;
            return false;
        }
    }

    // 粒子の枠と湧かせ方はレシピで決まるので、同じ大きさでも作り直す。
    s.ReleaseBuffers(resources);
    std::vector<GpuLiquidSpawn> spawns = BuildGpuLiquidEmission(recipe, kMaxParticles);
    if (spawns.empty()) {
        GpuLiquidSpawn never{};
        never.positionTime[3] = kNeverSpawn;
        spawns.push_back(never);
    }
    s.particleCount = static_cast<int>(spawns.size());
    s.sortCount = GpuLiquidSortCount(s.particleCount);
    const auto count = static_cast<std::uint32_t>(s.particleCount);
    constexpr auto kFloat4 = static_cast<std::uint32_t>(sizeof(float) * 4u);
    s.spawn = resources.CreateRWStructuredBuffer(spawns.data(), count * 2u, kFloat4);
    for (auto& buffer : s.state) buffer = resources.CreateRWStructuredBuffer(nullptr, count * 2u, kFloat4);
    for (auto& buffer : s.predicted) buffer = resources.CreateRWStructuredBuffer(nullptr, count, kFloat4);
    s.lambda = resources.CreateRWStructuredBuffer(nullptr, count, static_cast<std::uint32_t>(sizeof(float)));
    s.sortBuffer = resources.CreateRWStructuredBuffer(nullptr, s.sortCount,
                                                      static_cast<std::uint32_t>(sizeof(std::uint32_t) * 2u));
    s.rangeBuffer = resources.CreateRWStructuredBuffer(nullptr, count * 9u,
                                                       static_cast<std::uint32_t>(sizeof(std::uint32_t) * 2u));
    bool ok = s.spawn.IsValid() && s.lambda.IsValid() && s.sortBuffer.IsValid() && s.rangeBuffer.IsValid();
    for (const auto& buffer : s.state) ok &= buffer.IsValid();
    for (const auto& buffer : s.predicted) ok &= buffer.IsValid();
    if (!ok) {
        s.ReleaseBuffers(resources);
        outError = "液体の粒子バッファを作れません (このバックエンドは未対応か、VRAM が足りません)";
        s.ready = false;
        return false;
    }

    const std::size_t constantCount = static_cast<std::size_t>(kMaxFramesPerTick) * kMaxSubsteps;
    while (s.stepConstants.size() < constantCount)
        s.stepConstants.push_back(resources.CreateConstantBuffer(sizeof(LiquidStepConstants)));
    if (!s.outputConstants.IsValid()) s.outputConstants = resources.CreateConstantBuffer(sizeof(LiquidStepConstants));
    const auto makePass = [&](const LiquidPassConstants& value) {
        const Impl::ConstantHandle handle = resources.CreateConstantBuffer(sizeof(LiquidPassConstants));
        if (handle.IsValid()) resources.Update(handle, &value, sizeof(value));
        return handle;
    };
    if (!s.keysFromPredicted.IsValid()) s.keysFromPredicted = makePass({ 1u, 0u, 0u, 0u });
    if (!s.keysFromState.IsValid()) s.keysFromState = makePass({ 2u, 0u, 0u, 0u });
    if (s.sortStagesFor != s.sortCount) {
        s.ReleaseSortStageConstants(resources);
        s.sortStages = BuildGpuLiquidSortStages(s.sortCount);
        for (const GpuLiquidSortStage& stage : s.sortStages)
            s.sortStageConstants.push_back(makePass({ stage.k, stage.j, 0u, 0u }));
        s.sortStagesFor = s.sortCount;
    }

    s.recipe = recipe;
    s.kernel = MakeGpuLiquidKernel(recipe.liquid.particleRadius);
    s.grid = MakeGpuLiquidGrid(recipe.liquid.particleRadius);
    s.resolution = std::clamp(resolution, 1, 256);
    s.frameDt = (std::max)(frameDt, 1.0e-4f);
    s.substeps = std::clamp(recipe.output.substeps, 1, kMaxSubsteps);
    s.iterations = std::clamp(recipe.liquid.solverIterations, 1, 20);
    s.splatRadius = s.kernel.radius * (std::max)(radiusScale, 0.5f);
    s.warmupFrames = FluidWarmupFrames(recipe.output.warmup, s.frameDt);
    s.ready = true;
    Restart();
    return true;
}

void FluidGpuLiquidSolver::Release(renderer::ResourceManager& resources)
{
    Impl& s = *m_impl;
    s.ReleaseBuffers(resources);
    s.ReleaseConstants(resources);
    s.kernels = {};
    s.ready = false;
}

bool FluidGpuLiquidSolver::IsReady() const { return m_impl->ready; }
int FluidGpuLiquidSolver::Resolution() const { return m_impl->resolution; }
int FluidGpuLiquidSolver::SolvedFrame() const { return m_impl->steppedFrames - m_impl->warmupFrames - 1; }

void FluidGpuLiquidSolver::Restart()
{
    Impl& s = *m_impl;
    s.time = 0.0f;
    s.steppedFrames = 0;
    s.needsClear = true;
    s.currentState = 0;
}

void FluidGpuLiquidSolver::BeginTick()
{
    m_impl->nextStepConstant = 0;
    m_impl->dispatchesThisTick = 0;
}

bool FluidGpuLiquidSolver::StepFrame(renderer::IRenderer& renderer, renderer::ResourceManager& resources)
{
    Impl& s = *m_impl;
    if (!s.ready || s.nextStepConstant + static_cast<std::size_t>(s.substeps) > s.stepConstants.size())
        return false;
    // 1 コマ目は予算を超えても進める (でないと刻みの多いレシピが永遠に進まない)。
    if (s.dispatchesThisTick > 0 && s.dispatchesThisTick + s.DispatchesPerFrame() > kDispatchBudgetPerTick)
        return false;
    const float stepDt = s.frameDt / static_cast<float>(s.substeps);
    for (int step = 0; step < s.substeps; ++step) s.Step(renderer, resources, stepDt);
    ++s.steppedFrames;
    return true;
}

void FluidGpuLiquidSolver::WriteVolumes(renderer::IRenderer& renderer, renderer::ResourceManager& resources,
                                        renderer::ResourceHandle<renderer::TextureTag> medium,
                                        renderer::ResourceHandle<renderer::TextureTag> velocity)
{
    Impl& s = *m_impl;
    if (!s.ready || !medium.IsValid() || !velocity.IsValid()) return;
    const LiquidStepConstants packed = s.Pack(s.time, 0.0f);
    resources.Update(s.outputConstants, &packed, sizeof(packed));
    if (s.needsClear) s.ClearState(renderer, resources, s.outputConstants);

    // 格子は最後の刻みの «解く前の» 予測位置で組んである。解いた後の位置で組み直してから塗る。
    const Impl::BufferHandle current = s.state[s.currentState];
    s.BuildSort(renderer, resources, s.outputConstants, s.keysFromState, current);

    const std::uint32_t groups = (static_cast<std::uint32_t>(s.resolution) + kVoxelGroupSize - 1u) / kVoxelGroupSize;
    renderer::ComputeCall splat = s.Call(Impl::Splat, s.outputConstants, groups);
    splat.dispatchY = splat.dispatchZ = groups;
    splat.srvBuffers[kSlotPositions] = current;
    splat.srvBuffers[kSlotSorted] = s.sortBuffer;
    splat.srvBuffers[kSlotAux] = s.spawn;
    splat.uavOutputs[0] = medium;
    splat.uavOutputs[1] = velocity;
    s.Run(renderer, resources, splat);
}

} // namespace fbzz::asset
