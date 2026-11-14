/// @file    WorldPcgAuthoring.cpp
/// @brief   Read-only PCG planning and checked cell-owned authoring transactions.
/// @author  Hasegawa Jin
/// @date    2026-11-07
#include "WorldAuthoring.hpp"
#include "TerrainBrush.hpp"
#include "TerrainPaintClearance.hpp"
#include <Editor/EditorContext.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/Selection.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/GuidRefCodec.hpp>
#include <Engine/Asset/MeshInstancesAsset.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Profiler/ProfileScope.hpp>
#include <Engine/Scene/Components/InstancedMeshComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Components/WorldPartitionComponent.hpp>
#include <Engine/Scene/Components/WorldPcgAuthoringComponent.hpp>
#include <Engine/Scene/ComponentRegistry.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Scene/TerrainSurfaceQuery.hpp>
#include <Engine/Scene/WaterSurfaceQuery.hpp>
#include <Engine/Scene/WorldPartitionManifest.hpp>
#include <Engine/Scene/WorldPcg.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Math/Vector4.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <unordered_set>
#include <unordered_map>
#include <type_traits>
#include <utility>

namespace fbzz::editor {
namespace world_pcg_profile_draft {
struct NodeIdentity {scene::EntityID entity{};};
bool ReadProfileBytes(const std::string& path,std::string& bytes);
std::shared_ptr<NodeIdentity> NodeToken(uint64_t generation,const std::string& id,scene::EntityID entity);
template<typename T> std::shared_ptr<uint64_t> LifetimeToken(uint64_t lifetime);
void SyncGeneratedToken(uint64_t generation,scene::GameObject& owner,const std::shared_ptr<uint64_t>& token,const std::shared_ptr<uint64_t>& instanceToken);
}
namespace world_pcg_authoring {

std::string ProfilePath(EditorContext& context,const std::string& input,bool allowMissing);
std::string HashText(std::string_view text);
bool Editable(EditorContext& context);

void AddHash(uint64_t& hash,uint64_t value) { hash=(hash^value)*1099511628211ull; }
void AddHash(uint64_t& hash,std::string_view value) { for(unsigned char c:value)AddHash(hash,c);AddHash(hash,0); }
void AddFloat(uint64_t& hash,float value) { AddHash(hash,std::bit_cast<uint32_t>(value)); }
void AddVector(uint64_t& hash,const math::Vector3& value) { AddFloat(hash,value.x);AddFloat(hash,value.y);AddFloat(hash,value.z); }
std::string Hex(uint64_t hash) { std::ostringstream text;text<<std::hex<<hash;return text.str(); }
bool Finite(const math::Vector3& p) { return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z); }

std::string HashText(std::string_view text) {uint64_t hash=1469598103934665603ull;for(unsigned char c:text)AddHash(hash,c);return Hex(hash);}
bool Editable(EditorContext& context)
{
    return context.activeScene&&!context.managedBuildBusy&&(context.editScene==nullptr||context.editScene==context.activeScene)
        &&(!context.playMode||(context.playMode->IsInEditor()&&!context.playMode->HasPendingRestore()));
}
std::string ProfilePath(EditorContext& context,const std::string& input,bool allowMissing)
{
    if(input.empty()||context.projectRoot.empty())return {};
    auto value=input.starts_with("guid:")?asset::AssetDatabase::PathFromGuid(asset::AssetDatabase::GuidFromRef(input)):input;if(value.empty())return {};
    auto path=util::FileSystem::PathFromUtf8(value);if(path.is_relative())path=util::FileSystem::PathFromUtf8(context.projectRoot)/path;
    std::error_code error;path=std::filesystem::weakly_canonical(path,error);if(error||path.extension()!=".pcg")return {};
    const auto root=std::filesystem::weakly_canonical(util::FileSystem::PathFromUtf8(context.projectRoot)/"Assets",error);if(error)return {};
    const auto relative=path.lexically_relative(root);if(relative.empty()||relative=="."||relative.is_absolute()||*relative.begin()=="..")return {};
    if(!allowMissing&&!std::filesystem::is_regular_file(path,error))return {};if(error)return {};
    return util::FileSystem::PathToUtf8(path);
}

bool ReadWorld(const scene::GameObject& object,scene::Transform& out)
{
    if (!object.GetParent()) {
        out = object.transform;
        scene::UpdateWorldTransform(out, nullptr);
        return Finite(out.worldPosition) && Finite(out.worldScale)
            && std::isfinite(out.worldRotation.Length()) && out.worldRotation.Length() >= 0.000001f;
    }
    std::vector<const scene::GameObject*> chain;
    for(auto* node=&object;node;node=node->GetParent()){
        if(chain.size()>=4096||std::find(chain.begin(),chain.end(),node)!=chain.end())return false;
        chain.push_back(node);
    }
    scene::Transform world;bool parent=false;
    for(auto iterator=chain.rbegin();iterator!=chain.rend();++iterator){auto next=(*iterator)->transform;
        scene::UpdateWorldTransform(next,parent?&world:nullptr);if(!Finite(next.worldPosition)||!Finite(next.worldScale)||!std::isfinite(next.worldRotation.Length())||next.worldRotation.Length()<0.000001f)return false;
        world=next;parent=true;}
    out=world;return true;
}

bool SameGenerated(const scene::WorldPcgGeneratedComponent& a,const scene::WorldPcgGeneratedComponent& b)
{ return a.volumeId==b.volumeId&&a.ruleKey==b.ruleKey&&a.cellX==b.cellX&&a.cellZ==b.cellZ&&a.signature==b.signature&&a.manualOverride==b.manualOverride&&a.includeHlod==b.includeHlod; }

asset::MeshInstancesAsset* PlacementAsset(const std::string& reference, std::string* resolvedPath = nullptr)
{
    return asset::LoadMeshInstancesReference(reference, resolvedPath);
}

std::string_view PcgAssetIdentity(std::string_view encodedReference)
{
    return asset::AssetDatabase::IsGuidRef(encodedReference)
        ? encodedReference.substr(0, encodedReference.find(asset::AssetDatabase::kRefHintSeparator))
        : encodedReference;
}

std::string OwnerSignature(scene::GameObject& owner, const scene::WorldPcgGeneratedComponent& metadata,
    const scene::InstancedMeshComponent& component, const asset::MeshInstancesAsset& asset,
    const scene::Transform& world, const renderer::InstanceSetData& snapshot, std::string_view placementRef)
{
    uint64_t hash=1469598103934665603ull;
    AddHash(hash,owner.instanceId);AddHash(hash,owner.name);AddHash(hash,owner.tag);AddHash(hash,static_cast<uint64_t>(owner.layer));
    AddHash(hash,owner.activeSelf()?1:0);AddHash(hash,owner.GetChildCount());AddHash(hash,owner.GetParent()?owner.GetParent()->instanceId:std::string{});
    AddVector(hash,world.worldPosition);AddVector(hash,world.worldScale);
    AddFloat(hash,world.worldRotation.x);AddFloat(hash,world.worldRotation.y);AddFloat(hash,world.worldRotation.z);AddFloat(hash,world.worldRotation.w);
    AddHash(hash,metadata.volumeId);AddHash(hash,metadata.ruleKey);AddHash(hash,static_cast<uint32_t>(metadata.cellX));AddHash(hash,static_cast<uint32_t>(metadata.cellZ));AddHash(hash,metadata.includeHlod?1:0);
    AddHash(hash, placementRef);
    AddHash(hash,component.m_enabled?1:0);AddHash(hash,component.m_castShadows?1:0);AddFloat(hash,component.m_maxDistance);
    AddHash(hash,asset.terrainPaintRef);
    size_t extraComponents=0;scene::ForEachRegisteredComponent([&]<typename T,typename Registration>(){
        (void)sizeof(Registration);if constexpr(!std::is_same_v<T,scene::InstancedMeshComponent>&&!std::is_same_v<T,scene::WorldPcgGeneratedComponent>)
            if(owner.GetComponent<T>())++extraComponents;});AddHash(hash,extraComponents);
    for(const auto& part:asset.instances.Parts()){AddHash(hash,part.meshPath);AddHash(hash,part.materialPath);AddHash(hash,part.lod);}
    for(float height:asset.instances.LodHeights())AddFloat(hash,height);
    for(const auto& p:snapshot.placements){AddHash(hash,p.id);AddHash(hash,p.active?1:0);AddVector(hash,p.position);AddVector(hash,p.eulerRadians);AddVector(hash,p.scale);}
    return Hex(hash);
}

std::string OwnerSignature(scene::GameObject& owner, const scene::WorldPcgGeneratedComponent& metadata,
    const scene::InstancedMeshComponent& component, const asset::MeshInstancesAsset& asset)
{
    scene::Transform world;
    if (!ReadWorld(owner, world)) return {};
    const auto snapshot = component.Snapshot(asset.instances);
    if (!snapshot) return {};
    const auto reference = asset::EncodeGuidRef(component.m_instanceSetRef.path);
    return OwnerSignature(owner, metadata, component, asset, world, *snapshot, PcgAssetIdentity(reference));
}

bool InBounds(const scene::WorldCellBounds& bounds,const math::Vector3& point)
{ return point.x>=bounds.min.x&&point.x<bounds.max.x&&point.z>=bounds.min.z&&point.z<bounds.max.z; }

bool IntersectsXZ(const scene::WorldCellBounds& a, const scene::WorldCellBounds& b)
{
    return a.min.x < b.max.x && a.max.x > b.min.x && a.min.z < b.max.z && a.max.z > b.min.z;
}

bool OverlapsQueryXZ(const scene::WorldCellBounds& bounds, const scene::WorldCellBounds& closed)
{
    return closed.max.x >= bounds.min.x && closed.min.x < bounds.max.x
        && closed.max.z >= bounds.min.z && closed.min.z < bounds.max.z;
}

bool ExactIdentityRotation(const math::Quaternion& rotation)
{
    return rotation.x == 0.0f && rotation.y == 0.0f && rotation.z == 0.0f && rotation.w == 1.0f;
}

bool ExactIdentityPose(const scene::Transform& pose)
{
    const auto& p = pose.worldPosition;
    const auto& s = pose.worldScale;
    return p.x == 0.0f && p.y == 0.0f && p.z == 0.0f
        && s.x == 1.0f && s.y == 1.0f && s.z == 1.0f && ExactIdentityRotation(pose.worldRotation);
}

/// @note Compare actual immutable rows, including assets generated with a previous partition grid.
std::optional<scene::WorldCellBounds> PlacementBounds(const std::shared_ptr<const renderer::InstanceSetData>& snapshot)
{
    struct CachedBounds {
        std::weak_ptr<const renderer::InstanceSetData> snapshot;
        uint64_t revision = 0;
        std::optional<scene::WorldCellBounds> bounds;
    };
    static std::unordered_map<const renderer::InstanceSetData*, CachedBounds> cache;
    auto& cached = cache[snapshot.get()];
    if (cached.snapshot.lock() == snapshot && cached.revision == snapshot->revision) return cached.bounds;
    cached = {snapshot, snapshot->revision, {}};
    for (const auto& row : snapshot->placements) {
        if (!row.active) continue;
        const scene::WorldCellBounds point{row.position, row.position};
        if (cached.bounds) {
            auto& b = *cached.bounds;
            b.min.x = (std::min)(b.min.x, row.position.x); b.min.z = (std::min)(b.min.z, row.position.z);
            b.max.x = (std::max)(b.max.x, row.position.x); b.max.z = (std::max)(b.max.z, row.position.z);
        } else cached.bounds = point;
    }
    const auto result = cached.bounds;
    if (cache.size() > 4096) {
        std::erase_if(cache, [](const auto& item) { return item.second.snapshot.expired(); });
        if (cache.size() > 4096) cache.clear();
    }
    return result;
}

void IncludeXZ(scene::WorldCellBounds& bounds, const scene::WorldCellBounds& other)
{
    bounds.min.x = (std::min)(bounds.min.x, other.min.x);
    bounds.min.z = (std::min)(bounds.min.z, other.min.z);
    bounds.max.x = (std::max)(bounds.max.x, other.max.x);
    bounds.max.z = (std::max)(bounds.max.z, other.max.z);
}

class PcgPointIndex {
public:
    template<class BoundsFor>
    PcgPointIndex(std::size_t count, BoundsFor boundsFor)
    {
        m_entries.reserve(count);
        m_matches.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            if (const auto bounds = boundsFor(i)) m_entries.push_back({*bounds, i});
            else m_fallback.push_back(i);
        }
        if (!m_entries.empty()) Build(0, m_entries.size());
    }

    /// @note Overlaps and sources without conservative bounds retain their original sampling order.
    std::span<const std::size_t> Query(float x, float z)
    {
        if (m_nodes.empty()) return m_fallback;
        m_matches.assign(m_fallback.begin(), m_fallback.end());
        Collect(0, x, z);
        if (m_matches.size() > 1) std::sort(m_matches.begin(), m_matches.end());
        return m_matches;
    }

private:
    struct Entry { scene::WorldCellBounds bounds; std::size_t source; };
    struct Node { scene::WorldCellBounds bounds; std::size_t first, end, right; };

    static bool Contains(const scene::WorldCellBounds& bounds, float x, float z)
    {
        return x >= bounds.min.x && x <= bounds.max.x && z >= bounds.min.z && z <= bounds.max.z;
    }

    std::size_t Build(std::size_t first, std::size_t end)
    {
        auto bounds = m_entries[first].bounds;
        for (auto i = first + 1; i < end; ++i) IncludeXZ(bounds, m_entries[i].bounds);
        const auto node = m_nodes.size();
        m_nodes.push_back({bounds, first, end, 0});
        if (end - first <= 4) return node;

        const bool alongX = static_cast<double>(bounds.max.x) - bounds.min.x
            >= static_cast<double>(bounds.max.z) - bounds.min.z;
        const auto center = [alongX](const auto& entry) {
            return alongX ? static_cast<double>(entry.bounds.min.x) + entry.bounds.max.x
                : static_cast<double>(entry.bounds.min.z) + entry.bounds.max.z;
        };
        const auto middle = first + (end - first) / 2;
        std::nth_element(m_entries.begin() + first, m_entries.begin() + middle, m_entries.begin() + end,
            [&](const Entry& a, const Entry& b) { return center(a) < center(b); });
        Build(first, middle);
        m_nodes[node].right = Build(middle, end);
        return node;
    }

    void Collect(std::size_t index, float x, float z)
    {
        const auto& node = m_nodes[index];
        if (!Contains(node.bounds, x, z)) return;
        if (node.right) {
            Collect(index + 1, x, z);
            Collect(node.right, x, z);
            return;
        }
        if (node.end - node.first == 1) {
            m_matches.push_back(m_entries[node.first].source);
            return;
        }
        for (auto i = node.first; i < node.end; ++i)
            if (Contains(m_entries[i].bounds, x, z)) m_matches.push_back(m_entries[i].source);
    }

    std::vector<Entry> m_entries;
    std::vector<Node> m_nodes;
    std::vector<std::size_t> m_fallback;
    std::vector<std::size_t> m_matches;
};

