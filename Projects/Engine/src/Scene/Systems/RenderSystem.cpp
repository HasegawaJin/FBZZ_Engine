// FBZZ Engine
// RenderSystem.cpp | fbzz::scene
// Scene から DrawCall を生成する描画 System
// Mesh / Light / Camera / UI / Particle を集約し、IRenderer へ送信する。
// DX11 実装には直接依存せず、Renderer 抽象と ResourceManager を使う。
// Pass 1: シャドウマップ深度。
// Pass 2: HDR フォワードレンダリング、スカイドーム、パーティクル、デバッグオーバーレイ。
// Pass 3: ブルーム、コンポジット、オプションの FXAA。
#include "Engine/Scene/Systems/RenderSystem.hpp"
#include "Engine/Renderer/RenderSettings.hpp"
#include "Engine/Scene/Systems/ColliderDebugDrawSystem.hpp"
#include "Engine/Core/Time.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Components/LightComponent.hpp"
#include "Engine/Scene/Components/MeshRenderer.hpp"
#include "Engine/Scene/Components/MaterialComponent.hpp"
#include "Engine/Scene/Components/AnimatorComponent.hpp"
#include "Engine/Scene/Components/SkinnedMeshRenderer.hpp"
#include "Engine/Scene/Components/ParticleEmitter.hpp"
#include "Engine/Scene/Components/SkyRenderer.hpp"
#include "Engine/Scene/Transform.hpp"
#include "Engine/Renderer/IRenderer.hpp"
#include "Engine/Renderer/Camera.hpp"
#include "Engine/Renderer/LightSystem.hpp"
#include "Engine/Renderer/Mesh.hpp"
#include "Engine/Renderer/Material.hpp"
#include "Engine/Renderer/ComputeCall.hpp"
#include "Engine/Renderer/DrawCall.hpp"
#include "Engine/Renderer/PrimitiveMesh.hpp"
#include "Engine/Renderer/RenderState.hpp"
#include "Engine/Renderer/DebugDraw.hpp"
#include "Engine/Renderer/SamplerMode.hpp"
#include "Engine/Renderer/ResourceManager.hpp"
#include "Engine/Asset/Skeleton.hpp"
#include <Math/Matrix4.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>

