/// @file    TerrainGridComponent.cpp
/// @brief   地形グリッドのタイル生成と管理の実装。
/// @author  Hasegawa Jin
/// @date    2026-06-16
#include <Engine/Scene/Components/TerrainGridComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>

namespace fbzz::scene {

void TerrainGridComponent::ResolveFromScene(const Scene& scene)
{
    EnsureSize();
    const size_t total = static_cast<size_t>(cellCountX) * static_cast<size_t>(cellCountZ);
    for (size_t i = 0; i < total; ++i) {
        if (i < cellInstanceIds.size() && !cellInstanceIds[i].empty()) {
            if (const GameObject* go = scene.FindByGuid(cellInstanceIds[i]))
                cells[i] = go->GetID();
            else
                cells[i] = EntityID::INVALID;
        } else {
            cells[i] = EntityID::INVALID;
        }
    }
}

void TerrainGridComponent::SyncInstanceIds(const Scene& scene)
{
    const size_t total = static_cast<size_t>(cellCountX) * static_cast<size_t>(cellCountZ);
    cellInstanceIds.resize(total);
    for (size_t i = 0; i < total; ++i) {
        const EntityID id = (i < cells.size()) ? cells[i] : EntityID::INVALID;
        if (id.IsValid()) {
            if (const GameObject* go = scene.GetGameObject(id))
                cellInstanceIds[i] = go->instanceId;
            else
                cellInstanceIds[i] = {};
        } else {
            cellInstanceIds[i] = {};
        }
    }
}

} // namespace fbzz::scene
