/// @file    RayReflectionReconstructionResources.hpp
/// @brief   検証済み terminal 対応と境界検証付き反射再構成。
/// @author  Hasegawa Jin
/// @date    2026-10-01
#pragma once
#include <Graphics/Renderer/ResourceHandle.hpp>
#include <Math/Vector4.hpp>
#include <array>
#include <cstdint>
#include <vector>

namespace fbzz::renderer {
struct RenderPassContext;
struct RenderViewResources;
struct RenderSharedResources;

/// @note Static-content keys prove unchanged terminal geometry/material; kind0 rejects moving reuse, kind1 is a reflected mesh terminal, kind2 is thin transmission with a known constant reflected environment.
/// @note terminalPositionParameter.w is zero for opaque and the first thin interface IOR for transmission.
struct RayReflectionMotionGuide {
    math::Vector4 terminalPositionParameter;
    std::array<uint32_t, 4> objectPrimitiveKind{};
};
static_assert(sizeof(RayReflectionMotionGuide) == 32);

/// @note owner index/generation、同内容版の dense material ID、有効度を保持する。Scene 世代はビューの exact 内容キーで検証する。
/// @note geometricNormalOffset.w は既存の法線方向 SpawnOffset。alpha0 は補完せず、glass は空間 filter を通さない。
struct RayReflectionSurface {
    math::Vector4 positionDepth;
    math::Vector4 normalRoughness;
    math::Vector4 geometricNormalOffset;
    std::array<uint32_t, 4> objectMaterialValid{};
    RayReflectionMotionGuide motion;
};
static_assert(sizeof(RayReflectionSurface) == 96);

/// @note FP32 RGB 平均と有効重み。historyLimit までは算術平均、その後は重みを固定した bounded EMA。
/// @note count の負符号は薄板の透過入射信号、正符号は全 RAW/opaque 信号。絶対値が有効重み。
/// @note RAW Monte Carlo 和や完全な移動平均ではなく、Reference の蓄積へ混ぜない。
struct RayReflectionHistoryRecord {
    math::Vector4 radianceCount;
};
static_assert(sizeof(RayReflectionHistoryRecord) == 16);

/// @note b0。cameraPosition.w は orthographic0/1。動くカメラは実 terminal の対応を検証し、受け手 velocity を鏡面 motion に代用しない。
struct alignas(16) RayReflectionReconstructionConstants {
    math::Vector4 cameraPosition, cameraRight, cameraUp, cameraForward;
    uint32_t width = 0, height = 0, resetHistory = 1, stage = 0;
    float previousJitterX = 0, previousJitterY = 0, currentJitterX = 0, currentJitterY = 0;
    uint32_t historyLimit = 32, spatialRadius = 1, hasGBuffer = 0, temporalAllowed = 1;
    math::Vector4 previousCameraPosition, previousCameraRight, previousCameraUp, previousCameraForward;
    math::Vector4 constantEnvironmentRadiance;
    float nearDistance = 0, farDistance = 0;
    uint32_t cameraMotion = 0, movingHistoryLimit = 4;
};
static_assert(sizeof(RayReflectionReconstructionConstants) == 208);

/// @note Surface96×2 と RGB/count16×2 の224B/画素、RGBA16F output を含め232B/画素。色も ping-pong して他画素の再投影 read/write race を防ぐ。
/// @note trace metadata=u2、temporal t5 raw/t14 current/t15 previous/t18 previous history -> u2 current history、spatial t5/t14/t15(+t16/17 GBuffer0/1) -> u0。
/// @note 一つのビューが単独所有する。停止・失敗・非連続 frame・内容変化は履歴を無効にする。
/// @note Spatial 成功時だけ共通 index を進める。途中失敗後は部分更新も次フレームで破棄する。
struct RayReflectionReconstructionViewResources {
    std::array<ResourceHandle<StructuredBufferTag>, 2> surfaces{};
    std::array<ResourceHandle<StructuredBufferTag>, 2> histories{};
    ResourceHandle<TextureTag> output;
    ResourceHandle<ConstantBufferTag> constants;
    RayReflectionReconstructionConstants constantsData;
    std::vector<uint32_t> contentKey;
    std::vector<uint32_t> cameraKey;
    RayReflectionReconstructionConstants committedCamera;
    uint32_t width = 0, height = 0, surfaceReadIndex = 0;
    uint64_t lastFrameStamp = 0;
    float previousJitterX = 0, previousJitterY = 0;
    bool prepared = false, historyValid = false, rawSucceeded = false, temporalSucceeded = false;
};

/// @return false は再構成だけを無効化し、同フレームの生 Reflection を維持する。
[[nodiscard]] bool PrepareRayReflectionReconstruction(RenderPassContext& context,
    RenderViewResources& view, RenderSharedResources& shared);
} /// @note namespace fbzz::renderer
