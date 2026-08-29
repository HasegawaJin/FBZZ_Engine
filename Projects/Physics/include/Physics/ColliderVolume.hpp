/// @file    ColliderVolume.hpp
/// @brief   Trigger コライダーを範囲として使う空間効果。
/// @author  Hasegawa Jin
/// @date    2026-05-22
#pragma once
#include <Physics/Volume.hpp>
#include <Physics/Collider.hpp>

namespace fbzz::physics
{
    // Unity の Volume 的な表現。Scene 側の Trigger Collider + VolumeComponent から生成される。
    enum class VolumeType {
        Gravity,
        Vortex,
        Buoyancy,
        Explosion,
        TimeDilation,
        Magnetic
    };

    // Volume の振る舞いを 1 つの設定構造体にまとめ、エディタと Serializer から扱いやすくする。
    struct VolumeSettings {
        VolumeType type = VolumeType::Gravity;
        math::Vector3 gravity = { 0.0f, -9.81f, 0.0f };
        math::Vector3 magneticField = { 0.0f, 1.0f, 0.0f };
        float swirlStrength = 1.0f;
        float inwardStrength = 0.0f;
        float liftStrength = 0.0f;
        float buoyancy = 10.0f;
        float drag = 1.0f;
        float explosionImpulse = 10.0f;
        float timeScale = 1.0f;
        float duration = -1.0f;
    };

    // 形状判定は Collider に委譲し、効果だけを VolumeSettings で切り替える。
    class ColliderVolume : public Volume
    {
    public:
        // WHY: Collider の所有権は ColliderComponent (unique_ptr) が持つ。
        //      Volume は ColliderComponent より短命なため、非所有ポインタで参照する。
        ColliderVolume(Collider* collider, const VolumeSettings& settings);

        bool Contains(const math::Vector3& position) const override;
        void Apply(RigidBody& body, float dt) override;
        float GetTimeScale() const override;
        bool OverridesGravity() const override;
        bool IsExpired() const override;

        void Tick(float dt) override;

    private:
        Collider* m_collider = nullptr;
        VolumeSettings m_settings;
        float m_elapsed = 0.0f;
    };
} // namespace fbzz::physics
