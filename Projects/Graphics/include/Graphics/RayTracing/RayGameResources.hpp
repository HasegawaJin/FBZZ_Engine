/// @file    RayGameResources.hpp
/// @brief   Game Path の新規輸送成分とビュー別ノイズ再構成履歴。
/// @author  Hasegawa Jin
/// @date    2026-10-01
#pragma once
#include <Graphics/Renderer/Camera.hpp>
#include <Graphics/Renderer/ResourceHandle.hpp>
#include <Math/Vector4.hpp>
#include <array>
#include <cstdint>
#include <vector>

namespace fbzz::renderer {

/// @note 生 FP32 成分。first receiver の full BSDF 比で配分し、選択した lobe のタグでは分離しない。
/// @see https://pbr-book.org/4ed/Light_Transport_I_Surface_Reflection/A_Better_Path_Tracer Full BSDF throughput
struct RayGameTransportRecord {
    math::Vector4 diffuse;
    math::Vector4 specular;
    math::Vector4 independent;
};
static_assert(sizeof(RayGameTransportRecord) == 48);

/// @note valid: 1=Raster 照合済み、2=背景、3=中心境界の recast 補完、UINT_MAX=輸送失敗。
/// @note sceneGeneration を含む完全 ID。materialId は同じ view 内容版の dense instance 添字。
struct RayReconstructionSurface {
    math::Vector4 positionDepth;
    math::Vector4 normalRoughness;
    math::Vector4 albedoMetallic;
    math::Vector4 geometricNormalHitDistance;
    math::Vector4 previousPositionValid;
    std::array<uint32_t, 4> objectMaterialValid{};
    std::array<uint32_t, 4> sceneFlags{};
};
static_assert(sizeof(RayReconstructionSurface) == 112);

/// @note diffuse.w は有効履歴長。specular は hit motion 未供給の初期契約で時間方向へ再利用しない。
struct RayReconstructionHistoryRecord {
    math::Vector4 diffuse;
    math::Vector4 specular;
    RayReconstructionSurface surface;
};
static_assert(sizeof(RayReconstructionHistoryRecord) == 144);

/// @note CPU Matrix4 と同じ row-major storage。GPU は column vector で変換する。
struct RayGameMotionRecord {
    math::Matrix4 currentWorldInverse = math::Matrix4::Identity();
    math::Matrix4 previousWorld = math::Matrix4::Identity();
    uint32_t temporalValid = 0;
    std::array<uint32_t, 3> reserved{};
};
static_assert(sizeof(RayGameMotionRecord) == 144);

/// @note b1。未束縛の Reference は gameMode=0 として既存 b0 と UAV layout を保つ。
struct alignas(16) RayGameTraceConstants {
    uint32_t gameMode = 1;
    uint32_t frameSampleIndex = 0;
    std::array<uint32_t, 2> reserved{};
};

/// @note previous camera basis で再投影し、逆 VP の far/ortho 特異点に依存しない。
struct alignas(16) RayReconstructionConstants {
    math::Vector4 previousCameraPosition;
    math::Vector4 previousCameraRight;
    math::Vector4 previousCameraUp;
    math::Vector4 previousCameraForward;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t previousOrthographic = 0;
    uint32_t resetHistory = 1;
    uint32_t stage = 0;
    uint32_t diffuseHistoryLimit = 8;
    uint32_t spatialRadius = 1;
    uint32_t reserved = 0;
};
static_assert(sizeof(RayReconstructionConstants) == 96);

/// @note initial cut: translation >1m、forward rotation >45deg、射影入力の変更。通常の動きは画素照合へ渡す。
[[nodiscard]] bool IsRayGameCameraCut(const Camera& previous, const Camera& current);
/// @note ID/material/position/normal の検証を共有する CPU 契約。履歴に未計算の黒を観測値として入れない。
[[nodiscard]] bool IsRayReconstructionHistoryCompatible(
    const RayReconstructionSurface& current, const RayReconstructionSurface& previous, const Camera& previousCamera);

/// @note Reference RAW と独立して view が所有する。frameSampleIndex は成功 dispatch 後だけ進める。
struct RayGameViewResources {
    ResourceHandle<StructuredBufferTag> transport;
    ResourceHandle<StructuredBufferTag> surface;
    std::array<ResourceHandle<StructuredBufferTag>, 2> reconstructionHistory{};
    ResourceHandle<StructuredBufferTag> motionInstances;
    ResourceHandle<ConstantBufferTag> traceConstants;
    ResourceHandle<ConstantBufferTag> reconstructionConstants;
    RayGameTraceConstants traceData;
    RayReconstructionConstants reconstructionData;
    Camera previousCamera;
    std::vector<uint32_t> contentKey;
    uint32_t frameSampleIndex = 0;
    uint32_t historyReadIndex = 0;
    uint64_t lastFrameStamp = 0;
    bool prepared = false;
    bool historyValid = false;
    bool reconstructed = false;
    bool temporalSucceeded = false;
};

} /// @note namespace fbzz::renderer
