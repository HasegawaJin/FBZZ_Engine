/// @file    VolumeOverrides.hpp
/// @brief   組み込みの VolumeOverride 派生一覧。
/// @author  Hasegawa Jin
/// @date    2026-08-14
///
/// @note 効果 1 つ = クラス 1 つ。対応する XxxSettings::enabled は Apply 側で立て、オーバーライド自体は
///       enabled を持たない (リストに載っていること自体が使用の意思表示で、一時的に外すときは基底の
///       active を落とす。両方あると「enabled=false の Bloom」という効かないのに存在する状態ができる)。
/// @note 効果を足す手順: ここにクラスを 1 つ足し、VolumeOverrides.cpp に Apply / Reflect を書いて
///       FBZZ_REGISTER_VOLUME_OVERRIDE。Add Override メニューと .fzdata の読み書きは自動で追随する。
#pragma once
#include <Engine/Asset/RenderSettings/VolumeOverride.hpp>
#include <Graphics/Renderer/View/RenderSettings.hpp>

namespace fbzz::asset {

/// @brief 定型の override 宣言。TYPE_NAME / 表示名 / カテゴリ / Clone を一括で与える。
/// @note 4 つの仮想関数はどのクラスでも中身が同じ形で、手書きすると「Clone のコピー先クラス名を
///       直し忘れる」類の事故が起きるため、マクロにする。
#define FBZZ_VOLUME_OVERRIDE_BODY(Class, TypeNameLiteral, DisplayLiteral, CategoryEnum) \
public:                                                                                \
    static constexpr const char* TYPE_NAME = TypeNameLiteral;                          \
    const char* GetTypeName() const override { return TYPE_NAME; }                     \
    const char* GetDisplayName() const override { return DisplayLiteral; }             \
    VolumeOverrideCategory GetCategory() const override                                \
    { return VolumeOverrideCategory::CategoryEnum; }                                   \
    std::unique_ptr<VolumeOverride> Clone() const override                             \
    { return std::make_unique<Class>(*this); }                                         \
    void Apply(renderer::VolumeSettings& target, float weight) const override;         \
    void Reflect(scene::IReflector& r) override;

/// @name 露出
/// @{
class ExposureOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(ExposureOverride, "Exposure", "Exposure", Exposure)
    float exposure = 1.0f; ///< 後処理でシーン色の明るさに掛ける露出倍率。
};
/// @}

/// @name アンチエイリアシング (FXAA / TAA は同一スロットで排他)
/// @{
class FxaaOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(FxaaOverride, "FXAA", "FXAA", AntiAliasing)
};

class TaaOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(TaaOverride, "TAA", "TAA (Temporal)", AntiAliasing)
    float feedback = 0.9f; ///< TAA の現在画素へ蓄積済み履歴を混ぜる重み。
};
/// @}

/// @name アンビエントオクルージョン (SSAO / GTAO は同一スロットで排他)
/// @{
class SsaoOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(SsaoOverride, "SSAO", "SSAO (Simple)", AmbientOcclusion)
    float intensity = 1.0f; ///< スクリーン空間環境遮蔽で周辺を暗くする強さ。
};

class GtaoOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(GtaoOverride, "GTAO", "GTAO (Ground Truth)", AmbientOcclusion)
    float intensity     = 1.0f; ///< 地平線サンプルから求めた環境遮蔽の適用強度。
    float radius        = 1.5f; ///< 視点空間で遮蔽物をサンプリングする最大半径 [m]。
    int   slices        = 8; ///< GTAO で周辺を調べる断面方向の数。
    int   stepsPerSlice = 8; ///< GTAO の各断面方向で行う深度サンプル数。
};
/// @}

/// @name 色
/// @{
class BloomOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(BloomOverride, "Bloom", "Bloom", Color)
    float intensity = 0.9f; ///< 明部から抽出してぼかした光をシーン色に足す倍率。
    float threshold = 0.65f; ///< Bloom の光として拾う輝度の基準値。
    float softKnee  = 0.4f; ///< Bloom の採取がしきい値付近でなだらかに立ち上がる幅。
};

class ColorGradingOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(ColorGradingOverride, "ColorGrading", "Color Grading", Color)
    float contrast    = 0.0f; ///< カラーグレーディングで明暗の差を調整する量。
    float saturation  = 1.0f; ///< カラーグレーディングで色差の成分に掛ける倍率。
    float hueShift    = 0.0f; ///< カラーグレーディングで色相をずらす量。
    float temperature = 0.0f; ///< ホワイトバランスの暖色・寒色方向の補正量。
    float tint        = 0.0f; ///< ホワイトバランスの緑・マゼンタ方向の補正量。
};

class LutColorGradingOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(LutColorGradingOverride, "LUTColorGrading",
                              "LUT Color Grading (Procedural)", Color)
    float blend       = 1.0f; ///< LUT で引いた色と通常のグレーディング色を混ぜる重み。
    float contrast    = 0.0f; ///< カラーグレーディングで明暗の差を調整する量。
    float saturation  = 1.0f; ///< カラーグレーディングで色差の成分に掛ける倍率。
    float hueShift    = 0.0f; ///< カラーグレーディングで色相をずらす量。
    float temperature = 0.0f; ///< ホワイトバランスの暖色・寒色方向の補正量。
    float tint        = 0.0f; ///< ホワイトバランスの緑・マゼンタ方向の補正量。
};

class ColorFilterOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(ColorFilterOverride, "ColorFilter", "Color Filter", Color)
    float color[3] = { 1.0f, 1.0f, 1.0f }; ///< 画像へ色フィルターとして掛ける RGB 成分。
    float intensity = 0.5f; ///< 元の色に色フィルターを混ぜる重み。
};

class ClarityOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(ClarityOverride, "Clarity", "Clarity", Color)
    float strength = 0.12f; ///< 周辺輝度との差から細部の局所コントラストを増やす強さ。
    float radius   = 2.0f; ///< 局所コントラスト用の近傍領域を調べる半径 [px]。
};

class ShadowHighlightOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(ShadowHighlightOverride, "ShadowHighlight",
                              "Shadows / Highlights", Color)
    float shadowLift           = 0.08f; ///< 暗い画素を持ち上げる補正量。
    float highlightCompression = 0.08f; ///< 明るい画素の階調を圧縮する補正量。
};

class SharpenOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(SharpenOverride, "Sharpen", "Sharpen", Color)
    float strength = 0.35f; ///< 周囲のぼかしとの差から輪郭を強める倍率。
    float radius   = 1.0f; ///< 輪郭抽出で参照する近傍の距離 [px]。
};

class FilmGrainOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(FilmGrainOverride, "FilmGrain", "Film Grain", Color)
    float intensity = 0.03f; ///< 画素に重ねるフィルム粒子のノイズ強度。
    float response  = 0.8f; ///< 画素輝度に応じてフィルム粒子の出方を変える量。
};
/// @}

/// @name レンズ
/// @{
class VignetteOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(VignetteOverride, "Vignette", "Vignette", Lens)
    float intensity  = 0.25f; ///< 画面の周縁にビネット色を混ぜる強さ。
    float smoothness = 0.45f; ///< ビネットの中心域から周縁へ切り替わる遷移のなだらかさ。
    float roundness  = 1.0f; ///< ビネットの周縁形状の丸み。
    float color[3]   = { 0.0f, 0.0f, 0.0f }; ///< 画面の周縁へ重ねる RGB 色。
};

class DepthOfFieldOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(DepthOfFieldOverride, "DepthOfField", "Depth of Field", Lens)
    float focusDistance = 8.0f; ///< カメラから合焦面までの距離 [m]。
    float focusRange    = 4.0f; ///< 合焦位置の周りでぼけを増やす距離の範囲 [m]。
    float blurRadius    = 3.0f; ///< 被写界深度のぼかしで使う画面上の半径 [px]。
};

class ChromaticAberrationOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(ChromaticAberrationOverride, "ChromaticAberration",
                              "Chromatic Aberration", Lens)
    float amount = 0.005f; ///< 色収差で色チャンネルのサンプル位置をずらす強さ。
};

class LensDistortionOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(LensDistortionOverride, "LensDistortion", "Lens Distortion", Lens)
    float distortion = 0.0f; ///< 画面中心からの距離に応じて UV を歪める係数。
};

class LensFlareOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(LensFlareOverride, "LensFlare", "Lens Flare", Lens)
    float intensity  = 0.5f; ///< 抽出したフレア光をシーン色へ加える強度。
    float haloWidth  = 0.4f; ///< レンズフレアのハローを配置する画面上の幅。
    float distortion = 1.0f; ///< レンズフレアの色チャンネルを分離する量。
    int   ghostCount = 4; ///< 光源の反対側へ表示するフレアのゴースト像の数。
};

class MotionBlurOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(MotionBlurOverride, "MotionBlur", "Motion Blur", Lens)
    float strength = 0.5f; ///< 画素の移動ベクトルに掛けるぼかしの強さ。
    int   samples  = 16; ///< 移動ベクトルに沿って色を合成するサンプル数。
};
/// @}

/// @name 大気
/// @{
class FogOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(FogOverride, "Fog", "Fog", Atmosphere)
    float density     = 0.04f; ///< カメラからの距離に応じた霧の増え方を制御する密度。
    float farDistance = 80.0f; ///< 距離霧の評価で使うカメラからの遠方距離 [m]。
    float color[3]    = { 0.55f, 0.65f, 0.75f }; ///< 距離に応じてシーン色へ混ぜる霧の RGB 色。
    int   source      = 0; ///< 0 = Exponential (固定色)、1 = Atmosphere (空の大気散乱色)。
};

class VolumetricLightOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(VolumetricLightOverride, "VolumetricLight",
                              "Volumetric Light", Atmosphere)
    float scattering  = 0.3f; ///< 霧の中でライトの方向に沿って光が散乱する度合い。
    float intensity   = 0.8f; ///< ボリュームライトの散乱光をシーン色へ足す強さ。
    float minDistance = 0.0f; ///< カメラから体積散乱のサンプリングを始める距離 [m]。
    float maxDistance = 30.0f; ///< カメラから体積散乱のサンプリングを打ち切る距離 [m]。
    float edgeFade    = 0.2f; ///< ボリュームライトの距離範囲の端をなだらかにする幅。
    float density     = 0.0f; ///< 体積散乱と減衰の計算へ渡す媒質の密度。
    float heightFalloff = 0.0f; ///< 基準の高さから上方へ媒質密度を減らす係数。
    float heightStart   = 0.0f; ///< 高さ方向の密度減衰を始めるワールド Y [m]。
    float tint[3]     = { 1.0f, 1.0f, 1.0f }; ///< ボリュームライトの散乱光へ掛ける RGB 色。
    int   steps       = 32; ///< カメラレイに沿って体積散乱を積分するステップ数。
};

/// @brief 視錐台を 3D グリッドへ切って焼く体積フォグ。VolumetricLight と違い、点光源・スポット・
///        面光源も霧へ映り込む。
/// @note グリッド寸法だけは Volume でブレンドしない。解像度が変わるとボリュームの再確保が走り、
///       プロファイル間の遷移中に毎フレーム作り直すことになるため、寸法は RenderSettings の既定
///       (160x90x64) を使う。
class FroxelFogOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(FroxelFogOverride, "FroxelFog", "Froxel Fog", Atmosphere)
    float density       = 0.02f; ///< 体積散乱と減衰の計算へ渡す媒質の密度。
    float albedo[3]     = { 1.0f, 1.0f, 1.0f }; ///< 散乱で維持する色と割合を表す媒質の RGB アルベド。
    float emissive[3]   = { 0.0f, 0.0f, 0.0f }; ///< フロクセル媒質自体が放射する RGB 発光値。
    float anisotropy    = 0.4f; ///< フロクセルの散乱位相関数で光の前後への偏りを決める係数。
    float heightFalloff = 0.0f; ///< 基準の高さから上方へ媒質密度を減らす係数。
    float heightStart   = 0.0f; ///< 高さ方向の密度減衰を始めるワールド Y [m]。
    float nearDistance  = 0.1f; ///< フロクセル格子をカメラ側で開始する距離 [m]。
    float farDistance   = 64.0f; ///< フロクセル格子をカメラから遠方側で終える距離 [m]。
    float ambient       = 1.0f; ///< フロクセル媒質に注入する環境光の強度。
    float lightScattering = 2.0f; ///< フロクセルへ注入した直接光を散乱光に変える倍率。
};