namespace fbzz::scene {

namespace {

// b0: CameraConstants。Constants.hlsli と同期を保つ。
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

// b1: ObjectConstants。Constants.hlsli と同期を保つ。
struct PerObjectCB {
    math::Matrix4 world;
    math::Matrix4 worldInvTranspose;
};

// b4: ShadowConstants。Constants.hlsli と同期を保つ。
struct ShadowConstantsCB {
    math::Matrix4 lightViewProjection;
    float         shadowMapTexelSize[2];
    float         shadowBias;
    float         _pad;
};

// b6: AtmosphereConstants。Constants.hlsli と同期を保つ。
struct AtmosphereCB {
    float rayleighScattering[3];
    float mieScattering;
    float planetRadius;
    float atmosphereRadius;
    float sunIntensity;
    float mieG;
};

// b5: PostProcConstants。Constants.hlsli と同期を保つ。
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

// Particle.hlsl の ParticleVSIn に対応。DX11Shader がこのレイアウトをリフレクションする。
struct ParticleVertex {
    float center[3];  // POSITION   12 bytes
    float uv[2];      // TEXCOORD0   8 bytes
    float color[4];   // COLOR       16 bytes
    float size;       // TEXCOORD1   4 bytes
};                    // 40 bytes

inline math::Vector4 LerpVec4(const math::Vector4& a, const math::Vector4& b, float t)
{
    return { a.x + (b.x - a.x) * t,
             a.y + (b.y - a.y) * t,
             a.z + (b.z - a.z) * t,
             a.w + (b.w - a.w) * t };
}

renderer::Material* SyncMaterial(MaterialComponent& mc, renderer::ResourceManager& resources)
{
    if (!mc.enabled) return nullptr;

    if (!mc.material)
        mc.material = std::make_shared<renderer::Material>();

    if (mc.material.use_count() > 1) {
        auto cloned = std::make_shared<renderer::Material>(*mc.material);
        cloned->paramsBuffer = renderer::ResourceHandle<renderer::ConstantBufferTag>{};
        mc.material = std::move(cloned);
    }

    auto& material = *mc.material;
    material.shaderPath = mc.shaderPath;
    material.shader = material.shaderPath.empty()
        ? renderer::ResourceHandle<renderer::ShaderTag>{}
        : resources.LoadShader(material.shaderPath);
    material.albedoTexture = mc.albedoTexPath.empty()
        ? renderer::ResourceHandle<renderer::TextureTag>{}
        : resources.LoadTexture(mc.albedoTexPath);
    material.normalTexture = mc.normalTexPath.empty()
        ? renderer::ResourceHandle<renderer::TextureTag>{}
        : resources.LoadTexture(mc.normalTexPath);
    material.Upload(resources);
    return &material;
}

} // namespace

void RenderSystem(Scene& scene,
                  renderer::IRenderer& renderer,
                  renderer::ResourceManager& resources,
                  const renderer::Camera& camera,
                  renderer::ResourceHandle<renderer::RenderTargetTag> outputRT,
                  const renderer::RenderSettings* settings,
                  fbzz::LayerMask cullingMask)
{
    static renderer::RenderSettings sDefaultSettings;
    const renderer::RenderSettings& rs = settings ? *settings : sDefaultSettings;
    static auto shadowMapRT     = resources.CreateRenderTarget(SHADOW_MAP_SIZE, SHADOW_MAP_SIZE, 0);
    static auto shadowShader    = resources.LoadShader("assets/shaders/Pipeline/ShadowMap.hlsl");
    static auto skinnedShadowShader = resources.LoadShader("assets/shaders/Pipeline/SkinnedShadowMap.hlsl");
    static auto skinnedPbrShader = resources.LoadShader("assets/shaders/Material/SkinnedPBR.hlsl");
    // アニメーターがない SkinnedMeshRenderer のフォールバック用バインドポーズ。
    // スキンメッシュシェーダーは常に b7 からスキニング行列を読む仕様のため、
    // AnimatorComponent がない場合でも単位行列パレットを送ってメッシュ変形なし状態を表す。
    static renderer::ResourceHandle<renderer::ConstantBufferTag> bindPoseSkinningCB;
    if (!bindPoseSkinningCB.IsValid()) {
        struct BindPoseData { math::Matrix4 bones[asset::MAX_SKINNING_BONES]; };
        BindPoseData bp{};
        for (auto& m : bp.bones) m = math::Matrix4::Identity();
        bindPoseSkinningCB = resources.CreateConstantBuffer(sizeof(BindPoseData));
        resources.Update(bindPoseSkinningCB, &bp, sizeof(BindPoseData));
    }
    static auto compositeShader = resources.LoadShader("assets/shaders/PostProcess/Composite.hlsl");
    static auto bloomDownShader = resources.LoadShader("assets/shaders/PostProcess/BloomDownsample.cs.hlsl");
    static auto bloomUpShader   = resources.LoadShader("assets/shaders/PostProcess/BloomUpsample.cs.hlsl");
    static auto frameCB         = resources.CreateConstantBuffer(sizeof(PerFrameCB));
    static auto objectCB        = resources.CreateConstantBuffer(sizeof(PerObjectCB));
    static auto lightCB         = resources.CreateConstantBuffer(sizeof(renderer::LightConstantsCB));
    static auto shadowCB        = resources.CreateConstantBuffer(sizeof(ShadowConstantsCB));
    static auto postprocCB      = resources.CreateConstantBuffer(sizeof(PostProcCB));
    static auto pso             = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::OPAQUE,
        renderer::DepthMode::DEPTH_ON
    });
    static auto wireframePso    = resources.CreatePipelineState({
        renderer::RasterizerMode::WIREFRAME,
        renderer::BlendMode::OPAQUE,
        renderer::DepthMode::DEPTH_ON
    });
    // スカイドーム CSO は Material/Sky/Skydome.hlsl から生成される。
    static auto skydomeShader = resources.LoadShader("assets/shaders/Material/Sky/Skydome.hlsl");
    static auto skydomePSO    = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::OPAQUE,
        renderer::DepthMode::DEPTH_SKY
    });
    static auto skydomeMesh = renderer::PrimitiveMesh::Sphere(resources, 32);
    static auto atmCB       = resources.CreateConstantBuffer(sizeof(AtmosphereCB));

    static auto particleShader = resources.LoadShader("assets/shaders/Material/Particle.hlsl");
    static auto particlePSO    = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::ADDITIVE,
        renderer::DepthMode::DEPTH_READ
    });
    // 深度不要なフルスクリーンパス共通 PSO。
    static auto postprocPSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::OPAQUE,
        renderer::DepthMode::DEPTH_OFF
    });
    static auto fxaaShader = resources.LoadShader("assets/shaders/PostProcess/FXAA.hlsl");

    // 最大パーティクル数分のバッファを事前確保する。
    static renderer::ResourceHandle<renderer::BufferTag> particleVB;
    static renderer::ResourceHandle<renderer::BufferTag> particleIB;
    constexpr int MAX_PARTICLE_DRAW = 1000;
    constexpr uint32_t MAX_PARTICLE_VERTICES = MAX_PARTICLE_DRAW * 4;
    if (!particleVB.IsValid())
    {
        particleVB = resources.CreateVertexBuffer(
            nullptr,
            MAX_PARTICLE_VERTICES * sizeof(ParticleVertex),
            sizeof(ParticleVertex));
    }
    if (!particleIB.IsValid())
    {
        std::vector<uint32_t> idx;
        idx.reserve(MAX_PARTICLE_DRAW * 6);
        for (int i = 0; i < MAX_PARTICLE_DRAW; ++i) {
            uint32_t b = static_cast<uint32_t>(i * 4);
            idx.insert(idx.end(), { b, b+1, b+2, b+1, b+3, b+2 });
        }
        particleIB = resources.CreateIndexBuffer(idx.data(), static_cast<uint32_t>(idx.size()));
    }

    // HDR・ブルームターゲットを出力サイズに追従させる。
    static renderer::ResourceHandle<renderer::RenderTargetTag> hdrRT;
    static renderer::ResourceHandle<renderer::RenderTargetTag> ldrRT;      // FXAA 入力用コンポジット出力。
    static renderer::ResourceHandle<renderer::TextureTag>      bloomHalf;  // 半解像度ブルームテクスチャ。
    static renderer::ResourceHandle<renderer::TextureTag>      bloomFull;  // コンポジット用フル解像度ブルームテクスチャ。
    static uint32_t sHdrW = 0, sHdrH = 0;
    {
        const auto* output = resources.Get(outputRT);
        uint32_t curW = output ? output->GetWidth()  : renderer.GetWidth();
        uint32_t curH = output ? output->GetHeight() : renderer.GetHeight();
        if (curW == 0 || curH == 0) return;
        if (!hdrRT.IsValid() || sHdrW != curW || sHdrH != curH)
        {
            if (hdrRT.IsValid()) resources.Release(hdrRT);
            if (ldrRT.IsValid()) resources.Release(ldrRT);
            if (bloomHalf.IsValid()) resources.Release(bloomHalf);
            if (bloomFull.IsValid()) resources.Release(bloomFull);
            hdrRT     = resources.CreateRenderTarget(curW, curH, 1);
            ldrRT     = resources.CreateRenderTarget(curW, curH, 1);
            bloomHalf = resources.CreateComputeTexture(std::max(1u, curW / 2), std::max(1u, curH / 2));
            bloomFull = resources.CreateComputeTexture(curW, curH);
            sHdrW     = curW;
            sHdrH     = curH;
        }
    }

    // =========================================================================
    // LightComponent データから LightConstantsCB を構築する。
    // =========================================================================
    renderer::LightConstantsCB lightData{};
    lightData.lightDir       = { 0.0f, -1.0f, 0.5f };  // フォールバック平行光源
    lightData.lightColor     = { 1.0f,  1.0f, 1.0f };
    lightData.lightIntensity = 1.0f;

    constexpr float kDegToRad = 3.14159265f / 180.0f;
    for (auto [tf, lc] : scene.View<Transform, LightComponent>()) {
        if (!lc.enabled) continue;
        if (lc.type == LightComponent::Type::Directional) {
            lightData.lightDir       = tf.Forward().Normalized();
            lightData.lightColor     = lc.color;
            lightData.lightIntensity = lc.intensity;
        } else if (lc.type == LightComponent::Type::Point
                   && lightData.pointLightCount < 8) {
            auto& pl    = lightData.pointLights[lightData.pointLightCount++];
            pl.position  = tf.position;
            pl.range     = lc.range;
            pl.color     = lc.color;
            pl.intensity = lc.intensity;
        } else if (lc.type == LightComponent::Type::Spot
                   && lightData.spotLightCount < 4) {
            auto& sl    = lightData.spotLights[lightData.spotLightCount++];
            sl.position  = tf.position;
            sl.direction = tf.Forward().Normalized();
            sl.range     = lc.range;
            sl.innerCos  = std::cos(lc.innerCone * kDegToRad);
            sl.outerCos  = std::cos(lc.outerCone * kDegToRad);
            sl.color     = lc.color;
            sl.intensity = lc.intensity;
        }
    }

    math::Vector3 lightDir = lightData.lightDir.Normalized();
    math::Vector3 sceneCenter = { 0.0f, 1.0f, 4.0f };
    math::Vector3 lightPos    = sceneCenter - lightDir * 30.0f;
    math::Vector3 up = (std::abs(lightDir.y) > 0.99f)
                       ? math::Vector3{ 1.0f, 0.0f, 0.0f }
                       : math::Vector3{ 0.0f, 1.0f, 0.0f };
    math::Matrix4 lightView = math::Matrix4::LookAt(lightPos, sceneCenter, up);
    math::Matrix4 lightProj = math::Matrix4::Orthographic(-20.0f, 20.0f, -20.0f, 20.0f, 1.0f, 60.0f);
    math::Matrix4 lightVP   = lightProj * lightView;

    // =========================================================================
    // Pass 1: ライト視点からシャドウマップ深度を描画する。
    // シャドウ無効時もクリアしてサンプリングデータを安定させる。
    // =========================================================================
    renderer.SetRenderTarget(shadowMapRT, resources);
    renderer.ClearDepth();

    if (rs.shadowEnabled)
    {
        PerFrameCB lightFrameData{};
        lightFrameData.viewProjection = lightVP;
        resources.Update(frameCB, &lightFrameData, sizeof(PerFrameCB));

        // Pass 1a: 静的 MeshRenderer のシャドウ描画
        for (auto& go : scene.GameObjects()) {
            if (!fbzz::Layer::Contains(cullingMask, go.layer)) continue;
            auto* mr  = go.GetComponent<MeshRenderer>();
            auto* mat = go.GetComponent<MaterialComponent>();
            if (!mr || !mr->enabled || !mr->mesh || !mat || !mat->enabled) continue;
            if (!mr->mesh->vertexBuffer.IsValid() || !mr->mesh->indexBuffer.IsValid()) continue;
            if (mr->mesh->isSkinned) continue; // スキンメッシュは下で処理する。

            PerObjectCB objData{};
            objData.world = go.transform.GetWorldMatrix();
            resources.Update(objectCB, &objData, sizeof(PerObjectCB));

            renderer::DrawCall dc;
            dc.vertexBuffer       = mr->mesh->vertexBuffer;
            dc.indexBuffer        = mr->mesh->indexBuffer;
            dc.indexCount         = mr->mesh->indexCount;
            dc.shader             = shadowShader;
            dc.pipelineState      = pso;
            dc.constantBuffers[0] = frameCB;
            dc.constantBuffers[1] = objectCB;
            renderer.Submit(dc, resources);
        }

        // Pass 1b: SkinnedMeshRenderer のシャドウ描画。モデル内の全メッシュを送出する。
        if (skinnedShadowShader.IsValid()) {
            for (auto& go : scene.GameObjects()) {
                if (!fbzz::Layer::Contains(cullingMask, go.layer)) continue;
                auto* smr  = go.GetComponent<SkinnedMeshRenderer>();
                auto* mat  = go.GetComponent<MaterialComponent>();
                auto* anim = go.GetComponent<AnimatorComponent>();
                if (!smr || !smr->enabled || !smr->model) continue;
                if (!mat || !mat->enabled) continue;

                PerObjectCB objData{};
                objData.world = go.transform.GetWorldMatrix();
                resources.Update(objectCB, &objData, sizeof(PerObjectCB));

                const auto skinCB = (anim && anim->skinningBuffer.IsValid())
                    ? anim->skinningBuffer : bindPoseSkinningCB;

                for (const auto& meshPtr : smr->model->meshes) {
                    if (!meshPtr) continue;
                    if (!meshPtr->vertexBuffer.IsValid() || !meshPtr->indexBuffer.IsValid()) continue;

                    renderer::DrawCall dc;
                    dc.vertexBuffer       = meshPtr->vertexBuffer;
                    dc.indexBuffer        = meshPtr->indexBuffer;
                    dc.indexCount         = meshPtr->indexCount;
                    dc.shader             = skinnedShadowShader;
                    dc.pipelineState      = pso;
                    dc.constantBuffers[0] = frameCB;
                    dc.constantBuffers[1] = objectCB;
                    dc.constantBuffers[7] = skinCB;
                    renderer.Submit(dc, resources);
                }
            }
        }
    }

    // =========================================================================
    // Pass 2: カメラ視点から HDR フォワードパスを描画する。
    // =========================================================================
    renderer.SetRenderTarget(hdrRT, resources);
    renderer.Clear({ 0.005f, 0.005f, 0.02f, 1.0f });

    PerFrameCB frameData{};
    frameData.view              = camera.GetViewMatrix();
    frameData.projection        = camera.GetProjectionMatrix();
    frameData.viewProjection    = camera.GetViewProjection();
    frameData.invViewProjection = math::Matrix4::Inverse(frameData.viewProjection);
    frameData.cameraPos         = camera.m_position;
    frameData.nearZ             = camera.m_near;
    frameData.farZ              = camera.m_far;
    resources.Update(frameCB, &frameData, sizeof(PerFrameCB));

    resources.Update(lightCB, &lightData, sizeof(renderer::LightConstantsCB));

    ShadowConstantsCB shadowData{};
    shadowData.lightViewProjection    = lightVP;
    shadowData.shadowMapTexelSize[0]  = 1.0f / static_cast<float>(SHADOW_MAP_SIZE);
    shadowData.shadowMapTexelSize[1]  = 1.0f / static_cast<float>(SHADOW_MAP_SIZE);
    shadowData.shadowBias             = 0.005f;
    resources.Update(shadowCB, &shadowData, sizeof(ShadowConstantsCB));

    renderer.SetSampler(0, renderer::SamplerMode::WRAP_ANISOTROPIC);
    renderer.SetSampler(1, renderer::SamplerMode::BORDER_ZERO);

    auto shadowDepthTex = resources.GetDepthTexture(shadowMapRT);

    // Pass 2a: 静的 MeshRenderer
    for (auto& go : scene.GameObjects()) {
        if (!fbzz::Layer::Contains(cullingMask, go.layer)) continue;
        auto* mr  = go.GetComponent<MeshRenderer>();
        auto* mat = go.GetComponent<MaterialComponent>();
        if (!mr || !mr->enabled || !mr->mesh || !mat) continue;
        if (!mr->mesh->vertexBuffer.IsValid() || !mr->mesh->indexBuffer.IsValid()) continue;
        if (mr->mesh->isSkinned) continue; // スキンメッシュは SkinnedMeshRenderer で処理する。
        auto* material = SyncMaterial(*mat, resources);
        if (!material || !material->shader.IsValid()) continue;

        PerObjectCB objData{};
        objData.world             = go.transform.GetWorldMatrix();
        objData.worldInvTranspose = math::Matrix4::Transpose(math::Matrix4::Inverse(objData.world));
        resources.Update(objectCB, &objData, sizeof(PerObjectCB));

        renderer::DrawCall dc;
        dc.vertexBuffer       = mr->mesh->vertexBuffer;
        dc.indexBuffer        = mr->mesh->indexBuffer;
        dc.indexCount         = mr->mesh->indexCount;
        dc.vertexCount        = mr->mesh->vertexCount;
        dc.shader             = material->shader;
        dc.pipelineState      = rs.wireframeMode ? wireframePso : pso;
        dc.constantBuffers[0] = frameCB;
        dc.constantBuffers[1] = objectCB;
        dc.constantBuffers[2] = material->paramsBuffer;
        dc.constantBuffers[3] = lightCB;
        dc.constantBuffers[4] = shadowCB;
        if (material->albedoTexture.IsValid()) dc.textures[0] = material->albedoTexture;
        if (material->normalTexture.IsValid()) dc.textures[1] = material->normalTexture;
        dc.textures[8] = shadowDepthTex;
        renderer.Submit(dc, resources);
    }

    // Pass 2b: SkinnedMeshRenderer — モデル内の全メッシュを走査する
    if (skinnedPbrShader.IsValid()) {
        for (auto& go : scene.GameObjects()) {
            if (!fbzz::Layer::Contains(cullingMask, go.layer)) continue;
            auto* smr  = go.GetComponent<SkinnedMeshRenderer>();
            auto* mat  = go.GetComponent<MaterialComponent>();
            auto* anim = go.GetComponent<AnimatorComponent>();
            if (!smr || !smr->enabled || !smr->model) continue;
            if (!mat) continue;
            auto* material = SyncMaterial(*mat, resources);
            if (!material) continue;

            PerObjectCB objData{};
            objData.world             = go.transform.GetWorldMatrix();
            objData.worldInvTranspose = math::Matrix4::Transpose(math::Matrix4::Inverse(objData.world));
            resources.Update(objectCB, &objData, sizeof(PerObjectCB));

            const auto skinCB = (anim && anim->skinningBuffer.IsValid())
                ? anim->skinningBuffer : bindPoseSkinningCB;

            for (const auto& meshPtr : smr->model->meshes) {
                if (!meshPtr) continue;
                if (!meshPtr->vertexBuffer.IsValid() || !meshPtr->indexBuffer.IsValid()) continue;

                renderer::DrawCall dc;
                dc.vertexBuffer       = meshPtr->vertexBuffer;
                dc.indexBuffer        = meshPtr->indexBuffer;
                dc.indexCount         = meshPtr->indexCount;
                dc.vertexCount        = meshPtr->vertexCount;
                dc.shader             = skinnedPbrShader;
                dc.pipelineState      = rs.wireframeMode ? wireframePso : pso;
                dc.constantBuffers[0] = frameCB;
                dc.constantBuffers[1] = objectCB;
                dc.constantBuffers[2] = material->paramsBuffer;
                dc.constantBuffers[3] = lightCB;
                dc.constantBuffers[4] = shadowCB;
                dc.constantBuffers[7] = skinCB;
                if (material->albedoTexture.IsValid()) dc.textures[0] = material->albedoTexture;
                if (material->normalTexture.IsValid()) dc.textures[1] = material->normalTexture;
                dc.textures[8] = shadowDepthTex;
                renderer.Submit(dc, resources);
            }
        }
    }

    // =========================================================================
    // Pass 2c: スカイドーム。有効な SkyRenderer を最初の 1 つだけ遠平面に描画する。
    // DEPTH_SKY は深度書き込みなしの less-equal 深度テストを使用する。
    // =========================================================================
    if (skydomeShader.IsValid() && skydomeMesh && skydomeMesh->vertexBuffer.IsValid() && skydomeMesh->indexBuffer.IsValid())
    {
        for (auto& go : scene.GameObjects()) {
            if (!fbzz::Layer::Contains(cullingMask, go.layer)) continue;
            auto* sky = go.GetComponent<SkyRenderer>();
            if (!sky || !sky->enabled) continue;

            // スカイドームはコンポジット前に実行するため、必要なポストデータをここで書き込む。
            PostProcCB skyPostData{};
            skyPostData.exposure = 1.0f;
            skyPostData.time     = core::Time::TotalTime();
            resources.Update(postprocCB, &skyPostData, sizeof(PostProcCB));

            AtmosphereCB atmData{};
            atmData.rayleighScattering[0] = sky->rayleighScattering.x;
            atmData.rayleighScattering[1] = sky->rayleighScattering.y;
            atmData.rayleighScattering[2] = sky->rayleighScattering.z;
            atmData.mieScattering         = sky->mieScattering;
            atmData.planetRadius          = 6371.0f;
            atmData.atmosphereRadius      = 6471.0f;
            atmData.sunIntensity          = sky->sunIntensity;
            atmData.mieG                  = sky->mieG;
            resources.Update(atmCB, &atmData, sizeof(AtmosphereCB));

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
            renderer.Submit(skyDC, resources);
            break; // スカイドームは 1 つだけ描画する。
        }
    }

    // =========================================================================
    // Pass 2d: パーティクルシステム。エミッターごとにシミュレーションしてビルボードを送出する。
    // =========================================================================
    if (particleShader.IsValid() && particleVB.IsValid() && particleIB.IsValid())
    {
        const float dt = core::Time::DeltaTime();

        for (auto& go : scene.GameObjects()) {
            if (!fbzz::Layer::Contains(cullingMask, go.layer)) continue;
            auto* emitter = go.GetComponent<ParticleEmitter>();
            if (!emitter || !emitter->enabled) continue;
            auto& tf = go.transform;

            // パーティクルを放出する。
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

            // パーティクルを更新し、寿命切れを削除する。
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

            // ビルボード頂点を構築して送出する。
            int count = std::min(static_cast<int>(emitter->particles.size()), MAX_PARTICLE_DRAW);
            if (count == 0) continue;

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

            resources.Update(particleVB, verts.data(), verts.size() * sizeof(ParticleVertex));

            renderer::DrawCall dc;
            dc.vertexBuffer       = particleVB;
            dc.indexBuffer        = particleIB;
            dc.indexCount         = static_cast<uint32_t>(count * 6);
            dc.shader             = particleShader;
            dc.pipelineState      = particlePSO;
            dc.constantBuffers[0] = frameCB;
            renderer.Submit(dc, resources);
        }
    }

    if (rs.showColliders)
    {
        renderer::DebugDraw::BeginFrame(renderer, resources, camera.GetViewProjection());
        ColliderDebugDrawSystem(scene, renderer);
        renderer::DebugDraw::Flush();
    }

    // =========================================================================
    // Pass 3a: HDR から半解像度テクスチャへブルームダウンサンプル。
    // Pass 3b: 半解像度テクスチャからフル解像度へブルームアップサンプル。
    // =========================================================================
    if (rs.bloomEnabled && bloomDownShader.IsValid() && bloomUpShader.IsValid() && bloomHalf.IsValid() && bloomFull.IsValid())
    {
        // ダウンサンプル: HDR SRV → bloomHalf UAV。
        PostProcCB halfData{};
        halfData.texelSize[0]  = 1.0f / static_cast<float>(sHdrW);
        halfData.texelSize[1]  = 1.0f / static_cast<float>(sHdrH);
        halfData.screenSize[0] = static_cast<float>(sHdrW);
        halfData.screenSize[1] = static_cast<float>(sHdrH);
        resources.Update(postprocCB, &halfData, sizeof(PostProcCB));

        renderer::ComputeCall bloomDownDC;
        bloomDownDC.shader                  = bloomDownShader;
        bloomDownDC.constantBuffers[5]      = postprocCB;
        bloomDownDC.srvInputs[10]           = resources.GetColorTexture(hdrRT, 0);  // TEX_BLOOM = t10
        bloomDownDC.uavOutputs[0]           = bloomHalf;
        bloomDownDC.dispatchX               = (sHdrW / 2 + 7) / 8;
        bloomDownDC.dispatchY               = (sHdrH / 2 + 7) / 8;
        bloomDownDC.dispatchZ               = 1;
        renderer.Dispatch(bloomDownDC, resources);

        // アップサンプル: bloomHalf SRV → bloomFull UAV。
        PostProcCB fullData{};
        fullData.texelSize[0]  = 1.0f / static_cast<float>(sHdrW / 2);
        fullData.texelSize[1]  = 1.0f / static_cast<float>(sHdrH / 2);
        fullData.screenSize[0] = static_cast<float>(sHdrW);
        fullData.screenSize[1] = static_cast<float>(sHdrH);
        resources.Update(postprocCB, &fullData, sizeof(PostProcCB));

        renderer::ComputeCall bloomUpDC;
        bloomUpDC.shader             = bloomUpShader;
        bloomUpDC.constantBuffers[5] = postprocCB;
        bloomUpDC.srvInputs[10]      = bloomHalf;   // TEX_BLOOM = t10
        bloomUpDC.uavOutputs[0]      = bloomFull;
        bloomUpDC.dispatchX          = (sHdrW + 7) / 8;
        bloomUpDC.dispatchY          = (sHdrH + 7) / 8;
        bloomUpDC.dispatchZ          = 1;
        renderer.Dispatch(bloomUpDC, resources);
    }

    // =========================================================================
    // Pass 4: HDR とブルームを ACES トーンマッピングで LDR にコンポジットする。
    // FXAA 無効時は outputRT に直接書き込む。
    // =========================================================================
    renderer.SetRenderTarget(rs.fxaaEnabled ? ldrRT : outputRT, resources);

    PostProcCB postData{};
    postData.texelSize[0]  = 1.0f / static_cast<float>(sHdrW);
    postData.texelSize[1]  = 1.0f / static_cast<float>(sHdrH);
    postData.screenSize[0] = static_cast<float>(sHdrW);
    postData.screenSize[1] = static_cast<float>(sHdrH);
    postData.exposure      = rs.exposure;
    postData.fogDensity    = rs.fogEnabled ? rs.fogDensity : 0.0f;
    postData.fogFar        = rs.fogFar;
    postData.fogColor[0]   = rs.fogColor[0];
    postData.fogColor[1]   = rs.fogColor[1];
    postData.fogColor[2]   = rs.fogColor[2];
    resources.Update(postprocCB, &postData, sizeof(PostProcCB));

    renderer.SetSampler(0, renderer::SamplerMode::CLAMP_LINEAR);

    renderer::DrawCall compositeDC;
    compositeDC.shader             = compositeShader;
    compositeDC.pipelineState      = postprocPSO;
    compositeDC.vertexCount        = 3;
    compositeDC.constantBuffers[0] = frameCB;
    compositeDC.constantBuffers[5] = postprocCB;
    compositeDC.textures[5]        = resources.GetColorTexture(hdrRT, 0);
    compositeDC.textures[7]        = resources.GetDepthTexture(hdrRT);
    compositeDC.textures[10]       = rs.bloomEnabled ? bloomFull : renderer::ResourceHandle<renderer::TextureTag>{};
    renderer.Submit(compositeDC, resources);

    // =========================================================================
    // Pass 5: FXAA。LDR ターゲットをフィルタリングして outputRT に出力する。
    // =========================================================================
    if (rs.fxaaEnabled && fxaaShader.IsValid() && ldrRT.IsValid())
    {
        renderer.SetRenderTarget(outputRT, resources);

        renderer::DrawCall fxaaDC;
        fxaaDC.shader             = fxaaShader;
        fxaaDC.pipelineState      = postprocPSO;
        fxaaDC.vertexCount        = 3;
        fxaaDC.constantBuffers[5] = postprocCB;
        fxaaDC.textures[5]        = resources.GetColorTexture(ldrRT, 0);
        renderer.Submit(fxaaDC, resources);
    }
}

} // namespace fbzz::scene
