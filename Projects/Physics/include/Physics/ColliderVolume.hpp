/// @file    ColliderVolume.hpp
/// @brief   Trigger コライダーを範囲として使う空間効果。
/// @author  Hasegawa Jin
/// @date    2026-05-22
#pragma once
#include <Physics/Api.hpp>
#include <Physics/Volume.hpp>
#include <Physics/Collider.hpp>

namespace fbzz::physics
{
    /// @note Unity の Volume 的な表現。Scene 側の Trigger Collider + VolumeComponent から生成される。
    /// @note 整数を明示し 2 を空けたままにする理由: VolumeComponent::Reflect が type を int で
    /// @note 保存・clamp しているため、廃止した Buoyancy (= 2) の枠を詰めると既存シーンの
    /// @note Volume が別の型になる。浮力は水面 (FluidVolume) が持つ。
    /// @see Docs/design/buoyancy.md
    enum class VolumeType {
        Gravity      = 0,
        Vortex       = 1,
        Explosion    = 3,
        TimeDilation = 4,
        Magnetic     = 5
    };

    /// @note Volume の振る舞いを 1 つの設定構造体にまとめ、エディタと Serializer から扱いやすくする。
    struct VolumeSettings {
        VolumeType type = VolumeType::Gravity;
        math::Vector3 gravity = { 0.0f, -9.81f, 0.0f };
        math::Vector3 magneticField = { 0.0f, 1.0f, 0.0f };
        float swirlStrength = 1.0f;
        float inwardStrength = 0.0f;
        float liftStrength = 0.0f;
        float explosionImpulse = 10.0f;
        float timeScale = 1.0f;
        float duration = -1.0f;
    };

    /// @note 形状判定は Collider に委譲し、効果だけを VolumeSettings で切り替える。
    class ColliderVolume : public Volume
    {
    public:
        /// @note Collider の所有権は ColliderComponent (unique_ptr) が持つ。Volume はそれより短命な
        /// @note ため、非所有ポインタで参照する。
        FBZZ_PHYSICS_API ColliderVolume(Collider* collider, const VolumeSettings& settings);

        FBZZ_PHYSICS_API bool Contains(const math::Vector3& position) const override;
        FBZZ_PHYSICS_API void Apply(RigidBody& body, float dt) override;
        FBZZ_PHYSICS_API float GetTimeScale() const override;
        FBZZ_PHYSICS_API bool OverridesGravity() const override;
        FBZZ_PHYSICS_API bool IsExpired() const override;

        FBZZ_PHYSICS_API void Tick(float dt) override;

    private:
        Collider* m_collider = nullptr;
        VolumeSettings m_settings;
        float m_elapsed = 0.0f;
    };
} /// @note namespace fbzz::physics
