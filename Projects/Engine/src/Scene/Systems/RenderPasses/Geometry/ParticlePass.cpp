// FBZZ Engine
// RenderPasses/ParticlePass.cpp | fbzz::scene
// パーティクル更新 (CPU) と描画
#include "GeometryPasses.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Transform.hpp"
#include "Engine/Scene/Components/ParticleEmitter.hpp"
#include "Engine/Asset/AssetManager.hpp"
#include "Engine/Asset/MaterialAsset.hpp"
#include "Engine/Core/Time.hpp"
#include "Engine/Renderer/ComputeCall.hpp"
#include "Engine/Renderer/DrawCall.hpp"
#include "Engine/Renderer/RenderState.hpp"
#include <Math/Vector4.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace fbzz::scene {

namespace {

inline math::Vector4 LerpVec4(const math::Vector4& a, const math::Vector4& b, float t)
{
    return { a.x + (b.x - a.x) * t,
             a.y + (b.y - a.y) * t,
             a.z + (b.z - a.z) * t,
             a.w + (b.w - a.w) * t };
}

float Clamp01(float value)
{
    return (std::max)(0.0f, (std::min)(value, 1.0f));
}

uint32_t NextParticleRandom(ParticleEmitter& emitter)
{
    // WHAT: Numerical Recipes 系 LCG。軽量で、エミッターごとの seed から決定的な乱数列を作る。
    // WHY: std::rand() はグローバル状態のため、複数エミッターや再生順序で結果が変わりやすい。
    if (emitter.randomState == 0) {
        emitter.randomState = emitter.randomSeed != 0 ? emitter.randomSeed : 1;
    }
    emitter.randomState = emitter.randomState * 1664525u + 1013904223u;
    return emitter.randomState;
}

float RandomSigned01(ParticleEmitter& emitter)
{
    constexpr float INV_MAX_UINT = 1.0f / 4294967295.0f;
    return static_cast<float>(NextParticleRandom(emitter)) * INV_MAX_UINT * 2.0f - 1.0f;
}

float Random01(ParticleEmitter& emitter)
{
    return Clamp01(RandomSigned01(emitter) * 0.5f + 0.5f);
}

math::Vector3 RandomUnitVector(ParticleEmitter& emitter)
{
    constexpr float PI = 3.14159265358979323846f;
    const float z = RandomSigned01(emitter);
    const float a = Random01(emitter) * PI * 2.0f;
    const float r = std::sqrt((std::max)(0.0f, 1.0f - z * z));
    return { r * std::cos(a), z, r * std::sin(a) };
}

math::Vector4 ComputeSpriteRect(const ParticleEmitter& emitter, float normalizedAge)
{
    const int columns = (std::max)(emitter.spriteColumns, 1);
    const int rows = (std::max)(emitter.spriteRows, 1);
    const int frameCount = columns * rows;
    const int startFrame = std::clamp(emitter.spriteStartFrame, 0, frameCount - 1);
    const int endFrame = std::clamp(
        emitter.spriteEndFrame > 0 ? emitter.spriteEndFrame : frameCount - 1,
        startFrame,
        frameCount - 1);
    const int span = (std::max)(endFrame - startFrame, 0);
    const int frame = startFrame + static_cast<int>(Clamp01(normalizedAge) * static_cast<float>(span));
    const int x = frame % columns;
    const int y = frame / columns;
    const float invColumns = 1.0f / static_cast<float>(columns);
    const float invRows = 1.0f / static_cast<float>(rows);
    return {
        static_cast<float>(x) * invColumns,
        static_cast<float>(y) * invRows,
        static_cast<float>(x + 1) * invColumns,
        static_cast<float>(y + 1) * invRows
    };
}

math::Vector3 TransformEmitterPoint(const Transform& transform, const math::Vector3& localPoint)
{
    // WHY: Transform::position は親基準のローカル座標であり、子 GameObject に Emitter を置くと
    //      親 Player / Bone の移動が反映されない。Particle は描画時点のワールド空間で保持するため、
    //      worldPosition/worldRotation/worldScale から発生点を解決する。
    const math::Vector3 scaledLocal = {
        localPoint.x * transform.worldScale.x,
        localPoint.y * transform.worldScale.y,
        localPoint.z * transform.worldScale.z
    };
    return transform.worldPosition + transform.worldRotation * scaledLocal;
}

math::Vector3 TransformEmitterVector(const Transform& transform, const math::Vector3& localVector)
{
    // WHAT: 速度は位置ではないため平行移動を含めず、Emitter のワールド回転だけを適用する。
    return transform.worldRotation * localVector;
}

renderer::ResourceHandle<renderer::TextureTag> LoadParticleTextureOrWhite(
    renderer::ResourceManager& resources,
    const std::string& texturePath)
{
    // WHY: Particle は色カーブだけでも成立する VFX なので、参照先テクスチャの欠落で
    //      DrawCall 全体を無効化せず、白テクスチャにフォールバックして色だけは表示する。
    if (!texturePath.empty()) {
        auto texture = resources.LoadTexture(texturePath);
        if (texture.IsValid())
            return texture;
    }

    static const uint8_t white[4] = { 255, 255, 255, 255 };
    return resources.CreateTexture(white, 1, 1);
}

void EnsureParticleTexture(ParticleEmitter& emitter, renderer::ResourceManager& resources)
{
    // materialPath が設定されている場合: .mat の albedo テクスチャと blendMode を優先する。
    // WHY: materialPath が単一の描画設定の信頼元になることで、複数エミッターで同じ .mat を共有できる。
    if (!emitter.materialPath.empty()) {
        const bool matChanged = (emitter.loadedMaterialPath != emitter.materialPath);
        if (matChanged) {
            emitter.loadedMaterialPath = emitter.materialPath;
            emitter.loadedTexturePath.clear(); // .mat の変更でテクスチャも再ロードさせる
        }
        const auto matHandle = asset::AssetManager::LoadMaterial(emitter.materialPath);
        if (const auto* mat = asset::AssetManager::GetMaterial(matHandle)) {
            const auto it = mat->textures.find("albedo");
            const std::string& resolvedTex = (it != mat->textures.end()) ? it->second : std::string{};
            if (!emitter.texture.IsValid() || emitter.loadedTexturePath != resolvedTex) {
                emitter.texture = LoadParticleTextureOrWhite(resources, resolvedTex);
                emitter.loadedTexturePath = resolvedTex;
            }
            // blendMode を .mat から上書きする。
            // WHY: materialPath が描画設定の単一の信頼元であるため、Inspector の blendMode より優先する。
            emitter.blendMode = (mat->blendMode == renderer::BlendMode::ALPHA_BLEND)
                                  ? ParticleBlendMode::Alpha : ParticleBlendMode::Additive;
            return;
        }
    }

    // フォールバック: texturePath を直接使用する (materialPath 未設定時の既存挙動を維持)。
    if (emitter.texture.IsValid() && emitter.loadedTexturePath == emitter.texturePath)
        return;
    emitter.texture = LoadParticleTextureOrWhite(resources, emitter.texturePath);
    emitter.loadedTexturePath = emitter.texturePath;
}

void SpawnParticle(ParticleEmitter& emitter, const Transform& transform)
{
    Particle p;
    p.position = TransformEmitterPoint(transform, emitter.emitPosition);
    math::Vector3 shapeVelocity = math::Vector3::ZERO;

    switch (emitter.shape) {
    case ParticleEmitterShape::Sphere: {
        const math::Vector3 dir = RandomUnitVector(emitter);
        const float radius = (std::max)(emitter.sphereRadius, 0.0f) * std::cbrt(Random01(emitter));
        p.position = p.position + TransformEmitterVector(transform, dir * radius);
        shapeVelocity = TransformEmitterVector(transform, dir * emitter.velocitySpread);
        break;
    }
    case ParticleEmitterShape::Cone: {
        constexpr float DEG_TO_RAD = 3.14159265358979323846f / 180.0f;
        const float angle = (std::max)(emitter.coneAngleDegrees, 0.0f) * DEG_TO_RAD;
        const float theta = Random01(emitter) * angle;
        const float phi = Random01(emitter) * 3.14159265358979323846f * 2.0f;
        const float radius = (std::max)(emitter.coneRadius, 0.0f) * std::sqrt(Random01(emitter));
        const math::Vector3 localOffset = {
            std::cos(phi) * radius,
            0.0f,
            std::sin(phi) * radius
        };
        p.position = p.position + TransformEmitterVector(transform, localOffset);
        const math::Vector3 localShapeVelocity = {
            std::sin(theta) * std::cos(phi) * emitter.velocitySpread,
            std::cos(theta) * emitter.velocitySpread,
            std::sin(theta) * std::sin(phi) * emitter.velocitySpread
        };
        shapeVelocity = TransformEmitterVector(transform, localShapeVelocity);
        break;
    }
    case ParticleEmitterShape::Box: {
        const math::Vector3 localOffset = {
            RandomSigned01(emitter) * emitter.boxExtents.x,
            RandomSigned01(emitter) * emitter.boxExtents.y,
            RandomSigned01(emitter) * emitter.boxExtents.z
        };
        p.position = p.position + TransformEmitterVector(transform, localOffset);
        break;
    }
    case ParticleEmitterShape::Point:
    default:
        break;
    }

    const float rx = RandomSigned01(emitter) * emitter.velocitySpread;
    const float rz = RandomSigned01(emitter) * emitter.velocitySpread;
    const math::Vector3 localVelocity = {
        emitter.emitVelocity.x + rx,
        emitter.emitVelocity.y,
        emitter.emitVelocity.z + rz
    };
    p.velocity = TransformEmitterVector(transform, localVelocity);
    p.velocity = p.velocity + shapeVelocity;
    p.color = emitter.colorStart;
    p.size  = emitter.sizeStart;
    p.age   = 0.0f;
    p.rotation = Random01(emitter) * 3.14159265358979323846f * 2.0f;
    p.angularVelocity = emitter.angularVelocityMin
        + (emitter.angularVelocityMax - emitter.angularVelocityMin) * Random01(emitter);
    p.uvRect = ComputeSpriteRect(emitter, 0.0f);
    emitter.particles.push_back(std::move(p));
}

void ClearEmitterRuntime(ParticleEmitter& emitter)
{
    emitter.particles.clear();
    emitter.emitAccum = 0.0f;
    emitter.burstPending = 0;
}

// CPU の SpawnParticle と同じ Shape/Spread ロジックで GpuSpawnEntry を初期化する
void InitGpuSpawnEntry(GpuSpawnEntry& s, ParticleEmitter& emitter, const Transform& tf)
{
    s.position = TransformEmitterPoint(tf, emitter.emitPosition);
    math::Vector3 shapeVelocity = math::Vector3::ZERO;

    switch (emitter.shape) {
    case ParticleEmitterShape::Sphere: {
        const math::Vector3 dir = RandomUnitVector(emitter);
        const float radius = (std::max)(emitter.sphereRadius, 0.0f) * std::cbrt(Random01(emitter));
        s.position = s.position + TransformEmitterVector(tf, dir * radius);
        shapeVelocity = TransformEmitterVector(tf, dir * emitter.velocitySpread);
        break;
    }
    case ParticleEmitterShape::Cone: {
        constexpr float DEG_TO_RAD = 3.14159265358979323846f / 180.0f;
        const float angle = (std::max)(emitter.coneAngleDegrees, 0.0f) * DEG_TO_RAD;
        const float theta = Random01(emitter) * angle;
        const float phi   = Random01(emitter) * 3.14159265358979323846f * 2.0f;
        const float radius = (std::max)(emitter.coneRadius, 0.0f) * std::sqrt(Random01(emitter));
        const math::Vector3 localOffset = {
            std::cos(phi) * radius,
            0.0f,
            std::sin(phi) * radius
        };
        s.position = s.position + TransformEmitterVector(tf, localOffset);
        const math::Vector3 localShapeVelocity = {
            std::sin(theta) * std::cos(phi) * emitter.velocitySpread,
            std::cos(theta) * emitter.velocitySpread,
            std::sin(theta) * std::sin(phi) * emitter.velocitySpread
        };
        shapeVelocity = TransformEmitterVector(tf, localShapeVelocity);
        break;
    }
    case ParticleEmitterShape::Box: {
        const math::Vector3 localOffset = {
            RandomSigned01(emitter) * emitter.boxExtents.x,
            RandomSigned01(emitter) * emitter.boxExtents.y,
            RandomSigned01(emitter) * emitter.boxExtents.z
        };
        s.position = s.position + TransformEmitterVector(tf, localOffset);
        break;
    }
    default:
        break;
    }

    const float rx = RandomSigned01(emitter) * emitter.velocitySpread;
    const float rz = RandomSigned01(emitter) * emitter.velocitySpread;
    const math::Vector3 localVelocity = {
        emitter.emitVelocity.x + rx,
        emitter.emitVelocity.y,
        emitter.emitVelocity.z + rz
    };
    s.velocity        = TransformEmitterVector(tf, localVelocity);
    s.velocity        = s.velocity + shapeVelocity;
    s.lifetime        = (std::max)(emitter.lifetime, 0.001f);
    s.size            = emitter.sizeStart;
    s.colorStart      = emitter.colorStart;
    s.colorEnd        = emitter.colorEnd;
    s.uvRect          = ComputeSpriteRect(emitter, 0.0f);
    s.rotation        = Random01(emitter) * 6.28318530717958647692f;
    s.angularVelocity = emitter.angularVelocityMin
        + (emitter.angularVelocityMax - emitter.angularVelocityMin) * Random01(emitter);
}

// GPU パーティクル: バッファ初期化・スポーン・CS Dispatch・DrawInstanced
void TickGpuEmitter(ParticleEmitter&        emitter,
                    const Transform&        tf,
                    float                   dt,
                    bool                    canEmit,
                    RenderPassContext&       ctx)
{
    auto& resources = ctx.resources;
    auto& renderer  = ctx.renderer;
    auto& h         = ctx.handles;

    if (!h.particleGpuSimCS.IsValid() || !h.particleGpuShader.IsValid()) return;

    const int maxP = (std::max)(emitter.maxParticles, 1);

    // デバイスリセット (Play Mode 移行など) 後は古いハンドルが無効になるため再初期化する
    const uint64_t currentResetVersion = resources.GetResetVersion();
    if (emitter.gpuInitialized && emitter.gpuResetVersion != currentResetVersion)
    {
        emitter.gpuInitialized  = false;
        emitter.gpuParticleBuffer = {};
        emitter.gpuSpawnBuffer    = {};
        emitter.gpuEmitterCB      = {};
        emitter.gpuWriteHead      = 0;
        emitter.gpuSpawnCount     = 0;
    }

    // バッファ未作成なら初期化 (要素ゼロで確保し CS が age>=lifetime で無視する)
    if (!emitter.gpuInitialized)
    {
        std::vector<GpuParticle> init(static_cast<size_t>(maxP));
        for (auto& p : init) p.age = p.lifetime = 1.0f; // 全粒子を「死亡済み」で初期化
        emitter.gpuParticleBuffer = resources.CreateRWStructuredBuffer(
            init.data(), static_cast<uint32_t>(maxP), sizeof(GpuParticle));

        emitter.gpuSpawnBuffer = resources.CreateStructuredBuffer(
            nullptr, static_cast<uint32_t>(maxP), sizeof(GpuSpawnEntry));

        emitter.gpuEmitterCB = resources.CreateConstantBuffer(sizeof(GpuParticleEmitterCB));
        emitter.gpuWriteHead     = 0;
        emitter.gpuSpawnCount    = 0;
        emitter.gpuResetVersion  = resources.GetResetVersion();
        emitter.gpuInitialized   = true;
    }

    // 今フレームのスポーンエントリを構築
    std::vector<GpuSpawnEntry> spawns;
    if (canEmit || emitter.burstPending > 0)
    {
        const int burstCount = (std::max)(emitter.burstPending, 0);
        emitter.burstPending = 0;
        for (int i = 0; i < burstCount && static_cast<int>(spawns.size()) < maxP; ++i)
        {
            GpuSpawnEntry s;
            InitGpuSpawnEntry(s, emitter, tf);
            spawns.push_back(s);
        }
        if (canEmit)
        {
            emitter.emitAccum += emitter.emitRate * dt;
            while (emitter.emitAccum >= 1.0f && static_cast<int>(spawns.size()) < maxP)
            {
                emitter.emitAccum -= 1.0f;
                GpuSpawnEntry s;
                InitGpuSpawnEntry(s, emitter, tf);
                spawns.push_back(s);
            }
        }
    }
    else
    {
        emitter.emitAccum = 0.0f;
    }

    emitter.gpuSpawnCount = static_cast<uint32_t>(spawns.size());

    // スポーンバッファを CPU → GPU 転送
    if (emitter.gpuSpawnCount > 0)
        resources.Update(emitter.gpuSpawnBuffer, spawns.data(),
                         emitter.gpuSpawnCount * sizeof(GpuSpawnEntry));

    // CS 用定数バッファ更新
    GpuParticleEmitterCB cb{};
    cb.emitterPos      = TransformEmitterPoint(tf, emitter.emitPosition);
    cb.deltaTime       = dt;
    cb.gravity         = emitter.gravity;
    cb.maxParticles    = static_cast<uint32_t>(maxP);
    cb.colorStart      = emitter.colorStart;
    cb.colorEnd        = emitter.colorEnd;
    cb.spawnCount      = emitter.gpuSpawnCount;
    cb.spawnOffset     = emitter.gpuWriteHead;
    cb.colorCurvePower = emitter.colorCurvePower;
    cb.velocityDamping = emitter.velocityDamping;
    cb.sizeStart       = emitter.sizeStart;
    cb.sizeEnd         = emitter.sizeEnd;
    cb.sizeCurvePower  = emitter.sizeCurvePower;
    {
        const int cols       = (std::max)(emitter.spriteColumns, 1);
        const int rows       = (std::max)(emitter.spriteRows, 1);
        const int frameCount = cols * rows;
        const int startFrame = std::clamp(emitter.spriteStartFrame, 0, frameCount - 1);
        const int endFrame   = std::clamp(
            emitter.spriteEndFrame > 0 ? emitter.spriteEndFrame : frameCount - 1,
            startFrame, frameCount - 1);
        cb.spriteColumns    = static_cast<uint32_t>(cols);
        cb.spriteRows       = static_cast<uint32_t>(rows);
        cb.spriteStartFrame = static_cast<uint32_t>(startFrame);
        cb.spriteEndFrame   = static_cast<uint32_t>(endFrame);
    }
    resources.Update(emitter.gpuEmitterCB, &cb, sizeof(cb));

    // リングバッファヘッドを進める
    emitter.gpuWriteHead = (emitter.gpuWriteHead + emitter.gpuSpawnCount)
                           % static_cast<uint32_t>(maxP);

    // Dispatch CS
    renderer::ComputeCall cc;
    cc.shader        = h.particleGpuSimCS;
    cc.constantBuffers[0] = emitter.gpuEmitterCB;
    cc.srvBuffers[1] = emitter.gpuSpawnBuffer;   // t15
    cc.uavBuffers[0] = emitter.gpuParticleBuffer; // u2
    cc.dispatchX = (static_cast<uint32_t>(maxP) + 63u) / 64u;
    cc.dispatchY = 1;
    cc.dispatchZ = 1;
    renderer.Dispatch(cc, resources);
    // Dispatch() は OM のレンダーターゲットをアンバインドする。
    // 後続の Draw が正しい HDR RT へ出力されるよう再バインドする。
    renderer.SetRenderTarget(h.hdrRT, resources);

    // SV_VertexID ベース描画: 頂点バッファなし、VS が StructuredBuffer<GpuParticle> を t14 で読む
    const auto gpuShader = emitter.blendMode == ParticleBlendMode::Alpha
        ? h.particleGpuAlphaShader : h.particleGpuShader;
    const auto gpuPSO = emitter.blendMode == ParticleBlendMode::Alpha
        ? h.particleGpuAlphaPSO : h.particleGpuPSO;
    if (!gpuShader.IsValid() || !gpuPSO.IsValid() || !emitter.texture.IsValid())
        return;

    renderer::DrawCall dc;
    dc.shader        = gpuShader;
    dc.pipelineState = gpuPSO;
    dc.constantBuffers[0] = h.frameCB;
    dc.textures[0]        = emitter.texture;
    dc.vsBuffers[0]       = emitter.gpuParticleBuffer; // t14: StructuredBuffer<GpuParticle>
    dc.vertexCount        = static_cast<uint32_t>(maxP) * 6u;
    renderer.SetSampler(0, renderer::SamplerMode::WRAP_BILINEAR);
    renderer.Submit(dc, resources);
}

} // anonymous namespace

