/// @file    WorldRegistrationRetirement.cpp
/// @brief   寿命の切れた登録と、それらを参照し得る物理状態の破棄。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include <Physics/World/World.hpp>
#include "WorldRegistrationSlots.hpp"

namespace fbzz::physics {

using world_registration_detail::NextRegistrationGeneration;

void World::RetireExpiredRegistrations()
{
    bool borrowedRegistrationsRetired = false;
    for (BodySlot& bodySlot : m_bodyPool) {
        if (!bodySlot.body || !bodySlot.lifetime.expired()) {
            continue;
        }
        bodySlot.body = nullptr;
        bodySlot.lifetime.reset();
        bodySlot.generation = NextRegistrationGeneration(bodySlot.generation);
        borrowedRegistrationsRetired = true;
    }

    for (ColliderSlot& colliderSlot : m_colliderPool) {
        if (!colliderSlot.occupied || colliderSlot.lifetime.IsAlive(colliderSlot.collider)) {
            continue;
        }
        colliderSlot.collider = {};
        colliderSlot.lifetime = {};
        colliderSlot.occupied = false;
        colliderSlot.generation = NextRegistrationGeneration(colliderSlot.generation);
        borrowedRegistrationsRetired = true;
    }

    if (m_constraints.RetireExpired()) m_sceneSyncChanged = true;

    if (!borrowedRegistrationsRetired) {
        return;
    }
    /// @note アドレスを再利用した別個体へ接触、warm start、イベントを持ち越さない。
    m_sceneSyncChanged = true;
    m_collisionPairs.clear();
    m_contacts.clear();
    m_contactCache = {};
    m_prevEvents.clear();
    m_enterEvents.clear();
    m_stayEvents.clear();
    m_exitEvents.clear();
    m_frameImpacts.clear();
    m_queryNodes.clear();
    m_broadPhaseDebugSnapshot.reset();
    m_pendingBroadPhaseDebugSnapshot.reset();
    RebuildSceneViews();
}

} /// @note namespace fbzz::physics
