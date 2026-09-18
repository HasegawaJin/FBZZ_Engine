/// @file    SunMoonRenderer.hpp
/// @brief   太陽・月ディスクをスカイドームとは別パスで描画する設定コンポーネント。
/// @author  Hasegawa Jin
/// @date    2026-07-01
#pragma once
#include <Engine/Scene/Script.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::scene {

struct SunMoonRenderer {
    /// @note 大気散乱 (SkyRenderer) と発光ディスクを分ける設計。空色・IBL・雲と太陽/月の責務を独立させる。
    bool          enabled      = true;
    bool          sunEnabled   = true;
    /// @brief 太陽ディスクの明るさ。空の大気散乱の明るさは SkyRenderer::skyScatterIntensity が別に持つ。
    float         sunDiskIntensity = 20.0f;
    bool          moonEnabled  = false;
    float         moonSize     = 1.0f;
    float         moonBrightness = 0.6f;
    math::Vector3 moonColor    = { 0.85f, 0.9f, 1.0f };

    const char* GetTypeName() const { return "Sun Moon Renderer"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("sunEnabled", sunEnabled);
        r.Field("sunDiskIntensity", sunDiskIntensity);
        r.Field("moonEnabled", moonEnabled);
        r.Field("moonSize", moonSize);
        r.Field("moonBrightness", moonBrightness);
        r.ColorField("moonColor", moonColor);
    }
};

} // namespace fbzz::scene