std::optional<scene::WorldCellBounds> HeightfieldQueryBounds(const scene::TerrainComponent& terrain,
    const scene::Transform& world)
{
    if (terrain.columns < 2 || terrain.rows < 2 || !std::isfinite(terrain.cellSize)
        || terrain.cellSize <= 0.0f || world.worldScale.x == 0.0f || world.worldScale.z == 0.0f) return {};
    const auto axis = [](float origin, float scale, double length, float& min, float& max) {
        const double offset = length * scale;
        const double edge = origin + offset;
        /// @note Widen the inverse of the float subtraction and division, including local zero and exact tile seams.
        const double margin = 16.0 * std::numeric_limits<float>::epsilon()
            * (std::max)({1.0, std::abs(static_cast<double>(origin)), std::abs(offset), std::abs(edge)});
        const double lower = (std::min)(static_cast<double>(origin), edge) - margin;
        const double upper = (std::max)(static_cast<double>(origin), edge) + margin;
        constexpr double LIMIT = std::numeric_limits<float>::max();
        if (lower < -LIMIT || upper > LIMIT) return false;
        min = static_cast<float>(lower);
        max = static_cast<float>(upper);
        return true;
    };
    scene::WorldCellBounds bounds;
    if (!axis(world.worldPosition.x, world.worldScale.x,
            (terrain.columns - 1) * static_cast<double>(terrain.cellSize), bounds.min.x, bounds.max.x)
        || !axis(world.worldPosition.z, world.worldScale.z,
            (terrain.rows - 1) * static_cast<double>(terrain.cellSize), bounds.min.z, bounds.max.z)) return {};
    return bounds;
}

std::optional<scene::WorldCellBounds> SplineWaterQueryBounds(const scene::WaterComponent& water)
{
    if (scene::WaterSurfaceUsesHorizon(water) || !scene::UsesWaterSurfaceGeometry(water)) return {};
    const auto* geometry = scene::WaterSurfaceGeometry(water);
    if (!geometry) return {};
    /// @note Match the closed footprint rejection in WaterSurfaceQuery.cpp, including the displacement margin.
    const float scale = (std::max)({std::abs(geometry->worldBoundsMin.x), std::abs(geometry->worldBoundsMax.x),
        std::abs(geometry->worldBoundsMin.z), std::abs(geometry->worldBoundsMax.z), 1.0f});
    const float tolerance = (std::max)(1.0e-5f, scale * 8.0f * std::numeric_limits<float>::epsilon());
    const float margin = water.SurfaceHeightBound();
    scene::WorldCellBounds bounds{{geometry->worldBoundsMin.x - margin - tolerance, 0.0f,
        geometry->worldBoundsMin.z - margin - tolerance}, {geometry->worldBoundsMax.x + margin + tolerance, 0.0f,
        geometry->worldBoundsMax.z + margin + tolerance}};
    if (!Finite(bounds.min) || !Finite(bounds.max) || bounds.min.x > bounds.max.x || bounds.min.z > bounds.max.z)
        return {};
    return bounds;
}

float HorizonWetHeightLimit(const scene::WaterComponent& water, const scene::Transform& world)
{
    constexpr float NO_LIMIT = std::numeric_limits<float>::infinity();
    if (!scene::WaterSurfaceUsesHorizon(water) || water.surfaceFlowCount != 0
        || std::any_of(water.ripples.begin(), water.ripples.end(),
            [](const auto& ripple) { return ripple.amplitude > 0.0f; })
        || !scene::IsWaterSurfaceSupported(water, world)) return NO_LIMIT;
    /// @note The wave bound includes crossing crests. Round to the same world height and wet threshold as the full query.
    return static_cast<float>(static_cast<double>(world.worldPosition.y) + water.SurfaceHeightBound()) + 0.15f;
}

std::optional<scene::WorldCellBounds> TerrainRefreshBounds(const scene::WorldPcgProfile& profile,
    std::span<const scene::WorldCellBounds> edits)
{
    float margin = 0.0f;
    bool needsFullVolume = false;
    for (const auto& rule : profile.rules) {
        const float water = rule.maxWaterDistance < 1000000.0f ? rule.maxWaterDistance : rule.minWaterDistance + rule.waterFade;
        const float path = rule.maxPathDistance < 1000000.0f ? rule.maxPathDistance : rule.minPathDistance + rule.pathFade;
        const float habitat = (std::max)({water, path, rule.maxCliffDistance, rule.useWaterLevel ? 128.0f : 0.0f,
            rule.UsesNearRelief() ? 16.0f : 0.0f, rule.UsesFarRelief() ? 64.0f : 0.0f});
        margin = (std::max)(margin, habitat + rule.embedDepth + rule.embedJitter);
        needsFullVolume |= rule.minSpacing > 0.0f || !rule.neighborRule.empty()
            || !rule.competitionGroup.empty() || !rule.canopyGroup.empty();
    }
    std::optional<scene::WorldCellBounds> region;
    for (auto edit : edits) {
        edit.min.x -= margin; edit.min.z -= margin;
        edit.max.x += margin; edit.max.z += margin;
        if (!IntersectsXZ(edit, profile.bounds)) continue;
        /// @note Greedy spacing and dependent rules can change placements beyond the edited neighborhood.
        if (needsFullVolume) return profile.bounds;
        if (region) IncludeXZ(*region, edit);
        else region = edit;
    }
    if (!region) return {};
    if (profile.maxPlacementsPerCell) {
        /// @note Rank a whole budget cell against its original candidates before merging any retained rows.
        const scene::WorldCellGrid grid{profile.grid.origin, scene::VEGETATION_BUDGET_CELL_SIZE};
        scene::WorldCellId first, last;
        if (!scene::TryLocateWorldCell(grid, region->min, first) || !scene::TryLocateWorldCell(grid, region->max, last))
            return profile.bounds;
        region->min = scene::WorldCellFootprint(grid, first).min;
        region->max = scene::WorldCellFootprint(grid, last).max;
    }
    region->min.x = (std::max)(region->min.x, profile.bounds.min.x);
    region->min.z = (std::max)(region->min.z, profile.bounds.min.z);
    region->max.x = (std::min)(region->max.x, profile.bounds.max.x);
    region->max.z = (std::min)(region->max.z, profile.bounds.max.z);
    return region;
}

using GroupKey=std::pair<scene::WorldCellId,std::string>;
struct Owned {
    scene::GameObject* owner=nullptr;
    /// @note The synchronous plan borrows scene components only up to transaction publication.
    const scene::WorldPcgGeneratedComponent* metadata = nullptr;
    const scene::InstancedMeshComponent* component = nullptr;
    const asset::MeshInstancesAsset* asset = nullptr;
    std::shared_ptr<const renderer::InstanceSetData> snapshot;
    scene::Transform world;
    std::string signature;
    std::string path;
    bool outside = false;
    bool validPose = false;
    bool matchesSignature = false;

    void ReadSignature()
    {
        if (!validPose || !snapshot) return;
        const auto& placementRef = component->m_instanceSetRef.path;
        const auto encoded = fbzz::asset::EncodeGuidRef(placementRef);
        const auto identity = PcgAssetIdentity(encoded);
        const auto hash = [&](std::string_view reference) {
            return OwnerSignature(*owner, *metadata, *component, *asset, world, *snapshot, reference);
        };
        signature = hash(identity);
        matchesSignature = signature == metadata->signature;
        if (matchesSignature || metadata->signature.empty()) return;
        /// @note Older output signatures included the GUID path hint that Scene loading decodes.
        if (identity != encoded) matchesSignature = hash(encoded) == metadata->signature;
        /// @note A legacy path-only hash has no record of the former asset GUID.
        if (!matchesSignature && fbzz::asset::AssetDatabase::IsGuidRef(placementRef)) {
            const auto runtimeRef = fbzz::asset::DecodeGuidRef(placementRef);
            const auto encodedRuntime = fbzz::asset::EncodeGuidRef(runtimeRef);
            if (PcgAssetIdentity(encodedRuntime) == PcgAssetIdentity(placementRef))
                matchesSignature = hash(runtimeRef) == metadata->signature;
        }
    }

    [[nodiscard]] bool IsProtected() const
    {
        return metadata->manualOverride || !matchesSignature;
    }
};
struct PcgPlan {
    scene::GameObject* volume=nullptr;
    scene::WorldPcgProfile profile;
    scene::WorldPcgResult generated;
    std::string profilePath,profileText,expected;
    std::map<GroupKey,Owned> owned;
    OpData summary=OpData::MakeObject();
    bool terrainRefresh = false;
};

OpResult ValidateRetainedPlacements(const PcgPlan& plan)
{
    const auto limit = plan.profile.maxPlacementsPerCell;
    std::set<std::string_view> competitiveRules;
    std::set<std::string_view> canopyGroups, canopyRules;
    for (const auto& rule : plan.profile.rules)
        if (!rule.canopyGroup.empty()) canopyGroups.insert(rule.canopyGroup);
    for (const auto& rule : plan.profile.rules) {
        if (!rule.competitionGroup.empty()) competitiveRules.insert(rule.key);
        if (!rule.canopyGroup.empty() || canopyGroups.contains(rule.canopySourceGroup)) canopyRules.insert(rule.key);
    }
    const bool hasNewPlacements = plan.generated.stats.accepted != 0;
    if (plan.owned.empty() || (canopyRules.empty() && ((!limit && competitiveRules.empty()) || !hasNewPlacements)))
        return OpResult::Ok();
    const scene::WorldCellGrid budgetGrid{plan.profile.grid.origin, scene::VEGETATION_BUDGET_CELL_SIZE};
    std::map<scene::WorldCellId, uint32_t> occupied;
    if (limit) for (const auto& group : plan.generated.groups) {
        const auto origin = scene::WorldCellOrigin(plan.profile.grid, group.cell);
        for (const auto& placement : group.placements) {
            scene::WorldCellId cell;
            if (!scene::TryLocateWorldCell(budgetGrid, origin + placement.position, cell))
                return OpResult::Err("PCG_OUTPUT_INVALID", "A generated placement has no valid budget cell");
            ++occupied[cell];
        }
    }
    for (const auto& [key, owned] : plan.owned) {
        if (plan.terrainRefresh && (owned.outside || owned.IsProtected())) continue;
        const bool competes = hasNewPlacements && competitiveRules.contains(key.second);
        const bool usesCanopy = canopyRules.contains(key.second);
        if (!limit && !competes && !usesCanopy) continue;
        const auto& snapshot = owned.snapshot;
        if (!snapshot || !owned.validPose)
            return OpResult::Err("PCG_CONFLICT", "A retained generated owner cannot be sampled");
        const auto matrix = owned.world.GetWorldMatrix();
        for (const auto& placement : snapshot->placements) {
            if (!placement.active) continue;
            const auto p = matrix * math::Vector4{placement.position, 1};
            const math::Vector3 position{p.x, p.y, p.z};
            if (InBounds(plan.profile.bounds, position)) continue;
            if (competes)
                return OpResult::Err("PCG_COMPETITION_PARTIAL", "Regenerate the full region of the competing rules before replacing their placements");
            if (usesCanopy)
                return OpResult::Err("PCG_CANOPY_PARTIAL", "Regenerate the full region of the canopy group before replacing its placements");
            scene::WorldCellId cell;
            if (!scene::TryLocateWorldCell(budgetGrid, position, cell)) continue;
            const auto found = occupied.find(cell);
            if (found != occupied.end() && ++found->second > limit)
                return OpResult::Err("PCG_PARTIAL_PLACEMENT_BUDGET",
                    "Retained rows exceed the 128 m placement cap in cell " + scene::WorldCellName(cell)
                    + "; regenerate the entire cell");
        }
    }
    return OpResult::Ok();
}

void DescribePcgBounds(OpData& data, const scene::WorldPcgProfile& profile)
{
    data.Set("boundsMin", profile.bounds.min);
    data.Set("boundsMax", profile.bounds.max);
    auto exclusions = OpData::MakeArray();
    for (const auto& box : profile.exclusions) {
        auto item = OpData::MakeObject();
        item.Set("min", box.min); item.Set("max", box.max);
        exclusions.Push(std::move(item));
    }
    data.Set("exclusions", std::move(exclusions));
}

OpResult LivePreviewBudgetExceeded()
{
    return OpResult::Err("PCG_LIVE_BUDGET", "Narrow the update region or use Preview for the full calculation.");
}

bool FitsLiveCandidateBudget(const scene::WorldPcgProfile& profile)
{
    constexpr double MAX_LIVE_CANDIDATES = 65536.0;
    double total = 0.0;
    for (const auto& rule : profile.rules) {
        const double margin = static_cast<double>(rule.embedDepth) + rule.embedJitter;
        const auto count = [&](float low, float high) {
            /// @note The extra lattice cell covers a region offset from a grid boundary.
            return std::ceil((static_cast<double>(high) - low + 2.0 * margin) / rule.spacing) + 1.0;
        };
        total += count(profile.bounds.min.x, profile.bounds.max.x) * count(profile.bounds.min.z, profile.bounds.max.z);
        if (total > MAX_LIVE_CANDIDATES) return false;
    }
    return true;
}

