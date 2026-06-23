// FBZZ Engine
// NavMeshModifierComponent.hpp | fbzz::scene
// NavMesh Bake に対して、この GO のコライダーを「歩行不可」または「歩行可」として
// 明示的に修飾するコンポーネント。
//
// NotWalkable (デフォルト):
//   この GO のコライダーを NavMesh から除外する障害物として扱う。
//   旧 NavMeshObstacleComponent と同等。
//
// Walkable:
//   この GO のコライダーを歩行可能面として NavMesh Bake ソースに追加する。
//   Terrain のない室内床・プラットフォームなど、明示的にウォーカブル指定が
//   必要なオブジェクトに付ける。
#pragma once
#include <Engine/Scene/Script.hpp>
#include <cstdint>

namespace fbzz::scene {

enum class NavMeshModifierMode : uint8_t {
    NotWalkable,  // 障害物: Bake 時にこの GO の Collider を歩行不可領域として除外
    Walkable,     // 歩行可: Bake 時にこの GO の Collider を歩行可能面ソースとして追加
};

struct NavMeshModifierComponent {
    NavMeshModifierMode mode    = NavMeshModifierMode::NotWalkable;
    // Walkable モード専用: このコライダーが生成するポリゴンに割り当てるエリアタイプ ID (0〜31)。
    // NavMeshSurfaceComponent::areaCosts[areaType] がパスコスト計算に使われる。
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

} // namespace fbzz::scene
