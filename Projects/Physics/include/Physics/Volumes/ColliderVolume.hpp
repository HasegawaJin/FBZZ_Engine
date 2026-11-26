/// @file    ColliderVolume.hpp
/// @brief   Trigger コライダーを範囲として使う空間効果。
/// @author  Hasegawa Jin
/// @date    2026-05-22
#pragma once
#include <Physics/Api.hpp>
#include <Physics/Volumes/Volume.hpp>
#include <Physics/Collision/Shapes/Collider.hpp>

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
        VolumeType type = VolumeType::Gravity; ///< ボリューム内で剛体に適用する効果の種類。
        math::Vector3 gravity = { 0.0f, -9.81f, 0.0f }; ///< 内部の剛体へ適用するワールド空間の重力加速度 [m/s²]。
        math::Vector3 magneticField = { 0.0f, 1.0f, 0.0f }; ///< 内部の剛体に適用する磁場設定。
        float swirlStrength = 1.0f; ///< ボリューム中心のまわりに渦を作る力の強さ。
        float inwardStrength = 0.0f; ///< ボリューム中心へ向かわせる力の強さ。
        float liftStrength = 0.0f; ///< ボリュームの上方向へ掛ける力の強さ。
        float explosionImpulse = 10.0f; ///< 爆発効果の力積の強さ。
        float timeScale = 1.0f; ///< ボリューム内の剛体の刻みに掛ける時間倍率。
        float duration = -1.0f; ///< ボリュームの有効時間 [秒]。
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
        Collider* m_collider = nullptr; ///< 内外判定に使うコライダーの非所有参照。
        VolumeSettings m_settings; ///< 内部に入った剛体へ掛ける効果の設定。
        float m_elapsed = 0.0f; ///< 時限ボリュームを作成してからの経過時間 [秒]。
    };
} /// @note namespace fbzz::physics
