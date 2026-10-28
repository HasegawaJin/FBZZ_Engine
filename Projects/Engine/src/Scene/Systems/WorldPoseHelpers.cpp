/// @file    WorldPoseHelpers.cpp
/// @brief   scale を保持する位置・回転のワールド姿勢更新。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include "WorldPoseHelpers.hpp"
#include <Engine/Scene/GameObject.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <xmmintrin.h>
#include <vector>

namespace fbzz::scene::world_pose_detail {

constinit thread_local AiWorldMathWorkspace g_aiWorldMath;
namespace {
struct OwnedAiWorldMath {
    std::vector<AiWorldMathEntry> rows;
    ~OwnedAiWorldMath() { g_aiWorldMath.rows = nullptr; g_aiWorldMath.count = 0; }
};
thread_local OwnedAiWorldMath s_ownedAiWorldMath;
}


void PrepareAiWorldMath()
{
    const uint32_t control = _mm_getcsr() & 0xFFC0u;
    if (g_aiWorldMath.control == control) return;
    for (std::size_t index = 0; index < g_aiWorldMath.count; ++index) g_aiWorldMath.rows[index].valid = false;
    g_aiWorldMath.control = control;
}

void ReserveAiWorldMath(std::size_t slots)
{
    constexpr std::size_t MAX_MEMO_SLOTS = 65536;
    const auto count = slots < MAX_MEMO_SLOTS ? slots : MAX_MEMO_SLOTS;
    auto& rows = s_ownedAiWorldMath.rows;
    if (rows.size() < count) rows.resize(count);
    g_aiWorldMath.rows = rows.data(); g_aiWorldMath.count = rows.size();
    PrepareAiWorldMath();
}


void WriteWorldPoseToTransform(GameObject& go, const math::Vector3& worldPosition,
                                const math::Quaternion& worldRotation)
{
    auto& tf = go.transform;
    if (auto* parent = go.GetParent()) {
        const auto& parentTf = parent->transform;
        const math::Quaternion invParentRot = parentTf.worldRotation.Inverse();
        const math::Vector3 parentSpace = invParentRot * (worldPosition - parentTf.worldPosition);
        tf.position = {
            parentTf.worldScale.x == 0.0f ? 0.0f : parentSpace.x / parentTf.worldScale.x,
            parentTf.worldScale.y == 0.0f ? 0.0f : parentSpace.y / parentTf.worldScale.y,
            parentTf.worldScale.z == 0.0f ? 0.0f : parentSpace.z / parentTf.worldScale.z
        };
        tf.rotation = (invParentRot * worldRotation).Normalized();
    } else {
        tf.position = worldPosition;
        tf.rotation = worldRotation;
    }
    tf.worldPosition = worldPosition;
    tf.worldRotation = worldRotation;
}

}
