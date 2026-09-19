/// @file    VFXScreenEffect.hpp
/// @brief   VFX グラフの ScreenEffect ノードが生成する、画面演出の一時的な「上乗せ」成分。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// @note 爆発の白フラッシュ等は一瞬だけ足すもので、丸ごと差し替える先勝ち仕様の
///       PostProcessVolumeComponent (RenderSystem) では複数エフェクトの同時合成ができない。
///       加算専用の軽量コンポーネントとして分け、解決済み設定へ後段で足し込む。
/// @note VFXGraphSystem が生成・破棄する内部コンポーネントで、シーンには保存されない。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::scene {

struct VFXScreenEffect {
    bool enabled = true;

    /// 0..1 のエンベロープ。VFXGraphSystem がノードの生存時間に沿って毎フレーム更新する。
    /// 全ての加算量にこの重みが掛かるため、weight=0 なら実質無効。
    float weight = 0.0f;

    /// 窓の進捗 0..1。VFXSystem が VFXElement からそのまま配る (weight と違い単調)。
    /// @note weight は weightCurve で山なりに上下するため、輪の進み具合にそれを使うと
    ///       衝撃波が外へ出てから中心へ戻ってしまう。
    float progress = 0.0f;

    math::Vector3 flashColor = { 1.0f, 1.0f, 1.0f };
    float flashIntensity = 0.0f;      ///< 最終合成の screenFadeAlpha へ加算
    float bloomBoost = 0.0f;          ///< bloom.intensity へ加算
    float chromaticAberration = 0.0f; ///< lens.chromaticAberration へ加算
    float lensDistortion = 0.0f;      ///< lens.distortion へ加算
    float vignette = 0.0f;            ///< vignette.intensity へ加算
    float radialBlur = 0.0f;          ///< 画面中心から外へ引き伸ばす量 [0,1]

    /// @name 衝撃波リング
    /// @{
    /// 同心円の «輪» が中心から外へ走り、輪の上だけ色を読む座標を半径方向へずらす。
    /// @note lensDistortion (r^2 の樽型、画面全体が一様に膨らむ) では «縁が前を通り過ぎる» 効いた
    ///       感が出せない。半径は progress から割り出す —— .vfx に焼く以上、書き手が毎フレーム
    ///       半径を書き替える経路は前提にできない。
    float shockRingAmplitude = 0.0f; ///< 輪の上での UV ずらし量。0 (既定) で完全に無効
    float shockRingRadius = 0.9f;    ///< progress=1 で輪が到達する半径 [画面 UV 距離]
    float shockRingWidth = 0.12f;    ///< 輪の太さ [画面 UV 距離]
    math::Vector2 shockRingCenter = { 0.5f, 0.5f }; ///< 輪の中心 (画面 UV)
    /// @}

    /// @name 露出・彩度・コントラストの一時押し
    /// @{
    /// @note 止めの瞬間だけ露出を落とす演出は、丸ごと組み直す破壊的経路 (ScriptPostProcessProxy)
    ///       でしか作れなかった。«唯一の書き手» が居ると他のボリュームや .vfx を踏み潰す。
    float exposureOffset = 0.0f;   ///< postProcess.exposure へ加算 (下限 0)
    float saturationOffset = 0.0f; ///< colorGrading.saturation へ加算 (1=素、0=無彩色)
    float contrastOffset = 0.0f;   ///< colorGrading.contrast へ加算 (0=素)

    const char* GetTypeName() const { return "VFX Screen Effect"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.ColorField("flashColor", flashColor);
        r.Field("flashIntensity", flashIntensity);
        r.Field("bloomBoost", bloomBoost);
        r.Field("chromaticAberration", chromaticAberration);
        r.Field("lensDistortion", lensDistortion);
        r.Field("vignette", vignette);
        r.Field("radialBlur", radialBlur);
        r.Field("shockRingAmplitude", shockRingAmplitude);
        r.Field("shockRingRadius", shockRingRadius);
        r.Field("shockRingWidth", shockRingWidth);
        r.Field("shockRingCenter", shockRingCenter);
        r.Field("exposureOffset", exposureOffset);
        r.Field("saturationOffset", saturationOffset);
        r.Field("contrastOffset", contrastOffset);
    }
    /// @}
};

/// カメラ揺れ。RenderSystem がビュー行列へ適用する。
/// @note カメラ本体 (renderer::Camera) の position/rotation を直接書くと DebugCamera の yaw/pitch
///       と乖離し操作が壊れるため、描画時だけのオフセットとして分離する。
struct VFXCameraShake {
    bool enabled = true;
    float weight = 0.0f;   ///< 0..1。同じ GameObject の VFXElement が更新する
    float elapsed = 0.0f;  ///< 位相計算用の経過秒
    float amplitude = 0.12f;
    float rotationAmplitude = 1.2f;
    float frequency = 22.0f;
    float radius = 25.0f;  ///< 0 以下で距離減衰なし

    /// 揺れとは別の、一方向へ弾く成分。着弾の «押し返し» を振動ではなく変位で出す。
    float kick = 0.0f;
    math::Vector3 kickDirection = math::Vector3::ZERO;

    const char* GetTypeName() const { return "VFX Camera Shake"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("amplitude", amplitude);
        r.Field("rotationAmplitude", rotationAmplitude);
        r.Field("frequency", frequency);
        r.Field("radius", radius);
        r.Field("kick", kick);
        r.Field("kickDirection", kickDirection);
    }
};

/// ヒットストップ。VFXSystem が集計して `Time::vfxTimeScale` へ書き込む。
/// @note ゲーム側の timeScale (スクリプト/Inspector 書込み) とは別枠。こちらは Phase::LateScript
///       で集計するため、同じ変数だと .vfx 追加だけでゲーム側の止め・スローを踏み潰す。
///       2 本は Time::Tick が掛け合わせ «両方効く» が既定 (Time.hpp 参照)。
struct VFXTimeScale {
    bool enabled = true;
    float weight = 0.0f;      ///< 0..1 のブレンド量
    float timeScale = 0.15f;  ///< weight=1 のときの目標倍率

    const char* GetTypeName() const { return "VFX Time Scale"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("timeScale", timeScale);
    }
};

} // namespace fbzz::scene
