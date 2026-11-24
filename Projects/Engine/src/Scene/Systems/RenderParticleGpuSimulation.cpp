/// @file    RenderParticleGpuSimulation.cpp
/// @brief   粒子 GPU シミュレーションのバッファ・スポーン・定数の準備。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include "RenderParticleExtraction.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include "ParticleSimulationHelpers.hpp"
#include "RenderPasses/Geometry/FlowFieldGpu.hpp"
#include "RenderPasses/Geometry/GeometryPasses.hpp"
#include <Engine/Scene/Systems/ParticleSimulationRuntime.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/ParticleEmitterSpace.hpp>
#include <Engine/Scene/Fields/FlowFieldFrame.hpp>
#include <Engine/Scene/Fields/FlowField.hpp>
#include <Engine/Scene/SceneRenderResources.hpp>
#include <Engine/Asset/VelocityFieldAtlas.hpp>
#include <Fluid/VectorFieldAsset.hpp>
#include <Engine/Core/Time.hpp>
#include <Graphics/Renderer/DynamicBufferPool.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace fbzz::scene::particle_extraction_detail {

namespace {

using namespace particle_simulation_detail;

/// @brief GPU シミュレーションへ渡す力場とスポーンの StructuredBuffer。
/// @note エミッターに 1 本ずつ持たせず、シミュレーションのたびにプールから借りる。DX12 の
/// @note 読み取り専用 StructuredBuffer は Upload ヒープへの直 memcpy のため、1 本を毎フレーム
/// @note 書き直すと GPU がまだ読んでいる前フレームの内容を上書きする (詳細は `DynamicBufferPool.hpp`)。
renderer::DynamicStructuredBufferPool s_forceBuffers;
renderer::DynamicStructuredBufferPool s_spawnBuffers;

/// @brief 力場バッファを借りて今フレームの内容を書き込む。
void AcquireGpuForceBuffer(ParticleEmitter& emitter,
                           const std::vector<GpuFlowField>& forces,
                           renderer::ResourceManager& resources)
{
    /// @note 0 本でも 1 要素は借りる。要素数 0 の StructuredBuffer は作れず、
    /// @note 未束縛の SRV は DX12 で null ディスクリプタの次元不一致になる。
    const size_t required = (std::max)(forces.size(), size_t{ 1 });
    emitter.runtime.gpuForceBuffer =
        s_forceBuffers.Acquire(resources, required, static_cast<uint32_t>(sizeof(GpuFlowField)));
    if (!forces.empty() && emitter.runtime.gpuForceBuffer.IsValid()) {
        resources.Update(emitter.runtime.gpuForceBuffer, forces.data(),
                         forces.size() * sizeof(GpuFlowField));
    }
}

void PrepareGpuParticleStorage(ParticleEmitter& emitter, int particleCapacity, renderer::ResourceManager& resources)
{
    const bool invalidResources = emitter.runtime.gpuInitialized
        && (emitter.runtime.gpuResetVersion != resources.GetResetVersion()
            || !resources.Get(emitter.runtime.gpuParticleBuffer) || !resources.Get(emitter.runtime.gpuEmitterCB));
    if (emitter.runtime.gpuClearPending || invalidResources
        || (emitter.runtime.gpuInitialized && emitter.runtime.gpuCapacity != static_cast<uint32_t>(particleCapacity))) {
        ReleaseParticleSimulationGpuResources(emitter);
        emitter.runtime.gpuClearPending = false;
    }

    /// @note バッファ未作成なら初期化 (要素ゼロで確保し CS が age>=lifetime で無視する)
    if (!emitter.runtime.gpuInitialized)
    {
        emitter.runtime.flowMinimumAge = 0.0f;
        emitter.runtime.flowMaxLifetime = 0.0f;
        emitter.runtime.flowBoundsRadius = 0.0f;
        emitter.runtime.flowSpeedBound = 0.0f;
        std::vector<GpuParticle> initialParticles(static_cast<size_t>(particleCapacity));
        /// @note 全粒子を「死亡済み」で初期化
        for (auto& particle : initialParticles) particle.age = particle.lifetime = 1.0f;
        emitter.runtime.gpuParticleBuffer = resources.CreateRWStructuredBuffer(
            initialParticles.data(), static_cast<uint32_t>(particleCapacity), sizeof(GpuParticle));

        emitter.runtime.gpuEmitterCB = resources.CreateConstantBuffer(sizeof(GpuParticleEmitterCB));
        emitter.runtime.gpuWriteHead     = 0;
        emitter.runtime.gpuSpawnCount    = 0;
        emitter.runtime.gpuResetVersion  = resources.GetResetVersion();
        emitter.runtime.gpuCapacity      = static_cast<uint32_t>(particleCapacity);
        emitter.runtime.gpuInitialized   = true;
    }
}

void StageGpuParticleSpawns(ParticleEmitter& emitter, const Transform& transform, const AnimatorComponent* animator,
    bool canEmit, float deltaTime, int particleCapacity, renderer::ResourceManager& resources)
{
    /// @note SubEmitter からの «発火元つき» スポーンは GPU 経路では出せない (InitGpuSpawnEntry は
    /// @note 位置と速度の上書きを持たない)。捨てないと、GPU のエミッターを SubEmitter に指した
    /// @note 構成で古い発火位置が溜まり続ける。
    emitter.runtime.injectedSpawns.clear();
    /// @note 今フレームのスポーンエントリを構築
    std::vector<GpuSpawnEntry> spawns;
    if (canEmit || emitter.runtime.burstPending > 0)
    {
        const int burstCount = (std::max)(emitter.runtime.burstPending, 0);
        emitter.runtime.burstPending = 0;
        for (int burstIndex = 0; burstIndex < burstCount && static_cast<int>(spawns.size()) < particleCapacity; ++burstIndex)
        {
            GpuSpawnEntry spawn;
            InitGpuSpawnEntry(spawn, emitter, transform, animator);
            spawns.push_back(spawn);
        }
        if (canEmit)
        {
            /// @note lodRateScale は CPU 経路と同じく発生レートへ掛ける。掛け忘れると
            /// @note 遠距離のエミッターが GPU のときだけ間引かれない。
            emitter.runtime.emitAccum += emitter.settings.emitRate * emitter.runtime.lodRateScale
                * EmitRateCurveScale(emitter) * deltaTime;
            while (emitter.runtime.emitAccum >= 1.0f && static_cast<int>(spawns.size()) < particleCapacity)
            {
                emitter.runtime.emitAccum -= 1.0f;
                GpuSpawnEntry spawn;
                InitGpuSpawnEntry(spawn, emitter, transform, animator);
                spawns.push_back(spawn);
            }
        }
    }
    else
    {
        emitter.runtime.emitAccum = 0.0f;
    }

    /// @note 前回の移動上限で膨らませた球を引き継ぎ、今回の実スポーンを追加する。
    if (emitter.runtime.flowMinimumAge >= emitter.runtime.flowMaxLifetime) {
        emitter.runtime.flowBoundsCenter = TransformEmitterPoint(transform, emitter.settings.emitPosition);
        emitter.runtime.flowBoundsRadius = 0.0f;
        emitter.runtime.flowSpeedBound = 0.0f;
        emitter.runtime.flowMinimumAge = 0.0f;
        emitter.runtime.flowMaxLifetime = 0.0f;
    }
    for (const auto& spawn : spawns) {
        emitter.runtime.flowBoundsRadius = (std::max)(emitter.runtime.flowBoundsRadius,
            (spawn.position - emitter.runtime.flowBoundsCenter).Length());
        emitter.runtime.flowSpeedBound = (std::max)(emitter.runtime.flowSpeedBound, spawn.velocity.Length());
        emitter.runtime.flowMinimumAge = 0.0f;
        emitter.runtime.flowMaxLifetime = (std::max)(emitter.runtime.flowMaxLifetime, spawn.lifetime);
    }
    emitter.runtime.gpuSpawnCount = static_cast<uint32_t>(spawns.size());

    /// @note スポーンバッファを借りて CPU → GPU 転送する。0 件でも 1 要素は借りる (t15 を空にしない)。
    emitter.runtime.gpuSpawnBuffer = s_spawnBuffers.Acquire(
        resources, (std::max)(spawns.size(), size_t{ 1 }), static_cast<uint32_t>(sizeof(GpuSpawnEntry)));
    if (!emitter.runtime.gpuSpawnBuffer.IsValid())
        emitter.runtime.gpuSpawnCount = 0;
    if (emitter.runtime.gpuSpawnCount > 0)
        resources.Update(emitter.runtime.gpuSpawnBuffer, spawns.data(),
                         emitter.runtime.gpuSpawnCount * sizeof(GpuSpawnEntry));
}

renderer::GpuParticleEmitterCB BuildGpuSimulationConstants(ParticleEmitter& emitter, const Transform& transform,
    float deltaTime, float time, int particleCapacity, const std::vector<ActiveFlowField>& flowFields, RenderPassContext& context)
{
    auto& resources = context.resources;
    /// @note CS 用定数バッファ更新
    GpuParticleEmitterCB constants{};
    constants.emitterPos      = TransformEmitterPoint(transform, emitter.settings.emitPosition);
    constants.deltaTime       = deltaTime;
    /// @note 乱流・周回・放射は専用スロットではなく gFlowFields へ入る。
    /// @note 重力と結合係数は «媒質の運動ではない» のでエミッターの値をそのまま渡す。
    constants.gravity         = emitter.settings.gravity;
    constants.maxParticles    = static_cast<uint32_t>(particleCapacity);
    constants.colorStart      = emitter.settings.colorStart;
    constants.colorEnd        = emitter.settings.colorEnd;
    constants.spawnCount      = emitter.runtime.gpuSpawnCount;
    constants.spawnOffset     = emitter.runtime.gpuWriteHead;
    constants.colorCurvePower = emitter.settings.colorCurvePower;
    constants.flowCoupling    = emitter.settings.flowCoupling;
    constants.sizeStart       = emitter.settings.sizeStart;
    constants.sizeEnd         = emitter.settings.sizeEnd;
    constants.sizeCurvePower  = emitter.settings.sizeCurvePower;
    {
        /// @note 丸めは CPU 経路・Inspector プレビューと同じ関数で行う (end = 0 を «最後まで» と読む等)。
        const asset::FlipbookFrameRange range =
            asset::ResolveFlipbookRange(emitter.runtime.material.flipbook);
        constants.spriteColumns    = static_cast<uint32_t>(range.columns);
        constants.spriteRows       = static_cast<uint32_t>(range.rows);
        constants.spriteStartFrame = static_cast<uint32_t>(range.first);
        constants.spriteEndFrame   = static_cast<uint32_t>(range.last);
    }
    constants.time           = time;
    /// @note 乱流は gParticleForces 側へ
    constants.noiseStrength  = 0.0f;
    constants.noiseFrequency = 0.5f;
    constants.noiseSpeed     = 1.0f;
    constants.flipbookMode = static_cast<uint32_t>(emitter.runtime.material.flipbook.flipbookMode);
    constants.flipbookFramesPerSecond = (std::max)(emitter.runtime.material.flipbook.flipbookFramesPerSecond, 0.0f);

    /// @note 内蔵の流れもシーンの場も同じ StructuredBuffer へ詰める。CS 側は届いた分を全部
    /// @note 合流させればよい。本数の上限は無いので «場が捨てられて粒子が流れない» は起きない。
    std::vector<ActiveFlowField> emitterForces;
    ResolveEmitterForces(emitter, transform, flowFields, emitterForces, true);
    float flowSpeedBound = 0.0f;
    for (const auto& field : emitterForces) flowSpeedBound += FlowSpeedBound(field);
    /// @note 緩和は凸結合なので、重力後の速さと合成流速の大きい方が次の速さの上限になる。
    emitter.runtime.flowSpeedBound = (std::max)(
        emitter.runtime.flowSpeedBound + emitter.settings.gravity.Length() * std::abs(deltaTime), flowSpeedBound) * 1.00001f;
    /// @note 任意の速度カーブの上限が未確定ならカリングを無効化して粒子を取りこぼさない。
    if (emitter.settings.useVelocityCurve)
        emitter.runtime.flowBoundsRadius = std::numeric_limits<float>::infinity();
    else
        emitter.runtime.flowBoundsRadius += emitter.runtime.flowSpeedBound * std::abs(deltaTime) + 1.0e-4f;
    /// @note GPU と同じ加算で最小年齢を進める。残り寿命の減算では丸めが違い、早く球を捨て得る。
    emitter.runtime.flowMinimumAge += deltaTime;

    std::vector<GpuFlowField> gpuForces;
    gpuForces.reserve(emitterForces.size());
    for (const ActiveFlowField& f : emitterForces) gpuForces.push_back(PackGpuFlowField(f, context.resources));
    constants.flowFieldCount = static_cast<uint32_t>(gpuForces.size());
    AcquireGpuForceBuffer(emitter, gpuForces, resources);
    constants.curveFlags = {
        emitter.settings.useSizeCurve ? 1.0f : 0.0f,
        emitter.settings.useVelocityCurve ? 1.0f : 0.0f,
        emitter.settings.useColorGradient ? 1.0f : 0.0f,
        emitter.runtime.material.flipbook.flipbookFrameBlending ? 1.0f : 0.0f
    };
    /// @note カーブは float4 1 本へ 2 キー (time,value) ずつ詰める。有効キー数を超えた分は
    /// @note 最終キーで埋め、GPU 側が余分な区間を踏んでも CPU の Evaluate と同じ値になるようにする。
    const auto packCurve = [](const ParticleCurve& curve, size_t first) {
        const size_t last = static_cast<size_t>(
            (std::max)(1u, (std::min)(curve.keyCount, kMaxParticleCurveKeys)) - 1u);
        const auto& firstKey = curve.keys[(std::min)(first, last)];
        const auto& secondKey = curve.keys[(std::min)(first + 1u, last)];
        return math::Vector4{
            firstKey.time, firstKey.value, secondKey.time, secondKey.value
        };
    };
    const auto curveKeyCount = [](const ParticleCurve& curve) {
        return static_cast<float>(
            (std::max)(1u, (std::min)(curve.keyCount, kMaxParticleCurveKeys)));
    };
    constants.sizeCurveKeys01 = packCurve(emitter.settings.sizeCurve, 0);
    constants.sizeCurveKeys23 = packCurve(emitter.settings.sizeCurve, 2);
    constants.sizeCurveKeys45 = packCurve(emitter.settings.sizeCurve, 4);
    constants.sizeCurveKeys67 = packCurve(emitter.settings.sizeCurve, 6);
    constants.velocityCurveKeys01 = packCurve(emitter.settings.velocityCurve, 0);
    constants.velocityCurveKeys23 = packCurve(emitter.settings.velocityCurve, 2);
    constants.velocityCurveKeys45 = packCurve(emitter.settings.velocityCurve, 4);
    constants.velocityCurveKeys67 = packCurve(emitter.settings.velocityCurve, 6);
    /// @note 黒体モードを焼き込んだ実効グラデーションを渡す。CPU 経路も同じ runtimeGradient を見る。
    const auto& gradient = emitter.runtime.runtimeGradient;
    const size_t lastGradientKey = static_cast<size_t>(
        (std::max)(1u, (std::min)(gradient.keyCount, kMaxParticleCurveKeys)) - 1u);
    const auto gradientTime = [&](size_t index) {
        return gradient.keys[(std::min)(index, lastGradientKey)].time;
    };
    constants.gradientTimes  = { gradientTime(0), gradientTime(1), gradientTime(2), gradientTime(3) };
    constants.gradientTimes47 = { gradientTime(4), gradientTime(5), gradientTime(6), gradientTime(7) };
    for (size_t index = 0; index < 4; ++index) {
        constants.gradientColors[index] = gradient.keys[(std::min)(index, lastGradientKey)].color;
        constants.gradientColors47[index] =
            gradient.keys[(std::min)(index + 4, lastGradientKey)].color;
    }
    /// @note z = 補間色空間 (ParticleColorSpace)。EvaluateGradient8 がこれを見て CPU と同じ空間で混ぜる。
    constants.gradientMeta = { static_cast<float>(lastGradientKey + 1),
                        static_cast<float>(gradient.interpolation),
                        static_cast<float>(gradient.colorSpace), 0.0f };
    /// @note 深度コリジョンは深度バッファを screen space で引くので、それを焼いたのと同じ
    /// @note (TAA ジッター込みの) 行列で射影しないと当たり位置が半ピクセルずれる。
    constants.viewProjection = MakeJitteredViewProjection(context.camera, context.taaJitterNdcX, context.taaJitterNdcY);
    constants.screenWidth = static_cast<float>(context.width);
    constants.screenHeight = static_cast<float>(context.height);
    constants.depthThickness = (std::max)(0.001f, emitter.settings.collisionRadius * 0.002f);
    constants.depthBounciness = std::clamp(emitter.settings.collisionBounciness, 0.0f, 1.0f);
    constants.depthCollision = emitter.settings.collisionMode == ParticleCollisionMode::Depth ? 1u : 0u;
    constants.depthResponse = static_cast<std::uint32_t>(emitter.settings.collisionResponse);
    constants.depthDamping = std::clamp(emitter.settings.collisionDamping, 0.0f, 1.0f);
    /// @note over-lifetime モジュール (回転カーブ / drag カーブ / 周回・放射)
    constants.curveFlags2 = {
        emitter.settings.useRotationCurve ? 1.0f : 0.0f,
        emitter.settings.useDragCurve ? 1.0f : 0.0f,
        0.0f, 0.0f
    };
    constants.rotationCurveKeys01 = packCurve(emitter.settings.rotationCurve, 0);
    constants.rotationCurveKeys23 = packCurve(emitter.settings.rotationCurve, 2);
    constants.rotationCurveKeys45 = packCurve(emitter.settings.rotationCurve, 4);
    constants.rotationCurveKeys67 = packCurve(emitter.settings.rotationCurve, 6);
    constants.dragCurveKeys01 = packCurve(emitter.settings.dragCurve, 0);
    constants.dragCurveKeys23 = packCurve(emitter.settings.dragCurve, 2);
    constants.dragCurveKeys45 = packCurve(emitter.settings.dragCurve, 4);
    constants.dragCurveKeys67 = packCurve(emitter.settings.dragCurve, 6);
    /// @note 4 本のカーブの有効キー数と補間モード。GPU 側はこれを見て CPU と同じ区間を選ぶ。
    constants.curveKeyCounts = {
        curveKeyCount(emitter.settings.sizeCurve), curveKeyCount(emitter.settings.velocityCurve),
        curveKeyCount(emitter.settings.rotationCurve), curveKeyCount(emitter.settings.dragCurve)
    };
    constants.curveModes = {
        static_cast<float>(emitter.settings.sizeCurve.interpolation),
        static_cast<float>(emitter.settings.velocityCurve.interpolation),
        static_cast<float>(emitter.settings.rotationCurve.interpolation),
        static_cast<float>(emitter.settings.dragCurve.interpolation)
    };
    /// @note 周回と放射は gParticleForces (Vortex / Source) 側へ移った。CB の orbitalAxis /
    /// @note orbitalVelocity / radialVelocity は誰も書かない死んだ枠で、cb{} のゼロ初期化のまま
    /// @note GPU へ渡る (書いていた 0 と同じ値)。フィールド自体は残す ─ この CB は
    /// @note ParticleGpuSim.cs.hlsl と 1 バイト単位で対で、途中を削ると以降の全オフセットがずれる。
    constants.spriteRandomFlags = (emitter.runtime.material.flipbook.spriteRandomStartFrame ? 1u : 0u)
        | (emitter.runtime.material.flipbook.spriteRandomRow ? 2u : 0u);

    return constants;
}

void PrepareGpuParticleSort(ParticleEmitter& emitter, const renderer::RenderPassHandles& handles,
    int particleCapacity, renderer::ResourceManager& resources)
{
    if (ShouldSortGpuParticles(emitter, handles)) {
        uint32_t sortCapacity = kParticleSortBlock;
        while (sortCapacity < static_cast<uint32_t>(particleCapacity)) sortCapacity <<= 1;
        if (!emitter.runtime.gpuSortBuffer.IsValid() || emitter.runtime.gpuSortCapacity != sortCapacity) {
            if (emitter.runtime.gpuSortBuffer.IsValid()) resources.Release(emitter.runtime.gpuSortBuffer);
            /// @note 1 要素 = uint2 (key, particleIndex)。
            emitter.runtime.gpuSortBuffer = resources.CreateRWStructuredBuffer(
                nullptr, sortCapacity, static_cast<std::uint32_t>(sizeof(std::uint32_t) * 2u));
            emitter.runtime.gpuSortCapacity = sortCapacity;
        }
        if (!emitter.runtime.gpuSortCB.IsValid())
            emitter.runtime.gpuSortCB = resources.CreateConstantBuffer(sizeof(GpuParticleSortCB));
    } else if (emitter.runtime.gpuSortBuffer.IsValid()) {
        resources.Release(emitter.runtime.gpuSortBuffer);
        emitter.runtime.gpuSortBuffer = {}; emitter.runtime.gpuSortCapacity = 0;
    }
}

} /// @note namespace

