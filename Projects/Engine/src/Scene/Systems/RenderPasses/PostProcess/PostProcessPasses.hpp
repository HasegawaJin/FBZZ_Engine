/// @file    PostProcessPasses.hpp
/// @brief   Post-process render pass entry points.
/// @author  Hasegawa Jin
/// @date    2026-06-18
#pragma once
#include <Engine/Scene/Systems/RenderPasses/IRenderPass.hpp>

#include <cstdint>

namespace fbzz::scene {

struct RenderPassContext;

void ExecuteBloomPass(RenderPassContext& ctx);
void ExecuteSSAOPass(RenderPassContext& ctx);
void ExecuteCausticsPass(RenderPassContext& ctx);
void ExecuteCompositePass(RenderPassContext& ctx);
void ExecuteFxaaPass(RenderPassContext& ctx);
/// 内部解像度で仕上がった ctx.chainOutputRT を ctx.outputRT の実寸へ解像する。
/// 等倍のフレームでは登録されない (チェーンが outputRT へ直接書き終えている)。
void ExecuteUpscalePass(RenderPassContext& ctx);
/// UI を outputRT へ合成する。ポストプロセスではないが «最終出力へ載せる» 段の一員。
/// ctx.uiOptions が null / 無効なら何もしない。
void ExecuteUIPass(RenderPassContext& ctx);
void ExecuteCustomPostProcessPass(RenderPassContext& ctx, uint32_t customIndex, uint32_t outputIndex);
/// HDR の段 (AfterOpaque / SceneHDR) のユーザーシェーダーを hdrRT へ適用する。
/// blendMode が OPAQUE_BLEND なら «退避 → 描き戻し»、それ以外は直接。
/// iterations / downscale が効くのはこちらだけ (理由は CustomPostProcessPass.cpp を参照)。
void ExecuteCustomHdrPass(RenderPassContext& ctx, uint32_t customIndex);
/// .mat をキーにした解決済みマテリアルのキャッシュを破棄する。
/// シーン切り替えやリソースリセットの際に呼ぶこと。
void ReleaseCustomPassMaterialCache();


/// IBL BRDF LUT をスタートアップ時に一度だけ焼く Compute パス。
/// @note 512x512 の積分テーブルは全シーン共通で変わらないため、初回フレームのみ実行する。
void ExecuteIBLBakeBrdfLutPass(RenderPassContext& ctx);

/// GTAO — SSAO の代替。Horizon-Based AO で接触部の陰を自然に表現する。
void ExecuteGTAOPass(RenderPassContext& ctx);

/// SSR — スクリーンスペース反射。金属・濡れた床の映り込みをリアルタイムに計算する。
void ExecuteSSRPass(RenderPassContext& ctx);

/// Volumetric Lighting — レイマーチで体積光（ゴッドレイ・光柱）を生成する。
void ExecuteVolumetricLightPass(RenderPassContext& ctx);
/// フロクセル ボリューメトリック フォグ。視錐台の 3D グリッドへ霧を焼いて Z 積分する。
/// Shadow より後、Composite より前に走らせること (シャドウマップを読む)。
void ExecuteFroxelFogPass(RenderPassContext& ctx);
/// 自動露出 (眼の順応)。HDR が出揃ってから Composite より前に走らせること。
/// 結果は handles.exposureResult に残り、Composite が t29 で読む。
void ExecuteAutoExposurePass(RenderPassContext& ctx);
/// 次のフレームで順応を飛ばして即座に露出を合わせる。シーン切り替えやカット割りで呼ぶ。
void RequestAutoExposureReset();

/// Volumetric Cloud — 深度で遮蔽しながら雲層をレイマーチし、HDR へ合成する。
void ExecuteVolumetricCloudPass(RenderPassContext& ctx);

/// 雲パラメータ CB (b2) を現在のシーンから更新し、有効な雲があれば true を返す。
/// @note 体積光パスも同じ密度場を引いて雲の切れ間の光芒を作るため、雲パス実行の有無に
///       依らず CB の内容が保証されている必要がある。
bool UpdateVolumetricCloudConstants(RenderPassContext& ctx);

/// Contact Shadows — スクリーンスペースで小物直下の接触影を高精度に生成する。
void ExecuteContactShadowsPass(RenderPassContext& ctx);

/// TAA — テンポラルアンチエイリアシング。前フレームバッファと現フレームをブレンドする。
void ExecuteTAAPass(RenderPassContext& ctx);

/// TAA_Blit — TAA 出力 (fxaaInput = taaHistoryA/B) を OutputRT に転送する。
/// @note TAA は ping-pong 履歴バッファにのみ書き OutputRT には書かない。
///       FXAA が後続にない場合、この blit なしでは viewport が黒になる。
void ExecuteTAABlitPass(RenderPassContext& ctx);

/// Motion Blur — カメラモーションブラー。深度再投影でモーションベクトルを生成して適用する。
void ExecuteMotionBlurPass(RenderPassContext& ctx);

/// Lens Flare — スクリーンスペースレンズフレア。加算合成で HDR バッファに合成する。
/// 光源抽出のため bloomHalf を作業バッファとして上書きする (Bloom より前に走る前提)。
void ExecuteLensFlarePass(RenderPassContext& ctx);

/// どれも Forward のプリパス経路と Deferred 経路の両方に登録される。違うのは
/// «実行順のどこに置くか» だけで、申告も有効条件も本体も同じ。
/// @note ラムダでなくクラスにする理由: 申告と有効条件と本体を 1 箇所へまとめ、«読んでいるのに
///       申告していない» (Terrain/DeferredLighting で実際に発生) を防ぐ。
/// @note 描き先の束縛は各本体が行う。SetAutoTarget は呼ばない。

class GTAOPass final : public IRenderPass {
public:
    std::string_view Name() const override;
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    bool IsEnabled(const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

class ContactShadowsPass final : public IRenderPass {
public:
    std::string_view Name() const override;
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    bool IsEnabled(const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

class SSAOPass final : public IRenderPass {
public:
    std::string_view Name() const override;
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    bool IsEnabled(const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

class SSRPass final : public IRenderPass {
public:
    std::string_view Name() const override;
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    bool IsEnabled(const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

class VolumetricCloudPass final : public IRenderPass {
public:
    std::string_view Name() const override;
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

/// 512x512 の BRDF 積分テーブルを 1 度だけ焼く。
/// @note 出力は論理リソースではない外部 ComputeTexture。書き先を申告できない以上
///       «誰も読まない» と判定されるので、カリングを禁止しないと必ず刈られる。
class IBLBrdfBakePass final : public IRenderPass {
public:
    std::string_view Name() const override { return "IBLBrdfBake"; }
    void Setup(PassBuilder&, const RenderPassContext&) const override {}
    bool AllowCulling() const override { return false; }
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

class VolumetricLightPass final : public IRenderPass {
public:
    std::string_view Name() const override { return "VolumetricLight"; }
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    bool IsEnabled(const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

/// フロクセル霧。
/// @note 出力はボリュームテクスチャなので書き先を申告できない。Particle が同じフレームの
///       霧を読んで «自分の奥行きの霧» を逆算する (ApplyParticleFog) ため、登録順で先に置く。
class FroxelFogPass final : public IRenderPass {
public:
    std::string_view Name() const override { return "FroxelFog"; }
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    bool AllowCulling() const override { return false; }
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

/// @note HDR を読むだけで書かない。出力は StructuredBuffer (Composite が t29 で読む) で
///       グラフの論理リソースに乗らないため、FroxelFog と同じ理由でカリングを禁止する。
class AutoExposurePass final : public IRenderPass {
public:
    std::string_view Name() const override { return "AutoExposure"; }
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    bool IsEnabled(const RenderPassContext& ctx) const override;
    bool AllowCulling() const override { return false; }
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

/// @note bloomHalf を光源抽出の作業バッファとして上書きするので、Bloom より前に走る前提。
class LensFlarePass final : public IRenderPass {
public:
    std::string_view Name() const override { return "LensFlare"; }
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    bool IsEnabled(const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

class BloomPass final : public IRenderPass {
public:
    std::string_view Name() const override { return "Bloom"; }
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    bool IsEnabled(const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

} // namespace fbzz::scene

