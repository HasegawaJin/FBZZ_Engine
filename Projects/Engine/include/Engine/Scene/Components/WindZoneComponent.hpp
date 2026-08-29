/// @file    WindZoneComponent.hpp
/// @brief   シーングローバルの風設定コンポーネント。
/// @author  Hasegawa Jin
/// @date    2026-07-15
///
/// 雲 (VolumetricCloud)・パーティクルが同じ風を参照し、
/// 「風が吹くと煙も雲も同じ方向へ流れる」を 1 コンポーネントで成立させる。
/// EnvironmentLightComponent 等と同じ「シーンに 1 つ」パターン。複数ある場合は最初の有効な 1 つを使う。
#pragma once
#include <Engine/Scene/Script.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::scene {

struct WindZoneComponent {
    bool enabled = true;
    // 風向き (ローカル空間)。GameObject の回転が適用されるため、Transform を回せば風向きも回る。
    // WHY: 既定値は従来ハードコードされていた (0.7071, 0, 0.7071) に合わせ、
    //      WindZone を置いただけでは見た目が変わらないようにする。
    math::Vector3 direction = { 0.7071f, 0.0f, 0.7071f };
    // 基本風速スケール。各システムの固有強度に乗算される。
    float strength = 1.0f;
    // 乱れの強さ [m/s^2]。パーティクルへはカールノイズ乱流として加算される (0 で無効)。
    float turbulence = 0.0f;
    // 脈動周波数。揺れの sin 波周波数へ反映される。
    float pulseFrequency = 1.0f;

    const char* GetTypeName() const { return "Wind Zone"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("direction", direction);
        r.Field("strength", strength);
        r.Field("turbulence", turbulence);
        r.Field("pulseFrequency", pulseFrequency);
    }
};

} // namespace fbzz::scene