OpResult PreparePcg(OpContext& context,const OpArgs& args,PcgPlan& plan,bool preview = false,
    std::span<const scene::WorldCellBounds> terrainEdits = {})
{
    FBZZ_PROFILE_SCOPE("WorldPcg::Prepare");
    const int maxPoints = args.GetInt("maxPoints", 256);
    const int maxRejected = args.GetInt("maxRejectedPoints", 0);
    if (maxPoints < 0 || maxPoints > 1024 || maxRejected < 0 || maxRejected > static_cast<int>(scene::MAX_WORLD_PCG_DEBUG_SAMPLES))
        return OpResult::Err("BAD_ARG", "Preview point limits must be 0..1024");
    auto& ctx=context.ctx;if(!Editable(ctx))return OpResult::Err("NOT_AVAILABLE","PCG authoring requires the editable scene");
    auto& scene=*ctx.activeScene;plan.volume=scene.FindByGuid(args.GetString("volume"));
    const auto* savedVolume=plan.volume?plan.volume->GetComponent<scene::WorldPcgVolumeComponent>():nullptr;
    if(!savedVolume)return OpResult::Err("NO_VOLUME","An enabled PCG volume is required");
    const bool live = args.GetBool("live");
    const bool draft = args.Has("draftProfilePath") || args.Has("draftProfileText") || args.Has("draftSeed")
        || args.Has("draftBoundsMin") || args.Has("draftBoundsMax") || args.GetBool("draftUseProfileBounds")
        || args.Has("draftProtectManual") || args.Has("draftEnabled");
    if (!preview && (draft || live)) return OpResult::Err("BAD_ARG", "Draft and live parameters belong to pcg.preview");
    auto settings = *savedVolume;
    if (draft) {
        if (args.Has("draftBoundsMin") != args.Has("draftBoundsMax")
            || (args.Has("draftBoundsMin") && args.GetBool("draftUseProfileBounds")))
            return OpResult::Err("BAD_ARG", "Provide paired draft bounds or choose profile bounds");
        if (args.Has("draftProfilePath")) settings.profilePath = args.GetString("draftProfilePath");
        if (args.GetBool("draftUseProfileBounds")) settings.overrideBounds = false;
        if (args.Has("draftBoundsMin")) {
            settings.overrideBounds = true;
            settings.boundsMin = args.GetVec3("draftBoundsMin"); settings.boundsMax = args.GetVec3("draftBoundsMax");
        }
        if (args.Has("draftSeed")) {
            const int seed = args.GetInt("draftSeed", -1);
            if (seed < -1) return OpResult::Err("BAD_ARG", "Draft seed must be -1 or nonnegative");
            settings.overrideSeed = seed >= 0; settings.seed = seed >= 0 ? seed : 1;
        }
        settings.protectManual = args.GetBool("draftProtectManual", settings.protectManual);
        settings.enabled = args.GetBool("draftEnabled", settings.enabled);
    }
    const auto* volume = &settings;
    if (!volume->enabled) return OpResult::Err("NO_VOLUME", "An enabled PCG volume is required");
    const bool draftText = args.Has("draftProfileText");
    std::string diagnostic;
    {
        FBZZ_PROFILE_SCOPE("WorldPcg::PrepareProfile");
        plan.profilePath=ProfilePath(ctx,volume->profilePath,draftText);
        if (draftText) plan.profileText = args.GetString("draftProfileText");
        if(plan.profilePath.empty()||(!draftText&&!world_pcg_profile_draft::ReadProfileBytes(plan.profilePath,plan.profileText))
            ||!scene::DeserializeWorldPcgProfile(plan.profileText,plan.profile,diagnostic))return OpResult::Err("INVALID_PROFILE",diagnostic.empty()?"PCG profile cannot be read":diagnostic);
    }
    if(volume->overrideBounds)plan.profile.bounds={volume->boundsMin,volume->boundsMax};
    if(volume->overrideSeed){if(volume->seed<0)return OpResult::Err("BAD_ARG","PCG seed must be nonnegative");plan.profile.seed=static_cast<uint64_t>(volume->seed);}
    if(args.Has("boundsMin")!=args.Has("boundsMax"))return OpResult::Err("BAD_ARG","Both boundsMin and boundsMax are required");
    if(args.Has("boundsMin"))plan.profile.bounds={args.GetVec3("boundsMin"),args.GetVec3("boundsMax")};
    scene::GameObject* partitionOwner=nullptr;
    if(!args.GetString("partition").empty()){
        partitionOwner=scene.FindByGuid(args.GetString("partition"));
        if(!partitionOwner||!partitionOwner->GetComponent<scene::WorldPartitionComponent>())return OpResult::Err("NO_PARTITION","Configured partition owner is missing");
    }else for(auto id:scene.GetEntities<scene::WorldPartitionComponent>()){
        auto* owner=scene.GetGameObject(id);if(!owner||owner->IsRuntimeGenerated())continue;
        if(partitionOwner)return OpResult::Err("AMBIGUOUS_PARTITION","Choose a partition explicitly when multiple configured controllers exist");partitionOwner=owner;
    }
    if(partitionOwner){const auto* partition=partitionOwner->GetComponent<scene::WorldPartitionComponent>();plan.profile.grid.origin=partition->gridOrigin;plan.profile.grid.cellSize=partition->cellSize;}
    if(!scene::ValidateWorldPcgProfile(plan.profile,diagnostic))return OpResult::Err("BAD_PROFILE",diagnostic);
    plan.terrainRefresh = !terrainEdits.empty();
    if (plan.terrainRefresh) {
        const auto region = TerrainRefreshBounds(plan.profile, terrainEdits);
        if (!region) {
            auto result = OpResult::Ok();
            result.noChange = true;
            return result;
        }
        plan.profile.bounds = *region;
    }
    if (live && !FitsLiveCandidateBudget(plan.profile)) return LivePreviewBudgetExceeded();
    for(auto& rule:plan.profile.rules)if(rule.parts.empty()){
        const auto* definition=asset::ResolveMeshInstanceDefinition(rule.prototypeRef);
        if(!definition)return OpResult::Err("NO_TEMPLATE","Cannot resolve PCG prototype: "+rule.prototypeRef);
        rule.parts.assign(definition->Parts().begin(),definition->Parts().end());rule.lodHeights.assign(definition->LodHeights().begin(),definition->LodHeights().end());}
    struct Ground {
        const scene::TerrainComponent* data;
        scene::Transform world;
        std::string id;
        std::map<std::string_view, std::vector<int>> materialLayers;
        math::Matrix4 worldToLocal = math::Matrix4::Identity();
        bool uprightHeightfield = false;
    };
    struct Water { const scene::WaterComponent* data; scene::Transform world; float wetHeightLimit; };
    struct Biome { const scene::WorldBiomeVolumeComponent* data;std::string id; };
    std::vector<Ground> grounds;std::vector<Water> waters;std::vector<Biome> biomes;
    uint64_t guard=1469598103934665603ull;
    if (!plan.terrainRefresh) {
        AddHash(guard,scene.GetRenderSceneGeneration());AddHash(guard,context.undo.GetRevision());AddHash(guard,AssetDirtyRegistry::DiscardEpoch());
        AddHash(guard,HashText(plan.profileText));AddHash(guard,plan.volume->instanceId);AddHash(guard,volume->profilePath);AddHash(guard,volume->protectManual?1:0);
        AddHash(guard,partitionOwner?partitionOwner->instanceId:std::string{});
        AddHash(guard,volume->overrideBounds?1:0);AddVector(guard,volume->boundsMin);AddVector(guard,volume->boundsMax);AddHash(guard,volume->overrideSeed?1:0);AddHash(guard,static_cast<uint64_t>(volume->seed));
    }
    for(auto id:scene.GetEntities<scene::TerrainComponent>()){
        auto* owner=scene.GetGameObject(id);const auto* data=scene.GetComponent<scene::TerrainComponent>(id);scene::Transform world;
        if(!owner||!owner->activeInHierarchy()||!data||!data->enabled)continue;if(!ReadWorld(*owner,world))return OpResult::Err("INVALID_TRANSFORM","Terrain transform is invalid");
        grounds.push_back({data,world,owner->instanceId});
        grounds.back().uprightHeightfield = ExactIdentityRotation(world.worldRotation)
            && data->surfaceIndices.empty();
        if (!plan.terrainRefresh) {
            AddHash(guard,owner->instanceId);AddHash(guard,data->surfaceRevision);AddHash(guard,data->vegetationRevision);
            AddVector(guard,world.worldPosition);AddVector(guard,world.worldScale);AddFloat(guard,world.worldRotation.x);AddFloat(guard,world.worldRotation.y);AddFloat(guard,world.worldRotation.z);AddFloat(guard,world.worldRotation.w);
        }
    }
    if(grounds.empty() && !plan.terrainRefresh)return OpResult::Err("NO_TERRAIN","PCG requires authored terrain samples");
    if (live && grounds.size() > 8) return LivePreviewBudgetExceeded();
    std::unordered_map<std::string_view, std::string> materialIdentities;
    const auto materialIdentity = [&](const std::string& reference) -> const std::string& {
        auto [entry, inserted] = materialIdentities.try_emplace(reference);
        if (inserted) {
            auto guid = asset::AssetDatabase::GuidFromRef(reference);
            if (guid.empty()) guid = asset::AssetDatabase::TryGetGuidFromPath(asset::AssetManager::ResolveAssetPath(reference));
            entry->second = guid.empty() ? NormalizeAssetPath(reference) : "guid:" + guid;
        }
        return entry->second;
    };
    std::map<std::string, std::set<std::string_view>> requestedMaterials;
    {
        FBZZ_PROFILE_SCOPE("WorldPcg::PrepareMaterials");
        if (!plan.profile.pathMaterial.empty())
            requestedMaterials[materialIdentity(plan.profile.pathMaterial)].insert(plan.profile.pathMaterial);
        for (const auto& rule : plan.profile.rules) {
            for (const auto* material : {&rule.includeMaterial, &rule.excludeMaterial})
                if (!material->empty()) requestedMaterials[materialIdentity(*material)].insert(*material);
        }
        if (!requestedMaterials.empty()) {
            for (auto& ground : grounds) {
                if (!ground.data->HasValidSplat()) continue;
                for (int layer = 0; layer < ground.data->LayerCount(); ++layer) {
                    const auto& material = ground.data->layerMaterials[layer];
                    if (material.empty()) continue;
                    const auto matches = requestedMaterials.find(materialIdentity(material));
                    if (matches == requestedMaterials.end()) continue;
                    for (const auto ref : matches->second) ground.materialLayers[ref].push_back(layer);
                }
                if (!ground.materialLayers.empty()) ground.worldToLocal = math::Matrix4::Inverse(ground.world.GetWorldMatrix());
            }
        }
    }
    for(auto id:scene.GetEntities<scene::WaterComponent>()){
        auto* owner=scene.GetGameObject(id);const auto* data=scene.GetComponent<scene::WaterComponent>(id);scene::Transform world;
        if(owner&&owner->activeInHierarchy()&&data&&data->enabled&&ReadWorld(*owner,world))
            waters.push_back({data, world, HorizonWetHeightLimit(*data, world)});}
    for(auto id:scene.GetEntities<scene::WorldBiomeVolumeComponent>()){
        const auto* owner=scene.GetGameObject(id);const auto* data=scene.GetComponent<scene::WorldBiomeVolumeComponent>(id);
        if(owner&&owner->activeInHierarchy()&&data&&data->enabled){
            if(!Finite(data->boundsMin)||!Finite(data->boundsMax)||data->boundsMin.x>=data->boundsMax.x||data->boundsMin.y>data->boundsMax.y||data->boundsMin.z>=data->boundsMax.z
                ||!std::isfinite(data->density)||data->density<0||data->density>1)return OpResult::Err("BAD_BIOME","Biome volume bounds or density are invalid");
            biomes.push_back({data,owner->instanceId});}}
    std::sort(biomes.begin(),biomes.end(),[](const auto& a,const auto& b){return a.data->priority==b.data->priority?a.id<b.id:a.data->priority>b.data->priority;});
    if (live && (waters.size() > 8 || biomes.size() > 32)) return LivePreviewBudgetExceeded();
    std::size_t remainingRows = 65536;
    const auto spendRows = [&](std::size_t count) {
        if (!live) return true;
        if (count > remainingRows) return false;
        remainingRows -= count;
        return true;
    };
    auto protectionBounds = plan.profile.bounds;
    float protectionMargin = 0.0f;
    for (const auto& rule : plan.profile.rules)
        if (rule.protectedDistance > 0.0f)
            protectionMargin = (std::max)(protectionMargin, rule.protectedDistance + rule.embedDepth + rule.embedJitter);
    protectionBounds.min.x -= protectionMargin; protectionBounds.min.z -= protectionMargin;
    protectionBounds.max.x += protectionMargin; protectionBounds.max.z += protectionMargin;
    const bool sampleProtected = volume->protectManual && (!plan.terrainRefresh || protectionMargin > 0.0f);
    const bool validateOutsideDependencies = std::any_of(plan.profile.rules.begin(), plan.profile.rules.end(),
        [](const auto& rule) { return !rule.competitionGroup.empty() || !rule.canopyGroup.empty(); });
    auto ownedBounds = plan.profile.bounds;
    if (plan.profile.maxPlacementsPerCell) {
        const float padding = scene::VEGETATION_BUDGET_CELL_SIZE;
        ownedBounds.min.x -= padding; ownedBounds.min.z -= padding;
        ownedBounds.max.x += padding; ownedBounds.max.z += padding;
    }
    if (sampleProtected) IncludeXZ(ownedBounds, protectionBounds);
    std::vector<math::Vector3> protectedPositions;
    const auto protect = [&](const math::Vector3& position) {
        if (!plan.terrainRefresh || InBounds(protectionBounds, position)) protectedPositions.push_back(position);
    };
    const auto collectOwner = [&](scene::EntityID id) {
        auto* owner=scene.GetGameObject(id);const auto* component=scene.GetComponent<scene::InstancedMeshComponent>(id);
        if(!owner||owner->IsRuntimeGenerated()||!component)return OpResult::Ok();
        const auto* metadata=owner->GetComponent<scene::WorldPcgGeneratedComponent>();
        if(metadata&&metadata->volumeId==plan.volume->instanceId){
            Owned owned;
            auto* source=PlacementAsset(component->m_instanceSetRef.path, &owned.path);if(!source)return OpResult::Err("MISSING_OUTPUT","Generated placement asset is missing; detach this owner before replacing it");
            const auto base = source->instances.Snapshot();
            if (!base) return OpResult::Err("MISSING_OUTPUT", "Generated placement data is invalid");
            if (!spendRows(base->placements.size())) return LivePreviewBudgetExceeded();
            owned.owner=owner;owned.metadata=metadata;owned.component=component;owned.asset=source;
            owned.snapshot = component->Snapshot(source->instances);
            owned.validPose = ReadWorld(*owner, owned.world);
            const GroupKey key{{metadata->cellX,metadata->cellZ},metadata->ruleKey};
            if (plan.terrainRefresh && !validateOutsideDependencies && owned.snapshot && owned.validPose && ExactIdentityPose(owned.world)
                && !IntersectsXZ(plan.profile.bounds, scene::WorldCellFootprint(plan.profile.grid, key.first))) {
                const auto rows = PlacementBounds(owned.snapshot);
                owned.outside = rows && !OverlapsQueryXZ(ownedBounds, *rows);
            }
            if (!owned.outside && owned.validPose) owned.ReadSignature();
            if (!plan.terrainRefresh) {
                AddHash(guard,owner->instanceId);AddHash(guard,owned.signature);AddHash(guard,metadata->signature);AddHash(guard,metadata->manualOverride?1:0);
                const auto& budget = component->m_vegetationBudget;
                AddHash(guard, budget.enabled ? 1 : 0);
                AddHash(guard, static_cast<uint64_t>(budget.drawDetailPerCell));
                AddHash(guard, static_cast<uint64_t>(budget.nearDetailPerCell));
                AddHash(guard, static_cast<uint64_t>(budget.shadowDetailPerCell));
            }
            const bool protectOwner = !owned.outside && (metadata->manualOverride || (plan.terrainRefresh && owned.IsProtected()));
            if(!plan.owned.emplace(key,std::move(owned)).second)return OpResult::Err("DUPLICATE_OUTPUT","PCG ownership keys are duplicated");
            if(!protectOwner)return OpResult::Ok();
        }
        if(!sampleProtected||!owner->activeInHierarchy()||!component->m_enabled)return OpResult::Ok();
        const auto snapshot=component->Snapshot();scene::Transform world;if(!snapshot||!ReadWorld(*owner,world))return OpResult::Err("INVALID_PLACEMENT","Manual placement cannot be sampled");
        if (!spendRows(snapshot->placements.size())) return LivePreviewBudgetExceeded();
        const auto matrix=world.GetWorldMatrix();for(const auto& p:snapshot->placements)if(p.active){const auto v=matrix*math::Vector4{p.position,1};protect({v.x,v.y,v.z});}
        return OpResult::Ok();
    };
    {
        FBZZ_PROFILE_SCOPE("WorldPcg::CollectOwners");
        const auto entities = plan.terrainRefresh && !sampleProtected
            ? scene.GetEntities<scene::WorldPcgGeneratedComponent>() : scene.GetEntities<scene::InstancedMeshComponent>();
        for (const auto id : entities) {
            auto result = collectOwner(id);
            if (!result.ok) return result;
        }
    }
    if(sampleProtected)for(auto id:scene.GetEntities<scene::MeshRenderer>()){
        auto* owner=scene.GetGameObject(id);if(!owner||owner->IsRuntimeGenerated()||!owner->activeInHierarchy()||owner->GetComponent<scene::WorldPcgGeneratedComponent>())continue;
        scene::Transform world;if(ReadWorld(*owner,world)) {
            if (!spendRows(1)) return LivePreviewBudgetExceeded();
            protect(world.worldPosition);
        }}
    if (!plan.terrainRefresh) for(const auto& point:protectedPositions)AddVector(guard,point);
    float waterMargin = 0.0f, pathMargin = 0.0f, embedMargin = 0.0f;
    bool useWaterLevel = false, usePathBand = false;
    for (const auto& rule : plan.profile.rules) {
        const float distance = rule.maxWaterDistance < 1000000.0f ? rule.maxWaterDistance
            : rule.minWaterDistance + rule.waterFade;
        waterMargin = (std::max)(waterMargin, distance);
        embedMargin = (std::max)(embedMargin, rule.embedDepth + rule.embedJitter);
        useWaterLevel |= rule.useWaterLevel;
        if (rule.UsesPathBand()) {
            usePathBand = true;
            pathMargin = (std::max)(pathMargin, rule.maxPathDistance < 1000000.0f
                ? rule.maxPathDistance : rule.minPathDistance + rule.pathFade);
        }
    }
    if(waterMargin>10000)return OpResult::Err("WATER_DISTANCE_BUDGET","Authoring water clearance is limited to 10000 world metres");
    constexpr float WATER_LEVEL_SEARCH_RADIUS = 128.0f;
    const float waterSearch = useWaterLevel ? (std::max)(waterMargin, WATER_LEVEL_SEARCH_RADIUS) : waterMargin;
    std::vector<TerrainPaintClearance> clearances;
    std::size_t remainingClearanceWork = 131072;
    bool hasPathLayer = false;
    if ((waterSearch > 0 && !waters.empty()) || usePathBand) {
        std::vector<std::pair<const scene::WaterComponent*,const scene::Transform*>> views;for(const auto& water:waters)views.push_back({water.data,&water.world});
        const auto& b = plan.profile.bounds;
        const float embed = plan.terrainRefresh ? embedMargin : 0.0f;
        const std::vector<math::Vector3> corners{{b.min.x - embed, 0, b.min.z - embed}, {b.max.x + embed, 0, b.min.z - embed},
            {b.max.x + embed, 0, b.max.z + embed}, {b.min.x - embed, 0, b.max.z + embed}};
        const TerrainScatterBrush region{corners,0.25f,false,true,0};
        for (const auto& ground : grounds) {
            const auto found = ground.materialLayers.find(plan.profile.pathMaterial);
            const auto layers = found == ground.materialLayers.end() ? std::span<const int>{} : std::span<const int>{found->second};
            hasPathLayer |= usePathBand && !layers.empty();
            auto clearance = BuildTerrainPaintClearance(*ground.data, ground.world, region, layers, views,
                waters.empty() ? 0.0f : waterSearch, !usePathBand || layers.empty() ? 0.0f : (std::max)(0.25f, pathMargin),
                true, live ? &remainingClearanceWork : nullptr, useWaterLevel || clearances.empty());
            if (clearance.budgetExceeded) return LivePreviewBudgetExceeded();
            clearances.push_back(std::move(clearance));
        }
    }
    std::vector<scene::WorldPcgMaterialWeight> materialWeights;
    PcgPointIndex groundIndex(grounds.size(), [&](std::size_t i) -> std::optional<scene::WorldCellBounds> {
        const auto& ground = grounds[i];
        return ground.uprightHeightfield ? HeightfieldQueryBounds(*ground.data, ground.world) : std::nullopt;
    });
    PcgPointIndex waterIndex(waters.size(), [&](std::size_t i) { return SplineWaterQueryBounds(*waters[i].data); });
    PcgPointIndex biomeIndex(biomes.size(), [&](std::size_t i) -> std::optional<scene::WorldCellBounds> {
        return scene::WorldCellBounds{biomes[i].data->boundsMin, biomes[i].data->boundsMax};
    });
    const auto sampleGround = [&](float x, float z, scene::TerrainSurfaceSample& highest) {
        const Ground* highestGround = nullptr;
        for (const auto i : groundIndex.Query(x, z)) {
            const auto& ground = grounds[i];
            if (ground.uprightHeightfield) {
                const auto& world = ground.world;
                const auto& terrain = *ground.data;
                /// @note Use the same local division and double-precision edge bounds as the heightfield query.
                const float lx = (x - world.worldPosition.x) / world.worldScale.x;
                const float lz = (z - world.worldPosition.z) / world.worldScale.z;
                if (lx < 0.0f || lz < 0.0f || lx > (terrain.columns - 1) * static_cast<double>(terrain.cellSize)
                    || lz > (terrain.rows - 1) * static_cast<double>(terrain.cellSize)) continue;
            }
            scene::TerrainSurfaceSample one;
            if (!scene::TrySampleTerrainWorld(*ground.data, ground.world, x, z, one)) continue;
            if (!highestGround || one.position.y > highest.position.y
                || (!requestedMaterials.empty() && one.position.y == highest.position.y && ground.id < highestGround->id)) {
                highest = one;
                highestGround = &ground;
            }
        }
        return highestGround;
    };
    struct WaterReading {
        scene::WaterSurfaceHeightSample sample;
        bool attempted = false;
        bool valid = false;
    };
    std::vector<WaterReading> waterReadings;
    waterReadings.resize(waters.size());
    std::span<const std::size_t> waterCandidates;
    std::optional<math::Vector3> waterPoint;
    /// @note Base and habitat queries share an ordered prefix for the same exact world point.
    const auto prepareWaterPoint = [&](const math::Vector3& position) {
        if (waterPoint && waterPoint->x == position.x && waterPoint->y == position.y && waterPoint->z == position.z)
            return;
        waterCandidates = waterIndex.Query(position.x, position.z);
        for (std::size_t i = 0; i < waterCandidates.size(); ++i) waterReadings[i].attempted = false;
        waterPoint = position;
    };
    const auto readWater = [&](std::size_t i) -> const WaterReading& {
        auto& reading = waterReadings[i];
        if (!reading.attempted) {
            const auto& water = waters[waterCandidates[i]];
            reading.valid = scene::TrySampleInsideWaterSurfaceHeightWorld(*water.data, water.world, *waterPoint, 0, reading.sample);
            reading.attempted = true;
        }
        return reading;
    };
    constexpr float DIAGONAL = 0.70710678f;
    constexpr float CLIFF_NORMAL_Y = 0.64278761f;
    const std::array<math::Vector2, 8> directions{{{1,0},{DIAGONAL,DIAGONAL},{0,1},{-DIAGONAL,DIAGONAL},
        {-1,0},{-DIAGONAL,-DIAGONAL},{0,-1},{DIAGONAL,-DIAGONAL}}};
    std::size_t remainingProbes = live ? 262144 : 4'000'000;
    bool habitatBudgetExceeded = false;
    const auto probe = [&](float x, float z, scene::TerrainSurfaceSample& surface) {
        if (remainingProbes < grounds.size()) {
            habitatBudgetExceeded = true;
            return false;
        }
        remainingProbes -= grounds.size();
        return sampleGround(x, z, surface) != nullptr;
    };
    /// @see https://pro.arcgis.com/en/pro-app/latest/tool-reference/spatial-analyst/how-surface-parameters-works.htm Esri Surface Parameters, Topographic Position Index.
    const auto relief = [&](const math::Vector3& position, float radius, float& value) {
        double sum = 0.0;
        int count = 0;
        for (const auto& direction : directions) {
            scene::TerrainSurfaceSample neighbor;
            if (!probe(position.x + direction.x * radius, position.z + direction.y * radius, neighbor)) continue;
            sum += neighbor.position.y;
            ++count;
        }
        if (count >= 4) value = static_cast<float>(position.y - sum / count);
    };
    const auto sampler=[&](float x,float z,scene::WorldPcgSurfaceSample& out){
        if (habitatBudgetExceeded) return false;
        scene::TerrainSurfaceSample highest;
        const auto* highestGround = sampleGround(x, z, highest);
        if (!highestGround) return false;
        out.position = highest.position;
        out.normal = highest.normal;
        if (!highestGround->materialLayers.empty()) {
            const auto& terrain = *highestGround->data;
            const auto local = highestGround->worldToLocal * math::Vector4{highest.position, 1.0f};
            const float width = (terrain.columns - 1) * terrain.cellSize;
            const float depth = (terrain.rows - 1) * terrain.cellSize;
            materialWeights.clear();
            if (local.x >= -0.001f && local.z >= -0.001f && local.x <= width + 0.001f && local.z <= depth + 0.001f) {
                for (const auto& [material, layers] : highestGround->materialLayers) {
                    float weight = 0.0f;
                    for (const int layer : layers) weight += SampleTerrainLayerWeight(terrain, local.x, local.z, layer);
                    materialWeights.push_back({material, (std::min)(weight, 1.0f)});
                }
            }
            out.materialWeights = materialWeights;
        }
        for (const auto i : biomeIndex.Query(x, z)) {
            const auto& biome = *biomes[i].data;
            if (x < biome.boundsMax.x && z < biome.boundsMax.z
                && highest.position.y >= biome.boundsMin.y && highest.position.y <= biome.boundsMax.y) {
                out.biome = biome.biome;
                out.biomeDensity = biome.density;
                break;
            }
        }
        prepareWaterPoint(highest.position);
        for (std::size_t i = 0; i < waterCandidates.size(); ++i) {
            if (highest.position.y > waters[waterCandidates[i]].wetHeightLimit) continue;
            const auto& reading = readWater(i);
            if (reading.valid && highest.position.y <= reading.sample.position.y + 0.15f) {
                out.water = true;
                out.waterDistance = 0.0f;
                break;
            }
        }
        if (!out.water && waterMargin > 0)
            for (const auto& clearance : clearances)
                out.waterDistance = (std::min)(out.waterDistance, clearance.WaterDistance(highest.position, waterMargin));
        return true;
    };
    const auto sampleHabitat = [&](const scene::WorldPcgRule& rule, scene::WorldPcgSurfaceSample& out) {
        if (hasPathLayer && rule.UsesPathBand()) {
            out.pathDistance = 1000000.0f;
            for (const auto& [reference, weight] : out.materialWeights)
                if (reference == plan.profile.pathMaterial && weight >= 0.35f) out.onPath = true;
            for (const auto& clearance : clearances)
                out.pathDistance = (std::min)(out.pathDistance, clearance.PathDistance(out.position, (std::max)(0.25f, pathMargin)));
        }
        if (rule.UsesNearRelief()) relief(out.position, 16.0f, out.nearRelief);
        if (rule.UsesFarRelief()) relief(out.position, 64.0f, out.farRelief);
        if (const float cliffRadius = rule.maxCliffDistance; cliffRadius > 0.0f) {
            for (float radius = (std::min)(0.5f, cliffRadius); !habitatBudgetExceeded;
                    radius = (std::min)(radius * 1.5f, cliffRadius)) {
                for (const auto& direction : directions) {
                    scene::TerrainSurfaceSample source;
                    if (probe(out.position.x + direction.x * radius, out.position.z + direction.y * radius, source)
                        && source.position.y > out.position.y && source.normal.y >= 0.0f
                        && source.normal.y <= CLIFF_NORMAL_Y
                        && source.normal.x * direction.x + source.normal.z * direction.y < 0.0f) {
                        out.cliffDistance = radius;
                        break;
                    }
                }
                if (std::isfinite(out.cliffDistance) || radius == cliffRadius) break;
            }
        }
        if (!rule.useWaterLevel) return;
        prepareWaterPoint(out.position);
        for (std::size_t i = 0; i < waterCandidates.size(); ++i) {
            const auto& reading = readWater(i);
            if (!reading.valid) continue;
            const auto& water = waters[waterCandidates[i]];
            const float offset = (out.position.y - water.world.worldPosition.y) - reading.sample.baseHeightOffset;
            if (offset >= -reading.sample.waterDepthLimit
                && (!std::isfinite(out.waterOffset) || std::abs(offset) < std::abs(out.waterOffset)))
                out.waterOffset = offset;
        }
        if (!out.water && !std::isfinite(out.waterOffset)) {
            float nearestSquared = std::numeric_limits<float>::infinity();
            for (const auto& clearance : clearances) {
                math::Vector3 shore;
                if (!clearance.NearestWater(out.position, WATER_LEVEL_SEARCH_RADIUS, shore)) continue;
                const float dx = out.position.x - shore.x, dz = out.position.z - shore.z;
                const float squared = dx * dx + dz * dz;
                if (squared < nearestSquared) {
                    nearestSquared = squared;
                    out.waterOffset = (std::max)(0.0f, out.position.y - shore.y);
                }
            }
        }
    };
    scene::WorldPcgDiagnostics samples;
    samples.maxSamples = static_cast<std::size_t>(maxRejected);
    bool generated;
    {
        FBZZ_PROFILE_SCOPE("WorldPcg::EvaluateRules");
        generated = scene::GenerateWorldPcg(plan.profile, sampler, protectedPositions,
            plan.generated, diagnostic, sampleHabitat, maxRejected > 0 ? &samples : nullptr);
    }
    if (habitatBudgetExceeded) return live ? LivePreviewBudgetExceeded()
        : OpResult::Err("HABITAT_SAMPLE_BUDGET", "Narrow the region or increase placement spacing.");
    if (!generated) return OpResult::Err("PCG_FAILED",diagnostic);
    if (auto result = ValidateRetainedPlacements(plan); !result.ok) return result;
    if (plan.terrainRefresh) return OpResult::Ok();
    AddHash(guard,plan.generated.fingerprint);plan.expected=Hex(guard);
    auto& summary=plan.summary;summary.Set("schemaVersion",1);summary.Set("volume",plan.volume->instanceId);summary.Set("profile",plan.profilePath);
    if (draft) summary.Set("draft", true);
    else summary.Set("expected",plan.expected);
    summary.Set("partition",partitionOwner?partitionOwner->instanceId:std::string{});
    summary.Set("profileReference", savedVolume->profilePath);
    summary.Set("candidateCount",static_cast<int64_t>(plan.generated.stats.candidates));summary.Set("instanceCount",static_cast<int64_t>(plan.generated.stats.accepted));
    summary.Set("groupCount",static_cast<int64_t>(plan.generated.groups.size()));summary.Set("fingerprint",plan.generated.fingerprint);
    summary.Set("maxPlacementsPerCell", static_cast<int64_t>(plan.profile.maxPlacementsPerCell));
    summary.Set("placementBudgetCellSize", scene::VEGETATION_BUDGET_CELL_SIZE);
    DescribePcgBounds(summary, plan.profile);
    summary.Set("waterDistanceAccuracy","authored shoreline segments; 16-step distance bisection within requested clearance");
    auto rejected=OpData::MakeObject();const auto& stats=plan.generated.stats;
    rejected.Set("noSurface",static_cast<int64_t>(stats.noSurface));rejected.Set("biome",static_cast<int64_t>(stats.biomeRejected));rejected.Set("density",static_cast<int64_t>(stats.densityRejected));
    rejected.Set("height",static_cast<int64_t>(stats.heightRejected));rejected.Set("slope",static_cast<int64_t>(stats.slopeRejected));rejected.Set("water",static_cast<int64_t>(stats.waterRejected));
    rejected.Set("exclusion",static_cast<int64_t>(stats.exclusionRejected));rejected.Set("protected",static_cast<int64_t>(stats.protectedRejected));
    rejected.Set("cellBudget", static_cast<int64_t>(stats.cellBudgetRejected));
    rejected.Set("patch", static_cast<int64_t>(stats.patchRejected));
    rejected.Set("neighbor", static_cast<int64_t>(stats.neighborRejected));
    rejected.Set("spacing", static_cast<int64_t>(stats.spacingRejected));
    rejected.Set("material", static_cast<int64_t>(stats.materialRejected));
    rejected.Set("competition", static_cast<int64_t>(stats.competitionRejected));
    rejected.Set("aspect", static_cast<int64_t>(stats.aspectRejected));
    rejected.Set("canopy", static_cast<int64_t>(stats.canopyRejected));
    rejected.Set("waterLevel", static_cast<int64_t>(stats.waterLevelRejected));
    rejected.Set("relief", static_cast<int64_t>(stats.reliefRejected));
    rejected.Set("cliff", static_cast<int64_t>(stats.cliffRejected));
    rejected.Set("path", static_cast<int64_t>(stats.pathRejected));
    summary.Set("rejected",std::move(rejected));
    auto points = OpData::MakeArray();
    std::set<scene::WorldCellId> cells;
    std::map<std::string, int64_t> counts;
    for (const auto& rule : plan.profile.rules) counts.emplace(rule.key, 0);
    const auto pointCount = (std::min)(stats.accepted, static_cast<uint64_t>(maxPoints));
    uint64_t sampled = 0, groupStart = 0;
    for (const auto& group : plan.generated.groups) {
        cells.insert(group.cell);
        counts[group.ruleKey] += static_cast<int64_t>(group.placements.size());
        const auto groupEnd = groupStart + group.placements.size();
        while (sampled < pointCount) {
            const auto index = sampled * stats.accepted / pointCount;
            if (index >= groupEnd) break;
            const auto& placement = group.placements[static_cast<std::size_t>(index - groupStart)];
            auto point = OpData::MakeObject();
            point.Set("position", placement.position + scene::WorldCellOrigin(plan.profile.grid, group.cell));
            point.Set("rule", group.ruleKey); point.Set("id", static_cast<int64_t>(placement.id));
            points.Push(std::move(point));
            ++sampled;
        }
        groupStart = groupEnd;
    }
    auto ruleCounts = OpData::MakeObject();
    for (const auto& [rule, count] : counts) ruleCounts.Set(rule, count);
    summary.Set("ruleCounts", std::move(ruleCounts));
    auto rejectedPoints = OpData::MakeArray();
    for (const auto& site : samples.rejected) {
        auto point = OpData::MakeObject();
        point.Set("position", site.position); point.Set("rule", site.ruleKey); point.Set("reason", std::string(site.reason));
        rejectedPoints.Push(std::move(point));
    }
    summary.Set("rejectedPoints", std::move(rejectedPoints));
    summary.Set("cellCount",static_cast<int64_t>(cells.size()));summary.Set("points",std::move(points));summary.Set("applied",false);return OpResult::Ok();
}

