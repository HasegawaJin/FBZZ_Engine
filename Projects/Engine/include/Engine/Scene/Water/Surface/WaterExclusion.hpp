/// @file    WaterExclusion.hpp
/// @brief   水面照会と描画が共有する不変の方向付き除外ボックス。
/// @author  Hasegawa Jin
/// @date    2026-10-09
#pragma once
#include <Engine/Api.hpp>

#include <Math/LinearAlgebra/Quaternion.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>
#include <cstdint>
#include <vector>

namespace fbzz::scene {

class Scene;
struct WaterComponent;
enum class WaterSurfaceReadView : std::uint8_t;

/// @brief ワールド空間の方向付き除外ボックス。ワールドからローカルへの数値変換を保持する。
struct WaterExclusionBox {
    math::Vector3 worldPosition = math::Vector3::ZERO;
    math::Quaternion inverseWorldRotation = math::Quaternion::Identity();
    math::Vector3 inverseWorldScale = math::Vector3::ONE;
    math::Vector3 halfExtent = math::Vector3::ZERO;

    /// @brief 記録済みの符号付きワールドスケールでワールド座標を判定する。
    [[nodiscard]] FBZZ_ENGINE_API bool Contains(const math::Vector3& worldPoint) const;
};

/// @brief Scene の読み取り境界で確定した水面ごとの不変の除外集合。
struct WaterExclusionSnapshot {
    std::vector<WaterExclusionBox> boxes;
    std::uint64_t signature = 0;
    bool valid = true;
};

/// @brief Water の利用側が参照する LIVE_QUERY または固定ステップ用の除外スナップショットを選ぶ。
[[nodiscard]] FBZZ_ENGINE_API const WaterExclusionSnapshot* WaterSurfaceExclusions(
    const WaterComponent& water, WaterSurfaceReadView view);

/// @brief 記録済みの除外集合にワールド座標が含まれるとき true。
[[nodiscard]] FBZZ_ENGINE_API bool IsWaterPointExcluded(
    const WaterExclusionSnapshot* snapshot, const math::Vector3& worldPoint);

/// @brief 水面 Zone の所有関係を解決後に除外スナップショットを構築または再利用する。
FBZZ_ENGINE_API void PrepareWaterExclusionSnapshots(Scene& scene);

}