bool ShouldSortGpuParticles(const ParticleEmitter& emitter, const renderer::RenderPassHandles& handles)
{
    return emitter.settings.sortMode != ParticleSortMode::None
        && handles.particleGpuSortKeysCS.IsValid()
        && handles.particleGpuSortStepCS.IsValid()
        && handles.particleGpuSortLocalCS.IsValid();
}

void PrepareGpuEmitter(ParticleEmitter& emitter, const Transform& transform, const AnimatorComponent* animator,
    float deltaTime, float time, bool canEmit, const std::vector<ActiveFlowField>& flowFields,
    const MeshRenderer* meshParticleRenderer, RenderPassContext& context, renderer::RenderParticleInput& input)
{
    auto& resources = context.resources;
    auto& handles         = context.handles;

    if (!handles.particleGpuSimCS.IsValid() || !handles.particleGpuShader.IsValid()) return;

    const int particleCapacity = (std::max)(emitter.settings.maxParticles, 1);

    PrepareGpuParticleStorage(emitter, particleCapacity, resources);

    const bool simulateThisFrame = emitter.runtime.lastGpuSimulationFrame != Time::frameCount;
    if (simulateThisFrame) {
        emitter.runtime.lastGpuSimulationFrame = Time::frameCount;
        StageGpuParticleSpawns(emitter, transform, animator, canEmit, deltaTime, particleCapacity, resources);
        input.simulation = BuildGpuSimulationConstants(emitter, transform, deltaTime, time, particleCapacity, flowFields, context);
        input.simulate = true;
        emitter.runtime.gpuWriteHead = (emitter.runtime.gpuWriteHead + emitter.runtime.gpuSpawnCount)
            % static_cast<uint32_t>(particleCapacity);
    }
    input.runtime.gpuParticleBuffer = emitter.runtime.gpuParticleBuffer;
    if (input.simulate) {
        input.runtime.gpuEmitterCB = emitter.runtime.gpuEmitterCB;
        input.runtime.gpuSpawnBuffer = emitter.runtime.gpuSpawnBuffer;
        input.runtime.gpuForceBuffer = emitter.runtime.gpuForceBuffer;
        input.velocityField = asset::VelocityFieldAtlas::Texture(resources);
    }
    /// @note 予算のないビューでもシミュレーションを公開する。描画専用のソートとメッシュは用意しない。
    if (input.drawCount == 0) return;
    PrepareGpuParticleSort(emitter, handles, particleCapacity, resources);
    if (meshParticleRenderer && meshParticleRenderer->enabled && meshParticleRenderer->mesh && !meshParticleRenderer->mesh->isSkinned) {
        const auto& mesh = *meshParticleRenderer->mesh;
        input.meshVertices = mesh.vertexBuffer; input.meshIndices = mesh.indexBuffer;
        input.meshIndexCount = mesh.indexCount; input.meshVertexCount = mesh.vertexCount;
    }
}

} /// @note namespace fbzz::scene::particle_extraction_detail
