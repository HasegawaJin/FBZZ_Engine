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
#include <cstdlib>
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

        // パーティクル生成
        emitter->emitAccum += emitter->emitRate * dt;
        while (emitter->emitAccum >= 1.0f
               && static_cast<int>(emitter->particles.size()) < emitter->maxParticles)
        {
            emitter->emitAccum -= 1.0f;
            Particle p;
            p.position = tf.localPosition + emitter->emitPosition;
            float rx = ((std::rand() / float(RAND_MAX)) * 2.0f - 1.0f) * emitter->velocitySpread;
            float rz = ((std::rand() / float(RAND_MAX)) * 2.0f - 1.0f) * emitter->velocitySpread;
            p.velocity = { emitter->emitVelocity.x + rx,
                           emitter->emitVelocity.y,
                           emitter->emitVelocity.z + rz };
            p.color = emitter->colorStart;
            p.size  = emitter->sizeStart;
            p.age   = 0.0f;
            emitter->particles.push_back(std::move(p));
        }

        // パーティクル更新・寿命切れを削除
        for (auto it = emitter->particles.begin(); it != emitter->particles.end(); ) {
            it->age += dt;
            if (it->age >= emitter->lifetime) {
                it = emitter->particles.erase(it);
                continue;
            }
            float t = it->age / emitter->lifetime;
            it->position.x += it->velocity.x * dt;
            it->position.y += it->velocity.y * dt;
            it->position.z += it->velocity.z * dt;
            it->velocity.y -= 5.0f * dt;  // 重力
            it->color = LerpVec4(emitter->colorStart, emitter->colorEnd, t);
            it->size  = emitter->sizeStart + (emitter->sizeEnd - emitter->sizeStart) * t;
            ++it;
        }

        int count = std::min(static_cast<int>(emitter->particles.size()), kMaxParticleDraw);
        if (count == 0) continue;

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
        dc.pipelineState      = h.particlePSO;
        dc.constantBuffers[0] = h.frameCB;
        renderer.Submit(dc, resources);
    }
}

} // namespace fbzz::scene
