// FBZZ Engine
// ReflectionProbeComponent.hpp | fbzz::scene
// 局所的な環境反射を静的キューブマップで提供するプローブコンポーネント。
// WHY: EnvironmentLightComponent はシーン全体のグローバル IBL を管理するが、
//      室内や窓際など局所的に異なる反射環境が必要な場所には別のキューブマップが必要になる。
//      将来は Transform 位置からの動的キャプチャへ拡張予定。現時点は静的 .dds のみ対応。
#pragma once
#include <Engine/Scene/Script.hpp>
#include <Math/Vector3.hpp>
#include <string>

namespace fbzz::scene {

struct ReflectionProbeComponent {
    bool          enabled         = true;
    std::string   cubemapPath;               // 静的環境キューブマップ (.dds)
    float         influenceRadius = 5.0f;    // 球影響半径 [m]。カメラがこの範囲内に入ると適用される
    float         intensity       = 1.0f;    // 反射強度スケール
    bool          boxInfluence    = false;   // true のとき球ではなくボックス形状で影響範囲を定義
    math::Vector3 boxExtents      = { 1.0f, 1.0f, 1.0f }; // ボックス半径 [m] (boxInfluence=true 時のみ使用)

    const char* GetTypeName() const { return "ReflectionProbe"; }

    void Reflect(IReflector& r)
    {
        r.Field("enabled",         enabled);
        r.Field("cubemapPath",     cubemapPath);
        r.Field("influenceRadius", influenceRadius);
        r.Field("intensity",       intensity);
        r.Field("boxInfluence",    boxInfluence);
        r.Field("boxExtents",      boxExtents);
    }
};

} // namespace fbzz::scene
