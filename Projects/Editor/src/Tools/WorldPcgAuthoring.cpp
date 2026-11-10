/// @file    WorldPcgAuthoring.cpp
/// @brief   Read-only PCG planning and checked cell-owned authoring transactions.
/// @author  Hasegawa Jin
/// @date    2026-11-07
#include "WorldAuthoring.hpp"
#include "TerrainPaintClearance.hpp"
#include <Editor/EditorContext.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/Selection.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MeshInstancesAsset.hpp>
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
#include <cmath>
#include <filesystem>
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

bool ReadWorld(scene::GameObject& object,scene::Transform& out)
{
    std::vector<scene::GameObject*> chain;
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

asset::MeshInstancesAsset* PlacementAsset(const std::string& reference)
{
    const auto path=asset::AssetManager::ResolveAssetPath(reference);
    if(util::FileSystem::GetExtension(path)!=".meshinstances")return nullptr;
    return asset::AssetManager::Get(asset::AssetManager::Load<asset::MeshInstancesAsset>(reference));
}

std::string OwnerSignature(scene::GameObject& owner,const scene::WorldPcgGeneratedComponent& metadata,
    const scene::InstancedMeshComponent& component,const asset::MeshInstancesAsset& asset)
{
    uint64_t hash=1469598103934665603ull;
    AddHash(hash,owner.instanceId);AddHash(hash,owner.name);AddHash(hash,owner.tag);AddHash(hash,static_cast<uint64_t>(owner.layer));
    AddHash(hash,owner.activeSelf()?1:0);AddHash(hash,owner.GetChildCount());AddHash(hash,owner.GetParent()?owner.GetParent()->instanceId:std::string{});
    scene::Transform world;if(!ReadWorld(owner,world))return {};
    AddVector(hash,world.worldPosition);AddVector(hash,world.worldScale);
    AddFloat(hash,world.worldRotation.x);AddFloat(hash,world.worldRotation.y);AddFloat(hash,world.worldRotation.z);AddFloat(hash,world.worldRotation.w);
    AddHash(hash,metadata.volumeId);AddHash(hash,metadata.ruleKey);AddHash(hash,static_cast<uint32_t>(metadata.cellX));AddHash(hash,static_cast<uint32_t>(metadata.cellZ));AddHash(hash,metadata.includeHlod?1:0);
    AddHash(hash,component.m_instanceSetRef.path);AddHash(hash,component.m_enabled?1:0);AddHash(hash,component.m_castShadows?1:0);AddFloat(hash,component.m_maxDistance);
    AddHash(hash,asset.terrainPaintRef);
    size_t extraComponents=0;scene::ForEachRegisteredComponent([&]<typename T,typename Registration>(){
        (void)sizeof(Registration);if constexpr(!std::is_same_v<T,scene::InstancedMeshComponent>&&!std::is_same_v<T,scene::WorldPcgGeneratedComponent>)
            if(owner.GetComponent<T>())++extraComponents;});AddHash(hash,extraComponents);
    for(const auto& part:asset.instances.Parts()){AddHash(hash,part.meshPath);AddHash(hash,part.materialPath);AddHash(hash,part.lod);}
    for(float height:asset.instances.LodHeights())AddFloat(hash,height);
    const auto snapshot=component.Snapshot(asset.instances);if(!snapshot)return {};
    for(const auto& p:snapshot->placements){AddHash(hash,p.id);AddHash(hash,p.active?1:0);AddVector(hash,p.position);AddVector(hash,p.eulerRadians);AddVector(hash,p.scale);}
    return Hex(hash);
}

bool InBounds(const scene::WorldCellBounds& bounds,const math::Vector3& point)
{ return point.x>=bounds.min.x&&point.x<bounds.max.x&&point.z>=bounds.min.z&&point.z<bounds.max.z; }

using GroupKey=std::pair<scene::WorldCellId,std::string>;
struct Owned {
    scene::GameObject* owner=nullptr;
    scene::WorldPcgGeneratedComponent metadata;
    scene::InstancedMeshComponent component;
    std::shared_ptr<asset::MeshInstancesAsset> asset;
    std::string signature;
};
struct PcgPlan {
    scene::GameObject* volume=nullptr;
    scene::WorldPcgProfile profile;
    scene::WorldPcgResult generated;
    std::string profilePath,profileText,expected;
    std::map<GroupKey,Owned> owned;
    OpData summary=OpData::MakeObject();
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
        const bool competes = hasNewPlacements && competitiveRules.contains(key.second);
        const bool usesCanopy = canopyRules.contains(key.second);
        if (!limit && !competes && !usesCanopy) continue;
        scene::Transform world;
        const auto snapshot = owned.component.Snapshot(owned.asset->instances);
        if (!snapshot || !ReadWorld(*owned.owner, world))
            return OpResult::Err("PCG_CONFLICT", "A retained generated owner cannot be sampled");
        const auto matrix = world.GetWorldMatrix();
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

OpResult PreparePcg(OpContext& context,const OpArgs& args,PcgPlan& plan)
{
    auto& ctx=context.ctx;if(!Editable(ctx))return OpResult::Err("NOT_AVAILABLE","PCG authoring requires the editable scene");
    auto& scene=*ctx.activeScene;plan.volume=scene.FindByGuid(args.GetString("volume"));
    const auto* volume=plan.volume?plan.volume->GetComponent<scene::WorldPcgVolumeComponent>():nullptr;
    if(!volume||!volume->enabled)return OpResult::Err("NO_VOLUME","An enabled PCG volume is required");
    plan.profilePath=ProfilePath(ctx,volume->profilePath,false);
    std::string diagnostic;
    if(plan.profilePath.empty()||!world_pcg_profile_draft::ReadProfileBytes(plan.profilePath,plan.profileText)
        ||!scene::DeserializeWorldPcgProfile(plan.profileText,plan.profile,diagnostic))return OpResult::Err("INVALID_PROFILE",diagnostic.empty()?"PCG profile cannot be read":diagnostic);
    for(auto& rule:plan.profile.rules)if(rule.parts.empty()){
        const auto* definition=asset::ResolveMeshInstanceDefinition(rule.prototypeRef);
        if(!definition)return OpResult::Err("NO_TEMPLATE","Cannot resolve PCG prototype: "+rule.prototypeRef);
        rule.parts.assign(definition->Parts().begin(),definition->Parts().end());rule.lodHeights.assign(definition->LodHeights().begin(),definition->LodHeights().end());}
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
    struct Ground {
        const scene::TerrainComponent* data;
        scene::Transform world;
        std::string id;
        std::map<std::string_view, std::vector<int>> materialLayers;
        math::Matrix4 worldToLocal = math::Matrix4::Identity();
    };
    struct Water { const scene::WaterComponent* data;scene::Transform world; };
    struct Biome { const scene::WorldBiomeVolumeComponent* data;std::string id; };
    std::vector<Ground> grounds;std::vector<Water> waters;std::vector<Biome> biomes;
    uint64_t guard=1469598103934665603ull;AddHash(guard,scene.GetRenderSceneGeneration());AddHash(guard,context.undo.GetRevision());AddHash(guard,AssetDirtyRegistry::DiscardEpoch());
    AddHash(guard,HashText(plan.profileText));AddHash(guard,plan.volume->instanceId);AddHash(guard,volume->profilePath);AddHash(guard,volume->protectManual?1:0);
    AddHash(guard,partitionOwner?partitionOwner->instanceId:std::string{});
    AddHash(guard,volume->overrideBounds?1:0);AddVector(guard,volume->boundsMin);AddVector(guard,volume->boundsMax);AddHash(guard,volume->overrideSeed?1:0);AddHash(guard,static_cast<uint64_t>(volume->seed));
    for(auto id:scene.GetEntities<scene::TerrainComponent>()){
        auto* owner=scene.GetGameObject(id);const auto* data=scene.GetComponent<scene::TerrainComponent>(id);scene::Transform world;
        if(!owner||!owner->activeInHierarchy()||!data||!data->enabled)continue;if(!ReadWorld(*owner,world))return OpResult::Err("INVALID_TRANSFORM","Terrain transform is invalid");
        grounds.push_back({data,world,owner->instanceId});AddHash(guard,owner->instanceId);AddHash(guard,data->surfaceRevision);AddHash(guard,data->vegetationRevision);
        AddVector(guard,world.worldPosition);AddVector(guard,world.worldScale);AddFloat(guard,world.worldRotation.x);AddFloat(guard,world.worldRotation.y);AddFloat(guard,world.worldRotation.z);AddFloat(guard,world.worldRotation.w);
    }
    if(grounds.empty())return OpResult::Err("NO_TERRAIN","PCG requires authored terrain samples");
    const auto materialIdentity = [](const std::string& reference) {
        auto guid = asset::AssetDatabase::GuidFromRef(reference);
        if (guid.empty()) guid = asset::AssetDatabase::TryGetGuidFromPath(asset::AssetManager::ResolveAssetPath(reference));
        return guid.empty() ? NormalizeAssetPath(reference) : "guid:" + guid;
    };
    std::map<std::string, std::set<std::string_view>> requestedMaterials;
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
    for(auto id:scene.GetEntities<scene::WaterComponent>()){
        auto* owner=scene.GetGameObject(id);const auto* data=scene.GetComponent<scene::WaterComponent>(id);scene::Transform world;
        if(owner&&owner->activeInHierarchy()&&data&&data->enabled&&ReadWorld(*owner,world))waters.push_back({data,world});}
    for(auto id:scene.GetEntities<scene::WorldBiomeVolumeComponent>()){
        const auto* owner=scene.GetGameObject(id);const auto* data=scene.GetComponent<scene::WorldBiomeVolumeComponent>(id);
        if(owner&&owner->activeInHierarchy()&&data&&data->enabled){
            if(!Finite(data->boundsMin)||!Finite(data->boundsMax)||data->boundsMin.x>=data->boundsMax.x||data->boundsMin.y>data->boundsMax.y||data->boundsMin.z>=data->boundsMax.z
                ||!std::isfinite(data->density)||data->density<0||data->density>1)return OpResult::Err("BAD_BIOME","Biome volume bounds or density are invalid");
            biomes.push_back({data,owner->instanceId});}}
    std::sort(biomes.begin(),biomes.end(),[](const auto& a,const auto& b){return a.data->priority==b.data->priority?a.id<b.id:a.data->priority>b.data->priority;});
    std::vector<math::Vector3> protectedPositions;
    for(auto id:scene.GetEntities<scene::InstancedMeshComponent>()){
        auto* owner=scene.GetGameObject(id);const auto* component=scene.GetComponent<scene::InstancedMeshComponent>(id);
        if(!owner||owner->IsRuntimeGenerated()||!component)continue;const auto* metadata=owner->GetComponent<scene::WorldPcgGeneratedComponent>();
        if(metadata&&metadata->volumeId==plan.volume->instanceId){
            auto* source=PlacementAsset(component->m_instanceSetRef.path);if(!source)return OpResult::Err("MISSING_OUTPUT","Generated placement asset is missing; detach this owner before replacing it");
            Owned owned;owned.owner=owner;owned.metadata=*metadata;owned.component=*component;owned.asset=std::make_shared<asset::MeshInstancesAsset>(*source);
            owned.signature=OwnerSignature(*owner,*metadata,*component,*source);AddHash(guard,owner->instanceId);AddHash(guard,owned.signature);AddHash(guard,metadata->signature);AddHash(guard,metadata->manualOverride?1:0);
            const auto& budget = component->m_vegetationBudget;
            AddHash(guard, budget.enabled ? 1 : 0);
            AddHash(guard, static_cast<uint64_t>(budget.drawDetailPerCell));
            AddHash(guard, static_cast<uint64_t>(budget.nearDetailPerCell));
            AddHash(guard, static_cast<uint64_t>(budget.shadowDetailPerCell));
            if(!plan.owned.emplace(GroupKey{{metadata->cellX,metadata->cellZ},metadata->ruleKey},std::move(owned)).second)return OpResult::Err("DUPLICATE_OUTPUT","PCG ownership keys are duplicated");
            if(!metadata->manualOverride)continue;
        }
        if(!volume->protectManual||!owner->activeInHierarchy()||!component->m_enabled)continue;
        const auto snapshot=component->Snapshot();scene::Transform world;if(!snapshot||!ReadWorld(*owner,world))return OpResult::Err("INVALID_PLACEMENT","Manual placement cannot be sampled");
        const auto matrix=world.GetWorldMatrix();for(const auto& p:snapshot->placements)if(p.active){const auto v=matrix*math::Vector4{p.position,1};protectedPositions.push_back({v.x,v.y,v.z});}
    }
    if(volume->protectManual)for(auto id:scene.GetEntities<scene::MeshRenderer>()){
        auto* owner=scene.GetGameObject(id);if(!owner||owner->IsRuntimeGenerated()||!owner->activeInHierarchy()||owner->GetComponent<scene::WorldPcgGeneratedComponent>())continue;
        scene::Transform world;if(ReadWorld(*owner,world))protectedPositions.push_back(world.worldPosition);}
    for(const auto& point:protectedPositions)AddVector(guard,point);
    float waterMargin = 0.0f, pathMargin = 0.0f;
    bool useWaterLevel = false, usePathBand = false;
    for (const auto& rule : plan.profile.rules) {
        const float distance = rule.maxWaterDistance < 1000000.0f ? rule.maxWaterDistance
            : rule.minWaterDistance + rule.waterFade;
        waterMargin = (std::max)(waterMargin, distance);
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
    bool hasPathLayer = false;
    if ((waterSearch > 0 && !waters.empty()) || usePathBand) {
        std::vector<std::pair<const scene::WaterComponent*,const scene::Transform*>> views;for(const auto& water:waters)views.push_back({water.data,&water.world});
        const auto& b=plan.profile.bounds;const std::vector<math::Vector3> corners{{b.min.x,0,b.min.z},{b.max.x,0,b.min.z},{b.max.x,0,b.max.z},{b.min.x,0,b.max.z}};
        const TerrainScatterBrush region{corners,0.25f,false,true,0};
        for (const auto& ground : grounds) {
            const auto found = ground.materialLayers.find(plan.profile.pathMaterial);
            const auto layers = found == ground.materialLayers.end() ? std::span<const int>{} : std::span<const int>{found->second};
            hasPathLayer |= usePathBand && !layers.empty();
            clearances.push_back(BuildTerrainPaintClearance(*ground.data, ground.world, region, layers, views,
                waters.empty() ? 0.0f : waterSearch, !usePathBand || layers.empty() ? 0.0f : (std::max)(0.25f, pathMargin), true));
        }
    }
    std::vector<scene::WorldPcgMaterialWeight> materialWeights;
    const auto sampleGround = [&](float x, float z, scene::TerrainSurfaceSample& highest) {
        const Ground* highestGround = nullptr;
        for (const auto& ground : grounds) {
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
    constexpr float DIAGONAL = 0.70710678f;
    constexpr float CLIFF_NORMAL_Y = 0.64278761f;
    const std::array<math::Vector2, 8> directions{{{1,0},{DIAGONAL,DIAGONAL},{0,1},{-DIAGONAL,DIAGONAL},
        {-1,0},{-DIAGONAL,-DIAGONAL},{0,-1},{DIAGONAL,-DIAGONAL}}};
    std::size_t remainingProbes = 4'000'000;
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
        for(const auto& biome:biomes){const auto& b=*biome.data;if(x>=b.boundsMin.x&&x<b.boundsMax.x&&z>=b.boundsMin.z&&z<b.boundsMax.z&&highest.position.y>=b.boundsMin.y&&highest.position.y<=b.boundsMax.y){out.biome=b.biome;out.biomeDensity=b.density;break;}}
        for (const auto& water : waters) {
            scene::WaterSurfaceHeightSample sample;
            if (scene::TrySampleWaterSurfaceHeightWorld(*water.data, water.world, highest.position, 0, sample)
                && sample.insideSurface && !sample.excluded && highest.position.y <= sample.position.y + 0.15f) {
                out.water = true;
                out.waterDistance = 0.0f;
                break;
            }
        }
        if(!out.water&&waterMargin>0){float nearest=1000000;
            for(const auto& clearance:clearances)if(clearance.NearWater(highest.position,waterMargin)){float low=0,high=waterMargin;
                for(int step=0;step<16;++step){const float middle=(low+high)*0.5f;if(clearance.NearWater(highest.position,middle))high=middle;else low=middle;}nearest=std::min(nearest,high);}
            out.waterDistance=nearest;}
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
        for (const auto& water : waters) {
            scene::WaterSurfaceHeightSample sample;
            if (!scene::TrySampleWaterSurfaceHeightWorld(*water.data, water.world, out.position, 0, sample)
                || !sample.insideSurface || sample.excluded) continue;
            const float offset = (out.position.y - water.world.worldPosition.y) - sample.baseHeightOffset;
            if (offset >= -sample.waterDepthLimit
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
    const bool generated = scene::GenerateWorldPcg(plan.profile, sampler, protectedPositions,
        plan.generated, diagnostic, sampleHabitat);
    if (habitatBudgetExceeded) return OpResult::Err("HABITAT_SAMPLE_BUDGET", "Narrow the region or increase placement spacing.");
    if (!generated) return OpResult::Err("PCG_FAILED",diagnostic);
    if (auto result = ValidateRetainedPlacements(plan); !result.ok) return result;
    AddHash(guard,plan.generated.fingerprint);plan.expected=Hex(guard);
    auto& summary=plan.summary;summary.Set("schemaVersion",1);summary.Set("volume",plan.volume->instanceId);summary.Set("profile",plan.profilePath);summary.Set("expected",plan.expected);
    summary.Set("partition",partitionOwner?partitionOwner->instanceId:std::string{});
    summary.Set("candidateCount",static_cast<int64_t>(plan.generated.stats.candidates));summary.Set("instanceCount",static_cast<int64_t>(plan.generated.stats.accepted));
    summary.Set("groupCount",static_cast<int64_t>(plan.generated.groups.size()));summary.Set("fingerprint",plan.generated.fingerprint);
    summary.Set("maxPlacementsPerCell", static_cast<int64_t>(plan.profile.maxPlacementsPerCell));
    summary.Set("placementBudgetCellSize", scene::VEGETATION_BUDGET_CELL_SIZE);
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
    auto points=OpData::MakeArray();std::set<scene::WorldCellId> cells;
    const int maxPoints=args.GetInt("maxPoints",256);if(maxPoints<0||maxPoints>1024)return OpResult::Err("BAD_ARG","maxPoints must be 0..1024");
    size_t emitted=0;for(const auto& group:plan.generated.groups){cells.insert(group.cell);for(const auto& p:group.placements){if(emitted>=static_cast<size_t>(maxPoints))break;
        auto point=OpData::MakeObject();point.Set("position",p.position+scene::WorldCellOrigin(plan.profile.grid,group.cell));point.Set("rule",group.ruleKey);point.Set("id",static_cast<int64_t>(p.id));points.Push(std::move(point));++emitted;}}
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
    std::string beforeSignature,afterSignature;
};

void PublishDraft(const PcgChange& change,const std::shared_ptr<asset::MeshInstancesAsset>& value)
{
    if(auto* current=PlacementAsset(change.path))*current=*value;
    AssetDirtyRegistry::RegisterDraft(change.path,change.reference,"PCG MESH INSTANCES",value,
        [path=change.path](const asset::MeshInstancesAsset& draft){const auto* current=PlacementAsset(path);return current&&asset::MeshInstancesAssetsEqual(*current,draft)&&asset::SaveMeshInstancesAsset(path,draft);},
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
    bool ValidateHistory(std::string& diagnostic) const override
    {
        if(!Editable(m_context)||m_context.activeScene!=m_scene||m_scene->GetRenderSceneGeneration()!=m_generation||AssetDirtyRegistry::DiscardEpoch()!=m_epoch){diagnostic="PCG scene or draft lifetime changed";return false;}
        for(const auto& change:m_changes){
            auto* owner=m_scene->FindByGuid(change.ownerId);
            if(!m_applied&&change.created){if(owner){diagnostic="PCG owner ID has been claimed";return false;}}
            else {
                const auto* component=owner?owner->GetComponent<scene::InstancedMeshComponent>():nullptr;
                const auto* metadata=owner?owner->GetComponent<scene::WorldPcgGeneratedComponent>():nullptr;
                const auto& expectedMetadata=m_applied?change.afterMetadata:*change.beforeMetadata;
                const auto& expectedComponent = m_applied ? change.after : *change.before;
                const auto* asset=component?PlacementAsset(component->m_instanceSetRef.path):nullptr;
                if(!owner||owner->GetID()!=change.identity->entity||!component||!metadata||!asset||m_scene->GetComponentLifetime<scene::WorldPcgGeneratedComponent>(owner->GetID())!=*change.metadataLifetime
                    ||m_scene->GetComponentLifetime<scene::InstancedMeshComponent>(owner->GetID())!=*change.instanceLifetime||!SameGenerated(*metadata,expectedMetadata)
                    ||component->m_vegetationBudget != expectedComponent.m_vegetationBudget
                    ||OwnerSignature(*owner,*metadata,*component,*asset)!=(m_applied?change.afterSignature:change.beforeSignature)){
                    diagnostic="PCG generated owner was edited; detach it before regeneration";return false;}
                if(change.created&&(owner->GetChildCount()!=0||owner->GetParent()!=nullptr)){diagnostic="PCG owner acquired manual hierarchy changes";return false;}
            }
            const auto* current=PlacementAsset(change.path);const auto& expected=m_applied?change.afterAsset:(change.reuse?change.beforeAsset:change.afterAsset);
            asset::MeshInstancesAsset disk;
            if(!current||!expected||!asset::MeshInstancesAssetsEqual(*current,*expected)||!asset::LoadMeshInstancesAsset(change.path,disk)
                ||(!asset::MeshInstancesAssetsEqual(disk,*change.afterAsset)&&(!change.beforeAsset||!asset::MeshInstancesAssetsEqual(disk,*change.beforeAsset)))){
                diagnostic="PCG placement asset was replaced or edited";return false;}
            if(change.reuse)for(auto id:m_scene->GetEntities<scene::InstancedMeshComponent>()){
                const auto* sibling=m_scene->GetGameObject(id);const auto* component=m_scene->GetComponent<scene::InstancedMeshComponent>(id);
                if(sibling&&sibling->instanceId!=change.ownerId&&component&&util::FileSystem::SamePathText(asset::AssetManager::ResolveAssetPath(component->m_instanceSetRef.path),change.path)){
                    diagnostic="PCG placement asset became shared; history cannot overwrite another owner";return false;}}
        }
        diagnostic.clear();return true;
    }
private:
    bool Apply(bool after,std::string& diagnostic)
    {
        if(after==m_applied||!ValidateHistory(diagnostic))return false;
        size_t creates=0;for(const auto& change:m_changes)if(after&&change.created)++creates;
        if(!m_scene->CanCreateGameObjects(creates)){diagnostic="PCG scene object budget exceeded";return false;}
        for(const auto& change:m_changes){if(after)PublishDraft(change,change.afterAsset);else if(change.reuse)PublishDraft(change,change.beforeAsset);else AssetDirtyRegistry::MarkClean(change.path);}
        for(auto& change:m_changes){
            auto* owner=m_scene->FindByGuid(change.ownerId);
            if(!after&&change.created){m_scene->DestroyGameObject(owner->GetID());change.identity->entity={};*change.metadataLifetime=0;*change.instanceLifetime=0;continue;}
            if(after&&!owner){owner=&m_scene->CreateGameObject(change.name);owner->instanceId=change.ownerId;change.entity=owner->GetID();change.identity->entity=change.entity;}
            const auto& component=after?change.after:*change.before;
            if(auto* current=owner->GetComponent<scene::InstancedMeshComponent>())*current=component;else owner->AddComponent<scene::InstancedMeshComponent>(component);
            const auto& metadata=after?change.afterMetadata:*change.beforeMetadata;
            if(auto* current=owner->GetComponent<scene::WorldPcgGeneratedComponent>())*current=metadata;else owner->AddComponent<scene::WorldPcgGeneratedComponent>(metadata);
            if(after){change.afterSignature=OwnerSignature(*owner,change.afterMetadata,change.after,*change.afterAsset);owner->GetComponent<scene::WorldPcgGeneratedComponent>()->signature=change.afterSignature;change.afterMetadata.signature=change.afterSignature;}
            world_pcg_profile_draft::SyncGeneratedToken(m_generation,*owner,change.metadataLifetime,change.instanceLifetime);
        }
        m_applied=after;PruneSelection(m_context);m_context.sceneDirty=true;if(m_context.markSceneDirty)m_context.markSceneDirty();m_context.requestAssetBrowserRefresh=true;diagnostic.clear();return true;
    }
    EditorContext& m_context;scene::Scene* m_scene;uint64_t m_generation,m_epoch;std::vector<PcgChange> m_changes;bool m_applied=false;
};

OpResult BakePcg(OpContext& context,const OpArgs& args)
{
    if(!context.undo.IsRecordingEnabled())return OpResult::Err("NOT_AVAILABLE","PCG generation requires enabled Undo recording");
    PcgPlan plan;if(auto result=PreparePcg(context,args,plan);!result.ok)return result;
    if(args.GetString("expected").empty()||args.GetString("expected")!=plan.expected)return OpResult::Err("PCG_CONFLICT","PCG inputs changed; preview again before generation");
    auto& ctx=context.ctx;auto& scene=*ctx.activeScene;
    std::map<GroupKey,const scene::WorldPcgCellGroup*> desired;for(const auto& group:plan.generated.groups)desired.emplace(GroupKey{group.cell,group.ruleKey},&group);
    std::set<GroupKey> touched;for(const auto& [key,group]:desired){(void)group;touched.insert(key);}
    for(const auto& [key,owned]:plan.owned){scene::Transform world;if(!ReadWorld(*owned.owner,world))return OpResult::Err("PCG_CONFLICT","Generated transform is invalid");
        const auto snapshot=owned.component.Snapshot(owned.asset->instances);if(!snapshot)return OpResult::Err("PCG_CONFLICT","Generated snapshot is missing");
        for(const auto& p:snapshot->placements)if(p.active){const auto v=world.GetWorldMatrix()*math::Vector4{p.position,1};if(InBounds(plan.profile.bounds,{v.x,v.y,v.z})){touched.insert(key);break;}}
    }
    std::vector<PcgChange> changes;size_t keptOutside=0;std::unordered_set<std::string> plannedOwners;
    for(const auto& key:touched){
        const auto old=plan.owned.find(key);const auto wanted=desired.find(key);const Owned* owned=old==plan.owned.end()?nullptr:&old->second;
        if(owned&&(owned->metadata.manualOverride||owned->signature.empty()||owned->signature!=owned->metadata.signature))return OpResult::Err("PCG_MANUAL_OVERRIDE","A generated owner was edited; detach it before regeneration: "+owned->owner->instanceId);
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
        change.name="PCG "+key.second+" ["+scene::WorldCellName(key.first)+"]";
        change.identity=world_pcg_profile_draft::NodeToken(scene.GetRenderSceneGeneration(),change.ownerId,owned?owned->owner->GetID():scene::EntityID{});
        change.metadataLifetime=world_pcg_profile_draft::LifetimeToken<scene::WorldPcgGeneratedComponent>(owned?scene.GetComponentLifetime<scene::WorldPcgGeneratedComponent>(owned->owner->GetID()):0);
        change.instanceLifetime=world_pcg_profile_draft::LifetimeToken<scene::InstancedMeshComponent>(owned?scene.GetComponentLifetime<scene::InstancedMeshComponent>(owned->owner->GetID()):0);
        std::vector<renderer::InstancePlacement> placements;std::vector<asset::InstanceMeshPart> parts;std::vector<float> lods;
        if(owned){change.entity=owned->owner->GetID();change.before=owned->component;change.beforeMetadata=owned->metadata;change.beforeSignature=owned->signature;
            const auto snapshot=owned->component.Snapshot(owned->asset->instances);scene::Transform world;(void)ReadWorld(*owned->owner,world);
            if(world.worldPosition!=math::Vector3::ZERO||world.worldScale!=math::Vector3(1,1,1)||world.worldRotation!=math::Quaternion::Identity())return OpResult::Err("PCG_CONFLICT","Generated owner requires its original identity transform; detach it before editing");
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
                const bool same=owned->metadata.includeHlod==group.includeHlod&&oldParts.size()==group.parts.size()&&oldLods.size()==group.lodHeights.size()
                    &&std::equal(oldLods.begin(),oldLods.end(),group.lodHeights.begin())
                    &&std::equal(oldParts.begin(),oldParts.end(),group.parts.begin(),[](const auto& a,const auto& b){return a.meshPath==b.meshPath&&a.materialPath==b.materialPath&&a.lod==b.lod;});
                if(!same)return OpResult::Err("PCG_PARTIAL_DEFINITION_CHANGE","Changing a rule definition requires regenerating all its owned rows or using a new rule key");}
            parts=group.parts;lods=group.lodHeights;const auto origin=scene::WorldCellOrigin(plan.profile.grid,group.cell);
            for(auto p:group.placements){p.position=p.position+origin;placements.push_back(p);}
        }
        auto after=std::make_shared<asset::MeshInstancesAsset>();after->terrainPaintRef=plan.volume->GetComponent<scene::WorldPcgVolumeComponent>()->profilePath;
        if(!after->instances.Replace(parts,lods,placements))return OpResult::Err("PCG_OUTPUT_INVALID","PCG output contains conflicting stable IDs or invalid definitions");
        const bool includeHlod=wanted!=desired.end()?wanted->second->includeHlod:owned->metadata.includeHlod;
        if(owned&&asset::MeshInstancesAssetsEqual(*after,*owned->asset)&&owned->metadata.includeHlod==includeHlod)continue;
        change.afterAsset=after;change.afterMetadata.volumeId=plan.volume->instanceId;change.afterMetadata.ruleKey=key.second;change.afterMetadata.cellX=key.first.x;change.afterMetadata.cellZ=key.first.z;change.afterMetadata.includeHlod=includeHlod;
        if(owned){
            change.path=asset::AssetManager::ResolveAssetPath(owned->component.m_instanceSetRef.path);
            change.reuse=util::FileSystem::IsChildPathText(change.path,ctx.projectRoot+"/Assets");
            for(auto id:scene.GetEntities<scene::InstancedMeshComponent>()){const auto* other=scene.GetGameObject(id);const auto* component=scene.GetComponent<scene::InstancedMeshComponent>(id);
                if(other&&other->instanceId!=change.ownerId&&component&&util::FileSystem::SamePathText(asset::AssetManager::ResolveAssetPath(component->m_instanceSetRef.path),change.path))change.reuse=false;}
            if(change.reuse){change.beforeAsset=owned->asset;change.reference=owned->component.m_instanceSetRef.path;}
            change.after=owned->component;
        }
        if(!change.reuse){const auto file=asset::AssetDatabase::GenerateGuid();const auto folder=asset::AssetDatabase::DeriveGuid(plan.volume->instanceId,"PCG Assets");change.path=ctx.projectRoot+"/Assets/World/PCG/"+folder+"/"+file+".meshinstances";
            if(util::FileSystem::Exists(util::FileSystem::PathFromUtf8(change.path)))return OpResult::Err("PCG_FILE_CONFLICT","PCG output path already exists without exclusive ownership: "+change.path);}
        changes.push_back(std::move(change));
    }
    if(changes.empty()){auto result=OpResult::Data(plan.summary);result.noChange=true;return result;}
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
        }
        change.after.m_instanceSetRef={change.reference,asset::InstanceSetAsset::TYPE_NAME};change.after.ResetOverrides();
    }
    auto command=std::make_unique<PcgCommand>(ctx,std::move(changes));std::string diagnostic;
    if(!command->TryExecute(diagnostic))return OpResult::Err("PCG_CONFLICT",diagnostic);
    auto result=OpResult::Data(plan.summary);result.data.Set("applied",true);result.data.Set("preservedOutsideCount",static_cast<int64_t>(keptOutside));result.command=std::move(command);return result;
}

} /// @note namespace world_pcg_authoring

OpResult PreviewWorldPcg(OpContext& context,const OpArgs& args)
{world_pcg_authoring::PcgPlan plan;if(auto result=world_pcg_authoring::PreparePcg(context,args,plan);!result.ok)return result;return OpResult::Data(std::move(plan.summary));}
OpResult GenerateWorldPcg(OpContext& context,const OpArgs& args) {return world_pcg_authoring::BakePcg(context,args);}

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

std::shared_ptr<NodeIdentity> NodeToken(uint64_t generation,const std::string& id,scene::EntityID entity)
{
    static std::map<std::pair<uint64_t,std::string>,std::weak_ptr<NodeIdentity>> entries;
    std::erase_if(entries,[](const auto& item){return item.second.expired();});auto& weak=entries[{generation,id}];
    if(auto current=weak.lock())return current;
    auto token=std::make_shared<NodeIdentity>();token->entity=entity;weak=token;return token;
}
template<typename T> auto& LifetimeTokens()
{static std::unordered_map<uint64_t,std::weak_ptr<uint64_t>> tokens;return tokens;}
template<typename T> std::shared_ptr<uint64_t> LifetimeToken(uint64_t lifetime)
{
    auto& tokens=LifetimeTokens<T>();std::erase_if(tokens,[](const auto& item){return item.second.expired();});
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
    if(input.empty()){
        scene::WorldPcgRule rule;rule.key="trees";rule.name="Trees";rule.parts={{"primitive:cube","Assets/Materials/Default.mat",0}};rule.lodHeights={0};profile.rules.push_back(std::move(rule));
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
    vegetation.Set("useWaterLevel/minWaterOffset/maxWaterOffset", "Signed height in metres above the water level. Negative bands allow underwater plants.");
    vegetation.Set("minNearRelief/maxNearRelief/minFarRelief/maxFarRelief", "Signed height relative to neighboring terrain at 16 m and 64 m.");
    vegetation.Set("maxCliffDistance/cliffFarScale", "Taper rock density and size outward from an uphill cliff face.");
    vegetation.Set("pathMaterial/minPathDistance/maxPathDistance/pathFade", "Set pathMaterial on the profile and define each rule's band beyond its painted edge in world metres.");
    vegetation.Set("includeMaterial/minMaterialWeight", "Require this saved terrain material and multiply density by its weight. Default minimum 0.25.");
    vegetation.Set("excludeMaterial/excludeMaterialWeight", "Reject sites painted with this material. Default threshold 0.35.");
    vegetation.Set("competitionGroup/competitionRadius/competitionPriority", "Keep exclusion discs in this group apart. Radii use world metres; higher priority wins within a dependency stage.");
    data.Set("vegetationRules", std::move(vegetation));
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
