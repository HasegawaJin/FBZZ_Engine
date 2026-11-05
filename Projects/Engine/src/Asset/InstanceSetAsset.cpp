/// @file    InstanceSetAsset.cpp
/// @brief   Validate shared instance authoring and publish immutable placement snapshots.
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include <Engine/Asset/InstanceSetAsset.hpp>
#include <Engine/Asset/DataAssetFactory.hpp>
#include <Engine/Core/Logger.hpp>
#include <cmath>
#include <bit>
#include <limits>
#include <unordered_set>
#include <atomic>

namespace fbzz::asset {
namespace {
bool Finite(math::Vector3 value) { return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z); }
}
void InstanceSetAsset::Reflect(scene::IReflector& r)
{
    r.ListField("meshPaths", m_meshPaths); r.ListField("materialPaths", m_materialPaths);
    r.ListField("partLods", m_partLods); r.ListField("lodHeights", m_lodHeights);
    r.ListField("ids", m_ids); r.ListField("positions", m_positions);
    r.ListField("rotations", m_rotations); r.ListField("scales", m_scales);
    if (!Rebuild()) FBZZ_LOG_WARN("InstanceSetAsset: invalid parts, LODs, IDs or placement columns");
}
bool InstanceSetAsset::Replace(std::span<const InstanceMeshPart> parts, std::span<const float> heights,
    std::span<const renderer::InstancePlacement> placements)
{
    InstanceSetAsset staged;
    staged.m_lodHeights.assign(heights.begin(), heights.end());
    for (const auto& part : parts) {
        staged.m_meshPaths.push_back(part.meshPath); staged.m_materialPaths.push_back(part.materialPath);
        if (part.lod > 3) return false;
        staged.m_partLods.push_back(static_cast<int>(part.lod));
    }
    for (const auto& value : placements) {
        if (!value.active || value.id > static_cast<uint32_t>(std::numeric_limits<int>::max())) return false;
        staged.m_ids.push_back(static_cast<int>(value.id)); staged.m_positions.push_back(value.position);
        staged.m_rotations.push_back(value.eulerRadians); staged.m_scales.push_back(value.scale);
    }
    staged.m_revision = m_revision;
    if (!staged.Rebuild()) return false;
    *this = std::move(staged);
    return true;
}
bool InstanceSetAsset::Rebuild()
{
    const size_t count = m_positions.size();
    const auto invalid = [&] { m_snapshot.reset(); m_parts.clear(); return false; };
    if (count > 1048576 || m_ids.size()!=count || m_rotations.size()!=count || m_scales.size()!=count
        || m_meshPaths.empty() || m_meshPaths.size()>64 || m_materialPaths.size()!=m_meshPaths.size()
        || m_partLods.size()!=m_meshPaths.size() || m_lodHeights.empty() || m_lodHeights.size()>4) return invalid();
    float previous = 2.0f;
    for (float value : m_lodHeights) {
        if (!std::isfinite(value) || value<0 || value>=previous || value>1) return invalid();
        previous=value;
    }
    uint64_t hash=1469598103934665603ull;
    const auto add=[&](uint64_t value){ hash=(hash^value)*1099511628211ull; };
    const auto vector=[&](math::Vector3 value){ add(std::bit_cast<uint32_t>(value.x)); add(std::bit_cast<uint32_t>(value.y)); add(std::bit_cast<uint32_t>(value.z)); };
    std::unordered_set<int> ids;
    for (size_t i=0; i<count; ++i) {
        if (m_ids[i]<=0 || !ids.insert(m_ids[i]).second || !Finite(m_positions[i]) || !Finite(m_rotations[i])
            || !Finite(m_scales[i]) || std::abs(m_scales[i].x)<0.00001f || std::abs(m_scales[i].y)<0.00001f || std::abs(m_scales[i].z)<0.00001f) return invalid();
        add(static_cast<uint32_t>(m_ids[i])); vector(m_positions[i]); vector(m_rotations[i]); vector(m_scales[i]);
    }
    std::array<bool,4> hasLod{};
    for (size_t i=0; i<m_meshPaths.size(); ++i) {
        if (m_meshPaths[i].empty() || m_materialPaths[i].empty() || m_partLods[i]<0
            || static_cast<size_t>(m_partLods[i])>=m_lodHeights.size()) return invalid();
        hasLod[m_partLods[i]]=true;
        for (unsigned char c : m_meshPaths[i]) add(c); add(0);
        for (unsigned char c : m_materialPaths[i]) add(c); add(0);
        add(static_cast<uint32_t>(m_partLods[i]));
    }
    for (size_t i=0; i<m_lodHeights.size(); ++i) { if (!hasLod[i]) return invalid(); add(std::bit_cast<uint32_t>(m_lodHeights[i])); }
    add(count);
    if (m_snapshot && hash==m_signature) return true;
    /// @note Importer の再読込で別実体になっても、描画側へ渡す世代は過去の値と衝突させない。
    static std::atomic<uint64_t> nextRevision{1};
    auto snapshot=std::make_shared<renderer::InstanceSetData>(); snapshot->revision=m_revision=nextRevision.fetch_add(1, std::memory_order_relaxed);
    snapshot->placements.reserve(count);
    for (size_t i=0; i<count; ++i) snapshot->placements.push_back({static_cast<uint32_t>(m_ids[i]),m_positions[i],m_rotations[i],m_scales[i],true});
    m_parts.clear();
    for (size_t i=0; i<m_meshPaths.size(); ++i) m_parts.push_back({m_meshPaths[i],m_materialPaths[i],static_cast<uint32_t>(m_partLods[i])});
    m_signature=hash; m_snapshot=std::move(snapshot);
    return true;
}
} /// @note namespace fbzz::asset
FBZZ_REGISTER_BUILTIN_DATA_ASSET(::fbzz::asset::InstanceSetAsset);
