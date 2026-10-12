/// @file    ManagedAiPoseTransfer.hpp
/// @brief   Atomic world position and rotation commit for callback-scoped AI batches.
/// @author  Hasegawa Jin
/// @date    2026-10-11
#pragma once
#include "ManagedTransformBatch.hpp"
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
    ManagedBridgeStatus Run(Scene& scene, const ManagedBridgeRequest& request,
        ManagedBridgeReply& reply, ObjectResolver resolver, void* resolverState);

private:
    struct Entry {
        GameObject* object = nullptr;
        ManagedObjectStatus status = ManagedObjectStatus::UNSET;
        math::Vector3 position = math::Vector3::ZERO;
        math::Quaternion rotation = math::Quaternion::Identity();
    };
    void BeginEpoch(const Scene& scene);
    Transform* Shadow(GameObject& object);
    bool Update(GameObject& object);
    bool Synchronize(GameObject& root);
    bool Stage(Entry& entry);
    bool ValidateHierarchy(const Scene& scene);
    std::vector<Entry> m_entries;
    std::vector<Transform> m_shadow;
    std::vector<uint32_t> m_cloneEpoch, m_targetEpoch;
    std::vector<GameObject*> m_publish, m_ancestors, m_queue;
    uint32_t m_epoch = 0;
};

} /// @note namespace fbzz::scene::managed_detail
