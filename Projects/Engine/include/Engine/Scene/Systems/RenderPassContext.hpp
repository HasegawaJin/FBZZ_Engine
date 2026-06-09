// FBZZ Engine
// RenderPassContext.hpp | fbzz::scene
// RenderGraph 注入パスと各描画パスが共有する実行コンテキスト
#pragma once

#include <Engine/Renderer/Camera.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/LightSystem.hpp>
#include <Engine/Renderer/RenderGraph.hpp>
#include <Engine/Renderer/RenderSettings.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/Systems/OcclusionCuller.hpp>
#include <Math/Frustum.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <Physics/Layer.hpp>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace fbzz::scene {

class Scene;
struct RenderPassContext;

// UserRenderPassInjectionPoint — Script が追加するパスを既存パイプラインのどこへ挿入するかを表す。
// WHY: RenderGraph は依存関係で実行順を決めるが、HDR へ ReadWrite する透明系パスは同じ依存を持ちやすい。
//      明示的な挿入点を持たせ、Water / VFX / PostProcess 前処理の意図をコードから読めるようにする。
enum class UserRenderPassInjectionPoint : uint8_t {
    AfterOpaque,
    AfterTransparent,
    BeforePostProcess
};

// UserRenderPassDesc — Script / Scene が RenderGraph へ追加したい 1 パス分の宣言。
// WHAT: reads / writes は RenderGraph 上の論理リソース名、execute は実際の描画処理を受け持つ。
//       execute は RenderSystem が保持する RenderPassContext を渡して呼ぶため、Script 側は renderer/resources/handles を参照できる。
struct UserRenderPassDesc {
    std::string name;
    UserRenderPassInjectionPoint injectionPoint = UserRenderPassInjectionPoint::AfterTransparent;
    std::vector<renderer::RenderGraph::ResourceAccess> accesses;
    std::function<void(RenderPassContext&)> execute;
    bool allowCulling = true;
};

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
    float ssaoIntensity;
    float customIntensity;
    float customBlend;
    float _customPad[2];
    float customParameters[4];
    float underwaterStrength;
    float underwaterDepth;
    float _underwaterPad[2];
    float underwaterColor[3];
    float underwaterFogDensity;
    float sharpenStrength;
    float sharpenRadius;
    float dofFocusDistance;
    float dofFocusRange;
    float dofBlurRadius;
    float sepiaIntensity;
    float invertIntensity;
    float posterizeLevels;
    float pixelSize;
    float _stylizedPad[3];
    float bloomThreshold;
    float bloomSoftKnee;
    float clarityStrength;
    float clarityRadius;
    float shadowLift;
    float highlightCompression;
    float colorFilterIntensity;
    float _qualityPad0;
    float colorFilter[3];
    float _qualityPad1;
};

struct OutlineCB {
    math::Vector4 color;
    float         width;
    float         _pad[3];
};

// DecalConstants (b2) — HLSL の DecalConstants cbuffer と完全に一致させること。
struct DecalCB {
    math::Matrix4 invDecalWorld;
    float         albedo[4];
    float         emissiveColor[3];
    float         emissiveScale;
    float         normalStrength;
    float         alpha;
    uint32_t      textureMask;
    float         _pad;
    math::Vector3 decalTangent;
    float         _pad1;
    math::Vector3 decalBitangent;
    float         _pad2;
    math::Vector3 decalNormal;
    float         _pad3;
};

struct RenderPassHandles {
    renderer::ResourceHandle<renderer::RenderTargetTag> shadowMapRT;
    renderer::ResourceHandle<renderer::RenderTargetTag> hdrRT;
    renderer::ResourceHandle<renderer::RenderTargetTag> ldrRT;
    renderer::ResourceHandle<renderer::RenderTargetTag> selectionMaskRT;
    renderer::ResourceHandle<renderer::RenderTargetTag> outlineRT;
    renderer::ResourceHandle<renderer::RenderTargetTag> customPostProcessRT[2];
    renderer::ResourceHandle<renderer::RenderTargetTag> gbufferRT;

    renderer::ResourceHandle<renderer::TextureTag> bloomHalf;
    renderer::ResourceHandle<renderer::TextureTag> bloomFull;
    renderer::ResourceHandle<renderer::TextureTag> ssaoRaw;
    renderer::ResourceHandle<renderer::TextureTag> ssaoBlur;
    renderer::ResourceHandle<renderer::TextureTag> shadowDepthTex;
    renderer::ResourceHandle<renderer::TextureTag> fxaaInput;
    renderer::ResourceHandle<renderer::TextureTag> postProcessInput;

