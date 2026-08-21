// FBZZ Engine
// PostProcessPasses.hpp | fbzz::scene
// Post-process render pass entry points
#pragma once
#include <cstdint>

namespace fbzz::scene {

struct RenderPassContext;

void ExecuteBloomPass(RenderPassContext& ctx);
void ExecuteSSAOPass(RenderPassContext& ctx);
void ExecuteCausticsPass(RenderPassContext& ctx);
void ExecuteCompositePass(RenderPassContext& ctx);
void ExecuteFxaaPass(RenderPassContext& ctx);
void ExecuteCustomPostProcessPass(RenderPassContext& ctx, uint32_t customIndex, uint32_t outputIndex);

// ---- Advanced Graphics Passes ----

// IBL BRDF LUT をスタートアップ時に一度だけ焼く Compute パス。
// WHY: 512x512 の積分テーブルは全シーン共通で変わらないため、初回フレームのみ実行する。
void ExecuteIBLBakeBrdfLutPass(RenderPassContext& ctx);

// GTAO — SSAO の代替。Horizon-Based AO で接触部の陰を自然に表現する。
void ExecuteGTAOPass(RenderPassContext& ctx);

// SSR — スクリーンスペース反射。金属・濡れた床の映り込みをリアルタイムに計算する。
void ExecuteSSRPass(RenderPassContext& ctx);

// Volumetric Lighting — レイマーチで体積光（ゴッドレイ・光柱）を生成する。
void ExecuteVolumetricLightPass(RenderPassContext& ctx);

// Volumetric Cloud — 深度で遮蔽しながら雲層をレイマーチし、HDR へ合成する。
void ExecuteVolumetricCloudPass(RenderPassContext& ctx);

// 雲パラメータ CB (b2) を現在のシーンから更新し、有効な雲があれば true を返す。
// WHY: 体積光パスも同じ密度場を引いて雲の切れ間の光芒を作るため、雲パス実行の有無に
//      依らず CB の内容が保証されている必要がある。
bool UpdateVolumetricCloudConstants(RenderPassContext& ctx);

// Contact Shadows — スクリーンスペースで小物直下の接触影を高精度に生成する。
void ExecuteContactShadowsPass(RenderPassContext& ctx);

// TAA — テンポラルアンチエイリアシング。前フレームバッファと現フレームをブレンドする。
void ExecuteTAAPass(RenderPassContext& ctx);

// TAA_Blit — TAA 出力 (fxaaInput = taaHistoryA/B) を OutputRT に転送する。
// WHY: TAA は ping-pong 履歴バッファにのみ書き OutputRT には書かない。
//      FXAA が後続にない場合、この blit なしでは viewport が黒になる。
void ExecuteTAABlitPass(RenderPassContext& ctx);

// Motion Blur — カメラモーションブラー。深度再投影でモーションベクトルを生成して適用する。
void ExecuteMotionBlurPass(RenderPassContext& ctx);

// Lens Flare — スクリーンスペースレンズフレア。加算合成で HDR バッファに合成する。
void ExecuteLensFlarePass(RenderPassContext& ctx);

} // namespace fbzz::scene

