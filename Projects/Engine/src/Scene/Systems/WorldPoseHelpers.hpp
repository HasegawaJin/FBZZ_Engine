/// @file    WorldPoseHelpers.hpp
/// @brief   物理と Navigation が共有する位置・回転だけのワールド姿勢書き戻し。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#pragma once
#include <Engine/Scene/Transform.hpp>
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
void WriteWorldPoseToTransform(GameObject& go, const math::Vector3& worldPosition,
                                const math::Quaternion& worldRotation);


/// @note Only successfully validated AI arithmetic is recorded; entries contain no Scene or object references.
struct AiWorldMathEntry {
    Transform value;
    math::Vector3 parentPosition, parentScale;
    math::Quaternion parentRotation;
    bool valid = false, hasParent = false;
};
/// @note A constant-initialized borrowed view avoids dynamic TLS initialization checks on every bone lookup.
struct AiWorldMathWorkspace { AiWorldMathEntry* rows = nullptr; std::size_t count = 0; uint32_t control = UINT32_MAX; };
extern constinit thread_local AiWorldMathWorkspace g_aiWorldMath;

/// @note Scratch grows up to 65536 entity slots (under 8 MiB); larger indices retain ordinary computation.
void ReserveAiWorldMath(std::size_t slots);
/// @note Traversals contain no callbacks; reset pure arithmetic proofs when SSE rounding, denormals or exception masks change.
void PrepareAiWorldMath();

/// @note Bitwise input keys preserve signed zero and exact arithmetic; stale world fields are always replaced.
inline bool TryReuseAiWorldMath(uint32_t slot, Transform& value, const Transform* parent)
{
    if (slot >= g_aiWorldMath.count) return false;
    const auto& cached = g_aiWorldMath.rows[slot];
    const auto same = [](const auto& a, const auto& b) { return std::memcmp(&a, &b, sizeof(a)) == 0; };
    if (!cached.valid || cached.hasParent != (parent != nullptr)
        || (parent && (!same(parent->worldPosition, cached.parentPosition) || !same(parent->worldRotation, cached.parentRotation)
            || !same(parent->worldScale, cached.parentScale)))
        || !same(value.position, cached.value.position) || !same(value.rotation, cached.value.rotation)
        || !same(value.scale, cached.value.scale)) return false;
    value.worldPosition = cached.value.worldPosition;
    value.worldRotation = cached.value.worldRotation;
    value.worldScale = cached.value.worldScale;
    return true;
}

/// @pre Local inputs, parent multiplication and world outputs passed the AI pose service's finite/normalizable validation.
inline void RememberAiWorldMath(uint32_t slot, const Transform& value, const Transform* parent)
{
    if (slot >= g_aiWorldMath.count) return;
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
