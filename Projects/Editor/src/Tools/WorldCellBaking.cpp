/// @file    WorldCellBaking.cpp
/// @brief   Content-addressed near-cell and instancing-HLOD publication without rewriting the authored scene.
/// @author  Hasegawa Jin
/// @date    2026-11-07
#include "WorldAuthoring.hpp"
#include "TerrainInstanceEditing.hpp"
#include <Editor/EditorContext.hpp>
#include <Editor/PlayModeController.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MeshInstancesAsset.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/ComponentRegistry.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/SceneForest.hpp>
#include <Engine/Scene/TerrainAssetSerializer.hpp>
#include <Engine/Scene/MeshResolver.hpp>
#include <Engine/Scene/WorldHlod.hpp>
#include <Engine/Scene/WorldPartitionManifest.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Scene/Components/WorldPartitionComponent.hpp>
#include <Engine/Scene/Components/WorldPcgAuthoringComponent.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/FiberComponent.hpp>
#include <Engine/Scene/Components/InstancedMeshComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Graphics/Renderer/Mesh.hpp>
#include <toml++/toml.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>

namespace fbzz::editor {
namespace {
struct Source {
    std::string id, name;
    math::Matrix4 world;
    std::vector<asset::InstanceMeshPart> parts;
    std::vector<float> heights;
    std::vector<renderer::InstancePlacement> placements;
    scene::WorldCellBounds bounds;
    uint32_t layer = 0;
    bool shadows = true, instanced = true, hlod = true;
    float maxDistance = 0;
    toml::table actor;
    const scene::TerrainComponent* terrain = nullptr;
};

struct NearGroup {
    size_t source = 0;
    scene::WorldCellId cell;
    std::vector<renderer::InstancePlacement> placements;
};

struct CellDraft {
    toml::array objects;
    toml::array farTerrain;
    scene::WorldCellBounds bounds;
    bool hasBounds = false;
    size_t terrainBytes = 0;
    std::string identityText;
    std::vector<scene::WorldPartitionDependency> dependencies;
};

bool Finite(const math::Vector3& p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }
uint64_t Hash(std::string_view text)
{
    uint64_t h = 14695981039346656037ull;
    for (const unsigned char c : text) { h ^= c; h *= 1099511628211ull; }
    return h ? h : 1;
}
std::string Dump(const toml::table& table) { std::ostringstream s; s << table; return s.str(); }

bool ResolvePath(const EditorContext& ctx, const std::string& reference, std::filesystem::path& absolute, std::string& relative)
{
    if (reference.empty() || reference.find('\0') != std::string::npos) return false;
    std::string resolved = reference.starts_with("guid:") ? asset::AssetManager::ResolveAssetPath(reference) : reference;
    auto p = util::FileSystem::PathFromUtf8(resolved);
    if (!p.is_absolute()) p = util::FileSystem::PathFromUtf8(ctx.projectRoot) / p;
    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(util::FileSystem::PathFromUtf8(ctx.projectRoot), error);
    if (error) return false;
    absolute = std::filesystem::weakly_canonical(p, error);
    if (error || !util::FileSystem::IsChildPathText(util::FileSystem::PathToUtf8(absolute), util::FileSystem::PathToUtf8(root / "Assets"))) return false;
    relative = util::FileSystem::PathToUtf8(absolute.lexically_relative(root));
    std::replace(relative.begin(), relative.end(), '\\', '/');
    return relative.starts_with("Assets/");
}

bool MakeDirectory(const std::filesystem::path& path, std::string& error)
{
    std::error_code ec; std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) { error = "Cannot create world asset directory: " + ec.message(); return false; }
    return true;
}

bool EnsureMeta(const std::filesystem::path& path, const std::string& owner, const std::string& relative, std::string& reference, std::string& error)
{
    const auto utf8 = util::FileSystem::PathToUtf8(path);
    std::string guid;
    {
        std::string existing;
        if (util::FileSystem::ReadText(utf8 + ".meta", existing)) {
            const auto parsed = toml::parse(existing);
            if (!parsed || !(parsed["meta"]["guid"].value<std::string>())) { error = "Existing asset metadata is malformed"; return false; }
            guid = *parsed["meta"]["guid"].value<std::string>();
            if(guid.size()!=32||std::any_of(guid.begin(),guid.end(),[](unsigned char c){return !((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'));})){error="Existing asset GUID is malformed";return false;}
            const auto indexed=asset::AssetDatabase::TryGetGuidFromPath(utf8);
            if(!indexed.empty()&&indexed!=guid){error="Asset metadata changed outside the current project index";return false;}
        } else {
            std::error_code ec;if(std::filesystem::exists(util::FileSystem::PathFromUtf8(utf8+".meta"),ec)||ec){error="Cannot read existing asset metadata";return false;}
            guid = asset::AssetDatabase::DeriveGuid(owner, relative);
            const std::string other = asset::AssetDatabase::PathFromGuid(guid);
            if (!other.empty() && asset::AssetManager::CanonicalKey(other) != asset::AssetManager::CanonicalKey(utf8)) { error = "Derived asset GUID is already assigned to another file"; return false; }
            if (!util::FileSystem::WriteTextAtomic(utf8 + ".meta", "# @file    " + util::FileSystem::PathToUtf8(path.filename()) + ".meta\n# @brief   Stable world-cell asset identity.\n# @author  Hasegawa Jin\n# @date    2026-11-07\n[meta]\nguid = '" + guid + "'\n")) { error = "Cannot write world asset metadata"; return false; }
        }
    }
    const std::string indexed = asset::AssetDatabase::GuidFromPath(utf8);
    if (!indexed.empty()) guid = indexed;
    reference = "guid:" + guid + "|" + relative;
    return !guid.empty();
}

bool WriteText(const EditorContext& ctx, const std::string& relative, const std::string& content, const std::string& owner, std::string& reference, std::string& error)
{
    std::filesystem::path path; std::string canonical;
    if (!ResolvePath(ctx, relative, path, canonical) || !MakeDirectory(path, error)) { if (error.empty()) error = "World output must remain inside project Assets"; return false; }
    std::string existing;
    if (util::FileSystem::ReadText(path, existing)) {
        if (existing != content) { error = "Immutable cell content address is occupied by different data"; return false; }
    } else if (!util::FileSystem::WriteTextAtomic(util::FileSystem::PathToUtf8(path), content)) { error = "Cannot write cell payload"; return false; }
    return EnsureMeta(path, owner, canonical, reference, error);
}

bool WriteInstances(const EditorContext& ctx, const std::string& relative, const asset::MeshInstancesAsset& data, const std::string& owner, std::string& reference, std::string& error)
{
    std::filesystem::path path; std::string canonical;
    if (!ResolvePath(ctx, relative, path, canonical) || !MakeDirectory(path, error)) { if (error.empty()) error = "Instance output must remain inside project Assets"; return false; }
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        asset::MeshInstancesAsset existing;
        if (!asset::LoadMeshInstancesAsset(util::FileSystem::PathToUtf8(path), existing) || !asset::MeshInstancesAssetsEqual(existing, data)) { error = "Immutable instance asset content address conflict"; return false; }
    } else if (!asset::SaveMeshInstancesAsset(util::FileSystem::PathToUtf8(path), data)) { error = "Cannot save cell instance asset"; return false; }
    return EnsureMeta(path, owner, canonical, reference, error);
}

void Include(scene::WorldCellBounds& bounds, bool& initialized, const math::Vector3& p)
{
    if (!initialized) { bounds.min = bounds.max = p; initialized = true; return; }
    bounds.min = {(std::min)(bounds.min.x,p.x),(std::min)(bounds.min.y,p.y),(std::min)(bounds.min.z,p.z)};
    bounds.max = {(std::max)(bounds.max.x,p.x),(std::max)(bounds.max.y,p.y),(std::max)(bounds.max.z,p.z)};
}

void IncludeTransformed(CellDraft& draft, const scene::WorldCellBounds& local, const math::Matrix4& matrix)
{
    for (unsigned mask = 0; mask < 8; ++mask) {
        const math::Vector3 p{mask&1 ? local.max.x:local.min.x,mask&2 ? local.max.y:local.min.y,mask&4 ? local.max.z:local.min.z};
        const auto q = matrix * math::Vector4{p,1}; Include(draft.bounds,draft.hasBounds,{q.x,q.y,q.z});
    }
}

bool DefinitionBounds(EditorContext& ctx, std::span<const asset::InstanceMeshPart> parts, scene::WorldCellBounds& bounds)
{
    bool initialized = false;
    for (const auto& part : parts) {
        renderer::Mesh* mesh = ctx.resources ? scene::ResolveMeshPath(part.meshPath, *ctx.resources) : nullptr;
        math::Vector3 center{}, extents{};
        if (mesh && !mesh->isSkinned && Finite(mesh->boundsCenter) && Finite(mesh->boundsExtents)) {
            center = mesh->boundsCenter; extents = mesh->boundsExtents;
        } else if (part.meshPath == "primitive:cube" || part.meshPath == "primitive:sphere") extents = {.5f,.5f,.5f};
        else if (part.meshPath == "primitive:plane") extents = {.5f,0,.5f};
        else return false;
        Include(bounds,initialized,center-extents); Include(bounds,initialized,center+extents);
    }
    return initialized;
}

bool HasHlodMaterials(std::span<const asset::InstanceMeshPart> parts)
{
    return std::all_of(parts.begin(), parts.end(), [](const auto& part) { return !part.materialPath.empty(); });
}

std::string InstanceKey(const asset::InstanceSetAsset& instances)
{
    std::ostringstream s; s << std::setprecision(9);
    for (const auto& part : instances.Parts()) s << part.meshPath << '\n' << part.materialPath << '\n' << part.lod << '\n';
    for (float h : instances.LodHeights()) s << h << ',';
    if (const auto snapshot = instances.Snapshot()) for (const auto& p : snapshot->placements)
        s << '\n' << p.id << ',' << p.position.x << ',' << p.position.y << ',' << p.position.z << ','
            << p.eulerRadians.x << ',' << p.eulerRadians.y << ',' << p.eulerRadians.z << ',' << p.scale.x << ',' << p.scale.y << ',' << p.scale.z << ',' << p.active;
    return s.str();
}

toml::table InstanceNode(const std::string& name, const std::string& id, math::Vector3 origin,
    uint32_t layer, bool shadows, const std::string& reference)
{
    return toml::table{{"name",name},{"instanceId",id},{"active",true},{"layer",static_cast<int64_t>(layer)},
        {"tag","Untagged"},{"parent",""},{"parentInstanceId",""},{"prefabAssetPath",""},{"prefabSourceId",""},
        {"transform",toml::table{{"position",toml::array{origin.x,origin.y,origin.z}},
            {"rotation",toml::array{0.,0.,0.,1.}},{"scale",toml::array{1.,1.,1.}}}},
        {"InstancedMeshComponent",toml::table{{"enabled",true},{"castShadows",shadows},{"maxDistance",0.0},{"instanceSetRef",reference}}}};
}

/// @note Dependencies are discovered from the selected payload; sharing near dependencies with HLOD would retain high-detail resources.
void Dependencies(CellDraft&, std::span<const asset::InstanceMeshPart>, const std::string&) {}

scene::GameObject* FindPartition(EditorContext& ctx, const OpArgs& args, std::string& error)
{
    if (!ctx.activeScene || ctx.projectRoot.empty() || ctx.managedBuildBusy
        || (ctx.playMode && (!ctx.playMode->IsInEditor() || ctx.playMode->HasPendingRestore()))) { error = "World baking requires an editable scene"; return nullptr; }
    if (const auto id = args.GetString("partition"); !id.empty()) {
        auto* node = ctx.activeScene->FindByGuid(id);
        if (!node || !node->GetComponent<scene::WorldPartitionComponent>()) { error = "Partition target is invalid"; return nullptr; }
        return node;
    }
    scene::GameObject* found = nullptr;
    for (const auto id : ctx.activeScene->GetEntities<scene::WorldPartitionComponent>()) {
        auto* node = ctx.activeScene->GetGameObject(id);
        if (!node || node->runtimeGenerated) continue;
        if (found) { error = "Specify partition when multiple controllers exist"; return nullptr; }
        found = node;
    }
    if (!found) error = "Configure a world partition first";
    return found;
}

bool SupportedStatic(const scene::Scene& scene, const scene::GameObject& node, bool terrain)
{
    bool supported = true;
    scene::ForEachRegisteredComponent([&]<typename T,typename Registration>() {
        if constexpr (std::is_same_v<T,scene::TerrainComponent> || std::is_same_v<T,scene::TerrainColliderComponent>
            || std::is_same_v<T,scene::FiberComponent>) {
            if (!terrain && scene.HasComponent<T>(node.GetID())) supported = false;
        } else if constexpr (!std::is_same_v<T,scene::InstancedMeshComponent> && !std::is_same_v<T,scene::WorldPcgGeneratedComponent>
            && !std::is_same_v<T,scene::MeshRenderer> && !std::is_same_v<T,scene::MaterialComponent>
            && !std::is_same_v<T,scene::ColliderComponent> && !std::is_same_v<T,scene::WorldHlodSourceComponent>) {
            if (scene.HasComponent<T>(node.GetID())) supported = false;
        } else if (terrain && scene.HasComponent<T>(node.GetID())) supported = false;
    });
    return supported && node.GetChildCount() == 0;
}

bool CanBakeTerrain(scene::GameObject& node, const scene::TerrainComponent& terrain)
{
    const auto* fiber = node.GetComponent<scene::FiberComponent>();
    return terrain.enabled && terrain.surfacePositions.empty() && terrain.surfaceIndices.empty()
        && !node.GetParent() && node.prefabAssetPath.empty()
        && (!fiber || !fiber->m_enabled || (fiber->m_mode == scene::FiberRenderMode::BLADE && fiber->m_paintPath.empty()));
}

void ReferenceIds(const toml::node& node, const std::set<std::string>& candidates, std::set<std::string>& result)
{
    if(const auto* table=node.as_table()){
        for(const auto& [key,value]:*table){
            if(key=="instanceId"||key=="parentInstanceId"||key=="parent"||key=="prefabSourceId")continue;
            ReferenceIds(value,candidates,result);
        }
    }else if(const auto* array=node.as_array()){
        for(const auto& value:*array)ReferenceIds(value,candidates,result);
    }else if(const auto value=node.value<std::string>();value&&candidates.contains(*value))result.insert(*value);
}

struct TerrainTile {
    size_t source;
    int x, z, columns, rows;
    math::Vector3 position;
};

/// @note Start on the existing chunk lattice. The final tile includes the original outer edge.
bool PlanTerrainTiles(const Source& source, size_t index, const scene::WorldCellGrid& grid,
    size_t maxTiles, std::multimap<scene::WorldCellId, TerrainTile>& tiles)
{
    const auto& terrain = *source.terrain;
    const auto& world = source.world;
    const math::Vector3 scale{world.m[0][0], world.m[1][1], world.m[2][2]};
    const math::Vector3 origin{world.m[0][3], world.m[1][3], world.m[2][3]};
    if (!Finite(scale) || !Finite(origin) || scale.x <= 0 || scale.y <= 0 || scale.z <= 0
        || terrain.columns < 2 || terrain.rows < 2 || terrain.heightData.size() != terrain.VertexCount()
        || !std::isfinite(terrain.cellSize) || terrain.cellSize <= 0 || terrain.chunkSize < 1
        || terrain.chunkSize > 128 || !std::isfinite(terrain.maxHeight) || terrain.maxHeight <= 0) return false;
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            if (row != col && world.m[row][col] != 0.0f) return false;
    const float span = grid.cellSize / (terrain.cellSize * (std::max)(scale.x, scale.z));
    const int stride = (std::max)(1, static_cast<int>(std::clamp(span, 1.0f, 128.0f)) / terrain.chunkSize) * terrain.chunkSize;
    for (int z = 0; z < terrain.rows - 1;) {
        const int depth = (std::min)(stride, terrain.rows - 1 - z);
        for (int x = 0; x < terrain.columns - 1;) {
            const int width = (std::min)(stride, terrain.columns - 1 - x);
            const math::Vector3 position = origin + math::Vector3{x * terrain.cellSize * scale.x, 0, z * terrain.cellSize * scale.z};
            scene::WorldCellId cell;
            if (tiles.size() >= maxTiles || !scene::TryLocateWorldCell(grid, position, cell)) return false;
            tiles.emplace(cell, TerrainTile{index, x, z, width + 1, depth + 1, position});
            x += width;
        }
        z += depth;
    }
    return true;
}

/// @note Shared edge vertices are copied exactly. Hole cells belong to only one tile.
scene::TerrainComponent CopyTerrainTile(const scene::TerrainComponent& terrain, const TerrainTile& tile)
{
    scene::TerrainComponent result;
    result.columns = tile.columns;
    result.rows = tile.rows;
    result.cellSize = terrain.cellSize;
    result.maxHeight = terrain.maxHeight;
    result.chunkSize = terrain.chunkSize;
    result.heightBlendDepth = terrain.heightBlendDepth;
    result.layerMaterials = terrain.layerMaterials;
    const bool splat = terrain.HasValidSplat();
    const bool holes = terrain.holeData.size() == terrain.CellCount();
    result.heightData.reserve(result.VertexCount());
    if (splat) {
        result.splatIndices.reserve(result.VertexCount() * scene::TERRAIN_SPLAT_SLOTS);
        result.splatWeights.reserve(result.VertexCount() * scene::TERRAIN_SPLAT_SLOTS);
    }
    if (holes) result.holeData.reserve(result.CellCount());
    for (int z = 0; z < tile.rows; ++z) {
        const size_t start = static_cast<size_t>(tile.z + z) * terrain.columns + tile.x;
        result.heightData.insert(result.heightData.end(), terrain.heightData.begin() + start, terrain.heightData.begin() + start + tile.columns);
        if (splat) {
            const size_t first = start * scene::TERRAIN_SPLAT_SLOTS;
            const size_t end = first + static_cast<size_t>(tile.columns) * scene::TERRAIN_SPLAT_SLOTS;
            result.splatIndices.insert(result.splatIndices.end(), terrain.splatIndices.begin() + first, terrain.splatIndices.begin() + end);
            result.splatWeights.insert(result.splatWeights.end(), terrain.splatWeights.begin() + first, terrain.splatWeights.begin() + end);
        }
        if (z < tile.rows - 1 && holes) {
            const size_t first = static_cast<size_t>(tile.z + z) * (terrain.columns - 1) + tile.x;
            result.holeData.insert(result.holeData.end(), terrain.holeData.begin() + first, terrain.holeData.begin() + first + tile.columns - 1);
        }
    }
    return result;
}
}

OpResult BakeWorldCells(OpContext& context, const OpArgs& args)
{
    auto& ctx = context.ctx; std::string error;
    auto* manager = FindPartition(ctx,args,error);
    if (!manager) return OpResult::Err("NO_PARTITION",error);
    const auto& settings = *manager->GetComponent<scene::WorldPartitionComponent>();
    const scene::WorldCellGrid grid{settings.gridOrigin,settings.cellSize};
    if (!scene::ValidateWorldCellGrid(grid,error)) return OpResult::Err("BAD_GRID",error);
    std::filesystem::path manifestPath; std::string manifestRelative;
    if (!ResolvePath(ctx,settings.manifestPath,manifestPath,manifestRelative) || manifestPath.extension() != ".worldcells") return OpResult::Err("BAD_PATH","Manifest must be a .worldcells asset in this project's Assets directory");
    std::string priorManifest;const bool hadManifest=util::FileSystem::ReadText(manifestPath,priorManifest);
    const int maxCells = args.GetInt("maxCells",4096), maxInstances = args.GetInt("maxInstances",1048576);
    if (maxCells < 1 || maxCells > 4096 || maxInstances < 1 || maxInstances > 1048576) return OpResult::Err("BAD_ARG","Invalid cell or instance bake budget");
    scene::FlushWorldTransforms(*ctx.activeScene);
    const bool includeScene = args.GetBool("includeScene",false), buildHlod = args.GetBool("buildHlod",true);
    const bool includeTerrain = args.GetBool("includeTerrain", false);
    const std::string sceneText = scene::SceneSerializer::CaptureSaveComparison(*ctx.activeScene, manifestRelative);
    const auto parsed = toml::parse(sceneText);
    if (sceneText.empty() || !parsed) return OpResult::Err("SERIALIZE_FAILED", "Cannot capture authored scene");
    std::map<std::string,toml::table> records;
    if (const auto* objects = parsed["gameobjects"].as_array()) for (const auto& value : *objects)
        if (const auto* object = value.as_table()) records.emplace((*object)["instanceId"].value_or(std::string{}),*object);
    std::vector<Source> sources; size_t pinned = 0, total = 0;
    for (auto& node : ctx.activeScene->GameObjects()) {
        if (node.runtimeGenerated || !node.activeInHierarchy() || node.GetID() == manager->GetID()) continue;
        const auto* generated = node.GetComponent<scene::WorldPcgGeneratedComponent>();
        const auto* terrain = node.GetComponent<scene::TerrainComponent>();
        if (!generated && !includeScene && !(includeTerrain && terrain)) continue;
        auto* instances = node.GetComponent<scene::InstancedMeshComponent>(); auto* mesh = node.GetComponent<scene::MeshRenderer>();
        if ((terrain ? !includeTerrain || !CanBakeTerrain(node, *terrain) : !instances && !mesh)
            || node.name.starts_with("__") || !SupportedStatic(*ctx.activeScene,node, terrain != nullptr)) { ++pinned; continue; }
        if(node.layer<0||node.layer>=32)return OpResult::Err("BAD_LAYER","Static source layer must be in [0,31]");
        Source source; source.id=node.instanceId; source.name=node.name; source.world=node.transform.GetWorldMatrix();
        source.layer=static_cast<uint32_t>(node.layer);
        if (const auto found=records.find(source.id);found!=records.end())source.actor=found->second;
        if (terrain) {
            if (!source.actor["TerrainComponent"].is_table()) return OpResult::Err("SERIALIZE_FAILED", "Terrain was not captured");
            source.terrain = terrain;
            sources.push_back(std::move(source));
            continue;
        } else if (instances) {
            if (!instances->m_enabled) continue;
            const auto* definition = asset::ResolveMeshInstanceDefinition(instances->m_instanceSetRef.path);
            const auto snapshot = definition ? instances->Snapshot(*definition) : nullptr;
            if (!definition || !snapshot) return OpResult::Err("INVALID_INSTANCES","Cannot resolve placement definition: "+node.name);
            source.parts.assign(definition->Parts().begin(),definition->Parts().end()); source.heights.assign(definition->LodHeights().begin(),definition->LodHeights().end());
            for (const auto& p : snapshot->placements) if (p.active) source.placements.push_back(p);
            source.shadows=instances->m_castShadows;
            source.maxDistance=instances->m_maxDistance;
            source.hlod=instances->m_maxDistance<=0 || instances->m_maxDistance>=settings.hlodDistance;
        } else {
            if (!mesh->enabled || mesh->meshPath.empty()) continue;
            source.instanced=false; source.shadows=mesh->castShadows;
            const auto* material=node.GetComponent<scene::MaterialComponent>();
            if(material){
                const auto& values=material->Authoring();
                if(!values.paramOverrides.empty()||!values.integerParamOverrides.empty()||!values.textureOverrides.empty()
                    ||values.hasBlendModeOverride||values.hasDoubleSidedOverride||values.hasRenderQueueOverride
                    ||values.hasPresentationDefaults||material->GetBlendMode()!=renderer::BlendMode::OPAQUE_BLEND){++pinned;continue;}
            }
            source.parts.push_back({mesh->meshPath,material?material->GetMaterialPath():std::string{},0}); source.heights={0.0f};
            renderer::InstancePlacement p; p.id=1; source.placements.push_back(p);
            if(source.actor.empty()) return OpResult::Err("SERIALIZE_FAILED","Static actor was not captured");
        }
        if(generated)source.hlod=source.hlod&&generated->includeHlod;
        if(const auto* policy=node.GetComponent<scene::WorldHlodSourceComponent>())source.hlod=source.hlod&&policy->includeHlod;
        source.hlod=source.hlod&&HasHlodMaterials(source.parts);
        if (source.placements.empty()) continue;
        if (!DefinitionBounds(ctx,source.parts,source.bounds)) return OpResult::Err("BOUNDS_UNAVAILABLE","Cannot establish conservative static model bounds: "+node.name);
        total+=source.placements.size();
        if (total>static_cast<size_t>(maxInstances)) return OpResult::Err("BUDGET_EXCEEDED","World instance budget exceeded before any output was written");
        sources.push_back(std::move(source));
    }
    std::sort(sources.begin(),sources.end(),[](const auto& a,const auto& b){return a.id<b.id;});
    std::set<std::string> candidateIds;for(const auto& s:sources)if(!candidateIds.insert(s.id).second)return OpResult::Err("DUPLICATE_ID","Authored source identities must be unique");
    std::set<std::string> terrainIds;
    for (const auto& source : sources) if (source.terrain) terrainIds.insert(source.id);
    std::map<std::string,std::set<std::string>> referenceGraph;
    std::set<std::string> pinnedIds;
    std::vector<std::string> pendingPins;
    const auto pin = [&](const std::string& id) {
        if (pinnedIds.insert(id).second) pendingPins.push_back(id);
    };
    for (const auto& [id, record] : records) {
        auto& edges = referenceGraph[id];
        for (const auto& [key, value] : record) {
            /// @note The edit-only grid slot list identifies original Terrain owners, not the streamed copies.
            if (key == "TerrainGridComponent" && includeTerrain) continue;
            if (key == "instanceId" || key == "parentInstanceId" || key == "parent" || key == "prefabSourceId") continue;
            ReferenceIds(value, candidateIds, edges);
        }
        for (const auto& target : edges)
            if (!candidateIds.contains(id) || (id != target && terrainIds.contains(target))) pin(target);
    }
    for (size_t i = 0; i < pendingPins.size(); ++i) {
        const auto edges = referenceGraph.find(pendingPins[i]);
        if (edges != referenceGraph.end()) for (const auto& target : edges->second) pin(target);
    }
    pinned+=pinnedIds.size();std::erase_if(sources,[&](const auto& source){return pinnedIds.contains(source.id);});
    total=0;for(const auto& source:sources)total+=source.placements.size();
    if (sources.empty()) return OpResult::Err("NO_CONTENT","No eligible generated content, static objects or terrain");
    std::map<std::pair<scene::WorldCellId,size_t>,NearGroup> nearGroups;
    for (size_t i=0;i<sources.size();++i) for (const auto& placement:sources[i].placements) {
        const auto matrix=sources[i].world*math::Matrix4::TRS(placement.position,math::Quaternion::FromEuler(placement.eulerRadians),placement.scale);
        renderer::InstancePlacement local=placement;
        if(!DecomposeTerrainPlacement(matrix,local)) return OpResult::Err("UNSUPPORTED_TRANSFORM","Cell baking rejects non-positive scale or shear");
        scene::WorldCellId cell;
        if(!scene::TryLocateWorldCell(grid,local.position,cell)) return OpResult::Err("BAD_POSITION","World cell coordinate overflow");
        local.position=local.position-scene::WorldCellOrigin(grid,cell);
        auto& group=nearGroups[{cell,i}]; group.source=i;group.cell=cell;group.placements.push_back(local);
    }
    std::multimap<scene::WorldCellId, TerrainTile> terrainTiles;
    for (size_t i = 0; i < sources.size(); ++i)
        if (sources[i].terrain && !PlanTerrainTiles(sources[i], i, grid, static_cast<size_t>(maxCells), terrainTiles))
            return OpResult::Err("TERRAIN_TILE_PLAN_FAILED", "Unsupported terrain dimensions, transform or tile budget: " + sources[i].name);
    std::set<scene::WorldCellId> cellIds;for(const auto& [key,group]:nearGroups)cellIds.insert(group.cell);
    for (const auto& [cell, tile] : terrainTiles) cellIds.insert(cell);
    if(cellIds.size()>static_cast<size_t>(maxCells)) return OpResult::Err("BUDGET_EXCEEDED","World cell budget exceeded before any output was written");
    std::vector<scene::WorldHlodSource> hlodSources;
    for(const auto& s:sources) if (!s.terrain) hlodSources.push_back({s.id,s.world,s.parts,s.heights,s.placements,s.layer,s.shadows,s.hlod,true,s.bounds});
    std::vector<scene::WorldHlodGroup> proxies;
    if(buildHlod && !scene::BuildWorldHlod(hlodSources,{grid,65536,static_cast<uint32_t>(maxInstances)},proxies,error)) return OpResult::Err("HLOD_FAILED",error);
    std::map<scene::WorldCellId,CellDraft> drafts;
    const std::string owner=manager->instanceId;
    const auto folderPath=util::FileSystem::PathFromUtf8(manifestRelative).parent_path()/(manifestPath.stem().string()+".cells");
    std::string folder=util::FileSystem::PathToUtf8(folderPath);std::replace(folder.begin(),folder.end(),'\\','/');
    const size_t maxCellBytes = static_cast<size_t>((std::max)(settings.maxCellMegabytes, 0)) * 1024 * 1024;
    for (const auto& [cell, tile] : terrainTiles) {
        const auto& source = sources[tile.source];
        auto terrain = CopyTerrainTile(*source.terrain, tile);
        if (!std::all_of(terrain.heightData.begin(), terrain.heightData.end(), [](float value) { return std::isfinite(value); }))
            return OpResult::Err("INVALID_TERRAIN", "Terrain contains non-finite heights: " + source.name);
        std::string text;
        if (!scene::TerrainAssetSerializer::Serialize(terrain, text))
            return OpResult::Err("INVALID_TERRAIN", "Cannot serialize terrain tile: " + source.name);
        auto& draft = drafts[cell];
        if (text.size() > maxCellBytes - draft.terrainBytes)
            return OpResult::Err("BUDGET_EXCEEDED", "Terrain tiles exceed the cell byte budget: " + scene::WorldCellName(cell));
        draft.terrainBytes += text.size();
        const std::string suffix = std::to_string(tile.x) + "_" + std::to_string(tile.z);
        const bool first = tile.x == 0 && tile.z == 0;
        const auto id = first ? source.id : asset::AssetDatabase::DeriveGuid(source.id, "terrain:" + suffix);
        if (!first && records.contains(id)) return OpResult::Err("DUPLICATE_ID", "Terrain tile collides with an authored object");
        const std::string path = folder + "/" + scene::WorldCellName(cell) + "/terrain_" + std::to_string(Hash(text)) + ".terrain";
        std::string reference;
        if (!WriteText(ctx, path, text, owner, reference, error)) return OpResult::Err("WRITE_FAILED", error);
        auto object = source.actor;
        object.insert_or_assign("instanceId", id);
        object.insert_or_assign("name", source.name + " / " + suffix);
        for (const auto* key : {"parent", "parentInstanceId", "prefabAssetPath", "prefabSourceId"}) object.insert_or_assign(key, "");
        const math::Vector3 scale{source.world.m[0][0], source.world.m[1][1], source.world.m[2][2]};
        object.insert_or_assign("transform", toml::table{{"position", toml::array{tile.position.x, tile.position.y, tile.position.z}},
            {"rotation", toml::array{0., 0., 0., 1.}}, {"scale", toml::array{scale.x, scale.y, scale.z}}});
        auto& component = *object["TerrainComponent"].as_table();
        component.erase("inlineData");
        component.insert_or_assign("terrainAssetPath", reference);
        object.insert_or_assign("WorldHlodSourceComponent", toml::table{{"includeHlod", true}});
        const auto [low, high] = std::minmax_element(terrain.heightData.begin(), terrain.heightData.end());
        Include(draft.bounds, draft.hasBounds, tile.position + math::Vector3{0, *low * terrain.maxHeight * scale.y, 0});
        Include(draft.bounds, draft.hasBounds, tile.position + math::Vector3{(tile.columns - 1) * terrain.cellSize * scale.x,
            *high * terrain.maxHeight * scale.y, (tile.rows - 1) * terrain.cellSize * scale.z});
        if (buildHlod) draft.farTerrain.push_back(object);
        draft.objects.push_back(std::move(object));
    }
    std::set<size_t> assignedOriginal;
    for(auto& [key,group]:nearGroups) {
        const auto& s=sources[group.source];auto& draft=drafts[group.cell];
        const auto origin=scene::WorldCellOrigin(grid,group.cell);
        if(s.instanced) {
            asset::MeshInstancesAsset output;
            if(!output.instances.Replace(s.parts,s.heights,group.placements)) return OpResult::Err("INVALID_INSTANCES","Cell placements failed validation");
            const std::string signature=std::to_string(Hash(InstanceKey(output.instances)));
            const std::string relative=folder+"/"+scene::WorldCellName(group.cell)+"/near_"+asset::AssetDatabase::DeriveGuid(s.id,signature)+".meshinstances";
            std::string reference;
            if(!WriteInstances(ctx,relative,output,owner,reference,error)) return OpResult::Err("WRITE_FAILED",error);
            const bool first=assignedOriginal.insert(group.source).second;
            const auto id=first?s.id:asset::AssetDatabase::DeriveGuid(s.id,"cell:"+scene::WorldCellName(group.cell));
            auto object=InstanceNode("Cell "+scene::WorldCellName(group.cell)+" / "+s.name,id,origin,s.layer,s.shadows,reference);
            object.insert_or_assign("tag",s.actor["tag"].value_or(std::string{"Untagged"}));
            auto& component = *object["InstancedMeshComponent"].as_table();
            component.insert_or_assign("maxDistance", s.maxDistance);
            const auto* original = s.actor["InstancedMeshComponent"].as_table();
            if (original) for (const auto* key : {"vegetationCellBudget", "vegetationDrawDetailPerCell",
                "vegetationNearDetailPerCell", "vegetationShadowDetailPerCell"}) {
                if (const auto* value = original->get(key)) component.insert_or_assign(key, *value);
            }
            object.insert_or_assign("WorldHlodSourceComponent",toml::table{{"includeHlod",s.hlod}});
            if(const auto* provenance=s.actor["WorldPcgGeneratedComponent"].as_table())object.insert_or_assign("WorldPcgGeneratedComponent",*provenance);
            draft.objects.push_back(std::move(object));
            draft.identityText+=signature;Dependencies(draft,s.parts,reference);
        } else {
            toml::table object=s.actor;object.insert_or_assign("parent","");object.insert_or_assign("parentInstanceId","");
            object.insert_or_assign("WorldHlodSourceComponent",toml::table{{"includeHlod",s.hlod}});
            renderer::InstancePlacement world;
            if(!DecomposeTerrainPlacement(s.world,world)) return OpResult::Err("UNSUPPORTED_TRANSFORM","Static actor root cannot be flattened without shear");
            const auto q=math::Quaternion::FromEuler(world.eulerRadians);
            object.insert_or_assign("transform",toml::table{{"position",toml::array{world.position.x,world.position.y,world.position.z}},
                {"rotation",toml::array{q.x,q.y,q.z,q.w}},{"scale",toml::array{world.scale.x,world.scale.y,world.scale.z}}});
            draft.identityText+=Dump(object);draft.objects.push_back(std::move(object));
        }
        for(const auto& p:group.placements)IncludeTransformed(draft,s.bounds,math::Matrix4::TRS(origin,math::Quaternion::Identity(),math::Vector3::ONE)*math::Matrix4::TRS(p.position,math::Quaternion::FromEuler(p.eulerRadians),p.scale));
    }
    scene::WorldPartitionManifest manifest;manifest.grid=grid;
    for(const auto& s:sources)manifest.sourceInstanceIds.push_back(s.id);
    std::map<scene::WorldCellId,toml::array> proxyObjects;
    for (auto& [cell, draft] : drafts) if (!draft.farTerrain.empty()) proxyObjects.emplace(cell, std::move(draft.farTerrain));
    for(size_t i=0;i<proxies.size();++i){
        const auto& proxy=proxies[i];const std::string signature=std::to_string(Hash(InstanceKey(proxy.proxy.instances)));
        const std::string relative=folder+"/"+scene::WorldCellName(proxy.cell)+"/hlod_"+asset::AssetDatabase::DeriveGuid(owner,signature+std::to_string(i))+".meshinstances";
        std::string reference;if(!WriteInstances(ctx,relative,proxy.proxy,owner,reference,error))return OpResult::Err("WRITE_FAILED",error);
        proxyObjects[proxy.cell].push_back(InstanceNode("World HLOD "+scene::WorldCellName(proxy.cell)+" / "+std::to_string(i),asset::AssetDatabase::DeriveGuid(owner,"hlod:"+scene::WorldCellName(proxy.cell)+":"+signature+":"+std::to_string(i)),proxy.origin,proxy.layer,proxy.castShadows,reference));
        if(!drafts.contains(proxy.cell))return OpResult::Err("CELL_MISMATCH","HLOD output escaped its near-cell ownership");
    }
    std::string revisionKey;
    for(auto& [cell,draft]:drafts){
        scene::WorldPartitionCell entry;entry.id=cell;entry.bounds=draft.bounds;
        const std::string nearText=Dump(toml::table{{"scene",toml::table{{"format_version",2}}},{"gameobjects",std::move(draft.objects)}});
        if (draft.terrainBytes != 0 && nearText.size() > maxCellBytes - draft.terrainBytes)
            return OpResult::Err("BUDGET_EXCEEDED", "Terrain and near payload exceed the cell byte budget");
        entry.revision=Hash(nearText+draft.identityText);entry.nearPath=folder+"/"+scene::WorldCellName(cell)+"/near_"+std::to_string(entry.revision)+".scene";
        std::string unused;if(!WriteText(ctx,entry.nearPath,nearText,owner,unused,error))return OpResult::Err("WRITE_FAILED",error);
        if(auto found=proxyObjects.find(cell);found!=proxyObjects.end()){
            const auto text=Dump(toml::table{{"scene",toml::table{{"format_version",2}}},{"gameobjects",std::move(found->second)}});
            if (draft.terrainBytes != 0 && text.size() > maxCellBytes - draft.terrainBytes)
                return OpResult::Err("BUDGET_EXCEEDED", "Terrain and HLOD payload exceed the cell byte budget");
            entry.hlodPath=folder+"/"+scene::WorldCellName(cell)+"/hlod_"+std::to_string(Hash(text))+".scene";
            if(!WriteText(ctx,entry.hlodPath,text,owner,unused,error))return OpResult::Err("WRITE_FAILED",error);
        }
        std::sort(draft.dependencies.begin(),draft.dependencies.end(),[](const auto& a,const auto& b){return std::tie(a.typeName,a.reference)<std::tie(b.typeName,b.reference);});
        draft.dependencies.erase(std::unique(draft.dependencies.begin(),draft.dependencies.end()),draft.dependencies.end());entry.dependencies=std::move(draft.dependencies);
        revisionKey+=entry.nearPath+entry.hlodPath;manifest.cells.push_back(std::move(entry));
    }
    manifest.revision=Hash(revisionKey);
    if(!scene::ValidateWorldPartitionManifest(manifest,error))return OpResult::Err("MANIFEST_INVALID",error);
    if(!MakeDirectory(manifestPath,error))return OpResult::Err("WRITE_FAILED",error);
    std::string reference;if(!EnsureMeta(manifestPath,owner,manifestRelative,reference,error))return OpResult::Err("META_FAILED",error);
    std::string current;const bool hasCurrent=util::FileSystem::ReadText(manifestPath,current);
    if(hasCurrent!=hadManifest||(hadManifest&&current!=priorManifest))return OpResult::Err("STALE_MANIFEST","Manifest changed while cells were built");
    std::string published;
    if(!scene::SerializeWorldPartitionManifest(manifest,published,error))return OpResult::Err("MANIFEST_INVALID",error);
    const bool unchanged=hadManifest&&published==priorManifest;
    if(!unchanged&&!scene::SaveWorldPartitionManifest(util::FileSystem::PathToUtf8(manifestPath),manifest,error))return OpResult::Err("WRITE_FAILED",error);
    ctx.requestAssetBrowserRefresh=true;
    OpData data=OpData::MakeObject();data.Set("partition",owner);data.Set("manifest",reference);data.Set("cells",static_cast<int>(manifest.cells.size()));
    data.Set("instances",static_cast<int>(total));data.Set("sources",static_cast<int>(sources.size()));data.Set("pinned",static_cast<int>(pinned));
    data.Set("terrainTiles", static_cast<int>(terrainTiles.size()));
    data.Set("hlodGroups",static_cast<int>(proxies.size()));data.Set("revision",std::to_string(manifest.revision));data.Set("authoringScenePreserved",true);
    auto result=OpResult::Data(std::move(data));result.noChange=unchanged;return result;
}

OpResult BuildWorldHlod(OpContext& context, const OpArgs& args)
{
    auto& ctx=context.ctx;std::string error;
    auto* manager=FindPartition(ctx,args,error);if(!manager)return OpResult::Err("NO_PARTITION",error);
    const auto& settings=*manager->GetComponent<scene::WorldPartitionComponent>();
    std::filesystem::path manifestPath;std::string relative;
    if(!ResolvePath(ctx,settings.manifestPath,manifestPath,relative))return OpResult::Err("BAD_PATH","Invalid project manifest path");
    std::string before;
    if(!util::FileSystem::ReadText(manifestPath,before))return OpResult::Err("NO_MANIFEST","Bake cells before building HLOD");
    const std::string expected=args.GetString("expected");
    if(!expected.empty()&&expected!=std::to_string(Hash(before)))return OpResult::Err("STALE_MANIFEST","Manifest changed after inspection");
    scene::WorldPartitionManifest manifest;
    if(!scene::DeserializeWorldPartitionManifest(before,manifest,error))return OpResult::Err("INVALID_MANIFEST",error);
    const int maxGroups=args.GetInt("maxGroups",65536),maxInstances=args.GetInt("maxInstances",1048576);
    if(maxGroups<1||maxGroups>65536||maxInstances<1||maxInstances>1048576)return OpResult::Err("BAD_ARG","Invalid HLOD budget");
    std::vector<Source> sources;
    std::map<scene::WorldCellId,toml::array> objects;
    for(const auto& cell:manifest.cells){
        std::filesystem::path path;std::string normalized,text;
        if(!ResolvePath(ctx,cell.nearPath,path,normalized)||!util::FileSystem::ReadText(path,text))return OpResult::Err("READ_FAILED","Cannot read baked near payload");
        std::vector<std::string> forestIds;
        if(!scene::ValidateSceneForestDocument(text,forestIds,error))return OpResult::Err("INVALID_PAYLOAD",error);
        auto document = toml::parse(text);
        auto* gameObjects = document.table()["gameobjects"].as_array();
        toml::array staticObjects;
        /// @note Terrain remains visible through the far radius. Reuse the persisted tile reference without opening its height data.
        for (auto& object : *gameObjects) {
            auto& record = *object.as_table();
            if (record.contains("TerrainComponent") && record["WorldHlodSourceComponent"]["includeHlod"].value_or(false))
                objects[cell.id].push_back(std::move(record));
            else staticObjects.push_back(std::move(record));
        }
        if (staticObjects.empty()) continue;
        document.table().insert_or_assign("gameobjects", std::move(staticObjects));
        auto prepared=scene::SceneSerializer::LoadPassiveDataFromText(Dump(document.table()),util::FileSystem::PathToUtf8(path));
        if(!prepared)return OpResult::Err("INVALID_PAYLOAD","Cannot prepare baked near payload");
        scene::FlushWorldTransforms(*prepared);
        for(auto& node:prepared->GameObjects()){
            if(!node.activeInHierarchy())continue;
            const auto* policy=node.GetComponent<scene::WorldHlodSourceComponent>();
            if(policy&&!policy->includeHlod)continue;
            Source source;source.id=node.instanceId;source.world=node.transform.GetWorldMatrix();source.layer=static_cast<uint32_t>(node.layer);
            if(auto* component=node.GetComponent<scene::InstancedMeshComponent>()){
                if(!component->m_enabled)continue;
                const auto* definition=asset::ResolveMeshInstanceDefinition(component->m_instanceSetRef.path);
                const auto snapshot=definition?component->Snapshot(*definition):nullptr;
                if(!definition||!snapshot)return OpResult::Err("INVALID_INSTANCES","Cannot resolve baked placement definition");
                source.parts.assign(definition->Parts().begin(),definition->Parts().end());source.heights.assign(definition->LodHeights().begin(),definition->LodHeights().end());
                source.placements=snapshot->placements;source.shadows=component->m_castShadows;
                if(component->m_maxDistance>0&&component->m_maxDistance<settings.hlodDistance)continue;
            }else if(auto* mesh=node.GetComponent<scene::MeshRenderer>()){
                if(!mesh->enabled)continue;
                const auto* material=node.GetComponent<scene::MaterialComponent>();source.parts.push_back({mesh->meshPath,material?material->GetMaterialPath():std::string{},0});source.heights={0.0f};
                renderer::InstancePlacement placement;placement.id=1;source.placements.push_back(placement);source.shadows=mesh->castShadows;
            }else continue;
            if(!HasHlodMaterials(source.parts))continue;
            if(!DefinitionBounds(ctx,source.parts,source.bounds))return OpResult::Err("BOUNDS_UNAVAILABLE","Baked HLOD source bounds are unavailable");
            sources.push_back(std::move(source));
        }
    }
    std::vector<scene::WorldHlodSource> views;
    for(const auto& s:sources)views.push_back({s.id,s.world,s.parts,s.heights,s.placements,s.layer,s.shadows,true,true,s.bounds});
    std::vector<scene::WorldHlodGroup> proxies;
    if(!scene::BuildWorldHlod(views,{manifest.grid,static_cast<uint32_t>(maxGroups),static_cast<uint32_t>(maxInstances)},proxies,error))return OpResult::Err("HLOD_FAILED",error);
    std::string folder=util::FileSystem::PathToUtf8(util::FileSystem::PathFromUtf8(relative).parent_path()/(manifestPath.stem().string()+".cells"));std::replace(folder.begin(),folder.end(),'\\','/');
    for(size_t i=0;i<proxies.size();++i){
        const auto& p=proxies[i];const auto signature=std::to_string(Hash(InstanceKey(p.proxy.instances)));
        const std::string output=folder+"/"+scene::WorldCellName(p.cell)+"/hlod_"+asset::AssetDatabase::DeriveGuid(manager->instanceId,signature+std::to_string(i))+".meshinstances";
        std::string reference;if(!WriteInstances(ctx,output,p.proxy,manager->instanceId,reference,error))return OpResult::Err("WRITE_FAILED",error);
        objects[p.cell].push_back(InstanceNode("World HLOD "+scene::WorldCellName(p.cell)+" / "+std::to_string(i),asset::AssetDatabase::DeriveGuid(manager->instanceId,"hlod:"+scene::WorldCellName(p.cell)+":"+signature+":"+std::to_string(i)),p.origin,p.layer,p.castShadows,reference));
    }
    std::string revision;
    for(auto& cell:manifest.cells){
        cell.hlodPath.clear();
        if(auto found=objects.find(cell.id);found!=objects.end()){
            const std::string text=Dump(toml::table{{"scene",toml::table{{"format_version",2}}},{"gameobjects",std::move(found->second)}});
            cell.hlodPath=folder+"/"+scene::WorldCellName(cell.id)+"/hlod_"+std::to_string(Hash(text))+".scene";
            std::string reference;if(!WriteText(ctx,cell.hlodPath,text,manager->instanceId,reference,error))return OpResult::Err("WRITE_FAILED",error);
        }
        revision+=cell.nearPath+cell.hlodPath;
    }
    for(const auto& [cell,array]:objects)if(std::none_of(manifest.cells.begin(),manifest.cells.end(),[&](const auto& c){return c.id==cell;}))return OpResult::Err("CELL_MISMATCH","Proxy ownership does not match persisted near cells");
    manifest.revision=Hash(revision);
    std::string current;if(!util::FileSystem::ReadText(manifestPath,current)||current!=before)return OpResult::Err("STALE_MANIFEST","Manifest changed while HLOD was built");
    std::string manifestReference;
    if(!EnsureMeta(manifestPath,manager->instanceId,relative,manifestReference,error))return OpResult::Err("META_FAILED",error);
    std::string published;if(!scene::SerializeWorldPartitionManifest(manifest,published,error))return OpResult::Err("MANIFEST_INVALID",error);
    const bool unchanged=published==before;
    if(!unchanged&&!scene::SaveWorldPartitionManifest(util::FileSystem::PathToUtf8(manifestPath),manifest,error))return OpResult::Err("WRITE_FAILED",error);
    ctx.requestAssetBrowserRefresh=true;
    OpData data=OpData::MakeObject();data.Set("manifest",settings.manifestPath);data.Set("cells",static_cast<int>(manifest.cells.size()));data.Set("hlodGroups",static_cast<int>(proxies.size()));data.Set("revision",std::to_string(manifest.revision));data.Set("fromPersistedNear",true);
    auto result=OpResult::Data(std::move(data));result.noChange=unchanged;return result;
}

} /// @note namespace fbzz::editor
