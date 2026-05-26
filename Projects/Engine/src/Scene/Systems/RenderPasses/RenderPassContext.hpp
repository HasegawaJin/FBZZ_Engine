// FBZZ Engine
// RenderPassContext.hpp | fbzz::scene
// RenderSystem pass shared state
#pragma once

#include <Engine/Renderer/Camera.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/LightSystem.hpp>
#include <Engine/Renderer/RenderSettings.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <Physics/Layer.hpp>
#include <cstdint>

namespace fbzz::scene {

class Scene;

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

struct PerObjectCB {
    math::Matrix4 world;
    math::Matrix4 worldInvTranspose;
};

struct ShadowConstantsCB {
    math::Matrix4 lightViewProjection;
    float         shadowMapTexelSize[2];
    float         shadowBias;
    float         _pad;
};

struct AtmosphereCB {
    float rayleighScattering[3];
    float mieScattering;
    float planetRadius;
    float atmosphereRadius;
    float sunIntensity;
    float mieG;
};

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

struct OutlineCB {
    math::Vector4 color;
    float         width;
    float         _pad[3];
};

struct RenderPassHandles {
    renderer::ResourceHandle<renderer::RenderTargetTag> shadowMapRT;
    renderer::ResourceHandle<renderer::RenderTargetTag> hdrRT;
    renderer::ResourceHandle<renderer::RenderTargetTag> ldrRT;
    renderer::ResourceHandle<renderer::RenderTargetTag> selectionMaskRT;
    renderer::ResourceHandle<renderer::RenderTargetTag> outlineRT;

    renderer::ResourceHandle<renderer::TextureTag> bloomHalf;
    renderer::ResourceHandle<renderer::TextureTag> bloomFull;
    renderer::ResourceHandle<renderer::TextureTag> shadowDepthTex;
    renderer::ResourceHandle<renderer::TextureTag> fxaaInput;

    renderer::ResourceHandle<renderer::ShaderTag> bloomDownShader;
    renderer::ResourceHandle<renderer::ShaderTag> bloomUpShader;
    renderer::ResourceHandle<renderer::ShaderTag> compositeShader;
    renderer::ResourceHandle<renderer::ShaderTag> selectionMaskShader;
    renderer::ResourceHandle<renderer::ShaderTag> selectionMaskSkinnedShader;
    renderer::ResourceHandle<renderer::ShaderTag> selectionOutlineShader;
    renderer::ResourceHandle<renderer::ShaderTag> fxaaShader;

    renderer::ResourceHandle<renderer::PipelineStateTag> selectionMaskPSO;
    renderer::ResourceHandle<renderer::PipelineStateTag> postprocPSO;

    renderer::ResourceHandle<renderer::ConstantBufferTag> frameCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> objectCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> bindPoseSkinningCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> postprocCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> outlineCB;
};

struct RenderPassContext {
    Scene& scene;
    renderer::IRenderer& renderer;
    renderer::ResourceManager& resources;
    const renderer::Camera& camera;
    const renderer::RenderSettings& settings;
    renderer::ResourceHandle<renderer::RenderTargetTag> outputRT;
    fbzz::LayerMask cullingMask;

    RenderPassHandles& handles;
    uint32_t width = 0;
    uint32_t height = 0;
    bool selectionOutlineEnabled = false;
};

} // namespace fbzz::scene
