// FBZZ Engine
// SunMoonRenderer.hpp | fbzz::scene
// 太陽・月ディスクをスカイドームとは別パスで描画する設定コンポーネント
#pragma once
#include <Engine/Scene/Script.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::scene {

struct SunMoonRenderer {
    // WHY: 大気散乱 (SkyRenderer) と発光ディスクを分けることで、空色・IBL・雲と太陽/月の責務を独立させる。
    bool          enabled      = true;
    bool          sunEnabled   = true;
    float         sunIntensity = 20.0f;
    bool          moonEnabled  = false;
    float         moonSize     = 1.0f;
    float         moonBrightness = 0.6f;
    math::Vector3 moonColor    = { 0.85f, 0.9f, 1.0f };

    const char* GetTypeName() const { return "Sun Moon Renderer"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("sunEnabled", sunEnabled);
        r.Field("sunIntensity", sunIntensity);
        r.Field("moonEnabled", moonEnabled);
        r.Field("moonSize", moonSize);
        r.Field("moonBrightness", moonBrightness);
        r.Field("moonColor", moonColor);
    }
};

} // namespace fbzz::scene
