// FBZZ Engine
// RenderSystem.cpp | fbzz::scene
// MeshRenderer + Transform を走査し、DrawCall を発行する
#include "engine/Scene/Systems/RenderSystem.hpp"
#include "engine/Scene/Scene.hpp"
#include "engine/Scene/Components/MeshRenderer.hpp"
#include "engine/Scene/Transform.hpp"
#include "engine/Renderer/IRenderer.hpp"
#include "engine/Renderer/Camera.hpp"
#include "engine/Renderer/LightSystem.hpp"
#include "engine/Renderer/Mesh.hpp"
#include "engine/Renderer/Material.hpp"
#include "engine/Renderer/DrawCall.hpp"
#include "engine/Renderer/RenderState.hpp"
#include "engine/Renderer/SamplerMode.hpp"
#include "engine/Renderer/ShaderManager.hpp"
#include <math/Matrix4.hpp>
#include <math/Vector3.hpp>

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

// b4: ShadowConstants — シャドウパス未実装時のダミー定数
// lightViewProjection を UV が [0,1] の外に出るよう設定して
// ComputeShadow が常に 1.0 (影なし) を返すようにする。
struct ShadowConstantsCB {
    math::Matrix4 lightViewProjection;
    float         shadowMapTexelSize[2];
    float         shadowBias;
    float         _pad;
};

} // namespace

void RenderSystem(Scene& scene,
                  renderer::IRenderer& renderer,
                  const renderer::Camera& camera,
                  const renderer::LightSystem& lights)
{
    static auto frameCB  = renderer.CreateConstantBuffer(sizeof(PerFrameCB));
    static auto objectCB = renderer.CreateConstantBuffer(sizeof(PerObjectCB));
    static auto lightCB  = renderer.CreateConstantBuffer(sizeof(renderer::DirectionalLight));
    static auto shadowCB = renderer.CreateConstantBuffer(sizeof(ShadowConstantsCB));
    static auto pso      = renderer.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::OPAQUE,
        renderer::DepthMode::DEPTH_ON
    });

    // シャドウパス未実装: lightViewProjection の x 成分 (w=1 前提) を 2 にして
    // WorldToShadowUV が UV.x=1.5 > 1.0 となるよう強制し影なしを保証する。
    static bool shadowCBInited = false;
    if (!shadowCBInited) {
        ShadowConstantsCB shadowData{};
        shadowData.lightViewProjection.m[0][3] = 2.0f;  // clip.x = 2 → UV.x = 1.5
        shadowData.lightViewProjection.m[3][3] = 1.0f;  // clip.w = 1
        shadowData.shadowMapTexelSize[0] = 0.001f;
        shadowData.shadowMapTexelSize[1] = 0.001f;
        shadowData.shadowBias = 0.0f;
        shadowCB->Update(&shadowData, sizeof(ShadowConstantsCB));
        shadowCBInited = true;
    }

    // --- Per-frame upload ---
    PerFrameCB frameData{};
    frameData.view               = camera.GetViewMatrix();
    frameData.projection         = camera.GetProjectionMatrix();
    frameData.viewProjection     = camera.GetViewProjection();
    frameData.invViewProjection  = math::Matrix4::Inverse(frameData.viewProjection);
    frameData.cameraPos          = camera.m_position;
    frameData.nearZ              = camera.m_near;
    frameData.farZ               = camera.m_far;
    frameCB->Update(&frameData, sizeof(PerFrameCB));

    lights.Upload(*lightCB);

    renderer.SetSampler(0, renderer::SamplerMode::WRAP_ANISOTROPIC);

    // --- Per-object loop ---
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
        if (mr.material->albedoTexture)
            dc.textures[0] = mr.material->albedoTexture;

        renderer.Submit(dc);
    }
}

} // namespace fbzz::scene
