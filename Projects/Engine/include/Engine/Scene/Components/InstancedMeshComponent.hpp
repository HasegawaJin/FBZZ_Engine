/// @file    InstancedMeshComponent.hpp
/// @brief   共有メッシュの配置と Play 中の編集を一つのシーンコンポーネントで管理する。
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
    /// @pre resolvedDefinition は現在の m_instanceSetRef.path に対応し、呼び出し中に参照とレジストリを変更しないこと。
    /// @note 定義は呼び出し中だけ借りる。一致する実行時の上書きを優先し、アセットは引き直さない。
    [[nodiscard]] FBZZ_ENGINE_API std::shared_ptr<const renderer::InstanceSetData> Snapshot(const asset::InstanceSetAsset& resolvedDefinition) const;
    /// @note 編集や削除で既存の ID を振り直さない。編集時はアセットまたは Edit や他ビューと共有するスナップショットを複製する。
    [[nodiscard]] FBZZ_ENGINE_API bool SetTransform(uint32_t id, const math::Vector3& position, const math::Vector3& eulerRadians, const math::Vector3& scale);
    [[nodiscard]] FBZZ_ENGINE_API bool Remove(uint32_t id);
    /// @return 新規の安定 ID。不正な値、容量超過、無効なアセットの場合は 0。削除済み ID は再利用しない。
    [[nodiscard]] FBZZ_ENGINE_API uint32_t Add(const math::Vector3& position,const math::Vector3& eulerRadians,const math::Vector3& scale);
    FBZZ_ENGINE_API void ResetOverrides();
private:
    FBZZ_ENGINE_API bool MakeWritable();
    std::shared_ptr<renderer::InstanceSetData> m_runtime;
    std::unordered_map<uint32_t,size_t> m_idToSlot;
    std::string m_runtimeAssetPath;
    uint32_t m_nextId=1;
};
}
