/// @file    InstancedMeshComponent.hpp
/// @brief   One scene component owns shared mesh placements and Play-local edits.
/// @author  Hasegawa Jin
/// @date    2026-10-10
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Scene/Reflection.hpp>
#include <Graphics/Effects/RenderInstanceInput.hpp>
#include <memory>
#include <unordered_map>

namespace fbzz::asset { class InstanceSetAsset; }

namespace fbzz::scene {
class InstancedMeshComponent {
public:
    struct VegetationBudget {
        bool enabled = false;
        int drawDetailPerCell = 0;
        int nearDetailPerCell = 0;
        int shadowDetailPerCell = 0;
        bool operator==(const VegetationBudget&) const = default;
    };

    bool m_enabled=true, m_castShadows=true;
    float m_maxDistance=0;
    DataAssetRef m_instanceSetRef{"", "InstanceSetAsset"};
    VegetationBudget m_vegetationBudget;
    const char* GetTypeName() const { return "Instanced Mesh"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", m_enabled);
        r.Field("castShadows", m_castShadows);
        r.Field("maxDistance", m_maxDistance);
        r.Field("instanceSetRef", m_instanceSetRef);
        r.BeginField("vegetationCellBudget", "Vegetation Cell Budget (128 m)");
        r.Field("vegetationCellBudget", m_vegetationBudget.enabled);
        r.BeginField("vegetationDrawDetailPerCell", "Detailed Draws Per Cell");
        r.SetFieldMin(0);
        r.Field("vegetationDrawDetailPerCell", m_vegetationBudget.drawDetailPerCell);
        r.BeginField("vegetationNearDetailPerCell", "Highest LOD Per Cell");
        r.SetFieldMin(0);
        r.Field("vegetationNearDetailPerCell", m_vegetationBudget.nearDetailPerCell);
        r.BeginField("vegetationShadowDetailPerCell", "Detailed Shadows Per Cell");
        r.SetFieldMin(0);
        r.Field("vegetationShadowDetailPerCell", m_vegetationBudget.shadowDetailPerCell);
    }
    [[nodiscard]] FBZZ_ENGINE_API std::shared_ptr<const renderer::InstanceSetData> Snapshot() const;
    /// @pre resolvedDefinition is the current m_instanceSetRef.path asset; the reference and registry stay unchanged during this call.
    /// @note Borrows the definition only for this call; matching runtime overrides take priority without repeating the asset lookup.
    [[nodiscard]] FBZZ_ENGINE_API std::shared_ptr<const renderer::InstanceSetData> Snapshot(const asset::InstanceSetAsset& resolvedDefinition) const;
    /// @note IDs survive changes/removal; editing copies the asset or a snapshot shared with Edit/another view.
    [[nodiscard]] FBZZ_ENGINE_API bool SetTransform(uint32_t id, const math::Vector3& position, const math::Vector3& eulerRadians, const math::Vector3& scale);
    [[nodiscard]] FBZZ_ENGINE_API bool Remove(uint32_t id);
    /// @return Newly assigned stable ID, or zero for invalid values/capacity/asset; removed IDs are never reused.
    [[nodiscard]] FBZZ_ENGINE_API uint32_t Add(const math::Vector3& position,const math::Vector3& eulerRadians,const math::Vector3& scale);
    FBZZ_ENGINE_API void ResetOverrides();
private:
    FBZZ_ENGINE_API bool MakeWritable();
    std::shared_ptr<renderer::InstanceSetData> m_runtime;
    std::unordered_map<uint32_t,size_t> m_idToSlot;
    std::string m_runtimeAssetPath;
    uint32_t m_nextId=1;
};
} /// @note namespace fbzz::scene
