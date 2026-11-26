/// @file    WorldPoseHelpers.hpp
/// @brief   物理と Navigation が共有する位置・回転だけのワールド姿勢書き戻し。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Scene/Hierarchy/Transform.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace fbzz::math {
struct Vector3;
struct Quaternion;
}
namespace fbzz::scene {
class GameObject;
}
namespace fbzz::scene::world_pose_detail {

/// @pre 親のワールド姿勢が確定済みであること。子孫の同期は呼び出し側が行う。
/// @note scale は変更せず、worldRotation は入力をそのまま保持する。
/// @note 親 scale が厳密に 0 の軸だけ localPosition を 0 へ投影する。
FBZZ_ENGINE_API void WriteWorldPoseToTransform(GameObject& go, const math::Vector3& worldPosition,
                                const math::Quaternion& worldRotation);


/// @note AI サービスで検証に成功した演算だけを記録し、シーンやオブジェクトへの参照を保持しない。
struct AiWorldMathEntry {
    Transform value;
    math::Vector3 parentPosition, parentScale;
    math::Quaternion parentRotation;
    bool valid = false, hasParent = false;
};
/// @note 借用ビューを定数初期化し、ボーンの照会ごとの動的なスレッドローカル初期化確認を省く。
struct AiWorldMathWorkspace { AiWorldMathEntry* rows = nullptr; std::size_t count = 0; uint32_t control = UINT32_MAX; };
extern constinit thread_local AiWorldMathWorkspace g_aiWorldMath;

/// @note 作業領域は 65536 エンティティ（8 MiB 未満）まで拡張し、それを超える添字は通常の演算を使う。
FBZZ_ENGINE_API void ReserveAiWorldMath(std::size_t slots);
/// @note コールバックなしで走査する。SSE の丸め、非正規数、例外マスクが変わった場合は検証済みの記録を破棄する。
FBZZ_ENGINE_API void PrepareAiWorldMath();

/// @note 入力のビット列をキーにして符号付きゼロと計算結果を保持し、古いワールド値は常に上書きする。
inline bool TryReuseAiWorldMath(uint32_t slot, Transform& value, const Transform* parent)
{
    if (slot >= g_aiWorldMath.count) {
        return false;
    }
    const auto& cached = g_aiWorldMath.rows[slot];
    const auto same = [](const auto& a, const auto& b) { return std::memcmp(&a, &b, sizeof(a)) == 0; };
    if (!cached.valid || cached.hasParent != (parent != nullptr)
        || (parent && (!same(parent->worldPosition, cached.parentPosition) || !same(parent->worldRotation, cached.parentRotation)
            || !same(parent->worldScale, cached.parentScale)))
        || !same(value.position, cached.value.position) || !same(value.rotation, cached.value.rotation)
        || !same(value.scale, cached.value.scale)) {
        return false;
    }
    value.worldPosition = cached.value.worldPosition;
    value.worldRotation = cached.value.worldRotation;
    value.worldScale = cached.value.worldScale;
    return true;
}

/// @pre ローカル入力、親との乗算、ワールド出力が AI 姿勢サービスによる有限性・正規化可能性の検証を通過していること。
inline void RememberAiWorldMath(uint32_t slot, const Transform& value, const Transform* parent)
{
    if (slot >= g_aiWorldMath.count) {
        return;
    }
    auto& cached = g_aiWorldMath.rows[slot];
    cached.value = value; cached.hasParent = parent != nullptr;
    if (parent) {
        cached.parentPosition = parent->worldPosition;
        cached.parentRotation = parent->worldRotation;
        cached.parentScale = parent->worldScale;
    }
    cached.valid = true;
}

}
