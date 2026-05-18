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

struct PerFrameCB {
    math::Matrix4 viewProjection;
    math::Vector3 cameraPos;
    float         _pad = 0.0f;
};

struct PerObjectCB {
    math::Matrix4 world;
};

} // namespace

void RenderSystem(Scene& scene,
                  renderer::IRenderer& renderer,
                  const renderer::Camera& camera,
                  const renderer::LightSystem& lights)
{
    // 初回のみ生成。以降は毎フレーム Upload だけ行う
    static auto frameCB  = renderer.CreateConstantBuffer(sizeof(PerFrameCB));
    static auto objectCB = renderer.CreateConstantBuffer(sizeof(PerObjectCB));
    static auto lightCB  = renderer.CreateConstantBuffer(sizeof(renderer::DirectionalLight));
    static auto pso      = renderer.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::OPAQUE,
        renderer::DepthMode::DEPTH_ON
    });

    // --- Per-frame upload ---
    PerFrameCB frameData;
    frameData.viewProjection = camera.GetViewProjection();
    frameData.cameraPos      = camera.m_position;
    frameCB->Update(&frameData, sizeof(PerFrameCB));

    lights.Upload(*lightCB);

    renderer.SetSampler(0, renderer::SamplerMode::WRAP_ANISOTROPIC);

    // --- Per-object loop ---
    for (auto [tf, mr] : scene.View<Transform, MeshRenderer>()) {
        if (!mr.enabled || !mr.mesh || !mr.material) continue;
        if (!mr.mesh->vertexBuffer || !mr.mesh->indexBuffer) continue;
        if (!mr.material->shader) continue;

        PerObjectCB objData{ tf.GetWorldMatrix() };
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
        if (mr.material->albedoTexture)
            dc.textures[0] = mr.material->albedoTexture;

        renderer.Submit(dc);
    }
}

} // namespace fbzz::scene
