/// @file    RenderLightExtractor.hpp
/// @brief   ライト・影・Cookie の抽出入力。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
#include <Graphics/Renderer/RenderLightingInput.hpp>
namespace fbzz::scene {
class Scene;
struct RenderLightExtraction : renderer::RenderLightingInput {
    bool dirCastShadows = true;
    float dirShadowBias = 1;
    float dirShadowDistance = 0;
};
RenderLightExtraction ExtractRenderLights(Scene&, const renderer::Camera&, const renderer::RenderSettings&, uint32_t punctualShadowRes);
}
