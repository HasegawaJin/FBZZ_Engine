/// @file    InstanceSetAsset.hpp
/// @brief   Shared authored placements and mesh/material LOD parts in a native .fzdata asset.
/// @author  Hasegawa Jin
/// @date    2026-10-10
#pragma once
#include <Engine/Asset/DataAsset.hpp>
#include <Graphics/Effects/RenderInstanceInput.hpp>
#include <array>
#include <memory>
#include <span>
#include <vector>

namespace fbzz::asset {
struct InstanceMeshPart {
    std::string meshPath, materialPath;
    uint32_t lod = 0;
};
class InstanceSetAsset final : public DataAsset {
public:
    static constexpr const char* TYPE_NAME = "InstanceSetAsset";
    const char* GetTypeName() const override { return TYPE_NAME; }
    void Reflect(scene::IReflector& reflector) override;
    /// @return Null for invalid authoring data; otherwise a stable snapshot until the asset changes.
    [[nodiscard]] std::shared_ptr<const renderer::InstanceSetData> Snapshot() const { return m_snapshot; }
    [[nodiscard]] std::span<const InstanceMeshPart> Parts() const { return m_parts; }
    [[nodiscard]] std::span<const float> LodHeights() const { return m_lodHeights; }
    /// @note All parts use the same local placement transforms. IDs are positive and unique.
    [[nodiscard]] bool Replace(std::span<const InstanceMeshPart> parts, std::span<const float> lodHeights,
        std::span<const renderer::InstancePlacement> placements);
private:
    bool Rebuild();
    std::vector<std::string> m_meshPaths, m_materialPaths;
    std::vector<int> m_partLods, m_ids;
    std::vector<float> m_lodHeights{0.2f, 0.08f, 0.02f};
    std::vector<math::Vector3> m_positions, m_rotations, m_scales;
    std::vector<InstanceMeshPart> m_parts;
    std::shared_ptr<const renderer::InstanceSetData> m_snapshot;
    uint64_t m_signature = 0, m_revision = 0;
};
} /// @note namespace fbzz::asset