/// @brief 画面の明るさから露出を自動で決める (眼の順応)。
class AutoExposureOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(AutoExposureOverride, "AutoExposure", "Auto Exposure", Exposure)
    float minEV         = -6.0f; ///< 輝度ヒストグラムに集計する露出値の下限 [EV]。
    float maxEV         = 14.0f; ///< 輝度ヒストグラムに集計する露出値の上限 [EV]。
    float lowPercent    = 0.45f; ///< 平均露出計算から暗い側を除くヒストグラムの下側割合 [0, 1]。
    float highPercent   = 0.95f; ///< 平均露出計算から明るい側を除くヒストグラムの上側割合 [0, 1]。
    float speedUp       = 3.0f; ///< 目標露出が増加するときに露出を追従させる速さ。
    float speedDown     = 1.0f; ///< 目標露出が減少するときに露出を追従させる速さ。
    float compensation  = 0.0f; ///< 自動露出の目標値へ加える露出補正 [EV]。
    float minExposureEV = -8.0f; ///< 適応後の露出補正に許す下限 [EV]。
    float maxExposureEV =  8.0f; ///< 適応後の露出補正に許す上限 [EV]。
};
/// @}

/// @name 影・反射 (いずれも Deferred Pipeline 専用)
/// @{
class SsrOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(SsrOverride, "SSR", "Screen Space Reflections", Shadowing)
    float maxDistance = 50.0f; ///< 反射レイを追跡する最大距離 [m]。
    float thickness   = 0.15f; ///< 反射レイとスクリーン深度の交差を認める厚み [m]。
    float intensity   = 0.8f; ///< スクリーン空間で求めた反射をシーン色へ重ねる強度。
    int   steps       = 64; ///< 反射レイでスクリーン深度を調べる最大ステップ数。
};

class ContactShadowOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(ContactShadowOverride, "ContactShadow", "Contact Shadows", Shadowing)
    float strength  = 0.5f; ///< 画面上の近接遮蔽を影として適用する強さ。
    float rayLength = 1.5f; ///< 接触影を調べるライト方向のレイ長 [m]。
    float thickness = 0.2f; ///< 接触影のレイとスクリーン深度の交差を認める厚み [m]。
    int   steps     = 16; ///< 接触影のレイでスクリーン深度を調べるステップ数。
};
/// @}

/// @name 演出
/// @{
class SepiaOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(SepiaOverride, "Sepia", "Sepia", Stylize)
    float intensity = 0.75f; ///< 元の色とセピアに変換した色を混ぜる重み。
};

class InvertOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(InvertOverride, "Invert", "Invert", Stylize)
    float intensity = 1.0f; ///< 元の色と反転した色を混ぜる重み。
};

class PosterizeOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(PosterizeOverride, "Posterize", "Posterize", Stylize)
    float levels = 6.0f; ///< ポスタライズで各色成分を量子化する階調数。
};

class PixelateOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(PixelateOverride, "Pixelate", "Pixelate", Stylize)
    float pixelSize = 4.0f; ///< ピクセル化で同じ色を共有するブロックの一辺 [px]。
};
/// @}

/// @name カスタム
/// @{

/// @brief 自作 HLSL をフルスクリーンパスとして差し込む。別々のシェーダーを重ねられるよう、これだけは
///        1 プロファイルに複数入れられる。
class CustomEffectOverride final : public VolumeOverride {
    FBZZ_VOLUME_OVERRIDE_BODY(CustomEffectOverride, "CustomEffect", "Custom Effect", Custom)
public:
    [[nodiscard]] bool AllowsMultiple() const override { return true; }

    renderer::CustomPostProcessSettings effect; ///< 任意の後処理シェーダーの実行条件と入力パラメーター。
};

#undef FBZZ_VOLUME_OVERRIDE_BODY

} // namespace fbzz::asset
