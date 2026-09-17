/// @file    AtmosphericScatteringComponent.hpp
/// @brief   シーン単位の霧・大気散乱設定を GameObject で管理するコンポーネント。
/// @author  Hasegawa Jin
/// @date    2026-06-23
///
/// @note ProjectSettings の fog はグローバル設定。RenderSystem は最初のアクティブな本コンポーネントで
///       postProcess.fog を上書きし、シーン境界ごとに異なる大気表現を許す。
#pragma once
#include <Engine/Scene/Script.hpp>
#include <Math/Vector3.hpp>
#include <cstdint>

namespace fbzz::scene {

/// @brief フォグ色の出どころ (環境システム設計 §3-3)。
/// @note 適用ロジック (Composite の ApplyFog) は共通。Atmosphere は色を視線方向の大気散乱から引く。
enum class FogSource : uint8_t {
    Exponential = 0, ///< 固定 fogColor の指数フォグ (既定・従来)
    Atmosphere  = 1, ///< 大気散乱の in-scatter を視線方向から計算 (エアリアル)
};

struct AtmosphericScatteringComponent {
    bool          enabled    = true;

    /// @name 指数フォグ
    /// @{
    bool          fogEnabled = false;
    FogSource     fogSource  = FogSource::Exponential; ///< 色の出どころ (既定は後方互換の指数フォグ)
    float         fogDensity = 0.04f;              ///< 消散係数 (大きいほど霧が濃い)
    float         fogFar     = 80.0f;              ///< 霧が完全に不透明になる距離 [m]
    math::Vector3 fogColor   = { 0.55f, 0.65f, 0.75f }; ///< 霧の色 (Exponential 時のみ使用)

    const char* GetTypeName() const { return "AtmosphericScattering"; }

    void Reflect(IReflector& r)
    {
        r.Field("enabled",    enabled);
        r.Field("fogEnabled", fogEnabled);

        /// @note enum は int 経由で反映 (Inspector ドロップダウン / シリアライザは int)。範囲外は Exponential にクランプ。
        static constexpr const char* kFogSourceLabels[] = { "Exponential", "Atmosphere" };
        int fogSourceValue = static_cast<int>(fogSource);
        r.Enum("fogSource", fogSourceValue, kFogSourceLabels);
        fogSourceValue = (fogSourceValue < 0 || fogSourceValue > 1) ? 0 : fogSourceValue;
        fogSource = static_cast<FogSource>(fogSourceValue);

        r.Field("fogDensity", fogDensity);
        r.Field("fogFar",     fogFar);
        r.ColorField("fogColor",   fogColor);
    }
    /// @}
};

} // namespace fbzz::scene