struct PcgChange {
    std::string ownerId,name,path,reference;
    bool created=false,reuse=false;
    scene::EntityID entity;
    std::shared_ptr<world_pcg_profile_draft::NodeIdentity> identity;
    std::shared_ptr<uint64_t> metadataLifetime;
    std::shared_ptr<uint64_t> instanceLifetime;
    std::optional<scene::InstancedMeshComponent> before;
    std::optional<scene::WorldPcgGeneratedComponent> beforeMetadata;
    scene::InstancedMeshComponent after;
    scene::WorldPcgGeneratedComponent afterMetadata;
    std::shared_ptr<asset::MeshInstancesAsset> beforeAsset,afterAsset;
    struct Persisted {
        asset::MeshInstancesAsset asset;
        std::string bytes;
    };
    std::shared_ptr<Persisted> persisted;
    std::string beforeSignature,afterSignature;
};

bool ReadPlacementBytes(const std::string& path, std::string& bytes)
{
    std::ifstream file(util::FileSystem::PathFromUtf8(path), std::ios::binary | std::ios::ate);
    if (!file) return false;
    const auto size = file.tellg();
    if (size < 0 || size > std::streamoff{512 * 1024 * 1024}) return false;
    bytes.resize(static_cast<std::size_t>(size));
    if (!file.seekg(0) || !file.read(bytes.data(), static_cast<std::streamsize>(size))) return false;
    return file.peek() == std::char_traits<char>::eof() && !file.bad();
}

