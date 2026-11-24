/// @file    ManagedAiPoseTransfer.hpp
/// @brief   AI のコールバック内で、ワールド位置と回転のバッチを原子的に確定する。
/// @author  Hasegawa Jin
/// @date    2026-10-11
#pragma once
#include <Engine/Api.hpp>
#include "ManagedTransformBatch.hpp"
#include <cstddef>
#include <vector>

namespace fbzz::scene::managed_detail {

/// @note メインスレッドで同期実行する。作業領域が保持するのは容量で、シーンのオブジェクトは所有しない。
/// @see https://dev.epicgames.com/documentation/en-us/unreal-engine/overview-of-mass-gameplay-in-unreal-engine MassEntity data-oriented calculations; FBZZ retains its existing Scene hierarchy contract.
class ManagedAiPoseTransfer {
public:
    using ObjectResolver = ManagedTransformBatch::ObjectResolver;
    /// @note AI_CROWD_STEP は index=1、integer=count、flags=0。件数ヘッダーに続き、識別情報と姿勢の 44 バイト行が並ぶ。
    /// @note 欠けたスロットは飛ばす。同一対象では最後の姿勢を使い、祖先と子孫がともに対象ならパケット全体を拒否する。
    /// @return 失敗時は対象も出力も変更しない。BUFFER_TOO_SMALL は必要な outputBytes を返す。
    FBZZ_ENGINE_API ManagedBridgeStatus Run(Scene& scene, const ManagedBridgeRequest& request,
        ManagedBridgeReply& reply, ObjectResolver resolver, void* resolverState);

private:
    struct Entry {
        GameObject* object = nullptr;
        ManagedObjectStatus status = ManagedObjectStatus::UNSET;
        math::Vector3 position = math::Vector3::ZERO;
        math::Quaternion rotation = math::Quaternion::Identity();
    };
    FBZZ_ENGINE_API void BeginEpoch(const Scene& scene);
    FBZZ_ENGINE_API Transform* Shadow(GameObject& object);
    /// @note 階層の構造はパケットごとに一度検証し、途中の姿勢はすべて演算を検証する。
    FBZZ_ENGINE_API bool Update(GameObject& object);
    FBZZ_ENGINE_API bool Stage(Entry& entry);
    FBZZ_ENGINE_API bool ValidateHierarchy(const Scene& scene);
    std::vector<Entry> m_entries;
    std::vector<Transform> m_shadow;
    std::vector<uint32_t> m_cloneEpoch, m_targetEpoch, m_ancestorEpoch, m_pathEpoch, m_subtreeEpoch;
    /// @note スロット別の範囲は、パケット内の不変な部分木の作業領域を親から子の順で参照する。
    std::vector<std::size_t> m_subtreeBegin, m_subtreeEnd;
    std::vector<GameObject*> m_publish, m_ancestors, m_ancestorOrder, m_roots, m_queue;
    uint32_t m_epoch = 0;
};

}
