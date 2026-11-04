/// @file    ManagedAiPoseTransfer.hpp
/// @brief   Atomic world position and rotation commit for callback-scoped AI batches.
/// @author  Hasegawa Jin
/// @date    2026-10-11
#pragma once
#include <Engine/Api.hpp>
#include "ManagedTransformBatch.hpp"
#include <cstddef>
#include <vector>

namespace fbzz::scene::managed_detail {

/// @note Synchronous main-thread service; scratch retains capacity, never ownership of Scene objects.
/// @see https://dev.epicgames.com/documentation/en-us/unreal-engine/overview-of-mass-gameplay-in-unreal-engine MassEntity data-oriented calculations; FBZZ retains its existing Scene hierarchy contract.
class ManagedAiPoseTransfer {
public:
    using ObjectResolver = ManagedTransformBatch::ObjectResolver;
    /// @note AI_CROWD_STEP index=1, integer=count, flags=0; count header then 44-byte identity/pose rows.
    /// @note Missing slots are skipped. Duplicates use the last pose; ancestor/descendant target pairs fail atomically.
    /// @return Failure leaves targets and output unchanged; BUFFER_TOO_SMALL reports required outputBytes.
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
    /// @note Topology is validated once per packet; every intermediate pose still receives its arithmetic validation.
    FBZZ_ENGINE_API bool Update(GameObject& object);
    FBZZ_ENGINE_API bool Stage(Entry& entry);
    FBZZ_ENGINE_API bool ValidateHierarchy(const Scene& scene);
    std::vector<Entry> m_entries;
    std::vector<Transform> m_shadow;
    std::vector<uint32_t> m_cloneEpoch, m_targetEpoch, m_ancestorEpoch, m_pathEpoch, m_subtreeEpoch;
    /// @note Slot-indexed ranges address the packet's immutable, parent-before-child subtree workset.
    std::vector<std::size_t> m_subtreeBegin, m_subtreeEnd;
    std::vector<GameObject*> m_publish, m_ancestors, m_ancestorOrder, m_roots, m_queue;
    uint32_t m_epoch = 0;
};

} /// @note namespace fbzz::scene::managed_detail
