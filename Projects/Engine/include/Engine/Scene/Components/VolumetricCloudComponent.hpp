/// @file    VolumetricCloudComponent.hpp
/// @brief   レイマーチ雲の描画パラメータを Scene に保持する Component。
/// @author  Hasegawa Jin
/// @date    2026-07-01
///
/// WHY: 雲は Scene ごとに高さ・厚み・密度が大きく変わる環境要素のため、
/// RenderSettings ではなく Component として保存し、Inspector / SceneSerializer から調整できるようにする。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <algorithm>

namespace fbzz::scene {

// VolumetricCloudComponent — 画面空間レイマーチで描く大域的な雲レイヤー。
struct VolumetricCloudComponent {
    bool enabled = true;

    // ---- 雲層 ----
    float bottomHeight = 650.0f;  // 雲底高度 [world unit]
    float thickness    = 420.0f;  // 雲層の厚み [world unit]
    float coverage     = 0.58f;   // 雲量 [0,1]
    float density      = 0.72f;   // 密度スケール

    // ---- 形状 ----
    // すべて「ワールド単位の大きさ [m]」で持つ。シェーダーへ渡すときに逆数へ変換する。
    // WHY: 以前は world→noise UV のスケール (0.0009 等) をそのまま公開していたため、
    //      Inspector でドラッグすると 0.000063 のような読めない値になり、
    //      「雲をどれくらいの大きさにしたいか」という本来の意図と数字が結びつかなかった。
    //
    // WHY cloudSize が繰り返しに効くか: Shape ボリュームはタイラブルな 3D テクスチャなので、
    //      cloudSize ワールド単位ごとにまったく同じ雲が並ぶ。小さくすると雲は細かくなるが、
    //      視界内の繰り返しが目に付きやすくなる。
    float cloudSize   = 1100.0f;  // 雲塊 1 周期の大きさ [m]
    float detailSize  = 220.0f;   // 縁を侵食する細部ノイズの大きさ [m]
    float detailStrength = 0.35f; // detail による縁の侵食量。上げるとちぎれた綿状になる。
    // 雲量の大域的なムラ。cloudSize より 1 桁大きくすることで、
    // Shape ボリュームのタイリング (同じ雲が格子状に並ぶ) を視覚的に解く。
    float weatherSize   = 8500.0f;  // 晴れ間と厚い塊が入れ替わる周期 [m]
    float weatherAmount = 0.85f;    // 0 = 一様な雲量 (旧挙動), 1 = 晴れ間と厚い塊の差が最大
    float bottomSoftness = 0.18f;   // 雲底の丸み [0,0.9]
    float topSoftness    = 0.45f;   // 雲頂の散り方 [0,0.9]
    float evolutionSpeed = 2.0f;    // 流れとは別に形そのものが変化する速さ

    // ---- 風 ----
    float windSpeed    = 18.0f;   // 雲の流速 [world unit/sec]
    math::Vector2 windDirection = { 1.0f, 0.25f };

    // ---- ライティング ----
    float lightAbsorption = 1.35f; // 雲内部の減衰。大きいほど暗い雲になる。
    float extinction      = 0.03f; // 密度 → 消散係数のスケール。上げるほど不透明で締まった雲。
    float sunIntensity    = 1.5f;  // 太陽散乱の倍率。雲が暗くて見えないときはここを上げる。
    float ambientStrength = 0.45f; // 影側の明度 (空からの間接光)
    float ambientGradient = 0.35f; // 雲底に届く間接光の割合。0 で底が真っ黒になる。
    float silverLining    = 0.42f; // 太陽方向の縁取り強度
    float multiScatter    = 0.5f;  // 多重散乱オクターブの寄与 [0,1]。0 で単散乱のみ (内部が暗い)
    float powderStrength  = 0.7f;  // Beer-Powder。太陽側の縁を暗く落として立体感を出す
    float anisotropy      = 0.35f; // 位相関数の前方散乱の鋭さ [0,0.95]
    math::Vector3 albedo      = { 1.0f, 0.96f, 0.88f }; // 雲そのものの色
    math::Vector3 sunTint     = { 1.0f, 1.0f, 1.0f };   // 太陽に照らされた側へ掛ける色
    math::Vector3 ambientTint = { 1.0f, 1.0f, 1.0f };   // 影側 (空からの間接光) へ掛ける色

    // 体積光 (ゴッドレイ) をこの雲で遮る強さ [0,1]。0 で雲の切れ間の光芒を出さない。
    // WHY: シャドウマップには雲が入らないため、雲の隙間から差す光の線は体積光パスが
    //      雲の密度場を直接引いて作る。負荷が乗る機能なのでシーン単位で切れるようにする。
    float lightShaftStrength = 1.0f;

