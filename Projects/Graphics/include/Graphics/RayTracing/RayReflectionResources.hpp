/// @file    RayReflectionResources.hpp
/// @brief   一段反射のビュー資源と行列反転を使わない GPU カメラ定数。
/// @author  Hasegawa Jin
/// @date    2026-10-01
#pragma once
#include <Graphics/RayTracing/RayGeometryCache.hpp>
#include <Graphics/RayTracing/RayPathScene.hpp>
#include <Graphics/RayTracing/RayEnvironment.hpp>
#include <Graphics/RayTracing/RayReflectionReconstructionResources.hpp>
#include <Graphics/RayTracing/RayReflectionMediumCache.hpp>
#include <Math/Vector4.hpp>
#include <cstddef>

namespace fbzz::renderer {

/// @note RayReflection.cs.hlsl の b0 と一致。alpha0=invalid、1=opaque specular、2=dielectric full radiance。
/// @note cameraRight/up.w は半投影幅/高さ。near/far と Reversed-Z 深度からワールド位置を復元する。
struct RayReflectionConstants {
    math::Vector4 cameraPosition;
    math::Vector4 cameraRight;
    math::Vector4 cameraUp;
    math::Vector4 cameraForward;
    math::Vector4 lightDirection;
    math::Vector4 lightColorIntensity;
    math::Vector4 ambientRadiance;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t instanceCount = 0;
    uint32_t orthographic = 0;
    float nearDistance = 0;
    float farDistance = 0;
    float jitterNdcX = 0;
    float jitterNdcY = 0;
    uint32_t sampleCount = 4;
    uint32_t frameIndex = 0;
    uint32_t incomplete = 0;
    uint32_t iblReady = 0;
    float maxDistance = 0;
    uint32_t writeMetadata = 0;
    uint32_t diffuseIndirectEnabled = 0;
    uint32_t traceDistanceLimited = 0;
    uint32_t emitterCount = 0;
    uint32_t deltaLightCount = 0;
    uint32_t shapeCount = 0;
    uint32_t sceneLighting = 0;
    uint32_t environmentTableCount = 0;
    uint32_t environmentFaceSize = 0;
    float environmentRotation = 0;
    float environmentIntensity = 1;
    uint32_t environmentMode = 0;
    float environmentRadiance[3]{};
    /// @note SCREEN_FIRST は十分な SSR confidence の画素を trace 前に除外する。
    uint32_t reflectionResolveEnabled = 0;
    uint32_t reflectionSsrEnabled = 0;
    uint32_t glassEnabled = 0;
    uint32_t glassBoundaryLimit = 0;
    /// @note 1 は現 perspective camera origin が検証済み全 solid bounds 外。0 は従来の画素別媒体探索。
    uint32_t cameraOriginProvenAir = 0;
    /// @note bit0 は Hybrid 専用 AS の opaque/cull 証明、bit1 は smooth glass の単一 Fresnel 分岐。
    uint32_t hybridPolicyFlags = 0;
    /// @note Scene width/height は変更せず、粗い opaque 輸送だけ ceil(Scene/2) の独立資源を使う。
    uint32_t traceWidth = 0;
    uint32_t traceHeight = 0;
};
static_assert(sizeof(RayReflectionConstants) == 256);
static_assert(offsetof(RayReflectionConstants, cameraOriginProvenAir) == 240);
static_assert(offsetof(RayReflectionConstants, hybridPolicyFlags) == 244);
static_assert(offsetof(RayReflectionConstants, traceWidth) == 248);
static_assert(offsetof(RayReflectionConstants, traceHeight) == 252);

struct RayReflectionLightingResources {
    ResourceHandle<StructuredBufferTag> emitters, deltaLights, shapes, environmentTable;
    ResourceHandle<TextureTag> environment;
    uint32_t emitterCount = 0, deltaLightCount = 0, shapeCount = 0;
    uint32_t environmentTableCount = 0, environmentFaceSize = 0;
    float environmentRotation = 0, environmentIntensity = 1;
    bool sceneLighting = false;
    bool constantEnvironmentKnown = false;
    math::Vector3 constantEnvironmentRadiance;
    bool glassEnabled = false;
    bool diffuseIndirectEnabled = false;
    bool cameraOriginProvenAir = false;
    math::Vector3 provenAirOrigin;
};

/// @note output と guide/history は Scene extent、halfRaw は粗い opaque 輸送だけの RGBA16F。scene/gpu は同じ静的表面版を指す。
struct RayReflectionViewResources {
    ResourceHandle<TextureTag> output;
    ResourceHandle<TextureTag> halfRaw;
    ResourceHandle<ConstantBufferTag> constants;
    uint32_t width = 0;
    uint32_t height = 0;
    /// @note 再構成・半解像度資源の準備失敗時は divisor1 と Scene extent に戻す。
    uint32_t traceWidth = 0;
    uint32_t traceHeight = 0;
    uint32_t resolutionDivisor = 1;
    RayScene scene;
    RaySceneGpu gpu;
    RayPathSceneBuilder sceneBuilder;
    RayReflectionMediumCache mediumCache;
    RayPathScene pathScene;
    ResourceHandle<StructuredBufferTag> emitters, deltaLights, shapes, environmentTable;
    ResourceHandle<TextureTag> environment;
    RayEnvironmentDistribution environmentDistribution;
    uint64_t emitterRevision = 0;
    bool sceneLighting = false;
    bool diffuseIndirectEnabled = false;
    bool constantEnvironmentKnown = false;
    math::Vector3 constantEnvironmentRadiance;
    bool cameraOriginProvenAir = false;
    math::Vector3 provenAirOrigin;
    RayReflectionReconstructionViewResources reconstruction;
};

} /// @note namespace fbzz::renderer
