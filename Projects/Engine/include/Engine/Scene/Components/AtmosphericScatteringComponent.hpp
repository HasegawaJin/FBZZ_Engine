// FBZZ Engine
// AtmosphericScatteringComponent.hpp | fbzz::scene
// シーン単位の霧・大気散乱設定を GameObject で管理するコンポーネント。
// WHY: ProjectSettings の fog 設定はグローバルだが、このコンポーネントで Scene Inspector から
//      霧を制御できるようにし、屋外・室内などシーン境界で異なる大気表現を実現する。
//      RenderSystem が最初のアクティブなコンポーネントで postProcess.fog を上書きする。
#pragma once
#include <Engine/Scene/Script.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::scene {

struct AtmosphericScatteringComponent {
    bool          enabled    = true;

    // ── 指数フォグ ────────────────────────────────────────────────────────────
    bool          fogEnabled = false;
    float         fogDensity = 0.04f;              // 消散係数 (大きいほど霧が濃い)
    float         fogFar     = 80.0f;              // 霧が完全に不透明になる距離 [m]
    math::Vector3 fogColor   = { 0.55f, 0.65f, 0.75f }; // 霧の色 (空の色に合わせる)

    const char* GetTypeName() const { return "AtmosphericScattering"; }

    void Reflect(IReflector& r)
    {
        r.Field("enabled",    enabled);
        r.Field("fogEnabled", fogEnabled);
        r.Field("fogDensity", fogDensity);
        r.Field("fogFar",     fogFar);
        r.Field("fogColor",   fogColor);
    }
};

} // namespace fbzz::scene
