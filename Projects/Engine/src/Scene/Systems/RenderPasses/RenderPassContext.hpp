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
#include <Math/Frustum.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <Physics/Layer.hpp>
#include "OcclusionCuller.hpp"
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

    renderer::ResourceHandle<renderer::RenderTargetTag>   decalDepthRT;  // 深度専用 RT (decal 読み取り用コピー先)
    renderer::ResourceHandle<renderer::RenderTargetTag>   decalMaskRT;   // 除外オブジェクト描画先 (1-color)
    renderer::ResourceHandle<renderer::ShaderTag>         decalShader;
    renderer::ResourceHandle<renderer::ShaderTag>         decalMaskShader;
    renderer::ResourceHandle<renderer::PipelineStateTag>  decalPSO;
    renderer::ResourceHandle<renderer::PipelineStateTag>  decalMaskPSO;
    renderer::ResourceHandle<renderer::ConstantBufferTag> decalCB;

    // ── Shadow ────────────────────────────────────────────────────────────────
    renderer::ResourceHandle<renderer::ShaderTag>         shadowShader;
    renderer::ResourceHandle<renderer::ShaderTag>         shadowSkinnedShader;
    renderer::ResourceHandle<renderer::ConstantBufferTag> shadowCB;

    // ── 共用 PSO ─────────────────────────────────────────────────────────────
    renderer::ResourceHandle<renderer::PipelineStateTag>  defaultPSO;   // SOLID / OPAQUE / DEPTH_ON
    renderer::ResourceHandle<renderer::PipelineStateTag>  wireframePSO;

    // ── Sky ───────────────────────────────────────────────────────────────────
    renderer::ResourceHandle<renderer::ShaderTag>         skyShader;
    renderer::ResourceHandle<renderer::PipelineStateTag>  skyPSO;
    renderer::ResourceHandle<renderer::BufferTag>         skyVB;
    renderer::ResourceHandle<renderer::BufferTag>         skyIB;
    uint32_t                                              skyIndexCount = 0;
    renderer::ResourceHandle<renderer::ConstantBufferTag> atmosphereCB;

    // ── Particle ──────────────────────────────────────────────────────────────
    renderer::ResourceHandle<renderer::ShaderTag>         particleShader;
    renderer::ResourceHandle<renderer::PipelineStateTag>  particlePSO;
    renderer::ResourceHandle<renderer::BufferTag>         particleVB;
    renderer::ResourceHandle<renderer::BufferTag>         particleIB;

    // ── Deferred ジオメトリ ───────────────────────────────────────────────────
    renderer::ResourceHandle<renderer::ShaderTag>         gbufferShader;
    renderer::ResourceHandle<renderer::ShaderTag>         deferredLightingShader;
    renderer::ResourceHandle<renderer::ShaderTag>         depthCopyShader;
    renderer::ResourceHandle<renderer::ShaderTag>         skinnedPbrShader;
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

    renderer::LightConstantsCB lightData;   // RenderSystem が毎フレーム設定
    math::Matrix4               lightVP;    // 影投射ライトの VP 行列
    bool                        isDeferred  = false;
    bool                        ssaoEnabled = false;

    // カリング
    math::Frustum  cameraFrustum; // カメラ視錐台 (Gribb-Hartmann 法で VP から抽出)
    math::Frustum  lightFrustum;  // ライト視錐台 (シャドウパスのフラスタムカリング用)
    OcclusionCuller occlusionCuller; // CPU ソフトウェアオクルージョンカリング

    // レンダリング統計 (各パスでインクリメント → RenderSystem が Snapshot に書き出す)
    int statsTotalObjects    = 0; // フラスタムカリング前の候補オブジェクト数
    int statsFrustumCulled   = 0; // フラスタムで除外した数
    int statsOcclusionCulled = 0; // オクルージョンで除外した数
    int statsDrawCalls       = 0; // 実際に発行した DrawCall 数
    int statsVertexCount     = 0; // 描画頂点数の合計
    int statsTriangleCount   = 0; // 描画三角形数の合計
};

} // namespace fbzz::scene