std::shared_ptr<PcgChange::Persisted> PersistedPlacement(const std::string& path)
{
    static std::unordered_map<std::string, std::weak_ptr<PcgChange::Persisted>> snapshots;
    static auto epoch = AssetDirtyRegistry::DiscardEpoch();
    if (epoch != AssetDirtyRegistry::DiscardEpoch()) {
        snapshots.clear();
        epoch = AssetDirtyRegistry::DiscardEpoch();
    }
    auto& entry = snapshots[path];
    if (auto known = entry.lock()) return known;
    auto disk = std::make_shared<PcgChange::Persisted>();
    if (!ReadPlacementBytes(path, disk->bytes)
        || !asset::DeserializeMeshInstancesAsset(disk->bytes, path, disk->asset)) return {};
    entry = disk;
    return disk;
}

bool MatchesPersistedPlacement(const std::string& path, PcgChange::Persisted& persisted)
{
    std::string bytes;
    if (!ReadPlacementBytes(path, bytes)) return false;
    if (!persisted.bytes.empty() && bytes == persisted.bytes) return true;
    asset::MeshInstancesAsset disk;
    if (!asset::DeserializeMeshInstancesAsset(bytes, path, disk) || !asset::MeshInstancesAssetsEqual(disk, persisted.asset)) return false;
    persisted.bytes = std::move(bytes);
    return true;
}

std::string PlacementPathKey(std::string path)
{
    path = util::FileSystem::NormalizePathSeparators(std::move(path));
    for (auto& c : path) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return path;
}

using PlacementReferences = std::unordered_map<std::string, std::size_t>;

PlacementReferences CountPlacementReferences(scene::Scene& scene)
{
    PlacementReferences references;
    const auto entities = scene.GetEntities<scene::InstancedMeshComponent>();
    references.reserve(entities.size());
    for (const auto id : entities) {
        if (!scene.GetGameObject(id)) continue;
        const auto* component = scene.GetComponent<scene::InstancedMeshComponent>(id);
        if (component->m_instanceSetRef.path.empty()) continue;
        auto path = asset::FindLoadedMeshInstancesPath(component->m_instanceSetRef.path);
        if (path.empty())
            path = asset::AssetManager::ResolveAssetPath(component->m_instanceSetRef.path);
        ++references[PlacementPathKey(std::move(path))];
    }
    return references;
}

void PublishDraft(const PcgChange& change,const std::shared_ptr<asset::MeshInstancesAsset>& value)
{
    if(auto* current=PlacementAsset(change.path))*current=*value;
    AssetDirtyRegistry::RegisterDraft(change.path,change.reference,"PCG MESH INSTANCES",value,
        [path=change.path, persisted=change.persisted](const asset::MeshInstancesAsset& draft) {
            const auto* current = PlacementAsset(path);
            if (!current || !asset::MeshInstancesAssetsEqual(*current, draft)
                || !MatchesPersistedPlacement(path, *persisted)
                || !asset::SaveMeshInstancesAsset(path, draft)) return false;
            persisted->asset = draft;
            /// @note Validate new bytes on the next read instead of parsing a file we just wrote on every save.
            persisted->bytes.clear();
            return true;
        },
        [path=change.path]{(void)asset::AssetManager::ReloadPath(path);});
}

class PcgCommand final:public ICommand {
public:
    PcgCommand(EditorContext& context,std::vector<PcgChange> changes):m_context(context),m_scene(context.activeScene),
        m_generation(m_scene->GetRenderSceneGeneration()),m_epoch(AssetDirtyRegistry::DiscardEpoch()),m_changes(std::move(changes)){}
    void Execute() override {std::string diagnostic;(void)TryExecute(diagnostic);}
    void Undo() override {std::string diagnostic;(void)TryUndo(diagnostic);}
    bool TryExecute(std::string& diagnostic) override {return Apply(true,diagnostic);}
    bool TryUndo(std::string& diagnostic) override {return Apply(false,diagnostic);}
    bool SupportsLegacyComposition() const override {return false;}
    std::string GetDescription() const override {return "Generate World PCG";}
    bool ApplyValidated(bool after, std::string& diagnostic)
    {
        if (after == m_applied || !Editable(m_context) || m_context.activeScene != m_scene
            || m_scene->GetRenderSceneGeneration() != m_generation || AssetDirtyRegistry::DiscardEpoch() != m_epoch) {
            diagnostic = "PCG scene or draft lifetime changed";
            return false;
        }
        return Publish(after, diagnostic);
    }
    bool ValidateHistory(std::string& diagnostic) const override
    {
        FBZZ_PROFILE_SCOPE("WorldPcg::ValidateOutputHistory");
        if(!Editable(m_context)||m_context.activeScene!=m_scene||m_scene->GetRenderSceneGeneration()!=m_generation||AssetDirtyRegistry::DiscardEpoch()!=m_epoch){diagnostic="PCG scene or draft lifetime changed";return false;}
        std::optional<PlacementReferences> references;
        for(const auto& change:m_changes){
            const asset::MeshInstancesAsset* current = nullptr;
            auto* owner=m_scene->GetGameObject(change.identity->entity);
            if(!m_applied&&change.created){if(m_scene->FindByGuid(change.ownerId)){diagnostic="PCG owner ID has been claimed";return false;}}
            else {
                const auto* component=owner?owner->GetComponent<scene::InstancedMeshComponent>():nullptr;
                const auto* metadata=owner?owner->GetComponent<scene::WorldPcgGeneratedComponent>():nullptr;
                const auto& expectedMetadata=m_applied?change.afterMetadata:*change.beforeMetadata;
                const auto& expectedComponent = m_applied ? change.after : *change.before;
                std::string path;
                const auto* ownerAsset = component ? PlacementAsset(component->m_instanceSetRef.path, &path) : nullptr;
                if(!owner||owner->instanceId!=change.ownerId||!component||!metadata||!ownerAsset||m_scene->GetComponentLifetime<scene::WorldPcgGeneratedComponent>(owner->GetID())!=*change.metadataLifetime
                    ||m_scene->GetComponentLifetime<scene::InstancedMeshComponent>(owner->GetID())!=*change.instanceLifetime||!SameGenerated(*metadata,expectedMetadata)
                    ||component->m_vegetationBudget != expectedComponent.m_vegetationBudget
                    ||OwnerSignature(*owner,*metadata,*component,*ownerAsset)!=(m_applied?change.afterSignature:change.beforeSignature)){
                    diagnostic="PCG generated owner was edited; detach it before regeneration";return false;}
                if(change.created&&(owner->GetChildCount()!=0||owner->GetParent()!=nullptr)){diagnostic="PCG owner acquired manual hierarchy changes";return false;}
                if (path == change.path) current = ownerAsset;
            }
            if (!current) current = PlacementAsset(change.path);
            const auto& expected=m_applied?change.afterAsset:(change.reuse?change.beforeAsset:change.afterAsset);
            if(!current||!expected||(current->instances.Snapshot() != expected->instances.Snapshot()
                && !asset::MeshInstancesAssetsEqual(*current,*expected)) || current->terrainPaintRef != expected->terrainPaintRef
                ||!MatchesPersistedPlacement(change.path,*change.persisted)){
                diagnostic="PCG placement asset was replaced or edited";return false;}
            if (change.reuse) {
                if (!references) references = CountPlacementReferences(*m_scene);
                const auto found = references->find(PlacementPathKey(change.path));
                if (found == references->end() || found->second != 1) {
                    diagnostic="PCG placement asset became shared or moved; history cannot overwrite another owner";return false;}
            }
        }
        diagnostic.clear();return true;
    }
private:
    bool Apply(bool after,std::string& diagnostic)
    {
        if(after==m_applied||!ValidateHistory(diagnostic))return false;
        return Publish(after, diagnostic);
    }
    bool Publish(bool after, std::string& diagnostic)
    {
        size_t creates=0;for(const auto& change:m_changes)if(after&&change.created)++creates;
        if(!m_scene->CanCreateGameObjects(creates)){diagnostic="PCG scene object budget exceeded";return false;}
        for(const auto& change:m_changes){if(after)PublishDraft(change,change.afterAsset);else if(change.reuse)PublishDraft(change,change.beforeAsset);else AssetDirtyRegistry::MarkClean(change.path);}
        for(auto& change:m_changes){
            auto* owner=m_scene->GetGameObject(change.identity->entity);
            if(!after&&change.created){m_scene->DestroyGameObject(owner->GetID());change.identity->entity={};*change.metadataLifetime=0;*change.instanceLifetime=0;continue;}
            if(after&&!owner){owner=&m_scene->CreateGameObject(change.name);owner->instanceId=change.ownerId;change.entity=owner->GetID();change.identity->entity=change.entity;}
            const auto& component=after?change.after:*change.before;
            if(auto* current=owner->GetComponent<scene::InstancedMeshComponent>())*current=component;else owner->AddComponent<scene::InstancedMeshComponent>(component);
            const auto& metadata=after?change.afterMetadata:*change.beforeMetadata;
            if(auto* current=owner->GetComponent<scene::WorldPcgGeneratedComponent>())*current=metadata;else owner->AddComponent<scene::WorldPcgGeneratedComponent>(metadata);
            if(after){change.afterSignature=OwnerSignature(*owner,change.afterMetadata,change.after,*change.afterAsset);owner->GetComponent<scene::WorldPcgGeneratedComponent>()->signature=change.afterSignature;change.afterMetadata.signature=change.afterSignature;}
            world_pcg_profile_draft::SyncGeneratedToken(m_generation,*owner,change.metadataLifetime,change.instanceLifetime);
        }
        m_applied=after;PruneSelection(m_context);m_context.sceneDirty=true;if(m_context.markSceneDirty)m_context.markSceneDirty();diagnostic.clear();return true;
    }
    EditorContext& m_context;scene::Scene* m_scene;uint64_t m_generation,m_epoch;std::vector<PcgChange> m_changes;bool m_applied=false;
};

