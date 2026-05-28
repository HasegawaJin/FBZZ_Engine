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
#include <vector>

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
    float bloomIntensity;
    float fogColor[3];
    float fogFar;
    float contrast;
    float saturation;
    float hueShift;
    float temperature;
    float tint;
    float vignetteIntensity;
    float vignetteSmoothness;
    float vignetteRoundness;
    float vignetteColor[3];
    float filmGrainIntensity;
    float filmGrainResponse;
    float chromaticAberration;
    float lensDistortion;
    float customIntensity;
    float customBlend;
    float customParameters[4];
    float _pad[2];
};

struct OutlineCB {
    math::Vector4 color;
    float         width;
    float         _pad[3];
};

// DecalConstants (b2) — HLSL の DecalConstants cbuffer と完全に一致させること。
struct DecalCB {
    math::Matrix4 invDecalWorld;    // ワールド→デカールローカル (64 bytes)
    float         albedo[4];        // RGBA アルベドカラー        (16 bytes)
    float         emissiveColor[3]; // エミッシブカラー           (12 bytes)
    float         emissiveScale;    //                            ( 4 bytes)
    float         normalStrength;   //                            ( 4 bytes)
    float         alpha;            // フェードアルファ           ( 4 bytes)
    uint32_t      textureMask;      // bit0=albedo bit1=normal bit2=emissive bit3=decalMask ( 4 bytes)
    float         _pad;             //                            ( 4 bytes)
    math::Vector3 decalTangent;     // Decal local +X. Normal-map T axis.
    float         _pad1;
    math::Vector3 decalBitangent;   // Decal local +Z. Projected UV V axis.
    float         _pad2;
    math::Vector3 decalNormal;      // Base normal for flat normal maps.
    float         _pad3;
};                                  // 160 bytes (16 byte aligned)

struct RenderPassHandles {
    renderer::ResourceHandle<renderer::RenderTargetTag> shadowMapRT;
    renderer::ResourceHandle<renderer::RenderTargetTag> hdrRT;
    renderer::ResourceHandle<renderer::RenderTargetTag> ldrRT;
    renderer::ResourceHandle<renderer::RenderTargetTag> selectionMaskRT;
    renderer::ResourceHandle<renderer::RenderTargetTag> outlineRT;
    renderer::ResourceHandle<renderer::RenderTargetTag> customPostProcessRT[2];

    renderer::ResourceHandle<renderer::TextureTag> bloomHalf;
    renderer::ResourceHandle<renderer::TextureTag> bloomFull;
    renderer::ResourceHandle<renderer::TextureTag> shadowDepthTex;
    renderer::ResourceHandle<renderer::TextureTag> fxaaInput;
    renderer::ResourceHandle<renderer::TextureTag> postProcessInput;

    renderer::ResourceHandle<renderer::ShaderTag> bloomDownShader;
    renderer::ResourceHandle<renderer::ShaderTag> bloomUpShader;
    renderer::ResourceHandle<renderer::ShaderTag> compositeShader;
    renderer::ResourceHandle<renderer::ShaderTag> selectionMaskShader;
    renderer::ResourceHandle<renderer::ShaderTag> selectionMaskSkinnedShader;
    renderer::ResourceHandle<renderer::ShaderTag> selectionOutlineShader;
    renderer::ResourceHandle<renderer::ShaderTag> fxaaShader;
    std::vector<renderer::ResourceHandle<renderer::ShaderTag>> customPostProcessShaders;

    renderer::ResourceHandle<renderer::PipelineStateTag> selectionMaskPSO;
    renderer::ResourceHandle<renderer::PipelineStateTag> postprocPSO;

    renderer::ResourceHandle<renderer::ConstantBufferTag> frameCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> objectCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> lightCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> bindPoseSkinningCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> postprocCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> outlineCB;

    renderer::ResourceHandle<renderer::RenderTargetTag>   decalDepthRT;  // 深度専用 RT (decal 読み取り用コピー先)
    renderer::ResourceHandle<renderer::RenderTargetTag>   decalMaskRT;   // 除外オブジェクト描画先 (1-color)
    renderer::ResourceHandle<renderer::ShaderTag>         decalShader;
    renderer::ResourceHandle<renderer::ShaderTag>         decalMaskShader;
    renderer::ResourceHandle<renderer::PipelineStateTag>  decalPSO;
    renderer::ResourceHandle<renderer::PipelineStateTag>  decalMaskPSO;
    renderer::ResourceHandle<renderer::ConstantBufferTag> decalCB;
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
