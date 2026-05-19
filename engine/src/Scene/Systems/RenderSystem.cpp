// FBZZ Engine
// RenderSystem.cpp | fbzz::scene
// MeshRenderer + Transform を走査し、3パスで描画する
// Pass 1: ShadowMap  (深度専用 RT → shadow depth tex)
// Pass 2: HDR Forward (HDR RT → シャドウ + ライティング)
// Pass 3: Composite  (HDR → ACES ToneMap → バックバッファ)
#include "engine/Scene/Systems/RenderSystem.hpp"
#include "engine/Core/Time.hpp"
#include "engine/Scene/Scene.hpp"
#include "engine/Scene/Components/MeshRenderer.hpp"
#include "engine/Scene/Components/ParticleEmitter.hpp"
#include "engine/Scene/Components/SkyRenderer.hpp"
#include "engine/Scene/Transform.hpp"
#include "engine/Renderer/IRenderer.hpp"
#include "engine/Renderer/Camera.hpp"
#include "engine/Renderer/LightSystem.hpp"
#include "engine/Renderer/Mesh.hpp"
#include "engine/Renderer/Material.hpp"
#include "engine/Renderer/ComputeCall.hpp"
#include "engine/Renderer/DrawCall.hpp"
#include "engine/Renderer/PrimitiveMesh.hpp"
#include "engine/Renderer/RenderState.hpp"
#include "engine/Renderer/SamplerMode.hpp"
#include "engine/Renderer/ShaderManager.hpp"
#include <math/Matrix4.hpp>
#include <math/Vector3.hpp>
#include <math/Vector4.hpp>
#include <algorithm>
#include <cstdlib>

