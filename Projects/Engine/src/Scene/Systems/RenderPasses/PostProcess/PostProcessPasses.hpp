/// @file    PostProcessPasses.hpp
/// @brief   Post-process render pass entry points.
/// @author  Hasegawa Jin
/// @date    2026-06-18
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
// HDR の段 (AfterOpaque / SceneHDR) のユーザーシェーダーを hdrRT へ適用する。
// blendMode が OPAQUE_BLEND なら «退避 → 描き戻し»、それ以外は直接。
// iterations / downscale が効くのはこちらだけ (CustomPostProcessPass.cpp の WHY)。
void ExecuteCustomHdrPass(RenderPassContext& ctx, uint32_t customIndex);
// .mat をキーにした解決済みマテリアルのキャッシュを破棄する。
// シーン切り替えやリソースリセットの際に呼ぶこと。
void ReleaseCustomPassMaterialCache();

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
// フロクセル ボリューメトリック フォグ。視錐台の 3D グリッドへ霧を焼いて Z 積分する。
// Shadow より後、Composite より前に走らせること (シャドウマップを読む)。
void ExecuteFroxelFogPass(RenderPassContext& ctx);
// 自動露出 (眼の順応)。HDR が出揃ってから Composite より前に走らせること。
// 結果は handles.exposureResult に残り、Composite が t29 で読む。
void ExecuteAutoExposurePass(RenderPassContext& ctx);
// 次のフレームで順応を飛ばして即座に露出を合わせる。シーン切り替えやカット割りで呼ぶ。
void RequestAutoExposureReset();

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
// 光源抽出のため bloomHalf を作業バッファとして上書きする (Bloom より前に走る前提)。
void ExecuteLensFlarePass(RenderPassContext& ctx);

} // namespace fbzz::scene

