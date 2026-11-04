/// @file    FlowVolume.hpp
/// @brief   媒質の流れ (風・水流) が剛体を運ぶ Volume。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#pragma once
#include <Physics/Api.hpp>
#include <Physics/Volume.hpp>
#include <Math/Vector3.hpp>
#include <functional>

namespace fbzz::physics
{
    /// @note FlowVolume 1 つぶんの入力。流速は callback で受けるので、Physics は
    /// @note FlowField も環境風も焼いた速度場も知らない。
    struct FlowVolumeDesc
    {
        /// @note ワールド空間の媒質速度 [m/s]。空なら流れなし。
        /// @note FluidVolumeDesc::flowVelocity と同じ型にしてある。同じ場のスナップショットを
        /// @note 水にも風にもそのまま渡せる。
        std::function<math::Vector3(const math::Vector3& worldPosition)> flowVelocity;
    };

    /// @note 流れが剛体を運ぶ。**F = k * m * (v_flow - v)**。k は体が持つ
    /// @note (RigidBody::GetFlowCoupling)。

    /// @note 半径も減衰も持たない理由: «どこでどれだけ吹くか» は場の側の性質で、
    /// @note この Volume はその場をシーン全体へ配るだけの器。形を二重に持つと、
    /// @note 同じ場が消費者ごとに違う範囲で効くことになる。
    /// @see Docs/design/flow-field.md §2 / §9-6
    class FlowVolume final : public Volume
    {
    public:
        FBZZ_PHYSICS_API explicit FlowVolume(FlowVolumeDesc desc);

        /// @note 常に true。場はシーン全体にあり、半径と減衰は flowVelocity の中で解決済み。
        FBZZ_PHYSICS_API bool Contains(const math::Vector3& position) const override;
        FBZZ_PHYSICS_API void Apply(RigidBody& body, float dt) override;

    private:
        FlowVolumeDesc m_desc;
    };
} /// @note namespace fbzz::physics
