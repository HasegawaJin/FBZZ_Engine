/// @file    NavMeshModifierComponent.hpp
/// @brief   オブジェクトのコライダーを NavMesh ベイクの障害物または歩行面として扱う。
/// @author  Hasegawa Jin
/// @date    2026-06-17
/// @note 既定値 NotWalkable は旧 NavMeshObstacleComponent と同じ障害物指定。
/// @note Terrain を伴わない室内床や足場のコライダーは Walkable で歩行面に加えられる。
#pragma once
#include <Engine/Scene/Reflection.hpp>
#include <cstdint>

namespace fbzz::scene {

enum class NavMeshModifierMode : uint8_t {
    NotWalkable,  ///< @note 障害物: Bake 時にこの GO の Collider を歩行不可領域として除外
    Walkable,     ///< @note 歩行可: Bake 時にこの GO の Collider を歩行可能面ソースとして追加
};

struct NavMeshModifierComponent {
    NavMeshModifierMode mode    = NavMeshModifierMode::NotWalkable;
    /// @note Walkable モード専用: このコライダーが生成するポリゴンに割り当てるエリアタイプ ID (0〜31)。
    /// @note NavMeshSurfaceComponent::areaCosts[areaType] がパスコスト計算に使われる。
    int areaType = 0;
    bool                enabled = true;

    const char* GetTypeName() const { return "NavMesh Modifier"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        int modeValue = static_cast<int>(mode);
        r.Field("mode", modeValue);
        modeValue = modeValue < 0 ? 0 : (modeValue > 1 ? 1 : modeValue);
        mode = static_cast<NavMeshModifierMode>(modeValue);
        r.Field("areaType", areaType);
    }
};

}
