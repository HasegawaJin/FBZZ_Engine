/// @file    FlowDrag.hpp
/// @brief   媒質の流れが体を運ぶ結合力。FlowVolume と FluidVolume が共有する 1 本の式。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#pragma once
#include <Math/Vector3.hpp>

namespace fbzz::physics
{
    /// @brief 流れが体を引きずる力 F = k * m * (v_flow - v) [N]。
    /// @param coupling 結合係数 [1/s]。負や 0 を渡しても式は成り立つ (呼び手が弾く)。
    /// @param mass 体の質量 [kg]。
    /// @param flow ワールド空間の媒質速度 [m/s]。
    /// @param velocity 体のワールド速度 [m/s]。
    /// @note 風 (FlowVolume) と水 (FluidVolume) が同じ式を共有する理由: 片方だけ直すと同じノブが
    ///       媒質ごとに違う意味を持ち始めるため。重み付け (沈み率等) は呼び手が coupling に掛けて渡す。
    /// @see Docs/design/flow-field.md §2 / Docs/design/buoyancy.md §5
    [[nodiscard]] inline math::Vector3 FlowDragForce(float coupling, float mass,
                                                     const math::Vector3& flow,
                                                     const math::Vector3& velocity)
    {
        return (flow - velocity) * (coupling * mass);
    }
} // namespace fbzz::physics
