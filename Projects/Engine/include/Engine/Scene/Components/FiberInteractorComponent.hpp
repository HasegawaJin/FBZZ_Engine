/// @file    FiberInteractorComponent.hpp
/// @brief   Collider または手動の球で草を押し曲げる接触設定。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#pragma once
#include <Engine/Scene/Reflection.hpp>

namespace fbzz::scene {
/// @note Collider 使用時は同じ個体の Capsule / Sphere / Box / AABB の順で有効な形状を使う。物理力は発生させない。
/// @note 対応 Collider が無い場合は worldPosition を中心とする radius [m] の球を使う。
struct FiberInteractorComponent {
    bool m_enabled = true;
    bool m_useCollider = true;
    float m_radius = 0.5f;
    float m_strength = 1.0f;
    float m_recoverySeconds = 2.0f;
    const char* GetTypeName() const { return "Fiber Interactor"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", m_enabled);
        r.Field("useCollider", m_useCollider);
        r.FloatRange("radius", m_radius, 0.01f, 10.0f);
        r.FloatRange("strength", m_strength, 0.0f, 1.0f);
        r.FloatRange("recoverySeconds", m_recoverySeconds, 0.05f, 30.0f);
    }
};
} /// @note namespace fbzz::scene