    renderer::ResourceHandle<renderer::ShaderTag> bloomDownShader;
    renderer::ResourceHandle<renderer::ShaderTag> bloomUpShader;
    renderer::ResourceHandle<renderer::ShaderTag> ssaoShader;
    renderer::ResourceHandle<renderer::ShaderTag> ssaoBlurShader;
    renderer::ResourceHandle<renderer::ShaderTag> compositeShader;
    renderer::ResourceHandle<renderer::ShaderTag> causticsShader;
    renderer::ResourceHandle<renderer::ShaderTag> selectionMaskShader;
    renderer::ResourceHandle<renderer::ShaderTag> selectionMaskSkinnedShader;
    renderer::ResourceHandle<renderer::ShaderTag> selectionOutlineShader;
    renderer::ResourceHandle<renderer::ShaderTag> fxaaShader;
    std::vector<renderer::ResourceHandle<renderer::ShaderTag>> customPostProcessShaders;

    renderer::ResourceHandle<renderer::PipelineStateTag> selectionMaskPSO;
    renderer::ResourceHandle<renderer::PipelineStateTag> postprocPSO;
    renderer::ResourceHandle<renderer::PipelineStateTag> causticsPSO;

    renderer::ResourceHandle<renderer::ConstantBufferTag> frameCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> objectCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> lightCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> bindPoseSkinningCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> postprocCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> outlineCB;

    renderer::ResourceHandle<renderer::RenderTargetTag>   decalDepthRT;
    renderer::ResourceHandle<renderer::RenderTargetTag>   decalMaskRT;
    renderer::ResourceHandle<renderer::ShaderTag>         decalShader;
    renderer::ResourceHandle<renderer::ShaderTag>         decalMaskShader;
    renderer::ResourceHandle<renderer::PipelineStateTag>  decalPSO;
    renderer::ResourceHandle<renderer::PipelineStateTag>  decalMaskPSO;
    renderer::ResourceHandle<renderer::ConstantBufferTag> decalCB;

    renderer::ResourceHandle<renderer::ShaderTag>         shadowShader;
    renderer::ResourceHandle<renderer::ShaderTag>         shadowSkinnedShader;
    renderer::ResourceHandle<renderer::ConstantBufferTag> shadowCB;

    renderer::ResourceHandle<renderer::PipelineStateTag>  defaultPSO;
    renderer::ResourceHandle<renderer::PipelineStateTag>  wireframePSO;

    renderer::ResourceHandle<renderer::ShaderTag>         skyShader;
    renderer::ResourceHandle<renderer::PipelineStateTag>  skyPSO;
    renderer::ResourceHandle<renderer::BufferTag>         skyVB;
    renderer::ResourceHandle<renderer::BufferTag>         skyIB;
    uint32_t                                              skyIndexCount = 0;
    renderer::ResourceHandle<renderer::ConstantBufferTag> atmosphereCB;

    renderer::ResourceHandle<renderer::ShaderTag>         particleShader;
    renderer::ResourceHandle<renderer::PipelineStateTag>  particlePSO;
    renderer::ResourceHandle<renderer::PipelineStateTag>  particleAlphaPSO;
    renderer::ResourceHandle<renderer::BufferTag>         particleVB;
    renderer::ResourceHandle<renderer::BufferTag>         particleIB;

    renderer::ResourceHandle<renderer::ShaderTag>         trailShader;
    renderer::ResourceHandle<renderer::PipelineStateTag>  trailPSO;

    renderer::ResourceHandle<renderer::ShaderTag>         meshTrailShader;
    renderer::ResourceHandle<renderer::ShaderTag>         skinnedMeshTrailShader;
    renderer::ResourceHandle<renderer::PipelineStateTag>  meshTrailPSO;
    renderer::ResourceHandle<renderer::PipelineStateTag>  meshTrailDoubleSidedPSO;

    renderer::ResourceHandle<renderer::ShaderTag>         gbufferShader;
    renderer::ResourceHandle<renderer::ShaderTag>         deferredLightingShader;
    renderer::ResourceHandle<renderer::ShaderTag>         depthCopyShader;
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

    renderer::LightConstantsCB lightData;
    math::Matrix4               lightVP;
    bool                        isDeferred  = false;
    bool                        ssaoEnabled = false;

    const math::Frustum* cameraFrustum = nullptr;
    const math::Frustum* lightFrustum  = nullptr;
    OcclusionCuller* occlusionCuller = nullptr;

    int statsTotalObjects    = 0;
    int statsFrustumCulled   = 0;
    int statsOcclusionCulled = 0;
    int statsDrawCalls       = 0;
    int statsVertexCount     = 0;
    int statsTriangleCount   = 0;
};

} // namespace fbzz::scene
