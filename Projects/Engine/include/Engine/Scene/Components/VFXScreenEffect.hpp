/// @file    VFXScreenEffect.hpp
/// @brief   VFX グラフの ScreenEffect ノードが生成する、画面演出の一時的な「上乗せ」成分。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// WHY: 爆発の白フラッシュや被弾の色収差は、シーンのカラーグレーディングを
/// 置き換えるのではなく一瞬だけ足すもの。PostProcessVolumeComponent は
/// 設定を丸ごと差し替える先勝ち仕様 (RenderSystem) のため、
/// 同時に複数のエフェクトが走る VFX 用途では合成できない。
/// そこで加算専用の軽量コンポーネントを分け、RenderSystem が
/// 解決済みのポストプロセス設定へ後段で足し込む。
/// VFXGraphSystem が生成・破棄する内部コンポーネントで、シーンには保存されない。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::scene {

struct VFXScreenEffect {
    bool enabled = true;

    // 0..1 のエンベロープ。VFXGraphSystem がノードの生存時間に沿って毎フレーム更新する。
    // 全ての加算量にこの重みが掛かるため、weight=0 なら実質無効。
    float weight = 0.0f;

    math::Vector3 flashColor = { 1.0f, 1.0f, 1.0f };
    float flashIntensity = 0.0f;      // 最終合成の screenFadeAlpha へ加算
    float bloomBoost = 0.0f;          // bloom.intensity へ加算
    float chromaticAberration = 0.0f; // lens.chromaticAberration へ加算
    float lensDistortion = 0.0f;      // lens.distortion へ加算
    float vignette = 0.0f;            // vignette.intensity へ加算
    float radialBlur = 0.0f;          // 画面中心から外へ引き伸ばす量 [0,1]

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
    }
};

// カメラ揺れ。RenderSystem がビュー行列へ適用する。
// WHY: カメラ本体 (renderer::Camera) の position/rotation を直接書くと、
//      DebugCamera が保持する yaw/pitch と乖離して操作が壊れる。
//      揺れは「描画時だけのオフセット」として分離し、カメラの状態には触れない。
struct VFXCameraShake {
    bool enabled = true;
    float weight = 0.0f;   // 0..1。同じ GameObject の VFXElement が更新する
    float elapsed = 0.0f;  // 位相計算用の経過秒
    float amplitude = 0.12f;
    float rotationAmplitude = 1.2f;
    float frequency = 22.0f;
    float radius = 25.0f;  // 0 以下で距離減衰なし

    // 揺れとは別の、一方向へ弾く成分。着弾の «押し返し» を振動ではなく変位で出す。
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

// ヒットストップ。VFXSystem が集計して **Time::vfxTimeScale** へ書き込む。
//
// WHY ゲームの timeScale と別枠か: あちらの書き手はゲーム側 (スクリプト /
//     Inspector) で、こちらの集計は Phase::LateScript ── スクリプトより後 ──
//     で走る。同じ変数を使うと、.vfx を 1 つ足しただけでゲーム側の止めや
//     スローを踏み潰す。2 本は Time::Tick が掛け合わせるので、
//     «両方効く» が既定になる (Time.hpp の WHY)。
struct VFXTimeScale {
    bool enabled = true;
    float weight = 0.0f;      // 0..1 のブレンド量
    float timeScale = 0.15f;  // weight=1 のときの目標倍率

    const char* GetTypeName() const { return "VFX Time Scale"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("timeScale", timeScale);
    }
};

} // namespace fbzz::scene
