/// @file    PostProcessPasses.hpp
/// @brief   Graphics のポスト処理と Engine 固有拡張。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#pragma once
#include <Graphics/Passes/PostProcess/PostProcessPasses.hpp>
#include <Engine/Scene/Systems/RenderPasses/IRenderPass.hpp>
namespace fbzz::scene {
using renderer::ExecuteBloomPass;
using renderer::ExecuteSSAOPass;
using renderer::ExecuteCausticsPass;
using renderer::ExecuteCompositePass;
using renderer::ExecuteFxaaPass;
using renderer::ExecuteUpscalePass;
using renderer::ExecuteIBLBakeBrdfLutPass;
using renderer::ExecuteGTAOPass;
using renderer::ExecuteSSRPass;
using renderer::ExecuteVolumetricLightPass;
using renderer::ExecuteFroxelFogPass;
using renderer::ExecuteAutoExposurePass;
using renderer::RequestAutoExposureReset;
using renderer::ExecuteVolumetricCloudPass;
using renderer::UpdateVolumetricCloudConstants;
using renderer::ExecuteContactShadowsPass;
using renderer::ExecuteTAAPass;
using renderer::ExecuteTAABlitPass;
using renderer::ExecuteMotionBlurPass;
using renderer::ExecuteLensFlarePass;
using renderer::GTAOPass;
using renderer::ContactShadowsPass;
using renderer::SSAOPass;
using renderer::SSRPass;
using renderer::VolumetricCloudPass;
using renderer::IBLBrdfBakePass;
using renderer::VolumetricLightPass;
using renderer::FroxelFogPass;
using renderer::AutoExposurePass;
using renderer::LensFlarePass;
using renderer::BloomPass;
void ExecuteUIPass(RenderPassContext& ctx);
using renderer::ExecuteCustomPostProcessPass;
using renderer::ExecuteCustomHdrPass;
void ReleaseCustomPassMaterialCache();

}
