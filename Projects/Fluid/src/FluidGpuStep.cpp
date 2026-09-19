/// @file    FluidGpuStep.cpp
/// @brief   GPU 流体ソルバーの刻みの定数を詰める
/// @author  Hasegawa Jin
/// @date    2026-09-11
#include <Fluid/FluidGpuStep.hpp>

#include <Fluid/FluidOperatorEval.hpp>
#include <Math/CurlNoise.hpp>

#include <algorithm>
#include <cmath>

namespace fbzz::fluid {
namespace {

/// PackFluidGpuStep と FluidGpuMaskPaths の «どの発生源を詰めるか» を 1 か所に置く。
/// タイル番号はこの順で数えるので、両者で絞り込みがずれるとマスクが別の発生源に貼られる。
template <typename Visit>
void ForEachPackedSource(const FluidRecipe& recipe, Visit&& visit)
{
    int packed = 0;
    for (const FluidSource& source : recipe.sources) {
        if (!source.enabled) continue;
        if (packed >= kMaxFluidGpuSources) break;
        visit(source, packed++);
    }
}

} // namespace

math::Vector3 FluidNoiseOffset(std::uint32_t seed)
{
    const std::uint32_t hash = math::PcgHash(seed * 2654435761u + 1u);
    return { static_cast<float>(hash % 997u) * 0.731f, static_cast<float>((hash >> 10) % 997u) * 0.517f,
             static_cast<float>((hash >> 20) % 997u) * 0.379f };
}

FluidGpuStepConstants PackFluidGpuStep(const FluidRecipe& recipe, int resolution, float time, float dt,
                                       float densityScale, float temperatureScale)
{
    const FluidGasSettings& gas = recipe.gas;
    FluidGpuStepConstants c{};
    c.resolution = static_cast<std::uint32_t>((std::max)(resolution, 1));
    c.cellSize = 2.0f / static_cast<float>(c.resolution);
    c.dt = (std::max)(dt, 0.0f);
    c.time = time;
    c.buoyancy = gas.buoyancy;
    c.weight = gas.weight;
    c.vorticity = (std::max)(gas.vorticity, 0.0f);
    c.turbulence = gas.turbulence;
    c.turbulenceScale = (std::max)(gas.turbulenceScale, 0.01f);
    c.densityKeep = std::exp(-(std::max)(gas.densityDissipation, 0.0f) * c.dt);
    c.temperatureKeep = std::exp(-(std::max)(gas.temperatureDissipation, 0.0f) * c.dt);
    c.velocityKeep = std::exp(-(std::max)(gas.velocityDamping, 0.0f) * c.dt);
    c.ignitionTemperature = gas.ignitionTemperature;
    c.burnFraction = std::clamp((std::max)(gas.burnRate, 0.0f) * c.dt, 0.0f, 1.0f);
    c.burnHeat = gas.burnHeat;
    c.burnSmoke = gas.burnSmoke;
    c.burnExpansion = gas.burnExpansion;
    c.floor = gas.floor ? 1u : 0u;
    c.sharp = gas.sharpAdvection ? 1u : 0u;
    c.wind[0] = gas.wind.x;
    c.wind[1] = gas.wind.y;
    c.wind[2] = gas.wind.z;
    c.densityScale = densityScale;
    c.temperatureScale = temperatureScale;
    const math::Vector3 offset = FluidNoiseOffset(recipe.seed);
    c.noiseOffset[0] = offset.x;
    c.noiseOffset[1] = offset.y;
    c.noiseOffset[2] = offset.z;

    int sourceCount = 0;
    int textureTiles = 0;
    ForEachPackedSource(recipe, [&](const FluidSource& source, int index) {
        sourceCount = index + 1;
        FluidGpuSource& out = c.sources[index];
        const FluidOperatorPose pose = PoseFluidSource(source, time);
        const bool sphere = source.shape == FluidSourceShape::Sphere;
        /// @note FluidSourceWeight の minSize と同じく、1 セルより小さい寸法はセル幅まで広げる (でないと何も入らない)。
        const float sizeX = (std::max)(source.size.x, c.cellSize);
        out.centerShape[0] = pose.center.x;
        out.centerShape[1] = pose.center.y;
        out.centerShape[2] = pose.center.z;
        out.centerShape[3] = static_cast<float>(source.shape);
        out.sizeNoise[0] = sizeX;
        out.sizeNoise[1] = sphere ? sizeX : (std::max)(source.size.y, c.cellSize);
        out.sizeNoise[2] = sphere ? sizeX : (std::max)(source.size.z, c.cellSize);
        out.sizeNoise[3] = source.noise;
        /// @note 量のエンベロープはここで畳む。シェーダーは «基準の量 × 倍率» を受け取るだけなので、
        ///       定数バッファの形も FluidInject.cs.hlsl も変わらない (動きの中心と同じ流儀)。
        const float amount = FluidSourceAmount(source, time);
        out.amounts[0] = source.density * amount;
        out.amounts[1] = source.temperature * amount;
        out.amounts[2] = source.fuel * amount;
        out.amounts[3] = FluidSourceEmitting(source, time) ? 1.0f : 0.0f;
        const math::Vector3 target = source.velocity + pose.motionVelocity;
        out.velocity[0] = target.x;
        out.velocity[1] = target.y;
        out.velocity[2] = target.z;
        const bool drives = target.x != 0.0f || target.y != 0.0f || target.z != 0.0f;
        out.velocity[3] = drives ? 1.0f : 0.0f;
        math::Vector3 axis = source.direction.NormalizedOr({ 0.0f, 1.0f, 0.0f });
        float tile = -1.0f;
        if (source.shape == FluidSourceShape::Texture) {
            /// @note Texture は長さ 0 の向きを «手前向き» に倒す (Cone / Ring の上向きとは違う)。規則は Basis が正本。
            math::Vector3 right;
            math::Vector3 up;
            FluidTextureSourceBasis(source, right, up, axis);
            tile = static_cast<float>(textureTiles++);
        }
        out.axis[0] = axis.x;
        out.axis[1] = axis.y;
        out.axis[2] = axis.z;
        out.axis[3] = tile;
        out.extra[0] = source.colorKey;
        out.extra[1] = 0.0f;
        out.extra[2] = 0.0f;
        out.extra[3] = 0.0f;
    });
    c.sourceCount = static_cast<std::uint32_t>(sourceCount);

    int forceCount = 0;
    for (const FluidForce& force : recipe.forces) {
        if (!force.enabled) continue;
        if (forceCount >= kMaxFluidGpuForces) break;
        FluidGpuForce& out = c.forces[forceCount++];
        const FluidOperatorPose pose = PoseFluidForce(force, time);
        out.centerType[0] = pose.center.x;
        out.centerType[1] = pose.center.y;
        out.centerType[2] = pose.center.z;
        out.centerType[3] = static_cast<float>(force.type);
        const math::Vector3 direction = force.direction.NormalizedOr({ 1.0f, 0.0f, 0.0f });
        out.directionStrength[0] = direction.x;
        out.directionStrength[1] = direction.y;
        out.directionStrength[2] = direction.z;
        /// @note 効いていない刻みは強さ 0 で送る (Drag も 1 − exp(0) = 0 で素通りになる)。
        ///       量のエンベロープは強さへ畳む。シェーダーはこの後 influence を掛けるので、CPU 側
        ///       (FluidForceDelta の strengthScale) と掛ける順が揃う。
        out.directionStrength[3] =
            FluidForceActive(force, time) ? force.strength * FluidForceAmount(force, time) : 0.0f;
        out.params[0] = force.radius;
        out.params[1] = force.falloffPower;
        out.params[2] = force.noiseFrequency;
        out.params[3] = force.noiseSpeed;
    }
    c.forceCount = static_cast<std::uint32_t>(forceCount);

    int colliderCount = 0;
    for (const FluidCollider& collider : recipe.colliders) {
        if (!collider.enabled) continue;
        if (colliderCount >= kMaxFluidGpuColliders) break;
        FluidGpuCollider& out = c.colliders[colliderCount++];
        const FluidOperatorPose pose = PoseFluidCollider(collider, time);
        const bool sphere = collider.shape == FluidColliderShape::Sphere;
        /// @note FluidColliderDistance の minSize と同じ広げ方。1 セルより細い障害物はどのセル中心も覆えず、素通りになる。
        const float sizeX = (std::max)(collider.size.x, c.cellSize);
        out.centerShape[0] = pose.center.x;
        out.centerShape[1] = pose.center.y;
        out.centerShape[2] = pose.center.z;
        out.centerShape[3] = static_cast<float>(collider.shape);
        out.sizeActive[0] = sizeX;
        out.sizeActive[1] = sphere ? sizeX : (std::max)(collider.size.y, c.cellSize);
        out.sizeActive[2] = sphere ? sizeX : (std::max)(collider.size.z, c.cellSize);
        out.sizeActive[3] = FluidColliderActive(collider, time) ? 1.0f : 0.0f;
        const math::Vector3 normal = collider.direction.NormalizedOr({ 0.0f, 1.0f, 0.0f });
        out.normal[0] = normal.x;
        out.normal[1] = normal.y;
        out.normal[2] = normal.z;
        out.normal[3] = 0.0f;
        const math::Vector3 velocity =
            collider.motion.inheritVelocity ? pose.motionVelocity : math::Vector3{ 0.0f, 0.0f, 0.0f };
        out.velocity[0] = velocity.x;
        out.velocity[1] = velocity.y;
        out.velocity[2] = velocity.z;
        out.velocity[3] = 0.0f;
    }
    c.colliderCount = static_cast<std::uint32_t>(colliderCount);
    return c;
}

std::vector<std::string> FluidGpuMaskPaths(const FluidRecipe& recipe)
{
    std::vector<std::string> paths;
    ForEachPackedSource(recipe, [&](const FluidSource& source, int) {
        if (source.shape == FluidSourceShape::Texture) paths.push_back(source.texture);
    });
    return paths;
}

} // namespace fbzz::fluid