OpResult BakePcg(OpContext& context, const OpArgs& args, std::span<const scene::WorldCellBounds> terrainEdits = {},
    std::unique_ptr<PcgCommand>* terrainHistory = nullptr)
{
    FBZZ_PROFILE_SCOPE("WorldPcg::Generate");
    if(!context.undo.IsRecordingEnabled())return OpResult::Err("NOT_AVAILABLE","PCG generation requires enabled Undo recording");
    PcgPlan plan;
    if(auto result=PreparePcg(context,args,plan,false,terrainEdits);!result.ok || result.noChange)return result;
    if(!plan.terrainRefresh && (args.GetString("expected").empty()||args.GetString("expected")!=plan.expected))return OpResult::Err("PCG_CONFLICT","PCG inputs changed; preview again before generation");
    auto& ctx=context.ctx;auto& scene=*ctx.activeScene;
    std::map<GroupKey,const scene::WorldPcgCellGroup*> desired;for(const auto& group:plan.generated.groups)desired.emplace(GroupKey{group.cell,group.ruleKey},&group);
    std::set<GroupKey> touched;for(const auto& [key,group]:desired){(void)group;touched.insert(key);}
    for(const auto& [key,owned]:plan.owned){
        if (plan.terrainRefresh && (owned.outside || owned.IsProtected())) continue;
        const auto& snapshot = owned.snapshot;
        if(!snapshot || !owned.validPose)return OpResult::Err("PCG_CONFLICT","Generated snapshot or transform is invalid");
        const auto matrix = owned.world.GetWorldMatrix();
        for(const auto& p:snapshot->placements)if(p.active){const auto v=matrix*math::Vector4{p.position,1};if(InBounds(plan.profile.bounds,{v.x,v.y,v.z})){touched.insert(key);break;}}
    }
    std::vector<PcgChange> changes;size_t keptOutside=0;std::unordered_set<std::string> plannedOwners;
    std::optional<PlacementReferences> references;
    std::size_t totalPlacements = 0;
    if (plan.terrainRefresh) for (auto& [key, owned] : plan.owned) {
        const bool replaces = touched.contains(key);
        if (replaces && owned.outside) owned.ReadSignature();
        if (!replaces || owned.IsProtected()) totalPlacements += owned.asset->instances.Snapshot()->placements.size();
    }
    const auto profileReference = touched.empty() ? std::string{} : asset::EncodeGuidRef(plan.profilePath);
    for(const auto& key:touched){
        const auto old=plan.owned.find(key);const auto wanted=desired.find(key);const Owned* owned=old==plan.owned.end()?nullptr:&old->second;
        if(owned && owned->IsProtected()) {
            if (plan.terrainRefresh) continue;
            return OpResult::Err("PCG_MANUAL_OVERRIDE","A generated owner was edited; detach it before regeneration: "+owned->owner->instanceId);
        }
        PcgChange change;change.created=owned==nullptr;change.ownerId=owned?owned->owner->instanceId:asset::AssetDatabase::DeriveGuid(plan.volume->instanceId,"PCG/"+scene::WorldCellName(key.first)+"/"+key.second);
        if(change.created){
            uint64_t ordinal=0;
            while(scene.FindByGuid(change.ownerId)||plannedOwners.contains(change.ownerId)){
                if(auto* occupied=scene.FindByGuid(change.ownerId))if(const auto* metadata=occupied->GetComponent<scene::WorldPcgGeneratedComponent>();metadata&&metadata->volumeId==plan.volume->instanceId)
                    return OpResult::Err("PCG_MANUAL_OVERRIDE","An incomplete generated owner still claims this identity; detach it before regeneration");
                if(++ordinal>scene.GameObjectCount()+plannedOwners.size()+1)return OpResult::Err("PCG_CONFLICT","No free generated owner identity is available");
                change.ownerId=asset::AssetDatabase::DeriveGuid(plan.volume->instanceId,"PCG/"+scene::WorldCellName(key.first)+"/"+key.second+"/owner/"+std::to_string(ordinal));
            }
            plannedOwners.insert(change.ownerId);
        }
        std::vector<renderer::InstancePlacement> placements;std::vector<asset::InstanceMeshPart> parts;std::vector<float> lods;
        if(owned){
            const auto& snapshot=owned->snapshot;
            const auto& world = owned->world;
            if(world.worldPosition!=math::Vector3::ZERO||world.worldScale!=math::Vector3::ONE||world.worldRotation!=math::Quaternion::Identity())return OpResult::Err("PCG_CONFLICT","Generated owner requires its original identity transform; detach it before editing");
            for(const auto& p:snapshot->placements)if(p.active&&!InBounds(plan.profile.bounds,p.position)){
                scene::WorldCellId retainedCell;if(!scene::TryLocateWorldCell(plan.profile.grid,p.position,retainedCell)||retainedCell!=key.first)
                    return OpResult::Err("PCG_PARTIAL_REPARTITION","Grid changes require regenerating all owned rows; partial regeneration preserves outside ownership");
                placements.push_back(p);++keptOutside;}
            parts.assign(owned->asset->instances.Parts().begin(),owned->asset->instances.Parts().end());lods.assign(owned->asset->instances.LodHeights().begin(),owned->asset->instances.LodHeights().end());
        }
        if(wanted!=desired.end()){
            const auto& group=*wanted->second;
            if(!placements.empty()){
                const auto oldParts=owned->asset->instances.Parts();const auto oldLods=owned->asset->instances.LodHeights();
                const bool same=owned->metadata->includeHlod==group.includeHlod&&oldParts.size()==group.parts.size()&&oldLods.size()==group.lodHeights.size()
                    &&std::equal(oldLods.begin(),oldLods.end(),group.lodHeights.begin())
                    &&std::equal(oldParts.begin(),oldParts.end(),group.parts.begin(),[](const auto& a,const auto& b){return a.meshPath==b.meshPath&&a.materialPath==b.materialPath&&a.lod==b.lod;});
                if(!same)return OpResult::Err("PCG_PARTIAL_DEFINITION_CHANGE","Changing a rule definition requires regenerating all its owned rows or using a new rule key");}
            parts=group.parts;lods=group.lodHeights;const auto origin=scene::WorldCellOrigin(plan.profile.grid,group.cell);
            for(auto p:group.placements){p.position=p.position+origin;placements.push_back(p);}
        }
        if (plan.terrainRefresh) {
            totalPlacements += placements.size();
            if (owned && wanted != desired.end()) {
                const auto old = owned->asset->instances.Snapshot();
                std::unordered_map<uint32_t, std::size_t> order;
                for (std::size_t index = 0; index < old->placements.size(); ++index)
                    order.emplace(old->placements[index].id, index);
                const auto rank = [&](uint32_t id) {
                    const auto found = order.find(id);
                    return found == order.end() ? old->placements.size() : found->second;
                };
                std::stable_sort(placements.begin(), placements.end(),
                    [&](const auto& a, const auto& b) { return rank(a.id) < rank(b.id); });
            }
        }
        auto after=std::make_shared<asset::MeshInstancesAsset>();
        after->terrainPaintRef = profileReference;
        if (owned) {
            const auto& beforeProfile = owned->asset->terrainPaintRef;
            const auto encodedBefore = asset::EncodeGuidRef(beforeProfile);
            if (PcgAssetIdentity(encodedBefore) == PcgAssetIdentity(after->terrainPaintRef)) after->terrainPaintRef = beforeProfile;
        }
        if(!after->instances.Replace(parts,lods,placements))return OpResult::Err("PCG_OUTPUT_INVALID","PCG output contains conflicting stable IDs or invalid definitions");
        const bool includeHlod=wanted!=desired.end()?wanted->second->includeHlod:owned->metadata->includeHlod;
        if(owned&&asset::MeshInstancesAssetsEqual(*after,*owned->asset)&&owned->metadata->includeHlod==includeHlod)continue;
        change.name="PCG "+key.second+" ["+scene::WorldCellName(key.first)+"]";
        change.entity = owned ? owned->owner->GetID() : scene::EntityID{};
        change.identity = world_pcg_profile_draft::NodeToken(scene.GetRenderSceneGeneration(), change.ownerId, change.entity);
        change.metadataLifetime = world_pcg_profile_draft::LifetimeToken<scene::WorldPcgGeneratedComponent>(
            owned ? scene.GetComponentLifetime<scene::WorldPcgGeneratedComponent>(change.entity) : 0);
        change.instanceLifetime = world_pcg_profile_draft::LifetimeToken<scene::InstancedMeshComponent>(
            owned ? scene.GetComponentLifetime<scene::InstancedMeshComponent>(change.entity) : 0);
        change.afterAsset=after;change.afterMetadata.volumeId=plan.volume->instanceId;change.afterMetadata.ruleKey=key.second;change.afterMetadata.cellX=key.first.x;change.afterMetadata.cellZ=key.first.z;change.afterMetadata.includeHlod=includeHlod;
        if(owned){
            change.before=*owned->component;change.beforeMetadata=*owned->metadata;change.beforeSignature=owned->signature;
            change.path=owned->path;
            change.reuse=util::FileSystem::IsChildPathText(change.path,ctx.projectRoot+"/Assets");
            if (change.reuse) {
                if (!references) references = CountPlacementReferences(scene);
                const auto found = references->find(PlacementPathKey(change.path));
                change.reuse = found != references->end() && found->second == 1;
            }
            if(change.reuse){change.beforeAsset=std::make_shared<asset::MeshInstancesAsset>(*owned->asset);change.reference=owned->component->m_instanceSetRef.path;}
            change.after=*owned->component;
        }
        if(!change.reuse){const auto file=asset::AssetDatabase::GenerateGuid();const auto folder=asset::AssetDatabase::DeriveGuid(plan.volume->instanceId,"PCG Assets");change.path=ctx.projectRoot+"/Assets/World/PCG/"+folder+"/"+file+".meshinstances";
            if(util::FileSystem::Exists(util::FileSystem::PathFromUtf8(change.path)))return OpResult::Err("PCG_FILE_CONFLICT","PCG output path already exists without exclusive ownership: "+change.path);}
        changes.push_back(std::move(change));
    }
    if(changes.empty()){auto result=OpResult::Data(plan.summary);result.noChange=true;return result;}
    if (plan.terrainRefresh && totalPlacements > plan.profile.maxInstances)
        return OpResult::Err("PCG_FAILED", "PCG instance budget exceeded after including retained placements");
    size_t createCount=0;for(const auto& change:changes)if(change.created)++createCount;
    if(!scene.CanCreateGameObjects(createCount))return OpResult::Err("SCENE_CAPACITY","PCG owner budget exceeds scene capacity");
    for(auto& change:changes){
        if(!change.reuse){
            if(!asset::SaveMeshInstancesAsset(change.path,*change.afterAsset))return OpResult::Err("WRITE_FAILED","PCG output could not be staged: "+change.path);
            const auto guid=asset::AssetDatabase::GuidFromPath(change.path);if(guid.empty())return OpResult::Err("META_FAILED","PCG output metadata could not be staged: "+change.path);
            const auto relative=util::FileSystem::PathToUtf8(util::FileSystem::PathFromUtf8(change.path).lexically_relative(util::FileSystem::PathFromUtf8(ctx.projectRoot)));
            change.reference="guid:"+guid+"|"+relative;
            const auto* loaded=PlacementAsset(change.path);if(!loaded)return OpResult::Err("LOAD_FAILED","PCG output readback failed: "+change.path);
            change.afterAsset=std::make_shared<asset::MeshInstancesAsset>(*loaded);
            ctx.requestAssetBrowserRefresh = true;
        }
        /// @note Multiple unsaved strokes share the last saved baseline while history owns each draft revision.
        change.persisted = PersistedPlacement(change.path);
        if (!change.persisted) return OpResult::Err("LOAD_FAILED", "PCG saved placement baseline is missing: " + change.path);
        if (change.reuse && !AssetDirtyRegistry::IsDirty(change.path)
            && !asset::MeshInstancesAssetsEqual(*change.beforeAsset, change.persisted->asset))
            return OpResult::Err("PCG_CONFLICT", "PCG placement differs from the saved baseline: " + change.path);
        change.after.m_instanceSetRef={change.reference,asset::InstanceSetAsset::TYPE_NAME};change.after.ResetOverrides();
    }
    auto command=std::make_unique<PcgCommand>(ctx,std::move(changes));std::string diagnostic;
    if(!command->TryExecute(diagnostic))return OpResult::Err("PCG_CONFLICT",diagnostic);
    auto result = OpResult::Data(plan.summary);
    result.data.Set("applied", true);
    result.data.Set("preservedOutsideCount", static_cast<int64_t>(keptOutside));
    if (terrainHistory) *terrainHistory = std::move(command);
    else result.command = std::move(command);
    return result;
}

} /// @note namespace world_pcg_authoring

OpResult PreviewWorldPcg(OpContext& context,const OpArgs& args)
{world_pcg_authoring::PcgPlan plan;if(auto result=world_pcg_authoring::PreparePcg(context,args,plan,true);!result.ok)return result;return OpResult::Data(std::move(plan.summary));}

bool SameWorldPcgProfile(EditorContext& context, const std::string& first, const std::string& second)
{
    const auto path = world_pcg_authoring::ProfilePath(context, first, true);
    const auto other = world_pcg_authoring::ProfilePath(context, second, true);
    if (path.empty() || other.empty()) return false;
    if (path == other) return true;
    std::error_code error;
    return std::filesystem::equivalent(util::FileSystem::PathFromUtf8(path), util::FileSystem::PathFromUtf8(other), error);
}
OpResult GenerateWorldPcg(OpContext& context,const OpArgs& args) {return world_pcg_authoring::BakePcg(context,args);}

namespace {

bool CanRefreshTerrainPcg(const scene::GameObject& owner)
{
    const auto& scene = *owner.GetScene();
    return owner.activeInHierarchy() && !owner.IsRuntimeGenerated()
        && !scene.GetEntities<scene::WorldPcgVolumeComponent>().empty()
        && !scene.GetEntities<scene::WorldPcgGeneratedComponent>().empty();
}

scene::WorldCellBounds EmptyXZBounds()
{
    constexpr auto INF = std::numeric_limits<float>::infinity();
    return {{INF, 0, INF}, {-INF, 0, -INF}};
}

void IncludeTerrainWorldPoint(scene::WorldCellBounds& bounds, const scene::Transform& world, const math::Vector3& local)
{
    const auto point = ToTerrainWorld(world, local);
    world_pcg_authoring::IncludeXZ(bounds, {point, point});
}

void AppendTerrainPcgBounds(const scene::Transform& world, const scene::TerrainComponent& terrain,
    const scene::TerrainHeightRegion& region, bool includeExtraSurface, std::vector<scene::WorldCellBounds>& bounds)
{
    if (!terrain.enabled) return;
    auto result = EmptyXZBounds();
    if (!region.Empty()) {
        const float height = std::abs(terrain.maxHeight);
        for (const int x : {region.minX, region.maxX})
            for (const int z : {region.minZ, region.maxZ})
                for (const float y : {-height, height})
                    IncludeTerrainWorldPoint(result, world, {x * terrain.cellSize, y, z * terrain.cellSize});
    }
    if (includeExtraSurface)
        for (const auto& point : terrain.surfacePositions) IncludeTerrainWorldPoint(result, world, point);
    if (!world_pcg_authoring::Finite(result.min) || !world_pcg_authoring::Finite(result.max)) return;
    /// @note The terrain query accepts sites on the outer edge; PCG bounds are half-open.
    result.max.x = std::nextafter(result.max.x, std::numeric_limits<float>::infinity());
    result.max.z = std::nextafter(result.max.z, std::numeric_limits<float>::infinity());
    bounds.push_back(result);
}

template<typename T>
void IncludeTerrainArrayDiff(const std::vector<T>& before, const std::vector<T>& after,
    int width, int rows, int stride, scene::TerrainHeightRegion& region)
{
    const auto rowSize = static_cast<std::size_t>(width) * stride;
    if (before.size() != after.size()) {
        region.Include({0, 0, width - 1, rows - 1});
        return;
    }
    if (before.empty() || before.size() != rowSize * rows) return;
    for (int z = 0; z < rows; ++z) {
        const auto* oldRow = before.data() + z * rowSize;
        const auto* newRow = after.data() + z * rowSize;
        if (std::memcmp(oldRow, newRow, rowSize * sizeof(T)) == 0) continue;
        for (int x = 0; x < width; ++x)
            if (std::memcmp(oldRow + x * stride, newRow + x * stride, stride * sizeof(T)) != 0)
                region.Include({x, z, x, z});
    }
}

struct TerrainPcgHistory {
    EditorContext& context;
    std::unique_ptr<ICommand> terrain;
    std::vector<std::unique_ptr<world_pcg_authoring::PcgCommand>> generated;

    bool Apply(bool redo, std::string& reason)
    {
        const auto transition = [](ICommand& command, bool forward, std::string& diagnostic) {
            return forward ? command.TryExecute(diagnostic) : command.TryUndo(diagnostic);
        };
        for (const auto& command : generated)
            if (!command->ValidateHistory(reason)) return false;
        if (generated.size() == 1) {
            /// @note Terrain authoring does not edit PCG owners. Defer the dirty callback until both edits are applied.
            bool markedDirty = false;
            auto markSceneDirty = std::exchange(context.markSceneDirty, [&] { markedDirty = true; });
            bool applied = transition(*terrain, redo, reason);
            if (applied && !generated.front()->ApplyValidated(redo, reason)) {
                std::string rollbackReason;
                if (!transition(*terrain, !redo, rollbackReason))
                    FBZZ_LOG_ERROR("Terrain history rollback failed: %s", rollbackReason.c_str());
                applied = false;
            }
            context.markSceneDirty = std::move(markSceneDirty);
            if (markedDirty && context.markSceneDirty) context.markSceneDirty();
            return applied;
        }
        if (!transition(*terrain, redo, reason)) return false;
        const auto at = [&](std::size_t i) -> ICommand& {
            return *generated[redo ? i : generated.size() - 1 - i];
        };
        std::size_t applied = 0;
        for (; applied < generated.size(); ++applied)
            if (!transition(at(applied), redo, reason)) break;
        if (applied == generated.size()) return true;
        std::string rollbackReason;
        while (applied > 0)
            if (!transition(at(--applied), !redo, rollbackReason))
                FBZZ_LOG_ERROR("PCG terrain history rollback failed: %s", rollbackReason.c_str());
        if (!transition(*terrain, !redo, rollbackReason))
            FBZZ_LOG_ERROR("Terrain history rollback failed: %s", rollbackReason.c_str());
        return false;
    }
};

} /// @note namespace