void ExecuteParticlePass(RenderPassContext& ctx)
{
    auto& renderer  = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;

    if (!h.particleShader.IsValid() || !h.particleVB.IsValid() || !h.particleIB.IsValid()) return;

    const float dt = Time::deltaTime;

    for (auto& go : ctx.scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, ctx.cullingMask)) continue;
        auto* emitter = go.GetComponent<ParticleEmitter>();
        if (!emitter || !emitter->enabled) continue;
        const Transform& tf = go.transform;
        EnsureParticleTexture(*emitter, resources);

        const float clampedLifetime = (std::max)(emitter->lifetime, 0.001f);
        emitter->duration = (std::max)(emitter->duration, 0.0f);
        emitter->startDelay = (std::max)(emitter->startDelay, 0.0f);
        emitter->spriteColumns = (std::max)(emitter->spriteColumns, 1);
        emitter->spriteRows = (std::max)(emitter->spriteRows, 1);
        emitter->sizeCurvePower = (std::max)(emitter->sizeCurvePower, 0.001f);
        emitter->colorCurvePower = (std::max)(emitter->colorCurvePower, 0.001f);
        emitter->velocityDamping = (std::max)(emitter->velocityDamping, 0.0f);
        const bool isGpuMode = emitter->simulationMode == ParticleSimulationMode::Gpu;

        bool canEmit = emitter->playing;
        if (canEmit && emitter->delayTime < emitter->startDelay) {
            emitter->delayTime += dt;
            canEmit = false;
        }
        if (canEmit && emitter->duration > 0.0f) {
            emitter->playTime += dt;
            if (emitter->playTime >= emitter->duration) {
                if (emitter->loop) {
                    emitter->playTime = 0.0f;
                    emitter->delayTime = 0.0f;
                    canEmit = false;
                } else {
                    emitter->playing = false;
                    canEmit = false;
                    if (emitter->clearOnStop)
                        ClearEmitterRuntime(*emitter);
                }
            }
        }

        // GPU モードは CPU スポーン/更新をスキップして GPU パスへ
        if (isGpuMode) {
            TickGpuEmitter(*emitter, tf, dt, canEmit, ctx);
            continue;
        }

        // パーティクル生成
        const int particleCapacity = (std::max)(emitter->maxParticles, 0);
        const bool hasBurst = emitter->burstPending > 0;
        if (canEmit || hasBurst) {
            const int burstCount = (std::max)(emitter->burstPending, 0);
            for (int i = 0; i < burstCount && static_cast<int>(emitter->particles.size()) < particleCapacity; ++i)
                SpawnParticle(*emitter, tf);
            emitter->burstPending = 0;

            const int currentCount = static_cast<int>(emitter->particles.size());
            if (currentCount >= particleCapacity) {
                // WHY: 満杯中に emitAccum を積み続けると、寿命切れ直後に未放出分がまとめて出て不自然になる。
                //      生成できなかった分は破棄し、次フレーム以降の通常レートに戻す。
                emitter->emitAccum = 0.0f;
            } else if (canEmit) {
                emitter->emitAccum += emitter->emitRate * dt;
            }
            while (emitter->emitAccum >= 1.0f
                   && static_cast<int>(emitter->particles.size()) < particleCapacity)
            {
                emitter->emitAccum -= 1.0f;
                SpawnParticle(*emitter, tf);
            }
            if (static_cast<int>(emitter->particles.size()) >= particleCapacity) {
                // WHY: 生成ループの途中で上限に達した場合も、残った未放出分を次フレームへ持ち越さない。
                emitter->emitAccum = 0.0f;
            }
        } else {
            emitter->emitAccum = 0.0f;
            emitter->burstPending = 0;
        }

        // パーティクル更新・寿命切れを削除
        for (auto it = emitter->particles.begin(); it != emitter->particles.end(); ) {
            it->age += dt;
            if (it->age >= clampedLifetime) {
                it = emitter->particles.erase(it);
                continue;
            }
            float t = Clamp01(it->age / clampedLifetime);
            it->position.x += it->velocity.x * dt;
            it->position.y += it->velocity.y * dt;
            it->position.z += it->velocity.z * dt;
            it->velocity.x += emitter->gravity.x * dt;
            it->velocity.y += emitter->gravity.y * dt;
            it->velocity.z += emitter->gravity.z * dt;
            const float damping = (std::max)(0.0f, 1.0f - emitter->velocityDamping * dt);
            it->velocity = it->velocity * damping;
            it->rotation += it->angularVelocity * dt;
            it->uvRect = ComputeSpriteRect(*emitter, t);
            it->color = LerpVec4(emitter->colorStart, emitter->colorEnd, std::pow(t, emitter->colorCurvePower));
            const float sizeT = std::pow(t, emitter->sizeCurvePower);
            it->size  = emitter->sizeStart + (emitter->sizeEnd - emitter->sizeStart) * sizeT;
            ++it;
        }

        int count = std::min(static_cast<int>(emitter->particles.size()), kMaxParticleDraw);
        if (count == 0) continue;
        if (emitter->sortMode == ParticleSortMode::BackToFront) {
            const math::Vector3 cameraPos = ctx.camera.m_position;
            std::sort(emitter->particles.begin(), emitter->particles.end(),
                [cameraPos](const Particle& a, const Particle& b) {
                    const math::Vector3 da = a.position - cameraPos;
                    const math::Vector3 db = b.position - cameraPos;
                    return math::Vector3::Dot(da, da) > math::Vector3::Dot(db, db);
                });
        }

        // CPU で頂点バッファを構築 (ビルボードは VS でスクリーン展開)
        static const float kUV[4][2] = { {0,0},{1,0},{0,1},{1,1} };
        std::vector<ParticleVertex> verts;
        verts.reserve(static_cast<size_t>(count * 4));
        for (int i = 0; i < count; ++i) {
            const auto& p = emitter->particles[i];
            for (int c = 0; c < 4; ++c) {
                ParticleVertex v;
                v.center[0] = p.position.x;
                v.center[1] = p.position.y;
                v.center[2] = p.position.z;
                v.uv[0]     = kUV[c][0];
                v.uv[1]     = kUV[c][1];
                v.color[0]  = p.color.x;
                v.color[1]  = p.color.y;
                v.color[2]  = p.color.z;
                v.color[3]  = p.color.w;
                v.size      = p.size;
                v.rotation  = p.rotation;
                v.uvRect[0] = p.uvRect.x;
                v.uvRect[1] = p.uvRect.y;
                v.uvRect[2] = p.uvRect.z;
                v.uvRect[3] = p.uvRect.w;
                verts.push_back(v);
            }
        }

        resources.Update(h.particleVB, verts.data(),
                         static_cast<uint32_t>(verts.size() * sizeof(ParticleVertex)));

        const auto particlePSO = emitter->blendMode == ParticleBlendMode::Alpha
            ? h.particleAlphaPSO : h.particlePSO;
        if (!particlePSO.IsValid() || !emitter->texture.IsValid())
            continue;

        renderer::DrawCall dc;
        dc.vertexBuffer       = h.particleVB;
        dc.indexBuffer        = h.particleIB;
        dc.indexCount         = static_cast<uint32_t>(count * 6);
        dc.shader             = h.particleShader;
        dc.pipelineState      = particlePSO;
        dc.constantBuffers[0] = h.frameCB;
        dc.textures[0]        = emitter->texture;
        renderer.SetSampler(0, renderer::SamplerMode::WRAP_BILINEAR);
        renderer.Submit(dc, resources);
    }
}

} // namespace fbzz::scene
