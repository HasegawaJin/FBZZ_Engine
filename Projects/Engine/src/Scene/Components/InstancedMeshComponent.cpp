/// @file    InstancedMeshComponent.cpp
/// @brief   Stable instance IDs and copy-on-write overrides preserve authored and Play scene data.
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include <Engine/Scene/Components/InstancedMeshComponent.hpp>
#include <Engine/Asset/InstanceSetAsset.hpp>
#include <Engine/Asset/MeshInstancesAsset.hpp>
#include <Engine/Asset/DataAssetRegistry.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::scene {
std::shared_ptr<const renderer::InstanceSetData> InstancedMeshComponent::Snapshot() const
{
    if (m_runtime && m_runtimeAssetPath==m_instanceSetRef.path) return m_runtime;
    const auto* value=asset::ResolveMeshInstanceDefinition(m_instanceSetRef.path);
    return value ? value->Snapshot() : nullptr;
}
std::shared_ptr<const renderer::InstanceSetData> InstancedMeshComponent::Snapshot(const asset::InstanceSetAsset& resolvedDefinition) const
{
    if (m_runtime && m_runtimeAssetPath==m_instanceSetRef.path) return m_runtime;
    return resolvedDefinition.Snapshot();
}
bool InstancedMeshComponent::MakeWritable()
{
    const auto source=Snapshot();
    if (!source) return false;
    if (!m_runtime || m_runtimeAssetPath!=m_instanceSetRef.path) {
        m_runtime=std::make_shared<renderer::InstanceSetData>(*source);
        m_runtimeAssetPath=m_instanceSetRef.path;
        m_idToSlot.clear();
        m_nextId=1;
        for (size_t i=0; i<m_runtime->placements.size(); ++i) if (m_runtime->placements[i].active) m_idToSlot.emplace(m_runtime->placements[i].id,i);
        for (const auto& value : m_runtime->placements) m_nextId=std::max(m_nextId,value.id+1);
        m_runtime->parentRevision=source->revision;
        m_runtime->dirtySlots.clear();
    } else if (m_runtime.use_count()>2) {
        m_runtime=std::make_shared<renderer::InstanceSetData>(*m_runtime);
        m_runtime->parentRevision=source->revision;
        m_runtime->dirtySlots.clear();
    }
    return true;
}
bool InstancedMeshComponent::SetTransform(uint32_t id, const math::Vector3& position, const math::Vector3& rotation, const math::Vector3& scale)
{
    const auto finite=[](math::Vector3 v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);};
    if (!finite(position)||!finite(rotation)||!finite(scale)||std::abs(scale.x)<0.00001f||std::abs(scale.y)<0.00001f||std::abs(scale.z)<0.00001f) return false;
    if (!MakeWritable()) return false;
    const auto it=m_idToSlot.find(id); if (it==m_idToSlot.end()) return false;
    auto& value=m_runtime->placements[it->second]; value.position=position; value.eulerRadians=rotation; value.scale=scale;
    m_runtime->dirtySlots.push_back(static_cast<uint32_t>(it->second)); ++m_runtime->revision;
    return true;
}
bool InstancedMeshComponent::Remove(uint32_t id)
{
    if (!MakeWritable()) return false;
    const auto it=m_idToSlot.find(id); if (it==m_idToSlot.end()) return false;
    m_runtime->placements[it->second].active=false;
    m_runtime->dirtySlots.push_back(static_cast<uint32_t>(it->second)); m_idToSlot.erase(it); ++m_runtime->revision;
    return true;
}
void InstancedMeshComponent::ResetOverrides() { m_runtime.reset(); m_idToSlot.clear(); m_runtimeAssetPath.clear(); }
uint32_t InstancedMeshComponent::Add(const math::Vector3& position,const math::Vector3& rotation,const math::Vector3& scale)
{
    const auto finite=[](math::Vector3 v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);};
    if (!finite(position)||!finite(rotation)||!finite(scale)||std::abs(scale.x)<0.00001f||std::abs(scale.y)<0.00001f||std::abs(scale.z)<0.00001f) return 0;
    if (!MakeWritable() || m_nextId>INT32_MAX || m_runtime->placements.size()>=1048576) return 0;
    const uint32_t id=m_nextId++, slot=static_cast<uint32_t>(m_runtime->placements.size());
    m_runtime->placements.push_back({id,position,rotation,scale,true}); m_idToSlot.emplace(id,slot);
    m_runtime->dirtySlots.push_back(slot); ++m_runtime->revision; return id;
}
} /// @note namespace fbzz::scene