void CollectTerrainPcgBounds(const scene::GameObject& owner, const scene::TerrainComponent& terrain,
    const scene::TerrainHeightRegion& edited, std::vector<scene::WorldCellBounds>& bounds)
{
    if (!CanRefreshTerrainPcg(owner) || edited.Empty()) return;
    scene::Transform world;
    if (!world_pcg_authoring::ReadWorld(owner, world)) return;
    /// @note Interpolated normals use neighboring vertices on both sides of the edited sample.
    const scene::TerrainHeightRegion padded{(std::max)(0, edited.minX - 2), (std::max)(0, edited.minZ - 2),
        (std::min)(terrain.columns - 1, edited.maxX + 2), (std::min)(terrain.rows - 1, edited.maxZ + 2)};
    AppendTerrainPcgBounds(world, terrain, padded, false, bounds);
}

void CollectTerrainPcgBounds(const scene::GameObject& owner, const scene::TerrainComponent& before,
    const scene::TerrainComponent& after, std::vector<scene::WorldCellBounds>& bounds)
{
    if (!CanRefreshTerrainPcg(owner) || (!before.enabled && !after.enabled)) return;
    if (before.enabled != after.enabled || before.columns != after.columns || before.rows != after.rows
        || before.cellSize != after.cellSize || before.maxHeight != after.maxHeight
        || before.layerMaterials != after.layerMaterials || before.surfaceIndices != after.surfaceIndices
        || before.surfacePositions != after.surfacePositions) {
        scene::Transform world;
        if (!world_pcg_authoring::ReadWorld(owner, world)) return;
        AppendTerrainPcgBounds(world, before, {0, 0, before.columns - 1, before.rows - 1}, true, bounds);
        AppendTerrainPcgBounds(world, after, {0, 0, after.columns - 1, after.rows - 1}, true, bounds);
        return;
    }
    if (after.columns < 2 || after.rows < 2) return;
    scene::TerrainHeightRegion changed;
    IncludeTerrainArrayDiff(before.heightData, after.heightData, after.columns, after.rows, 1, changed);
    IncludeTerrainArrayDiff(before.splatIndices, after.splatIndices, after.columns, after.rows, scene::TERRAIN_SPLAT_SLOTS, changed);
    IncludeTerrainArrayDiff(before.splatWeights, after.splatWeights, after.columns, after.rows, scene::TERRAIN_SPLAT_SLOTS, changed);
    IncludeTerrainArrayDiff(before.holeData, after.holeData, after.columns - 1, after.rows - 1, 1, changed);
    CollectTerrainPcgBounds(owner, after, changed, bounds);
}

std::unique_ptr<ICommand> RefreshTerrainPcg(OpContext& context, std::span<const scene::WorldCellBounds> bounds,
    std::unique_ptr<ICommand> terrainCommand)
{
    if (bounds.empty() || !context.undo.IsRecordingEnabled() || !world_pcg_authoring::Editable(context.ctx))
        return terrainCommand;
    FBZZ_PROFILE_SCOPE("WorldPcg::RefreshTerrain");
    auto& scene = *context.ctx.activeScene;
    std::unordered_set<std::string_view> generatedVolumes;
    for (const auto id : scene.GetEntities<scene::WorldPcgGeneratedComponent>()) {
        const auto* metadata = scene.GetComponent<scene::WorldPcgGeneratedComponent>(id);
        if (metadata) generatedVolumes.insert(metadata->volumeId);
    }
    std::vector<std::string> volumes;
    for (const auto id : scene.GetEntities<scene::WorldPcgVolumeComponent>()) {
        const auto* owner = scene.GetGameObject(id);
        const auto* volume = scene.GetComponent<scene::WorldPcgVolumeComponent>(id);
        if (owner && owner->activeInHierarchy() && !owner->IsRuntimeGenerated() && volume && volume->enabled
            && generatedVolumes.contains(owner->instanceId)) volumes.push_back(owner->instanceId);
    }
    std::sort(volumes.begin(), volumes.end());
    std::vector<std::unique_ptr<world_pcg_authoring::PcgCommand>> generated;
    for (const auto& volume : volumes) {
        OpArgs args;
        args.Set("volume", volume);
        args.Set("maxPoints", 0);
        std::unique_ptr<world_pcg_authoring::PcgCommand> command;
        auto result = world_pcg_authoring::BakePcg(context, args, bounds, &command);
        if (!result.ok) {
            FBZZ_LOG_WARN("Terrain PCG refresh for volume %s: %s (%s)", volume.c_str(), result.message.c_str(), result.errorCode.c_str());
            continue;
        }
        if (command) generated.push_back(std::move(command));
    }
    if (generated.empty()) return terrainCommand;
    const auto description = terrainCommand->GetDescription();
    const auto history = std::make_shared<TerrainPcgHistory>(context.ctx, std::move(terrainCommand), std::move(generated));
    return std::make_unique<CheckedLambdaCommand>(description,
        [history](std::string& reason) { return history->Apply(true, reason); },
        [history](std::string& reason) { return history->Apply(false, reason); });
}

} /// @note namespace fbzz::editor

