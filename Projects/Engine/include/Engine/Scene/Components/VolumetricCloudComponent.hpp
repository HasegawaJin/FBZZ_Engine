// FBZZ Engine
// VolumetricCloudComponent.hpp | fbzz::scene
// レイマーチ雲の描画パラメータを Scene に保持する Component
//
// WHY: 雲は Scene ごとに高さ・厚み・密度が大きく変わる環境要素のため、
//      RenderSettings ではなく Component として保存し、Inspector / SceneSerializer から調整できるようにする。
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

    float bottomHeight = 650.0f;  // 雲底高度 [world unit]
    float thickness    = 420.0f;  // 雲層の厚み [world unit]
    float coverage     = 0.48f;   // 雲量 [0,1]
    float density      = 0.72f;   // 密度スケール

    float noiseScale   = 0.0018f; // world -> noise UV。小さいほど大きな雲になる。
    float detailScale  = 5.0f;    // 低周波ノイズへ重ねる細部ノイズ倍率
    float windSpeed    = 18.0f;   // 雲の流速 [world unit/sec]
    math::Vector2 windDirection = { 1.0f, 0.25f };

    float lightAbsorption = 1.35f; // 雲内部の減衰。大きいほど暗い雲になる。
    float ambientStrength = 0.28f; // 影側の最低明度
    float silverLining    = 0.42f; // 太陽方向の縁取り強度
    math::Vector3 albedo  = { 1.0f, 0.96f, 0.88f };

    int stepCount = 48;
    float maxDistance = 6000.0f;

    const char* GetTypeName() const { return "Volumetric Cloud"; }

    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("bottomHeight", bottomHeight);
        r.Field("thickness", thickness);
        r.Field("coverage", coverage);
        r.Field("density", density);
        r.Field("noiseScale", noiseScale);
        r.Field("detailScale", detailScale);
        r.Field("windSpeed", windSpeed);
        r.Field("windDirection", windDirection);
        r.Field("lightAbsorption", lightAbsorption);
        r.Field("ambientStrength", ambientStrength);
        r.Field("silverLining", silverLining);
        r.Field("albedo", albedo);
        r.Field("stepCount", stepCount);
        r.Field("maxDistance", maxDistance);

        coverage = math::Clamp01(coverage);
        density = (std::max)(density, 0.0f);
        thickness = (std::max)(thickness, 1.0f);
        noiseScale = (std::max)(noiseScale, 0.00001f);
        detailScale = (std::max)(detailScale, 1.0f);
        lightAbsorption = (std::max)(lightAbsorption, 0.0f);
        ambientStrength = math::Clamp01(ambientStrength);
        silverLining = (std::max)(silverLining, 0.0f);
        stepCount = stepCount < 8 ? 8 : (stepCount > 96 ? 96 : stepCount);
        maxDistance = (std::max)(maxDistance, 100.0f);
    }
};

} // namespace fbzz::scene
