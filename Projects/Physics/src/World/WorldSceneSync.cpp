/// @file    WorldSceneSync.cpp
/// @brief   シーン同期の開始・確定、未申告スロットの解除と非所有ビューの再構築。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include <Physics/World/World.hpp>
#include "WorldRegistrationSlots.hpp"

namespace fbzz::physics {

using world_registration_detail::NextRegistrationGeneration;

namespace {

template<typename Slots>
void ResetSyncMarks(Slots& slots)
{
    for (auto& slot : slots) {
        slot.touched = false;
    }
}

} /// @note namespace

void World::BeginSceneSync()
{
    RetireExpiredRegistrations();
    m_broadPhaseDebugSnapshot.reset();
    m_pendingBroadPhaseDebugSnapshot.reset();
    m_sceneSyncChanged = false;
    ResetSyncMarks(m_bodyPool);
    ResetSyncMarks(m_colliderPool);
    ResetSyncMarks(m_volumePool);
    m_constraints.BeginSceneSync();
}

void World::EndSceneSync()
{
    RetireExpiredRegistrations();

    for (BodySlot& bodySlot : m_bodyPool) {
        if (bodySlot.touched || !bodySlot.body) {
            continue;
        }
        bodySlot.body = nullptr;
        bodySlot.generation = NextRegistrationGeneration(bodySlot.generation);
        m_sceneSyncChanged = true;
    }

    for (ColliderSlot& colliderSlot : m_colliderPool) {
        if (colliderSlot.touched || !colliderSlot.occupied) {
            continue;
        }
        colliderSlot.collider = {};
        colliderSlot.occupied = false;
        colliderSlot.generation = NextRegistrationGeneration(colliderSlot.generation);
        m_sceneSyncChanged = true;
    }

    m_volumes.clear();
    for (VolumeSlot& volumeSlot : m_volumePool) {
        if (!volumeSlot.volume) {
            continue;
        }
        if (volumeSlot.touched) {
            m_volumes.push_back(volumeSlot.volume.get());
        } else {
            volumeSlot.volume.reset();
            volumeSlot.generation = NextRegistrationGeneration(volumeSlot.generation);
            m_sceneSyncChanged = true;
        }
    }

    if (m_constraints.EndSceneSync()) m_sceneSyncChanged = true;
    RebuildSceneViews();
    RefreshQueryIndex();
}

void World::RebuildSceneViews()
{
    m_bodies.clear();
    for (const BodySlot& bodySlot : m_bodyPool) {
        if (bodySlot.body) {
            m_bodies.push_back(bodySlot.body);
        }
    }

    m_colliders.clear();
    m_colliderLifetimes.clear();
    for (const ColliderSlot& colliderSlot : m_colliderPool) {
        if (colliderSlot.occupied) {
            m_colliders.push_back(colliderSlot.collider);
            m_colliderLifetimes.push_back(colliderSlot.lifetime);
        }
    }
}

} /// @note namespace fbzz::physics
