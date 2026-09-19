/// @file    FiberInteractorComponent.hpp
/// @brief   草を押し倒すワールド空間の球状接触範囲。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#pragma once
#include <Engine/Scene/Script.hpp>

namespace fbzz::scene {
/// @note 中心は GameObject の worldPosition。足元へ配置する。物理力は発生させない。
struct FiberInteractorComponent {
    bool m_enabled = true;
    float m_radius = 0.5f;
    float m_strength = 1.0f;
    float m_recoverySeconds = 2.0f;
    const char* GetTypeName() const { return "Fiber Interactor"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", m_enabled);
        r.FloatRange("radius", m_radius, 0.01f, 10.0f);
        r.FloatRange("strength", m_strength, 0.0f, 1.0f);
        r.FloatRange("recoverySeconds", m_recoverySeconds, 0.05f, 30.0f);
    }
};
} // namespace fbzz::scene