namespace fbzz::scene {

namespace {

// b0: CameraConstants — Constants.hlsli と一致させること
struct PerFrameCB {
    math::Matrix4 view;
    math::Matrix4 projection;
    math::Matrix4 viewProjection;
    math::Matrix4 invViewProjection;
    math::Vector3 cameraPos;
    float         nearZ;
    float         farZ;
    float         _pad[3];
};

// b1: ObjectConstants — Constants.hlsli と一致させること
struct PerObjectCB {
    math::Matrix4 world;
    math::Matrix4 worldInvTranspose;
};

// b4: ShadowConstants — Constants.hlsli と一致させること
struct ShadowConstantsCB {
    math::Matrix4 lightViewProjection;
    float         shadowMapTexelSize[2];
    float         shadowBias;
    float         _pad;
};

// b6: AtmosphereConstants — Constants.hlsli と一致させること
struct AtmosphereCB {
    float rayleighScattering[3];
    float mieScattering;
    float planetRadius;
    float atmosphereRadius;
    float sunIntensity;
    float mieG;
};

// b5: PostProcConstants — Constants.hlsli と一致させること
struct PostProcCB {
    float texelSize[2];
    float screenSize[2];
    float exposure;
    float time;
    float fogDensity;
    float _pad;
    float fogColor[3];
    float fogFar;
};

constexpr uint32_t SHADOW_MAP_SIZE = 8192;

// Particle.hlsl の ParticleVSIn と完全一致させる (DX11Shader がリフレクションで解析)
struct ParticleVertex {
    float center[3];  // POSITION   12 bytes
    float uv[2];      // TEXCOORD0   8 bytes
    float color[4];   // COLOR       16 bytes
    float size;       // TEXCOORD1   4 bytes
};                    // 合計 40 bytes

inline math::Vector4 LerpVec4(const math::Vector4& a, const math::Vector4& b, float t)
{
    return { a.x + (b.x - a.x) * t,
             a.y + (b.y - a.y) * t,
             a.z + (b.z - a.z) * t,
             a.w + (b.w - a.w) * t };
}

} // namespace

void RenderSystem(Scene& scene,
                  renderer::IRenderer& renderer,
                  const renderer::Camera& camera,
                  const renderer::LightSystem& lights)
{
    static auto shadowMapRT     = renderer.CreateRenderTarget(SHADOW_MAP_SIZE, SHADOW_MAP_SIZE, 0);
    static auto shadowShader    = renderer::ShaderManager::Load("assets/shaders/Pipeline/ShadowMap.hlsl");
    static auto compositeShader = renderer::ShaderManager::Load("assets/shaders/PostProcess/Composite.hlsl");
    static auto bloomDownShader = renderer::ShaderManager::Load("assets/shaders/PostProcess/BloomDownsample.cs.hlsl");
    static auto bloomUpShader   = renderer::ShaderManager::Load("assets/shaders/PostProcess/BloomUpsample.cs.hlsl");
    static auto frameCB         = renderer.CreateConstantBuffer(sizeof(PerFrameCB));
    static auto objectCB        = renderer.CreateConstantBuffer(sizeof(PerObjectCB));
    static auto lightCB         = renderer.CreateConstantBuffer(sizeof(renderer::LightConstantsCB));
    static auto shadowCB        = renderer.CreateConstantBuffer(sizeof(ShadowConstantsCB));
    static auto postprocCB      = renderer.CreateConstantBuffer(sizeof(PostProcCB));
    static auto pso             = renderer.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::OPAQUE,
        renderer::DepthMode::DEPTH_ON
    });
    // CSO は Material.Skydome.*.cso としてコンパイルされているため、パスは Sky/ を省く
    static auto skydomeShader = renderer::ShaderManager::Load("assets/shaders/Material/Skydome.hlsl");
    static auto skydomePSO    = renderer.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::OPAQUE,
        renderer::DepthMode::DEPTH_SKY
    });
    static auto skydomeMesh = renderer::PrimitiveMesh::Sphere(renderer, 32);
    static auto atmCB       = renderer.CreateConstantBuffer(sizeof(AtmosphereCB));

    static auto particleShader = renderer::ShaderManager::Load("assets/shaders/Material/Particle.hlsl");
    static auto particlePSO    = renderer.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::ADDITIVE,
        renderer::DepthMode::DEPTH_READ
    });
    // 深度テスト不要なフルスクリーンパス共用 PSO
    static auto postprocPSO = renderer.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::OPAQUE,
        renderer::DepthMode::DEPTH_OFF
    });
    static auto fxaaShader = renderer::ShaderManager::Load("assets/shaders/PostProcess/FXAA.hlsl");

    // パーティクル用インデックスバッファ (最大 1000 パーティクル分を事前生成)
    static std::shared_ptr<renderer::IBuffer> particleIB;
    if (!particleIB)
    {
        constexpr int MAX_P = 1000;
        std::vector<uint32_t> idx;
        idx.reserve(MAX_P * 6);
        for (int i = 0; i < MAX_P; ++i) {
            uint32_t b = static_cast<uint32_t>(i * 4);
            idx.insert(idx.end(), { b, b+1, b+2, b+1, b+3, b+2 });
        }
        particleIB = renderer.CreateIndexBuffer(idx.data(), static_cast<uint32_t>(idx.size()));
    }

    // ウィンドウリサイズに追従して HDR RT と Bloom テクスチャを再生成する
    static std::shared_ptr<renderer::IRenderTarget> hdrRT;
    static std::shared_ptr<renderer::IRenderTarget> ldrRT;   // Composite 出力 (FXAA 入力)
    static std::shared_ptr<renderer::ITexture>      bloomHalf;  // HDR 1/2 解像度 (Downsample 出力)
    static std::shared_ptr<renderer::ITexture>      bloomFull;  // フル解像度 (Upsample 出力 → Composite 入力)
    static uint32_t sHdrW = 0, sHdrH = 0;
    {
        uint32_t curW = renderer.GetWidth();
        uint32_t curH = renderer.GetHeight();
        if (!hdrRT || sHdrW != curW || sHdrH != curH)
        {
            hdrRT     = renderer.CreateRenderTarget(curW, curH, 1);
            ldrRT     = renderer.CreateRenderTarget(curW, curH, 1);
            bloomHalf = renderer.CreateComputeTexture(curW / 2, curH / 2);
            bloomFull = renderer.CreateComputeTexture(curW, curH);
            sHdrW     = curW;
            sHdrH     = curH;
        }
    }

    // =========================================================================
    // ライトの View-Projection を計算 (シャドウパス + ShadowConstants で共用)
    // =========================================================================
    const auto& dl = lights.GetDirectional();
    math::Vector3 lightDir = dl.direction.Normalized();
    math::Vector3 sceneCenter = { 0.0f, 1.0f, 4.0f };
    math::Vector3 lightPos    = sceneCenter - lightDir * 30.0f;
    math::Vector3 up = (std::abs(lightDir.y) > 0.99f)
                       ? math::Vector3{ 1.0f, 0.0f, 0.0f }
                       : math::Vector3{ 0.0f, 1.0f, 0.0f };
    math::Matrix4 lightView = math::Matrix4::LookAt(lightPos, sceneCenter, up);
    math::Matrix4 lightProj = math::Matrix4::Orthographic(-20.0f, 20.0f, -20.0f, 20.0f, 1.0f, 60.0f);
    math::Matrix4 lightVP   = lightProj * lightView;

    // =========================================================================
    // Pass 1: Shadow Map — ライト視点から深度のみ書き込む
    // =========================================================================
    renderer.SetRenderTarget(shadowMapRT);
    renderer.ClearDepth();

    // ライトの VP を b0 の viewProjection スロットにセット
    PerFrameCB lightFrameData{};
    lightFrameData.viewProjection = lightVP;
    frameCB->Update(&lightFrameData, sizeof(PerFrameCB));

    for (auto [tf, mr] : scene.View<Transform, MeshRenderer>()) {
        if (!mr.enabled || !mr.mesh || !mr.material) continue;
        if (!mr.mesh->vertexBuffer || !mr.mesh->indexBuffer) continue;

        PerObjectCB objData{};
        objData.world = tf.GetWorldMatrix();
        objectCB->Update(&objData, sizeof(PerObjectCB));

        renderer::DrawCall dc;
        dc.vertexBuffer       = mr.mesh->vertexBuffer;
        dc.indexBuffer        = mr.mesh->indexBuffer;
        dc.indexCount         = mr.mesh->indexCount;
        dc.shader             = shadowShader;
        dc.pipelineState      = pso;
        dc.constantBuffers[0] = frameCB;
        dc.constantBuffers[1] = objectCB;
        renderer.Submit(dc);
    }

    // =========================================================================
    // Pass 2: HDR Forward — カメラ視点でシャドウ + ライティング
    // =========================================================================
    renderer.SetRenderTarget(hdrRT);
    renderer.Clear({ 0.005f, 0.005f, 0.02f, 1.0f });

    PerFrameCB frameData{};
    frameData.view              = camera.GetViewMatrix();
    frameData.projection        = camera.GetProjectionMatrix();
    frameData.viewProjection    = camera.GetViewProjection();
    frameData.invViewProjection = math::Matrix4::Inverse(frameData.viewProjection);
    frameData.cameraPos         = camera.m_position;
    frameData.nearZ             = camera.m_near;
    frameData.farZ              = camera.m_far;
    frameCB->Update(&frameData, sizeof(PerFrameCB));

    lights.Upload(*lightCB);

    ShadowConstantsCB shadowData{};
    shadowData.lightViewProjection    = lightVP;
    shadowData.shadowMapTexelSize[0]  = 1.0f / static_cast<float>(SHADOW_MAP_SIZE);
    shadowData.shadowMapTexelSize[1]  = 1.0f / static_cast<float>(SHADOW_MAP_SIZE);
    shadowData.shadowBias             = 0.005f;
    shadowCB->Update(&shadowData, sizeof(ShadowConstantsCB));

    renderer.SetSampler(0, renderer::SamplerMode::WRAP_ANISOTROPIC);
    renderer.SetSampler(1, renderer::SamplerMode::BORDER_ZERO);

    auto shadowDepthTex = shadowMapRT->GetDepthTexture();

    for (auto [tf, mr] : scene.View<Transform, MeshRenderer>()) {
        if (!mr.enabled || !mr.mesh || !mr.material) continue;
        if (!mr.mesh->vertexBuffer || !mr.mesh->indexBuffer) continue;
        if (!mr.material->shader) continue;

        PerObjectCB objData{};
        objData.world             = tf.GetWorldMatrix();
        objData.worldInvTranspose = math::Matrix4::Transpose(math::Matrix4::Inverse(objData.world));
        objectCB->Update(&objData, sizeof(PerObjectCB));

        mr.material->Upload();

        renderer::DrawCall dc;
        dc.vertexBuffer       = mr.mesh->vertexBuffer;
        dc.indexBuffer        = mr.mesh->indexBuffer;
        dc.indexCount         = mr.mesh->indexCount;
        dc.vertexCount        = mr.mesh->vertexCount;
        dc.shader             = mr.material->shader;
        dc.pipelineState      = pso;
        dc.constantBuffers[0] = frameCB;
        dc.constantBuffers[1] = objectCB;
        dc.constantBuffers[2] = mr.material->paramsBuffer;
        dc.constantBuffers[3] = lightCB;
        dc.constantBuffers[4] = shadowCB;
        if (mr.material->albedoTexture)  dc.textures[0] = mr.material->albedoTexture;
        if (mr.material->normalTexture)  dc.textures[1] = mr.material->normalTexture;
        dc.textures[8] = shadowDepthTex;
        renderer.Submit(dc);
    }

    // =========================================================================
    // Pass 2b: Skydome — シーン内の SkyRenderer コンポーネントを探して描画
    //          DEPTH_SKY (LESS_EQUAL + 書き込みなし) で最遠面に配置
    // =========================================================================
    if (skydomeShader && skydomeMesh && skydomeMesh->vertexBuffer && skydomeMesh->indexBuffer)
    {
        for (auto [tf, sky] : scene.View<Transform, SkyRenderer>()) {
            if (!sky.enabled) continue;

            // exposure を Skydome 用に事前セット (Pass 3 より先に実行されるため)
            PostProcCB skyPostData{};
            skyPostData.exposure = 1.0f;
            skyPostData.time     = core::Time::TotalTime();
            postprocCB->Update(&skyPostData, sizeof(PostProcCB));

            AtmosphereCB atmData{};
            atmData.rayleighScattering[0] = sky.rayleighScattering.x;
            atmData.rayleighScattering[1] = sky.rayleighScattering.y;
            atmData.rayleighScattering[2] = sky.rayleighScattering.z;
            atmData.mieScattering         = sky.mieScattering;
            atmData.planetRadius          = 6371.0f;
            atmData.atmosphereRadius      = 6471.0f;
            atmData.sunIntensity          = sky.sunIntensity;
            atmData.mieG                  = sky.mieG;
            atmCB->Update(&atmData, sizeof(AtmosphereCB));

            renderer::DrawCall skyDC;
            skyDC.vertexBuffer       = skydomeMesh->vertexBuffer;
            skyDC.indexBuffer        = skydomeMesh->indexBuffer;
            skyDC.indexCount         = skydomeMesh->indexCount;
            skyDC.shader             = skydomeShader;
            skyDC.pipelineState      = skydomePSO;
            skyDC.constantBuffers[0] = frameCB;
            skyDC.constantBuffers[3] = lightCB;
            skyDC.constantBuffers[5] = postprocCB;
            skyDC.constantBuffers[6] = atmCB;
            renderer.Submit(skyDC);
            break; // スカイドームは 1 つのみ
        }
    }

    // =========================================================================
    // Pass 2c: Particle System — エミッターごとにシミュレーション → 描画
    // =========================================================================
    if (particleShader && particleIB)
    {
        const float dt = core::Time::DeltaTime();
        constexpr int MAX_PARTICLE_DRAW = 1000;

        for (auto [tf, emitter] : scene.View<Transform, ParticleEmitter>()) {
            if (!emitter.enabled) continue;

            // ----- パーティクル発生 -----
            emitter.emitAccum += emitter.emitRate * dt;
            while (emitter.emitAccum >= 1.0f
                   && static_cast<int>(emitter.particles.size()) < emitter.maxParticles)
            {
                emitter.emitAccum -= 1.0f;
                Particle p;
                p.position = tf.localPosition + emitter.emitPosition;
                float rx = ((std::rand() / float(RAND_MAX)) * 2.0f - 1.0f) * emitter.velocitySpread;
                float rz = ((std::rand() / float(RAND_MAX)) * 2.0f - 1.0f) * emitter.velocitySpread;
                p.velocity = { emitter.emitVelocity.x + rx,
                               emitter.emitVelocity.y,
                               emitter.emitVelocity.z + rz };
                p.color = emitter.colorStart;
                p.size  = emitter.sizeStart;
                p.age   = 0.0f;
                emitter.particles.push_back(std::move(p));
            }

            // ----- 更新 + 寿命切れ除去 -----
            for (auto it = emitter.particles.begin(); it != emitter.particles.end(); ) {
                it->age += dt;
                if (it->age >= emitter.lifetime) {
                    it = emitter.particles.erase(it);
                    continue;
                }
                float t = it->age / emitter.lifetime;
                it->position.x += it->velocity.x * dt;
                it->position.y += it->velocity.y * dt;
                it->position.z += it->velocity.z * dt;
                it->velocity.y -= 5.0f * dt;  // 重力
                it->color = LerpVec4(emitter.colorStart, emitter.colorEnd, t);
                it->size  = emitter.sizeStart + (emitter.sizeEnd - emitter.sizeStart) * t;
                ++it;
            }

            // ----- ビルボードクワッドを構築して描画 -----
            int count = std::min(static_cast<int>(emitter.particles.size()), MAX_PARTICLE_DRAW);
            if (count == 0) continue;

            static const float kUV[4][2] = { {0,0},{1,0},{0,1},{1,1} };
            std::vector<ParticleVertex> verts;
            verts.reserve(static_cast<size_t>(count * 4));
            for (int i = 0; i < count; ++i) {
                const auto& p = emitter.particles[i];
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

            auto vb = renderer.CreateVertexBuffer(
                verts.data(),
                verts.size() * sizeof(ParticleVertex),
                sizeof(ParticleVertex));
            if (!vb) continue;

            renderer::DrawCall dc;
            dc.vertexBuffer       = vb;
            dc.indexBuffer        = particleIB;
            dc.indexCount         = static_cast<uint32_t>(count * 6);
            dc.shader             = particleShader;
            dc.pipelineState      = particlePSO;
            dc.constantBuffers[0] = frameCB;
            renderer.Submit(dc);
        }
    }

    // =========================================================================
    // Pass 3a: Bloom Downsample — HDR → 半解像度 Bloom テクスチャ
    // Pass 3b: Bloom Upsample  — 半解像度 → フル解像度 (テントフィルタ)
    // =========================================================================
    if (bloomDownShader && bloomUpShader && bloomHalf && bloomFull)
    {
        // Downsample: 入力 HDR (SRV) → bloomHalf (UAV)
        PostProcCB halfData{};
        halfData.texelSize[0]  = 1.0f / static_cast<float>(sHdrW);
        halfData.texelSize[1]  = 1.0f / static_cast<float>(sHdrH);
        halfData.screenSize[0] = static_cast<float>(sHdrW);
        halfData.screenSize[1] = static_cast<float>(sHdrH);
        postprocCB->Update(&halfData, sizeof(PostProcCB));

        renderer::ComputeCall bloomDownDC;
        bloomDownDC.shader                  = bloomDownShader;
        bloomDownDC.constantBuffers[5]      = postprocCB;
        bloomDownDC.srvInputs[10]           = hdrRT->GetColorTexture(0);  // TEX_BLOOM = t10
        bloomDownDC.uavOutputs[0]           = bloomHalf;
        bloomDownDC.dispatchX               = (sHdrW / 2 + 7) / 8;
        bloomDownDC.dispatchY               = (sHdrH / 2 + 7) / 8;
        bloomDownDC.dispatchZ               = 1;
        renderer.Dispatch(bloomDownDC);

        // Upsample: 入力 bloomHalf (SRV) → bloomFull (UAV)
        PostProcCB fullData{};
        fullData.texelSize[0]  = 1.0f / static_cast<float>(sHdrW / 2);
        fullData.texelSize[1]  = 1.0f / static_cast<float>(sHdrH / 2);
        fullData.screenSize[0] = static_cast<float>(sHdrW);
        fullData.screenSize[1] = static_cast<float>(sHdrH);
        postprocCB->Update(&fullData, sizeof(PostProcCB));

        renderer::ComputeCall bloomUpDC;
        bloomUpDC.shader             = bloomUpShader;
        bloomUpDC.constantBuffers[5] = postprocCB;
        bloomUpDC.srvInputs[10]      = bloomHalf;   // TEX_BLOOM = t10
        bloomUpDC.uavOutputs[0]      = bloomFull;
        bloomUpDC.dispatchX          = (sHdrW + 7) / 8;
        bloomUpDC.dispatchY          = (sHdrH + 7) / 8;
        bloomUpDC.dispatchZ          = 1;
        renderer.Dispatch(bloomUpDC);
    }

    // =========================================================================
    // Pass 4: Composite — HDR + Bloom → ACES ToneMap → LDR バッファ
    // =========================================================================
    renderer.SetRenderTarget(ldrRT);

    PostProcCB postData{};
    postData.texelSize[0]  = 1.0f / static_cast<float>(sHdrW);
    postData.texelSize[1]  = 1.0f / static_cast<float>(sHdrH);
    postData.screenSize[0] = static_cast<float>(sHdrW);
    postData.screenSize[1] = static_cast<float>(sHdrH);
    postData.exposure      = 1.0f;
    postData.fogDensity    = 0.06f;
    postData.fogFar        = 10.0f;
    postData.fogColor[0]   = 0.01f;
    postData.fogColor[1]   = 0.01f;
    postData.fogColor[2]   = 0.04f;
    postprocCB->Update(&postData, sizeof(PostProcCB));

    renderer.SetSampler(0, renderer::SamplerMode::CLAMP_LINEAR);

    renderer::DrawCall compositeDC;
    compositeDC.shader             = compositeShader;
    compositeDC.pipelineState      = postprocPSO;
    compositeDC.vertexCount        = 3;
    compositeDC.constantBuffers[0] = frameCB;
    compositeDC.constantBuffers[5] = postprocCB;
    compositeDC.textures[5]        = hdrRT->GetColorTexture(0);
    compositeDC.textures[7]        = hdrRT->GetDepthTexture();
    compositeDC.textures[10]       = bloomFull;  // TEX_BLOOM = t10
    renderer.Submit(compositeDC);

    // =========================================================================
    // Pass 5: FXAA — LDR バッファのエッジをアンチエイリアシング → バックバッファ
    // =========================================================================
    if (fxaaShader && ldrRT)
    {
        renderer.SetRenderTarget(nullptr);

        renderer::DrawCall fxaaDC;
        fxaaDC.shader             = fxaaShader;
        fxaaDC.pipelineState      = postprocPSO;
        fxaaDC.vertexCount        = 3;
        fxaaDC.constantBuffers[5] = postprocCB;  // texelSize を参照
        fxaaDC.textures[5]        = ldrRT->GetColorTexture(0);
        renderer.Submit(fxaaDC);
    }
}

} // namespace fbzz::scene
