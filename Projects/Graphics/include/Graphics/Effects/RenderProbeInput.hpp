/// @file    RenderProbeInput.hpp
/// @brief   プローブ描画の値入力と結果。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
#include <Graphics/Pipeline/RenderConstants.hpp>
namespace fbzz::renderer {
inline constexpr size_t kLightProbeProjectCBSize = 448;
inline constexpr size_t kLightProbeDilateCBSize = 16;
struct RenderPassContext;
struct RenderReflectionProbeInput {
    math::Vector3 position;
    uint32_t sourceIndex = 0, sourceGeneration = 0;
    bool captureScene = false;
    ResourceHandle<RenderTargetTag> target;
    uint32_t resolution = 128;
    AtmosphereCB atmosphere;
};
struct RenderReflectionProbeResult {
    ResourceHandle<TextureTag> irradiance, prefilter;
    uint32_t prefilterMipCount = 0;
};
bool CaptureReflectionProbe(RenderPassContext&, const RenderReflectionProbeInput&, RenderReflectionProbeResult&);
struct RenderLightProbeInput {
    uint32_t sourceIndex = 0, sourceGeneration = 0;
    math::Vector3 boxMin, boxSize;
    std::array<int, 3> runtimeGrid{};
    std::array<ResourceHandle<RenderTargetTag>, 6> runtimeFaces{}, runtimeFacing{};
    ResourceHandle<TextureTag> runtimeRawVolume, runtimeVolume;
    int runtimeFaceSize = 0;
    bool rejectInsideGeometry = false;
    float deringing = 0;
};
void BakeLightProbe(RenderPassContext&, const RenderLightProbeInput&, int, ResourceHandle<TextureTag>);
void DilateLightProbeVolume(RenderPassContext&, const RenderLightProbeInput&);
}
