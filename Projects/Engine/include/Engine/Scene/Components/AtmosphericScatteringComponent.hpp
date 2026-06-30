// FBZZ Engine
// AtmosphericScatteringComponent.hpp | fbzz::scene
// シーン単位の霧・大気散乱設定を GameObject で管理するコンポーネント。
// WHY: ProjectSettings の fog 設定はグローバルだが、このコンポーネントで Scene Inspector から
//      霧を制御できるようにし、屋外・室内などシーン境界で異なる大気表現を実現する。
//      RenderSystem が最初のアクティブなコンポーネントで postProcess.fog を上書きする。
#pragma once
#include <Engine/Scene/Script.hpp>
#include <Math/Vector3.hpp>
#include <cstdint>

namespace fbzz::scene {

// FogSource — フォグ色の出どころ (環境システム設計 §3-3)。
// WHY: エアリアルパースペクティブ (大気散乱由来の距離フォグ) を従来の指数フォグと二重実装せず、
//      「フォグは 1 系統・係数の出どころだけ差し替える」方針で統合する。適用ロジック (Composite の
//      ApplyFog) は共通で、Atmosphere を選ぶと色を視線方向の大気散乱から引く。
enum class FogSource : uint8_t {
    Exponential = 0, // 固定 fogColor の指数フォグ (既定・従来)
    Atmosphere  = 1, // 大気散乱の in-scatter を視線方向から計算 (エアリアル)
};

struct AtmosphericScatteringComponent {
    bool          enabled    = true;

    // ── 指数フォグ ────────────────────────────────────────────────────────────
    bool          fogEnabled = false;
    FogSource     fogSource  = FogSource::Exponential; // 色の出どころ (既定は後方互換の指数フォグ)
    float         fogDensity = 0.04f;              // 消散係数 (大きいほど霧が濃い)
    float         fogFar     = 80.0f;              // 霧が完全に不透明になる距離 [m]
    math::Vector3 fogColor   = { 0.55f, 0.65f, 0.75f }; // 霧の色 (Exponential 時のみ使用)

    const char* GetTypeName() const { return "AtmosphericScattering"; }

    void Reflect(IReflector& r)
    {
        r.Field("enabled",    enabled);
        r.Field("fogEnabled", fogEnabled);

        // enum は int 経由で反映 (Inspector ドロップダウン / シリアライザは int)。範囲外は Exponential にクランプ。
        static constexpr const char* kFogSourceLabels[] = { "Exponential", "Atmosphere" };
        int fogSourceValue = static_cast<int>(fogSource);
        r.Enum("fogSource", fogSourceValue, kFogSourceLabels);
        fogSourceValue = (fogSourceValue < 0 || fogSourceValue > 1) ? 0 : fogSourceValue;
        fogSource = static_cast<FogSource>(fogSourceValue);

        r.Field("fogDensity", fogDensity);
        r.Field("fogFar",     fogFar);
        r.Field("fogColor",   fogColor);
    }
};

} // namespace fbzz::scene