namespace fbzz::editor {
namespace world_pcg_authoring {
std::string ProfilePath(EditorContext& context, const std::string& input, bool allowMissing);
std::string HashText(std::string_view text);
bool Editable(EditorContext& context);
}
namespace world_pcg_profile_draft {

bool ReadProfileBytes(const std::string& path, std::string& bytes)
{
    /// @note The filesystem::path overload is binary; the string overload normalizes CRLF on Windows.
    std::error_code error;const auto size=std::filesystem::file_size(util::FileSystem::PathFromUtf8(path),error);
    if(error||size>8*1024*1024)return false;
    return util::FileSystem::ReadText(util::FileSystem::PathFromUtf8(path), bytes);
}

std::string ExistingReference(const std::string& path)
{
    const auto guid=asset::AssetDatabase::TryGetGuidFromPath(path);
    const auto relative=util::FileSystem::PathFromUtf8(path).lexically_relative(
        util::FileSystem::PathFromUtf8(asset::AssetDatabase::ProjectRoot()));
    const auto hint=util::FileSystem::PathToUtf8(relative);
    return guid.empty()?hint:"guid:"+guid+"|"+hint;
}

OpData ProfileData(const std::string& path,const std::string& text,const scene::WorldPcgProfile& profile)
{
    auto data=OpData::MakeObject();data.Set("path",path);data.Set("text",text);
    data.Set("fingerprint",world_pcg_authoring::HashText(text));
    data.Set("reference",path.empty()?std::string{}:ExistingReference(path));
    data.Set("key",profile.key);data.Set("name",profile.name);
    data.Set("ruleCount",static_cast<int64_t>(profile.rules.size()));
    data.Set("maxPlacementsPerCell", static_cast<int64_t>(profile.maxPlacementsPerCell));
    data.Set("placementBudgetCellSize", scene::VEGETATION_BUDGET_CELL_SIZE);
    return data;
}

template<typename T> bool Same(const T& a,const T& b)
{
    if constexpr(std::is_same_v<T,scene::WorldPcgVolumeComponent>)
        return a.enabled==b.enabled&&a.profilePath==b.profilePath&&a.overrideBounds==b.overrideBounds
            &&a.boundsMin==b.boundsMin&&a.boundsMax==b.boundsMax&&a.overrideSeed==b.overrideSeed
            &&a.seed==b.seed&&a.protectManual==b.protectManual;
    else return a.volumeId==b.volumeId&&a.ruleKey==b.ruleKey&&a.signature==b.signature
        &&a.cellX==b.cellX&&a.cellZ==b.cellZ&&a.manualOverride==b.manualOverride&&a.includeHlod==b.includeHlod;
}

template<typename Ignored> std::string SnapshotMetadata(scene::Scene& scene,scene::GameObject& node)
{
    std::ostringstream out;out<<std::setprecision(std::numeric_limits<float>::max_digits10);
    const auto text=[&](std::string_view value){out<<value.size()<<':'<<value<<';';};
    text(node.instanceId);text(node.name);text(node.tag);text(node.prefabAssetPath);text(node.prefabSourceId);
    text(node.prefabSourceSnapshot);text(node.GetParent()?node.GetParent()->instanceId:std::string{});
    out<<node.layer<<';'<<node.activeSelf()<<';'<<node.IsRuntimeGenerated()<<';';
    const auto vec=[&](const math::Vector3& v){out<<v.x<<','<<v.y<<','<<v.z<<';';};
    vec(node.transform.position);vec(node.transform.scale);
    const auto q=node.transform.rotation;out<<q.x<<','<<q.y<<','<<q.z<<','<<q.w<<';';
    out<<node.GetChildCount()<<';';for(int i=0;i<node.GetChildCount();++i)text(node.GetChild(i)->instanceId);
    scene::ForEachRegisteredComponent([&]<typename T,typename Registration>(){
        if constexpr(!std::is_same_v<T,Ignored>){const auto lifetime=scene.GetComponentLifetime<T>(node.GetID());
            if(lifetime){text(Registration::serializedName);out<<(std::is_same_v<T,scene::InstancedMeshComponent>?0:lifetime)<<';';}}
    });
    if(const auto* instances=node.GetComponent<scene::InstancedMeshComponent>()){
        text(instances->m_instanceSetRef.path);out<<instances->m_enabled<<';'<<instances->m_castShadows<<';'<<instances->m_maxDistance<<';';
        const auto& budget = instances->m_vegetationBudget;
        out << budget.enabled << ';' << budget.drawDetailPerCell << ';' << budget.nearDetailPerCell << ';'
            << budget.shadowDetailPerCell << ';';
        const auto snapshot=instances->Snapshot();out<<(snapshot?snapshot->revision:0)<<';';
    }
    return out.str();
}

template<typename Entries>
void PruneExpiredTokens(Entries& entries, std::size_t& nextPrune)
{
    if (entries.size() < nextPrune) return;
    std::erase_if(entries, [](const auto& item) { return item.second.expired(); });
    nextPrune = (std::max)(std::size_t{256}, entries.size() * 2);
}

std::shared_ptr<NodeIdentity> NodeToken(uint64_t generation,const std::string& id,scene::EntityID entity)
{
    static std::map<std::pair<uint64_t,std::string>,std::weak_ptr<NodeIdentity>> entries;
    static std::size_t nextPrune = 256;
    PruneExpiredTokens(entries, nextPrune);
    auto& weak=entries[{generation,id}];
    if(auto current=weak.lock())return current;
    auto token=std::make_shared<NodeIdentity>();token->entity=entity;weak=token;return token;
}
template<typename T> auto& LifetimeTokens()
{static std::unordered_map<uint64_t,std::weak_ptr<uint64_t>> tokens;return tokens;}
template<typename T> std::shared_ptr<uint64_t> LifetimeToken(uint64_t lifetime)
{
    static std::size_t nextPrune = 256;
    auto& tokens=LifetimeTokens<T>();
    PruneExpiredTokens(tokens, nextPrune);
    if(lifetime){if(auto token=tokens[lifetime].lock())return token;}
    auto token=std::make_shared<uint64_t>(lifetime);if(lifetime)tokens[lifetime]=token;return token;
}

void SyncGeneratedToken(uint64_t generation,scene::GameObject& owner,const std::shared_ptr<uint64_t>& token,const std::shared_ptr<uint64_t>& instanceToken)
{
    NodeToken(generation,owner.instanceId,owner.GetID())->entity=owner.GetID();
    const auto lifetime=owner.GetScene()->GetComponentLifetime<scene::WorldPcgGeneratedComponent>(owner.GetID());
    *token=lifetime;
    if(lifetime)LifetimeTokens<scene::WorldPcgGeneratedComponent>()[lifetime]=token;
    const auto instances=owner.GetScene()->GetComponentLifetime<scene::InstancedMeshComponent>(owner.GetID());*instanceToken=instances;
    if(instances)LifetimeTokens<scene::InstancedMeshComponent>()[instances]=instanceToken;
}

template<typename T> class CheckedPcgComponentCommand final : public ICommand {
public:
    CheckedPcgComponentCommand(EditorContext& context,std::string id,std::optional<T> before,
        std::optional<T> after,bool created,std::string label)
        :m_context(context),m_scene(context.editScene),m_generation(m_scene->GetRenderSceneGeneration()),
         m_id(std::move(id)),m_before(std::move(before)),m_after(std::move(after)),m_created(created),m_label(std::move(label))
    {
        scene::EntityID entity{};uint64_t lifetime=0;
        if(auto* node=m_scene->FindByGuid(m_id)){entity=node->GetID();lifetime=m_scene->GetComponentLifetime<T>(entity);m_metadata=SnapshotMetadata<T>(*m_scene,*node);}
        m_identity=NodeToken(m_generation,m_id,entity);m_lifetime=LifetimeToken<T>(lifetime);
    }
    std::string GetDescription() const override{return m_label;}
    bool SupportsLegacyComposition() const override{return false;}
    void Execute() override{std::string reason;(void)TryExecute(reason);}
    void Undo() override{std::string reason;(void)TryUndo(reason);}
    bool ValidateHistory(std::string& reason) const override{return Check(reason);}
    bool TryExecute(std::string& reason) override
    {
        if(m_applied)return Reject(reason,"PCG command is already applied");
        if(!Check(reason))return false;
        auto* node=m_scene->FindByGuid(m_id);
        if(m_created){node=m_scene->TryCreateGameObject("World PCG Volume");if(!node)return Reject(reason,"Scene entity capacity exhausted");node->instanceId=m_id;m_identity->entity=node->GetID();}
        Apply(*node,m_after);m_applied=true;Capture(*node);m_context.sceneDirty=true;if(m_context.markSceneDirty)m_context.markSceneDirty();reason.clear();return true;
    }
    bool TryUndo(std::string& reason) override
    {
        if(!m_applied)return Reject(reason,"PCG command is not applied");
        if(!Check(reason))return false;
        auto* node=m_scene->FindByGuid(m_id);
        if(m_created){
            size_t count=0;scene::ForEachRegisteredComponent([&]<typename U,typename Registration>(){(void)sizeof(Registration);if(m_scene->HasComponent<U>(m_identity->entity))++count;});
            if(count!=1||node->GetChildCount()!=0)return Reject(reason,"Created PCG volume contains manual additions");
            if(!m_scene->DestroyGameObject(m_identity->entity))return Reject(reason,"PCG volume could not be removed");
            *m_lifetime=0;m_identity->entity={};m_metadata.clear();
        }else{Apply(*node,m_before);Capture(*node);}
        m_applied=false;m_context.sceneDirty=true;if(m_context.markSceneDirty)m_context.markSceneDirty();reason.clear();return true;
    }
private:
    static bool Reject(std::string& reason,const char* text){reason=text;return false;}
    bool Check(std::string& reason) const
    {
        if(!world_pcg_authoring::Editable(m_context)||m_context.editScene!=m_scene||m_scene->GetRenderSceneGeneration()!=m_generation)
            return Reject(reason,"PCG editing Scene changed");
        auto* node=m_scene->FindByGuid(m_id);
        if(m_created&&!m_applied){if(node)return Reject(reason,"PCG volume identity is already occupied");if(!m_scene->CanCreateGameObjects())return Reject(reason,"Scene entity capacity exhausted");reason.clear();return true;}
        if(!node||node->GetID()!=m_identity->entity||SnapshotMetadata<T>(*m_scene,*node)!=m_metadata)
            return Reject(reason,"PCG node was changed outside this command");
        const auto& expected=m_applied?m_after:m_before;const auto* current=node->GetComponent<T>();
        if(static_cast<bool>(expected)!=(current!=nullptr)||m_scene->GetComponentLifetime<T>(m_identity->entity)!=*m_lifetime
            ||(current&& !Same(*current,*expected)))return Reject(reason,"PCG component value or lifetime changed");
        reason.clear();return true;
    }
    void Apply(scene::GameObject& node,const std::optional<T>& value)
    {if(value){if(auto* current=node.GetComponent<T>())*current=*value;else node.AddComponent<T>(*value);}else node.RemoveComponent<T>();}
    void Capture(scene::GameObject& node){*m_lifetime=m_scene->GetComponentLifetime<T>(node.GetID());if(*m_lifetime)LifetimeTokens<T>()[*m_lifetime]=m_lifetime;m_metadata=SnapshotMetadata<T>(*m_scene,node);}
    EditorContext& m_context;scene::Scene* m_scene;uint64_t m_generation=0;
    std::shared_ptr<uint64_t> m_lifetime;std::shared_ptr<NodeIdentity> m_identity;
    std::string m_id,m_metadata;std::optional<T> m_before,m_after;
    bool m_created=false,m_applied=false;std::string m_label;
};

template<typename T> OpResult ApplyChecked(EditorContext& context,std::string node,std::optional<T> before,
    std::optional<T> after,bool created,const char* label)
{
    auto command=std::make_unique<CheckedPcgComponentCommand<T>>(context,node,std::move(before),std::move(after),created,label);
    std::string reason;if(!command->TryExecute(reason))return OpResult::Err("CONFLICT",reason);
    auto data=OpData::MakeObject();data.Set("node",node);data.Set("volume",node);data.Set("created",created);
    auto result=OpResult::Data(std::move(data));result.command=std::move(command);return result;
}

} /// @note namespace world_pcg_profile_draft

OpResult InspectWorldPcgProfile(OpContext& context,const OpArgs& args)
{
    using namespace world_pcg_profile_draft;
    const auto input=args.GetString("path");std::string path,text,diagnostic;scene::WorldPcgProfile profile;
    const auto preset = args.GetString("preset");
    if (!preset.empty() && (!input.empty() || preset != "rocks"))
        return OpResult::Err("BAD_ARG", "Use preset=rocks without a saved path");
    if(input.empty()){
        if (preset == "rocks") {
            const auto rock = args.GetString("rockSource"), pebble = args.GetString("pebbleSource");
            if (rock.empty() || pebble.empty()) return OpResult::Err("BAD_ARG", "Rock and pebble prototype references are required");
            profile = scene::CreateRockNaturePcgProfile(rock, pebble);
        } else {
            scene::WorldPcgRule rule;rule.key="trees";rule.name="Trees";rule.parts={{"primitive:cube","Assets/Materials/Default.mat",0}};rule.lodHeights={0};profile.rules.push_back(std::move(rule));
        }
        if(!scene::SerializeWorldPcgProfile(profile,text,diagnostic))return OpResult::Err("BAD_PROFILE",diagnostic);
    }else{
        path=world_pcg_authoring::ProfilePath(context.ctx,input,false);
        if(path.empty())return OpResult::Err("BAD_PATH","Choose an existing .pcg inside the project Assets directory");
        if(!ReadProfileBytes(path,text)||!scene::DeserializeWorldPcgProfile(text,profile,diagnostic))return OpResult::Err("BAD_PROFILE",diagnostic.empty()?"Profile could not be read":diagnostic);
    }
    auto data=ProfileData(path,text,profile);data.Set("example",input.empty());
    data.Set("format","fbzz.pcg");data.Set("version",1);data.Set("boundsCoordinates","world half-open XZ; Y ignored");
    auto vegetation = OpData::MakeObject();
    vegetation.Set("minSpacing", "Minimum XZ separation within this rule in metres. Zero disables it.");
    vegetation.Set("patchGroup/minPatch/maxPatch", "Optional shared habitat mask and accepted [0,1] band. Use the same noiseScale for a shared mask.");
    vegetation.Set("neighborRule/minNeighborDistance/maxNeighborDistance", "Filter by an earlier rule's emitted placements in world XZ metres. Zero upper bound is unlimited.");
    vegetation.Set("waterFade", "Metres used to feather density at the shoreline distance limits. Zero disables it.");
    vegetation.Set("heightFade/slopeFade", "Feather density inside height bounds in world metres and slope bounds in degrees. Zero disables each fade.");
    vegetation.Set("aspectHeading/aspectHalfAngle/aspectMinSlope/aspectFade", "Downhill sector in degrees: +Z is 0, +X is 90. Half-angle 180 disables it; slopes below aspectMinSlope stay neutral.");
    vegetation.Set("canopySourceGroup/canopyGroup/canopyRadius/minCanopyCount/maxCanopyCount", "Count emitted roots from multiple rules inside a world XZ radius. Counts are inclusive; -1 is an unlimited maximum.");
    vegetation.Set("scaleCompetitionRadius", "Multiply the rule's competition radius by the chosen placement scale.");
    vegetation.Set("scaleBias", "Scale distribution exponent [0.1,8]. Above one favors small stones; one preserves uniform legacy sizes.");
    vegetation.Set("embedJitter", "Stable additional embedding [0,embedJitter] in world metres. Does not move the unembedded sampling site.");
    vegetation.Set("alignDownslope/downslopeJitter", "Align +Y to the surface and +Z downhill, with a seeded angular spread in degrees. Flat sites use yaw.");
    vegetation.Set("waterLevelFade", "Feather density inside both signed water-height limits in metres. Requires useWaterLevel.");
    vegetation.Set("useWaterLevel/minWaterOffset/maxWaterOffset", "Signed height in metres above the water level. Negative bands allow underwater plants.");
    vegetation.Set("minNearRelief/maxNearRelief/minFarRelief/maxFarRelief", "Signed height relative to neighboring terrain at 16 m and 64 m.");
    vegetation.Set("maxCliffDistance/cliffFarScale", "Taper rock density and size outward from an uphill cliff face.");
    vegetation.Set("pathMaterial/minPathDistance/maxPathDistance/pathFade", "Set pathMaterial on the profile and define each rule's band beyond its painted edge in world metres.");
    vegetation.Set("includeMaterial/minMaterialWeight", "Require this saved terrain material and multiply density by its weight. Default minimum 0.25.");
    vegetation.Set("excludeMaterial/excludeMaterialWeight", "Reject sites painted with this material. Default threshold 0.35.");
    vegetation.Set("competitionGroup/competitionRadius/competitionPriority", "Keep exclusion discs in this group apart. Radii use world metres; higher priority wins within a dependency stage.");
    data.Set("vegetationRules", std::move(vegetation));
    data.Set("rockPreset", "preset=rocks, rockSource and pebbleSource returns six editable rules without modifying files or the Scene. Tune scales for your source geometry; material masks distinguish river and sea beds.");
    return OpResult::Data(std::move(data));
}

OpResult SaveWorldPcgProfile(OpContext& context,const OpArgs& args)
{
    using namespace world_pcg_profile_draft;
    const auto* play=context.ctx.playMode;
    if(context.ctx.projectRoot.empty()||context.ctx.managedBuildBusy||(play&&(!play->IsInEditor()||play->HasPendingRestore())))
        return OpResult::Err("NOT_EDITABLE","PCG profile saving requires an open project in Edit mode without a pending restore or managed build");
    const auto path=world_pcg_authoring::ProfilePath(context.ctx,args.GetString("path"),true);
    if(path.empty())return OpResult::Err("BAD_PATH","Choose a .pcg path inside the project Assets directory");
    std::string diagnostic;scene::WorldPcgProfile profile;
    if(!scene::DeserializeWorldPcgProfile(args.GetString("text"),profile,diagnostic))return OpResult::Err("BAD_PROFILE",diagnostic);
    std::string canonical;if(!scene::SerializeWorldPcgProfile(profile,canonical,diagnostic))return OpResult::Err("BAD_PROFILE",diagnostic);
    std::error_code error;const bool exists=std::filesystem::exists(util::FileSystem::PathFromUtf8(path),error);
    if(error)return OpResult::Err("IO_FAILED","Cannot inspect the current profile file");
    std::string before;if(exists&&!ReadProfileBytes(path,before))return OpResult::Err("IO_FAILED","Cannot read the current profile file");
    if(args.Has("expected")&&args.GetString("expected")!=(exists?world_pcg_authoring::HashText(before):std::string{}))
        return OpResult::Err("CONFLICT","Profile changed since it was inspected");
    if(exists&&canonical==before){auto result=OpResult::Data(ProfileData(path,before,profile));result.noChange=true;return result;}
    if(exists&&!args.Has("expected"))return OpResult::Err("CONFLICT","Replacing a profile requires its inspected fingerprint");
    if(!scene::SaveWorldPcgProfile(path,profile,diagnostic))return OpResult::Err("IO_FAILED",diagnostic);
    if(asset::AssetDatabase::GuidFromPath(path).empty())return OpResult::Err("META_FAILED","Profile was saved but its metadata could not be registered: "+path);
    context.ctx.requestAssetBrowserRefresh=true;
    std::string saved;if(!ReadProfileBytes(path,saved))return OpResult::Err("IO_FAILED","Profile was saved but could not be read back: "+path);
    return OpResult::Data(ProfileData(path,saved,profile));
}

OpResult ConfigureWorldPcgVolume(OpContext& context,const OpArgs& args)
{
    if(!context.undo.IsRecordingEnabled())return OpResult::Err("NOT_AVAILABLE","PCG volume editing requires enabled Undo recording");
    using namespace world_pcg_profile_draft;
    auto& ctx=context.ctx;if(!world_pcg_authoring::Editable(ctx)||!ctx.editScene)return OpResult::Err("NOT_EDITABLE","PCG authoring requires the Edit Scene");
    const auto requested=args.GetString("volume");auto* node=requested.empty()?nullptr:ctx.editScene->FindByGuid(requested);
    if(!requested.empty()&&!node)return OpResult::Err("NOT_FOUND","PCG volume node does not exist");
    std::optional<scene::WorldPcgVolumeComponent> before;if(node)if(const auto* old=node->GetComponent<scene::WorldPcgVolumeComponent>())before=*old;
    scene::WorldPcgVolumeComponent after=before.value_or(scene::WorldPcgVolumeComponent{});
    const auto input=args.Has("profile")?args.GetString("profile"):after.profilePath;
    if(input.empty())return OpResult::Err("BAD_ARG","A profile is required when creating a PCG volume");
    const auto path=world_pcg_authoring::ProfilePath(ctx,input,false);
    if(path.empty())return OpResult::Err("BAD_PATH","Choose an existing .pcg profile in project Assets");
    scene::WorldPcgProfile profile;std::string diagnostic;if(!scene::LoadWorldPcgProfile(path,profile,diagnostic))return OpResult::Err("BAD_PROFILE",diagnostic);
    if(args.Has("boundsMin")!=args.Has("boundsMax"))return OpResult::Err("BAD_ARG","Provide both boundsMin and boundsMax");
    if(args.GetBool("useProfileBounds")&&args.Has("boundsMin"))return OpResult::Err("BAD_ARG","useProfileBounds cannot be combined with explicit bounds");
    if(args.Has("profile")||!before)after.profilePath=ExistingReference(path);
    if(args.Has("enabled"))after.enabled=args.GetBool("enabled");if(args.Has("protectManual"))after.protectManual=args.GetBool("protectManual");
    if(args.GetBool("useProfileBounds"))after.overrideBounds=false;
    if(args.Has("boundsMin")){after.overrideBounds=true;after.boundsMin=args.GetVec3("boundsMin");after.boundsMax=args.GetVec3("boundsMax");}
    if(after.overrideBounds)profile.bounds={after.boundsMin,after.boundsMax};
    if(args.Has("seed")){const int seed=args.GetInt("seed",-1);if(seed< -1)return OpResult::Err("BAD_ARG","Seed must be -1 or a nonnegative Int");after.overrideSeed=seed>=0;after.seed=seed>=0?seed:1;}
    if(after.overrideSeed){if(after.seed<0)return OpResult::Err("BAD_ARG","Seed override must be nonnegative");profile.seed=static_cast<uint64_t>(after.seed);}
    if(!scene::ValidateWorldPcgProfile(profile,diagnostic))return OpResult::Err("BAD_ARG",diagnostic);
    if(before&&Same(*before,after)){auto data=OpData::MakeObject();data.Set("node",node->instanceId);data.Set("volume",node->instanceId);data.Set("created",false);auto result=OpResult::Data(std::move(data));result.noChange=true;return result;}
    const auto id=node?node->instanceId:asset::AssetDatabase::GenerateGuid();
    return ApplyChecked<scene::WorldPcgVolumeComponent>(ctx,id,std::move(before),after,node==nullptr,"Configure World PCG Volume");
}

OpResult InspectWorldPcgVolume(OpContext& context, const OpArgs& args)
{
    auto* scene = context.ctx.activeScene;
    auto* node = scene ? scene->FindByGuid(args.GetString("volume")) : nullptr;
    const auto* volume = node ? node->GetComponent<scene::WorldPcgVolumeComponent>() : nullptr;
    if (!volume) return OpResult::Err("NO_VOLUME", "Choose a PCG volume");
    const auto path = world_pcg_authoring::ProfilePath(context.ctx, volume->profilePath, false);
    scene::WorldPcgProfile profile;
    std::string diagnostic;
    if (path.empty() || !scene::LoadWorldPcgProfile(path, profile, diagnostic))
        return OpResult::Err("BAD_PROFILE", diagnostic.empty() ? "The volume profile could not be loaded" : diagnostic);
    if (volume->overrideBounds) profile.bounds = {volume->boundsMin, volume->boundsMax};
    if (!scene::ValidateWorldPcgProfile(profile, diagnostic)) return OpResult::Err("BAD_PROFILE", diagnostic);
    auto data = OpData::MakeObject();
    data.Set("volume", node->instanceId); data.Set("profile", volume->profilePath);
    data.Set("profileReference", volume->profilePath);
    data.Set("enabled", volume->enabled);
    world_pcg_authoring::DescribePcgBounds(data, profile);
    return OpResult::Data(std::move(data));
}

OpResult DetachWorldPcg(OpContext& context,const OpArgs& args)
{
    if(!context.undo.IsRecordingEnabled())return OpResult::Err("NOT_AVAILABLE","PCG detaching requires enabled Undo recording");
    using namespace world_pcg_profile_draft;
    auto& ctx=context.ctx;if(!world_pcg_authoring::Editable(ctx)||!ctx.editScene)return OpResult::Err("NOT_EDITABLE","PCG authoring requires the Edit Scene");
    auto* node=ctx.editScene->FindByGuid(args.GetString("node"));if(!node)return OpResult::Err("NOT_FOUND","Generated PCG node does not exist");
    const auto* generated=node->GetComponent<scene::WorldPcgGeneratedComponent>();
    if(!generated){auto result=OpResult::Ok();result.noChange=true;return result;}
    return ApplyChecked<scene::WorldPcgGeneratedComponent>(ctx,node->instanceId,*generated,std::nullopt,false,"Detach World PCG Output");
}

} /// @note namespace fbzz::editor
