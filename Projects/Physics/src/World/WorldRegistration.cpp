/// @file    WorldRegistration.cpp
/// @brief   剛体・コライダー・ボリュームの登録スロットの更新と再利用。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include <Physics/World/World.hpp>
#include "WorldRegistrationSlots.hpp"
#include <utility>

namespace fbzz::physics {

using world_registration_detail::AcquireVacantSlot;
using world_registration_detail::FindSlotAtHandleIndex;
using world_registration_detail::FindSlotWithGeneration;
using world_registration_detail::MakeRegistrationHandle;

BodyHandle World::SyncBody(BodyHandle previousHandle, RigidBody* body)
{
    if (!body) {
        return {};
    }
    if (const BodySlot* previousSlot = FindSlotAtHandleIndex(m_bodyPool, previousHandle);
        previousSlot && previousSlot->body && previousSlot->lifetime.expired()) {
        RetireExpiredRegistrations();
    }

    const auto updateBodySlot = [body](BodySlot& bodySlot) {
        bodySlot.body = body;
        bodySlot.lifetime = body->m_registrationLifetime.Observe();
        bodySlot.touched = true;
    };

    if (BodySlot* matchingSlot = FindSlotWithGeneration(m_bodyPool, previousHandle);
        matchingSlot && matchingSlot->body && !matchingSlot->touched) {
        updateBodySlot(*matchingSlot);
        return previousHandle;
    }

    // Scene 側でハンドルが失われても、生存中の同一個体は未申告の既存スロットに再接続する。
    for (size_t slotIndex = 0; slotIndex < m_bodyPool.size(); ++slotIndex) {
        BodySlot& bodySlot = m_bodyPool[slotIndex];
        if (bodySlot.body != body) {
            continue;
        }
        if (bodySlot.lifetime.expired()) {
            RetireExpiredRegistrations();
            return SyncBody({}, body);
        }
        if (!bodySlot.touched) {
            updateBodySlot(bodySlot);
            return MakeRegistrationHandle<BodyHandle>(slotIndex, bodySlot.generation);
        }
    }

    const size_t vacantSlotIndex = AcquireVacantSlot(m_bodyPool, [](const BodySlot& bodySlot) {
        return !bodySlot.body;
    });
    BodySlot& bodySlot = m_bodyPool[vacantSlotIndex];
    updateBodySlot(bodySlot);
    m_sceneSyncChanged = true;
    return MakeRegistrationHandle<BodyHandle>(vacantSlotIndex, bodySlot.generation);
}

ColliderHandle World::SyncCollider(ColliderHandle previousHandle, ColliderInstance instance)
{
    if (!instance.collider) {
        return {};
    }
    if (const ColliderSlot* previousSlot = FindSlotAtHandleIndex(m_colliderPool, previousHandle);
        previousSlot && previousSlot->occupied && !previousSlot->lifetime.IsAlive(previousSlot->collider)) {
        RetireExpiredRegistrations();
    }

    const ColliderLifetime instanceLifetime{
        instance.collider->m_registrationLifetime.Observe(),
        instance.body ? instance.body->m_registrationLifetime.Observe() : std::weak_ptr<const void>{},
    };
    // Physics 単体でも形状の慣性と AABB の回転制約を反映する。
    if (instance.body) {
        instance.body->SetInertiaFromCollider(instance.collider);
    }

    const auto updateColliderSlot = [this, &instance, &instanceLifetime](ColliderSlot& colliderSlot) {
        colliderSlot.collider = std::move(instance);
        colliderSlot.lifetime = instanceLifetime;
        colliderSlot.occupied = true;
        colliderSlot.touched = true;
        // 非所有の形状は同期前に直接更新できるので、既存スロットでも接触の再検出が必要。
        m_sceneSyncChanged = true;
    };

    if (ColliderSlot* matchingSlot = FindSlotWithGeneration(m_colliderPool, previousHandle);
        matchingSlot && matchingSlot->occupied && !matchingSlot->touched) {
        updateColliderSlot(*matchingSlot);
        return previousHandle;
    }

    for (size_t slotIndex = 0; slotIndex < m_colliderPool.size(); ++slotIndex) {
        ColliderSlot& colliderSlot = m_colliderPool[slotIndex];
        if (!colliderSlot.occupied || colliderSlot.collider.collider != instance.collider) {
            continue;
        }
        if (!colliderSlot.lifetime.IsAlive(colliderSlot.collider)) {
            RetireExpiredRegistrations();
            return SyncCollider({}, std::move(instance));
        }
        if (!colliderSlot.touched) {
            updateColliderSlot(colliderSlot);
            return MakeRegistrationHandle<ColliderHandle>(slotIndex, colliderSlot.generation);
        }
    }

    const size_t vacantSlotIndex = AcquireVacantSlot(m_colliderPool, [](const ColliderSlot& colliderSlot) {
        return !colliderSlot.occupied;
    });
    ColliderSlot& colliderSlot = m_colliderPool[vacantSlotIndex];
    updateColliderSlot(colliderSlot);
    return MakeRegistrationHandle<ColliderHandle>(vacantSlotIndex, colliderSlot.generation);
}

VolumeHandle World::SyncVolume(VolumeHandle previousHandle, std::unique_ptr<Volume> volume)
{
    if (!volume) {
        return {};
    }

    if (VolumeSlot* matchingSlot = FindSlotWithGeneration(m_volumePool, previousHandle);
        matchingSlot && matchingSlot->volume && !matchingSlot->touched) {
        matchingSlot->volume = std::move(volume);
        matchingSlot->touched = true;
        return previousHandle;
    }

    const size_t vacantSlotIndex = AcquireVacantSlot(m_volumePool, [](const VolumeSlot& volumeSlot) {
        return !volumeSlot.volume;
    });
    VolumeSlot& volumeSlot = m_volumePool[vacantSlotIndex];
    volumeSlot.volume = std::move(volume);
    volumeSlot.touched = true;
    m_sceneSyncChanged = true;
    return MakeRegistrationHandle<VolumeHandle>(vacantSlotIndex, volumeSlot.generation);
}

} /// @note namespace fbzz::physics
