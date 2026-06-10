// FBZZ Engine
// RenderPasses/ParticlePass.cpp | fbzz::scene
// パーティクル更新 (CPU) と描画
#include "GeometryPasses.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Transform.hpp"
#include "Engine/Scene/Components/ParticleEmitter.hpp"
#include "Engine/Core/Time.hpp"
#include "Engine/Renderer/DrawCall.hpp"
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

void EnsureParticleTexture(ParticleEmitter& emitter, renderer::ResourceManager& resources)
{
    if (emitter.texture.IsValid() && emitter.loadedTexturePath == emitter.texturePath)
        return;

    if (emitter.texturePath.empty()) {
        static const uint8_t white[4] = { 255, 255, 255, 255 };
        emitter.texture = resources.CreateTexture(white, 1, 1);
    } else {
        emitter.texture = resources.LoadTexture(emitter.texturePath);
    }
    emitter.loadedTexturePath = emitter.texturePath;
}

void SpawnParticle(ParticleEmitter& emitter, const Transform& transform)
{
    Particle p;
    p.position = transform.position + emitter.emitPosition;
    math::Vector3 shapeVelocity = math::Vector3::ZERO;

    switch (emitter.shape) {
    case ParticleEmitterShape::Sphere: {
        const math::Vector3 dir = RandomUnitVector(emitter);
        const float radius = (std::max)(emitter.sphereRadius, 0.0f) * std::cbrt(Random01(emitter));
        p.position = p.position + dir * radius;
        shapeVelocity = dir * emitter.velocitySpread;
        break;
    }
    case ParticleEmitterShape::Cone: {
        constexpr float DEG_TO_RAD = 3.14159265358979323846f / 180.0f;
        const float angle = (std::max)(emitter.coneAngleDegrees, 0.0f) * DEG_TO_RAD;
        const float theta = Random01(emitter) * angle;
        const float phi = Random01(emitter) * 3.14159265358979323846f * 2.0f;
        const float radius = (std::max)(emitter.coneRadius, 0.0f) * std::sqrt(Random01(emitter));
        p.position.x += std::cos(phi) * radius;
        p.position.z += std::sin(phi) * radius;
        shapeVelocity = {
            std::sin(theta) * std::cos(phi) * emitter.velocitySpread,
            std::cos(theta) * emitter.velocitySpread,
            std::sin(theta) * std::sin(phi) * emitter.velocitySpread
        };
        break;
    }
    case ParticleEmitterShape::Box:
        p.position.x += RandomSigned01(emitter) * emitter.boxExtents.x;
        p.position.y += RandomSigned01(emitter) * emitter.boxExtents.y;
        p.position.z += RandomSigned01(emitter) * emitter.boxExtents.z;
        break;
    case ParticleEmitterShape::Point:
    default:
        break;
    }

    const float rx = RandomSigned01(emitter) * emitter.velocitySpread;
    const float rz = RandomSigned01(emitter) * emitter.velocitySpread;
    p.velocity = { emitter.emitVelocity.x + rx,
                   emitter.emitVelocity.y,
                   emitter.emitVelocity.z + rz };
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

} // anonymous namespace

void ExecuteParticlePass(RenderPassContext& ctx)
{
    auto& renderer  = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;

    if (!h.particleShader.IsValid() || !h.particleVB.IsValid() || !h.particleIB.IsValid()) return;

    const float dt = core::Time::DeltaTime();

    for (auto& go : ctx.scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, ctx.cullingMask)) continue;
        auto* emitter = go.GetComponent<ParticleEmitter>();
        if (!emitter || !emitter->enabled) continue;
        auto& tf = go.transform;
        EnsureParticleTexture(*emitter, resources);

        const float clampedLifetime = (std::max)(emitter->lifetime, 0.001f);
        emitter->duration = (std::max)(emitter->duration, 0.0f);
        emitter->startDelay = (std::max)(emitter->startDelay, 0.0f);
        emitter->spriteColumns = (std::max)(emitter->spriteColumns, 1);
        emitter->spriteRows = (std::max)(emitter->spriteRows, 1);
        emitter->sizeCurvePower = (std::max)(emitter->sizeCurvePower, 0.001f);
        emitter->colorCurvePower = (std::max)(emitter->colorCurvePower, 0.001f);
        emitter->velocityDamping = (std::max)(emitter->velocityDamping, 0.0f);
        const bool useCpuFallback = emitter->simulationMode == ParticleSimulationMode::Gpu;
        (void)useCpuFallback;
        // WHY: 現時点の Renderer API は Compute/StructuredBuffer 更新を公開していないため、
        //      GPU モードは設定を保持しつつ CPU 経路で描画する。API 追加時にここを分岐点にする。

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

        renderer::DrawCall dc;
        dc.vertexBuffer       = h.particleVB;
        dc.indexBuffer        = h.particleIB;
        dc.indexCount         = static_cast<uint32_t>(count * 6);
        dc.shader             = h.particleShader;
        dc.pipelineState      = emitter->blendMode == ParticleBlendMode::Alpha
            ? h.particleAlphaPSO
            : h.particlePSO;
        dc.constantBuffers[0] = h.frameCB;
        dc.textures[0]        = emitter->texture;
        renderer.SetSampler(0, renderer::SamplerMode::WRAP_BILINEAR);
        renderer.Submit(dc, resources);
    }
}

} // namespace fbzz::scene
