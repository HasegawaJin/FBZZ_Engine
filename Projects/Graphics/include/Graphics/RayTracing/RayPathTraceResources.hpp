/// @file    RayPathTraceResources.hpp
/// @brief   Reference Path の生 FP32 蓄積とビュー表面出力。
/// @author  Hasegawa Jin
/// @date    2026-10-01
#pragma once
#include <Graphics/RayTracing/RayGeometryCache.hpp>
#include <Graphics/RayTracing/RayPathHistory.hpp>
#include <Graphics/RayTracing/RayPathScene.hpp>
#include <Graphics/RayTracing/RayEnvironment.hpp>
#include <Graphics/RayTracing/RayGameResources.hpp>

namespace fbzz::renderer {

/// @note GPU RAW は露出前の FP32 和と整数 sample count。表示用 RGBA16F を履歴へ戻さない。
/// @note UINT32_MAX は GPU failure の sticky 診断であり平均を持たない。内容 key の reset だけで解除する。
struct RayPathHistoryRecord {
    math::Vector3 radianceSum;
    uint32_t sampleCount = 0;
};
static_assert(sizeof(RayPathHistoryRecord) == 16);

/// @note materialId は immutable surface table の dense instance 添字。valid=0 は背景、UINT32_MAX は sticky GPU 診断。
struct RayPathIdRecord {
    uint32_t objectIndex = 0;
    uint32_t objectGeneration = 0;
    uint32_t materialId = 0;
    uint32_t valid = 0;
};
static_assert(sizeof(RayPathIdRecord) == 16);

/// @note b0=208 bytes。environmentMode: 0=black、1=明示定数、2=生 cube (t4)。shape=t6、environment CDF=t7。
/// @note finite bounce 上限は明示した切り詰め。clamp/denoise/SSR/probe/ambient 近似を RAW に適用しない。
struct RayPathTraceConstants {
    math::Vector4 cameraPosition;
    math::Vector4 cameraRight;
    math::Vector4 cameraUp;
    math::Vector4 cameraForward;
    math::Vector4 lightDirection;
    math::Vector4 lightRadiance;
    math::Vector4 environmentRadiance;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t instanceCount = 0;
    uint32_t emitterCount = 0;
    float nearDistance = 0;
    float farDistance = 0;
    uint32_t orthographic = 0;
    uint32_t environmentMode = 0;
    uint32_t maxBounces = 8;
    uint32_t samplesPerDispatch = 1;
    uint32_t sampleBase = 0;
    uint32_t resetHistory = 1;
    uint32_t rouletteStart = 3;
    uint32_t enableNee = 1;
    uint32_t enableMis = 1;
    uint32_t samplerSeed = 0;
    uint32_t deltaLightCount = 0;
    uint32_t shapeLightCount = 0;
    std::array<uint32_t, 2> padding{};
    uint32_t environmentTableCount = 0;
    uint32_t environmentFaceSize = 0;
    float environmentRotation = 0;
    float environmentIntensity = 1;
};
static_assert(sizeof(RayPathTraceConstants) == 208);

/// @note auxiliary surface は center ray。radiance AA のサンプル位置や frameStamp で履歴をリセットしない。
struct RayPathViewResources {
    ResourceHandle<TextureTag> output;
    ResourceHandle<TextureTag> firstSurface;
    ResourceHandle<TextureTag> firstMaterial;
    ResourceHandle<TextureTag> firstGeometry;
    ResourceHandle<StructuredBufferTag> historyBuffer;
    ResourceHandle<StructuredBufferTag> idsBuffer;
    ResourceHandle<ConstantBufferTag> constants;
    ResourceHandle<StructuredBufferTag> emitters;
    ResourceHandle<StructuredBufferTag> deltaLights;
    ResourceHandle<StructuredBufferTag> shapes;
    ResourceHandle<StructuredBufferTag> environmentTable;
    ResourceHandle<TextureTag> environment;
    RayEnvironmentDistribution environmentDistribution;
    uint32_t width = 0;
    uint32_t height = 0;
    RayScene scene;
    RaySceneGpu gpu;
    RayPathSceneBuilder sceneBuilder;
    RayPathScene pathScene;
    uint64_t emitterRevision = 0;
    RayPathTraceConstants constantsData;
    RayPathHistory history;
    bool dispatchSucceeded = false;
    RayGameViewResources game;
    bool gameProfile = false;
};

/// @return カメラ契約が無効なら false。カメラ以外の integrator 設定は変更しない。
[[nodiscard]] bool MakeRayPathCameraConstants(const Camera& camera, RayPathTraceConstants& output);

} /// @note namespace fbzz::renderer
