/// @file    WorldConstraintRegistration.cpp
/// @brief   制約の所有、世代付き登録、実行用ビュー。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include <Physics/World/World.hpp>
#include "WorldRegistrationSlots.hpp"
#include <utility>

namespace fbzz::physics {

using world_registration_detail::AcquireVacantSlot;
using world_registration_detail::FindSlotWithGeneration;
using world_registration_detail::MakeRegistrationHandle;
using world_registration_detail::NextRegistrationGeneration;

void World::ConstraintRegistry::Add(std::unique_ptr<Constraint> constraint)
{
    m_ownedConstraints.push_back(std::move(constraint));
    RebuildActiveView();
}

ConstraintHandle World::ConstraintRegistry::Sync(ConstraintHandle previousHandle, std::unique_ptr<Constraint> constraint)
{
    if (!constraint) {
        return {};
    }

    const auto updateConstraintSlot = [this, &constraint](ConstraintSlot& constraintSlot) {
        constraintSlot.constraint = std::move(constraint);
        constraintSlot.touched = true;
        RebuildActiveView();
    };

    if (ConstraintSlot* matchingSlot = FindSlotWithGeneration(m_constraintPool, previousHandle);
        matchingSlot && !matchingSlot->touched) {
        updateConstraintSlot(*matchingSlot);
        return previousHandle;
    }

    const size_t vacantSlotIndex = AcquireVacantSlot(m_constraintPool, [](const ConstraintSlot& constraintSlot) {
        return !constraintSlot.constraint && !constraintSlot.touched;
    });
    ConstraintSlot& constraintSlot = m_constraintPool[vacantSlotIndex];
    updateConstraintSlot(constraintSlot);
    return MakeRegistrationHandle<ConstraintHandle>(vacantSlotIndex, constraintSlot.generation);
}

bool World::ConstraintRegistry::Keep(ConstraintHandle handle)
{
    ConstraintSlot* constraintSlot = FindSlotWithGeneration(m_constraintPool, handle);
    if (!constraintSlot || !constraintSlot->constraint) {
        return false;
    }
    constraintSlot->touched = true;
    return true;
}

Constraint* World::ConstraintRegistry::Find(ConstraintHandle handle) const
{
    const ConstraintSlot* constraintSlot = FindSlotWithGeneration(m_constraintPool, handle);
    return constraintSlot ? constraintSlot->constraint.get() : nullptr;
}

bool World::ConstraintRegistry::Remove(ConstraintHandle handle)
{
    ConstraintSlot* constraintSlot = FindSlotWithGeneration(m_constraintPool, handle);
    if (!constraintSlot || !constraintSlot->constraint) {
        return false;
    }
    constraintSlot->constraint.reset();
    constraintSlot->touched = false;
    constraintSlot->generation = NextRegistrationGeneration(constraintSlot->generation);
    RebuildActiveView();
    return true;
}

void World::ConstraintRegistry::RebuildActiveView()
{
    m_activeConstraints.clear();
    m_activeConstraints.reserve(m_ownedConstraints.size() + m_constraintPool.size());
    for (const auto& constraint : m_ownedConstraints) {
        if (constraint) {
            m_activeConstraints.push_back(constraint.get());
        }
    }
    for (const ConstraintSlot& constraintSlot : m_constraintPool) {
        if (constraintSlot.constraint) {
            m_activeConstraints.push_back(constraintSlot.constraint.get());
        }
    }
}

void World::ConstraintRegistry::BeginSceneSync()
{
    for (auto& slot : m_constraintPool) slot.touched = false;
}

bool World::ConstraintRegistry::EndSceneSync()
{
    bool removed = false;
    for (ConstraintSlot& constraintSlot : m_constraintPool) {
        if (constraintSlot.touched || !constraintSlot.constraint) {
            continue;
        }
        constraintSlot.constraint.reset();
        constraintSlot.generation = NextRegistrationGeneration(constraintSlot.generation);
        removed = true;
    }

    RebuildActiveView();
    return removed;
}

bool World::ConstraintRegistry::RetireExpired()
{
    bool constraintsRetired = std::erase_if(m_ownedConstraints, [](const auto& constraint) {
        return constraint && !constraint->HasLiveBodies();
    }) != 0;
    for (ConstraintSlot& constraintSlot : m_constraintPool) {
        if (!constraintSlot.constraint || constraintSlot.constraint->HasLiveBodies()) {
            continue;
        }
        constraintSlot.constraint.reset();
        constraintSlot.generation = NextRegistrationGeneration(constraintSlot.generation);
        constraintsRetired = true;
    }
    if (constraintsRetired) {
        RebuildActiveView();
    }
    return constraintsRetired;
}

void World::AddConstraint(std::unique_ptr<Constraint> constraint)
{
    m_constraints.Add(std::move(constraint));
}

ConstraintHandle World::SyncConstraint(ConstraintHandle handle, std::unique_ptr<Constraint> constraint)
{
    const ConstraintHandle synced = m_constraints.Sync(handle, std::move(constraint));
    if (synced.IsValid()) m_sceneSyncChanged = true;
    return synced;
}

bool World::KeepConstraint(ConstraintHandle handle)
{
    return m_constraints.Keep(handle);
}

Constraint* World::FindConstraint(ConstraintHandle handle) const
{
    return m_constraints.Find(handle);
}

void World::RemoveConstraint(ConstraintHandle handle)
{
    if (m_constraints.Remove(handle)) m_sceneSyncChanged = true;
}

const std::vector<Constraint*>& World::GetConstraints() const
{
    return m_constraints.Active();
}

} /// @note namespace fbzz::physics
