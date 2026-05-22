// FBZZ Engine
// SkyRenderer.hpp | fbzz::scene
// 大気散乱スカイドームのパラメータを保持するコンポーネント
#pragma once
#include <Engine/Scene/Script.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::scene {

struct SkyRenderer {
    math::Vector3 rayleighScattering = { 5.8e-3f, 13.5e-3f, 33.1e-3f };
    float         mieScattering      = 21.0e-4f;
    float         sunIntensity       = 20.0f;
    float         mieG               = 0.76f;
    bool          enabled            = true;

    const char* GetTypeName() const { return "Sky Renderer"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("rayleighScattering", rayleighScattering);
        r.Field("mieScattering", mieScattering);
        r.Field("sunIntensity", sunIntensity);
        r.Field("mieG", mieG);
    }
};

} // namespace fbzz::scene