    // ---- 描画範囲 ----
    // カメラからこの距離までは雲を出さず、そこから fadeDistance かけて濃くしていく。
    // WHY: 雲層の高さまでカメラを上げると視線がいきなり雲の内部から始まり、1 ステップ目で
    //      透過率が飽和して画面全体が真っ白になる。手前を抜くことで内部を通り抜けられる。
    float minDistance  = 0.0f;
    float fadeDistance = 250.0f;
    // 水平線ぎわのフェード幅。maxDistance に対する割合 [0,1]。0 で無効。
    // WHY: 視線を水平へ倒すと雲層に入る距離が伸び、maxDistance を越えた瞬間に雲が消える。
    //      その直前まで交差区間は不透明なので、地平線に沿って硬い切れ目が出る。
    float horizonFade  = 0.25f;
    float maxDistance  = 6000.0f;

    // ---- 品質 / 負荷 ----
    int   stepCount      = 48;     // 視線レイマーチのステップ数
    int   lightStepCount = 4;      // 太陽方向ライトマーチ (セルフシャドウ) のステップ数
    // 半解像度でレイマーチしてから拡大合成する。描画ピクセルが 1/4 になる代わりに輪郭が甘くなる。
    bool  halfResolution = false;

    const char* GetTypeName() const { return "Volumetric Cloud"; }

    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("bottomHeight", bottomHeight);
        r.Field("thickness", thickness);
        r.Field("coverage", coverage);
        r.Field("density", density);
        r.Field("cloudSize", cloudSize);
        r.Field("detailSize", detailSize);
        r.Field("detailStrength", detailStrength);
        r.Field("weatherSize", weatherSize);
        r.Field("weatherAmount", weatherAmount);
        r.Field("bottomSoftness", bottomSoftness);
        r.Field("topSoftness", topSoftness);
        r.Field("evolutionSpeed", evolutionSpeed);
        r.Field("windSpeed", windSpeed);
        r.Field("windDirection", windDirection);
        r.Field("lightAbsorption", lightAbsorption);
        r.Field("extinction", extinction);
        r.Field("sunIntensity", sunIntensity);
        r.Field("ambientStrength", ambientStrength);
        r.Field("ambientGradient", ambientGradient);
        r.Field("silverLining", silverLining);
        r.Field("multiScatter", multiScatter);
        r.Field("powderStrength", powderStrength);
        r.Field("anisotropy", anisotropy);
        r.Field("lightShaftStrength", lightShaftStrength);
        r.Field("albedo", albedo);
        r.Field("sunTint", sunTint);
        r.Field("ambientTint", ambientTint);
        r.Field("minDistance", minDistance);
        r.Field("fadeDistance", fadeDistance);
        r.Field("horizonFade", horizonFade);
        r.Field("maxDistance", maxDistance);
        r.Field("stepCount", stepCount);
        r.Field("lightStepCount", lightStepCount);
        r.Field("halfResolution", halfResolution);

        coverage = math::Clamp01(coverage);
        density = (std::max)(density, 0.0f);
        thickness = (std::max)(thickness, 1.0f);
        cloudSize = math::Clamp(cloudSize, 50.0f, 100000.0f);
        // 細部は必ず雲塊より小さくする (等倍以上だと侵食ではなく形そのものを潰す)。
        detailSize = math::Clamp(detailSize, 1.0f, cloudSize);
        detailStrength = math::Clamp01(detailStrength);
        weatherSize = math::Clamp(weatherSize, 100.0f, 1000000.0f);
        weatherAmount = math::Clamp01(weatherAmount);
        bottomSoftness = math::Clamp(bottomSoftness, 0.01f, 0.9f);
        topSoftness = math::Clamp(topSoftness, 0.01f, 0.9f);
        evolutionSpeed = (std::max)(evolutionSpeed, 0.0f);
        lightAbsorption = (std::max)(lightAbsorption, 0.0f);
        extinction = math::Clamp(extinction, 0.001f, 0.5f);
        sunIntensity = (std::max)(sunIntensity, 0.0f);
        ambientStrength = (std::max)(ambientStrength, 0.0f);
        ambientGradient = math::Clamp01(ambientGradient);
        silverLining = (std::max)(silverLining, 0.0f);
        multiScatter = math::Clamp01(multiScatter);
        powderStrength = math::Clamp01(powderStrength);
        anisotropy = math::Clamp(anisotropy, 0.0f, 0.95f);
        lightShaftStrength = math::Clamp01(lightShaftStrength);
        stepCount = stepCount < 8 ? 8 : (stepCount > 96 ? 96 : stepCount);
        lightStepCount = lightStepCount < 1 ? 1 : (lightStepCount > 8 ? 8 : lightStepCount);
        maxDistance = (std::max)(maxDistance, 100.0f);
        minDistance = math::Clamp(minDistance, 0.0f, maxDistance);
        fadeDistance = (std::max)(fadeDistance, 0.0f);
        horizonFade = math::Clamp01(horizonFade);
    }
};

} // namespace fbzz::scene
