/// @file    WorldPartition.cpp
/// @brief   Stream validated cell forests through bounded CPU jobs and main-thread residency transactions.
/// @author  Hasegawa Jin
/// @date    2026-11-07
#include <Engine/Scene/WorldPartition.hpp>
#include <Engine/Scene/Components/WorldPartitionComponent.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneForest.hpp>
#include <Engine/Scene/SceneAssetReferences.hpp>
#include <Engine/Scene/MeshResolver.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/AssetLeaseSet.hpp>
#include <Engine/Asset/CellAssetStreaming.hpp>
#include <Engine/Asset/MeshInstancesAsset.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Asset/ModelAsset.hpp>
#include <Engine/Asset/TextureAsset.hpp>
#include <Engine/Core/Concurrency/TaskSystem.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <toml++/toml.hpp>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace fbzz::scene {
namespace {
bool Finite(const math::Vector3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

/// @note Terrain baking only emits static leaves. A scripted owner must stay live across cell changes.
bool CanRetireAuthoredSource(GameObject& object)
{
    return !object.GetComponent<CameraComponent>() && !object.GetComponent<WorldPartitionComponent>()
        && !object.GetComponent<ScriptComponent>()
        && (!object.GetComponent<TerrainComponent>() || object.GetChildCount() == 0);
}

std::string FoldPath(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool Inside(const std::filesystem::path& root, const std::filesystem::path& path)
{
    auto r = root.begin(), p = path.begin();
    for (; r != root.end(); ++r, ++p)
        if (p == path.end() || FoldPath(util::FileSystem::PathToUtf8(*r)) != FoldPath(util::FileSystem::PathToUtf8(*p))) return false;
    return true;
}

bool ResolvePayload(const std::string& projectRoot, const std::string& relative, std::string& out, std::string& diagnostic)
{
    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(util::FileSystem::PathFromUtf8(projectRoot) / "Assets", error);
    if (error) { diagnostic = "Cannot resolve project Assets root"; return false; }
    const auto path = std::filesystem::weakly_canonical(util::FileSystem::PathFromUtf8(projectRoot) / util::FileSystem::PathFromUtf8(relative), error);
    if (error || !Inside(root, path) || root == path) { diagnostic = "Cell payload escapes project Assets: " + relative; return false; }
    out = util::FileSystem::PathToUtf8(path);
    return true;
}

bool ReadBounded(const std::string& path, uint64_t limit, const std::atomic<bool>& canceled, std::string& out, std::string& diagnostic)
{
    if (canceled.load(std::memory_order_relaxed)) return false;
    std::ifstream file(util::FileSystem::PathFromUtf8(path), std::ios::binary | std::ios::ate);
    if (!file) { diagnostic = "Cannot open cell payload: " + path; return false; }
    const auto size = file.tellg();
    if (size <= 0 || static_cast<uint64_t>(size) > limit || static_cast<uint64_t>(size) > static_cast<uint64_t>((std::numeric_limits<std::streamsize>::max)())) {
        diagnostic = "Cell payload is empty or exceeds maxCellBytes: " + path; return false;
    }
    std::string text(static_cast<size_t>(size), '\0');
    file.seekg(0);
    file.read(text.data(), static_cast<std::streamsize>(size));
    if (!file || file.peek() != std::char_traits<char>::eof()) { diagnostic = "Cell payload changed or could not be read completely: " + path; return false; }
    if (canceled.load(std::memory_order_relaxed)) return false;
    out = std::move(text);
    return true;
}

/// @note Validation runs on value-only documents. Component construction and asset/GUID resolution remain on the main thread.
bool ValidateDocument(const std::string& text, std::vector<std::string>& ids, std::string& diagnostic)
{
    return ValidateSceneForestDocument(text, ids, diagnostic);
}

std::string ChannelName(const std::string& name)
{
    if (name == "Texture" || name == "TextureAsset") return std::string(asset::AssetStreamTypeName<asset::TextureAsset>());
    if (name == "Model") return std::string(asset::AssetStreamTypeName<asset::Model>());
    if (name == "ModelAsset") return std::string(asset::AssetStreamTypeName<asset::ModelAsset>());
    if (name == "Material" || name == "MaterialAsset") return std::string(asset::AssetStreamTypeName<asset::MaterialAsset>());
    if (name == "MeshInstances" || name == "MeshInstancesAsset") return std::string(asset::AssetStreamTypeName<asset::MeshInstancesAsset>());
    return name;
}

std::string StripMeshIndex(const std::string& path, size_t& index)
{
    index = 0;
    const auto colon = path.find_last_of(':');
    const auto dot = path.find_last_of('.');
    if (colon == std::string::npos || dot == std::string::npos || colon < dot || colon + 1 == path.size()) return path;
    size_t value = 0;
    for (size_t i = colon + 1; i < path.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(path[i]))) return path;
        const auto digit = static_cast<size_t>(path[i] - '0');
        if (value > (static_cast<size_t>((std::numeric_limits<int>::max)()) - digit) / 10) return {};
        value = value * 10 + digit;
    }
    index = value;
    return path.substr(0, colon);
}

void BindResidentMeshes(Scene& scene, const std::vector<EntityID>& owners, asset::AssetStreamer& streamer, renderer::ResourceManager* resources)
{
    for (const auto id : owners) {
        auto* mesh = scene.GetComponent<MeshRenderer>(id);
        if (!mesh || mesh->meshPath.empty()) continue;
        if (mesh->meshPath.starts_with("primitive:")) {
            if (resources) mesh->mesh = ResolveMeshPath(mesh->meshPath, *resources);
            continue;
        }
        size_t index = 0;
        const auto reference = StripMeshIndex(mesh->meshPath, index);
        if (reference.empty()) continue;
        const auto key = asset::AssetManager::CanonicalKey(reference);
        const auto& cache = asset::AssetStore<asset::Model>::Get().cache;
        const auto found = cache.find(key);
        auto* model = found == cache.end() ? nullptr : streamer.TryGet(found->second);
        mesh->mesh = model && index < model->meshes.size() ? model->meshes[index].get() : nullptr;
    }
}

using ServiceMap = std::unordered_map<const Scene*, std::shared_ptr<WorldPartitionRuntime>>;
ServiceMap& Services() { static ServiceMap services; return services; }
std::unordered_map<const Scene*, WorldPartitionRuntime*>& Owners()
{
    static std::unordered_map<const Scene*, WorldPartitionRuntime*> owners;
    return owners;
}
std::shared_ptr<WorldPartitionRuntime> RetainService(const WorldPartitionRuntime* runtime)
{
    for (const auto& [scene, service] : Services()) { (void)scene; if (service.get() == runtime) return service; }
    return {};
}
} /// @note namespace

bool ValidateWorldPartitionSettings(const WorldPartitionSettings& settings, std::string& diagnostic)
{
    if (!std::isfinite(settings.nearDistance) || !std::isfinite(settings.hlodDistance) || !std::isfinite(settings.hysteresis)
        || settings.nearDistance < 0 || settings.hlodDistance < settings.nearDistance || settings.hysteresis < 0
        || settings.hysteresis > settings.nearDistance || settings.hysteresis * 2 > settings.hlodDistance - settings.nearDistance) {
        diagnostic = "Streaming distances must be finite, ordered and leave room for hysteresis"; return false;
    }
    if (settings.maxConcurrentLoads == 0 || settings.maxConcurrentLoads > 32 || settings.maxPublishesPerFrame == 0 || settings.maxPublishesPerFrame > 64
        || settings.maxResidentCells == 0 || settings.maxResidentCells > 65536 || settings.maxCellBytes == 0 || settings.maxCellBytes > 1024ull * 1024ull * 1024ull
        || settings.maxResidentBytes == 0) { diagnostic = "Invalid world streaming job, publication or memory budget"; return false; }
    diagnostic.clear(); return true;
}

const char* WorldCellRepresentationName(WorldCellRepresentation value)
{
    switch (value) { case WorldCellRepresentation::NEAR: return "near"; case WorldCellRepresentation::HLOD: return "hlod"; default: return "unloaded"; }
}
const char* WorldCellLoadStateName(WorldCellLoadState value)
{
    switch (value) {
    case WorldCellLoadState::QUEUED: return "queued"; case WorldCellLoadState::READING: return "reading";
    case WorldCellLoadState::WAITING_ASSETS: return "waiting_assets"; case WorldCellLoadState::RESIDENT: return "resident";
    case WorldCellLoadState::FAILED: return "failed"; default: return "unloaded";
    }
}

struct WorldPartitionRuntime::Impl {
    struct Ticket { std::atomic<bool> canceled{false}; };
    struct Completion {
        uint64_t epoch = 0, attempt = 0;
        size_t cell = 0;
        bool sourcePreflight = false;
        std::string text, diagnostic;
        std::vector<std::string> objectIds;
        std::vector<std::pair<size_t, std::string>> terrainReferences;
        std::vector<std::string> instanceReferences;
    };
    struct Completions { std::mutex mutex; std::vector<Completion> pending; };
    struct Cell {
        WorldPartitionCell spec;
        WorldCellRepresentation desired = WorldCellRepresentation::UNLOADED, resident = WorldCellRepresentation::UNLOADED;
        WorldCellLoadState state = WorldCellLoadState::UNLOADED;
        std::optional<WorldCellRepresentation> overrideRepresentation;
        uint64_t attempt = 0, bytes = 0;
        std::shared_ptr<Ticket> ticket;
        std::vector<EntityID> roots, entities;
        asset::AssetLeaseSet leases, pendingLeases;
        std::string pendingText, pendingPath, diagnostic;
        std::vector<std::string> instanceReferences, pendingInstanceReferences;
        WorldCellRepresentation pendingRepresentation = WorldCellRepresentation::UNLOADED;
        bool quarantined = false;
    };
    JobExecutor executor;
    asset::AssetStreamer* streamer;
    std::shared_ptr<Completions> completions = std::make_shared<Completions>();
    Scene* scene = nullptr;
    uint64_t generation = 0, epoch = 1, stale = 0;
    uint32_t jobs = 0;
    WorldPartitionManifest manifest;
    WorldPartitionSettings settings;
    std::string projectRoot, manifestPath, diagnostic;
    math::Vector3 source{};
    std::vector<Cell> cells;
    std::map<WorldCellId, size_t> index;
    std::shared_ptr<Ticket> sourceTicket;
    bool sourcePending = false, sourcesReady = true, hostDiagnostic = false;
    bool inUpdate = false, inReset = false, resetRequested = false;
    std::unordered_set<std::string> retiredSources;
    const Scene* retiredScene = nullptr;
    uint64_t retiredGeneration = 0;
    std::unordered_map<std::string, EntityID> sourcesToRetire;

    Impl(JobExecutor jobsExecutor, asset::AssetStreamer* assets) : executor(std::move(jobsExecutor)), streamer(assets ? assets : &asset::AssetStreamer::Engine())
    {
        if (!executor) executor = [](std::function<void()> job) {
            if (TaskSystem::WorkerCount() > 0) (void)TaskSystem::Submit(std::move(job));
            else job();
        };
    }
    bool Live() const { return scene && !resetRequested && scene->GetRenderSceneGeneration() == generation; }
    uint64_t ResidentBytes() const { uint64_t bytes = 0; for (const auto& cell : cells) bytes += cell.bytes; return bytes; }
    void EvictPlacements(const std::string& reference)
    {
        (void)streamer->EvictUnusedRaw(asset::AssetStreamTypeName<asset::MeshInstancesAsset>(), reference);
    }
    void Cancel(Cell& cell)
    {
        if (cell.ticket) cell.ticket->canceled.store(true, std::memory_order_relaxed);
        cell.ticket.reset(); ++cell.attempt;
        std::string{}.swap(cell.pendingText); cell.pendingPath.clear();
        if (!cell.quarantined) {
            cell.pendingLeases.Release();
            for (const auto& reference : cell.pendingInstanceReferences) EvictPlacements(reference);
            cell.pendingInstanceReferences.clear();
        }
        cell.pendingRepresentation = WorldCellRepresentation::UNLOADED;
    }
    bool Unload(Cell& cell)
    {
        Cancel(cell);
        if (Live() && !scene->DestroyOwnedGameObjects(cell.entities)) {
            cell.state = WorldCellLoadState::FAILED;
            cell.diagnostic = "Cell unload refused: a nonowned descendant is attached; detach it before unloading";
            return false;
        }
        std::vector<EntityID>{}.swap(cell.roots); std::vector<EntityID>{}.swap(cell.entities); cell.bytes = 0;
        cell.resident = WorldCellRepresentation::UNLOADED; cell.state = WorldCellLoadState::UNLOADED; cell.diagnostic.clear();
        cell.leases.Release();
        for (const auto& reference : cell.instanceReferences) EvictPlacements(reference);
        cell.instanceReferences.clear();
        cell.quarantined = false; cell.pendingLeases.Release();
        for (const auto& reference : cell.pendingInstanceReferences) EvictPlacements(reference);
        cell.pendingInstanceReferences.clear();
        return true;
    }
    bool SourceClosure(std::string& error)
    {
        if (!Live()) { error = "Source Scene generation changed"; return false; }
        for (const auto& [guid, identity] : sourcesToRetire) {
            auto* owner = scene->GetGameObject(identity);
            if (!owner || owner->instanceId != guid) { error = "Authored source changed during streaming preflight: " + guid; return false; }
            if (!CanRetireAuthoredSource(*owner)) {
                error = "Authored streaming sources cannot include a camera, partition manager, script or non-leaf terrain"; return false;
            }
            for (int i = 0; i < owner->GetChildCount(); ++i) {
                const auto* object = owner->GetChild(i);
                if (object && !sourcesToRetire.contains(object->instanceId)) { error = "Authored source forest contains an unbaked child"; return false; }
            }
        }
        return true;
    }
    void SubmitSourcePreflight()
    {
        auto ticket = std::make_shared<Ticket>(); sourceTicket = ticket; sourcePending = true; ++jobs;
        auto queue = completions; const auto revision = epoch; const auto maximum = settings.maxCellBytes, identityBudget = settings.maxResidentBytes;
        std::vector<std::string> paths;
        for (const auto& cell : cells) { std::string path, error; if (ResolvePayload(projectRoot, cell.spec.nearPath, path, error)) paths.push_back(std::move(path)); else { diagnostic = error; sourcesReady = false; sourcePending = false; --jobs; return; } }
        const auto expected = manifest.sourceInstanceIds;
        executor([ticket, queue, revision, maximum, identityBudget, paths = std::move(paths), expected] {
            Completion result; result.epoch = revision; result.sourcePreflight = true;
            std::unordered_set<std::string> all;
            uint64_t identityBytes = 0;
            for (const auto& path : paths) {
                std::string text; std::vector<std::string> ids;
                if (!ReadBounded(path, maximum, ticket->canceled, text, result.diagnostic) || !ValidateDocument(text, ids, result.diagnostic)) break;
                for (auto& id : ids) {
                    const auto bytes = static_cast<uint64_t>(id.size()) * 2 + 192;
                    if (bytes > identityBudget || identityBytes > identityBudget - bytes) { result.diagnostic = "Source identity preflight exceeds world resident byte budget"; break; }
                    if (!all.insert(id).second) { result.diagnostic = "Near payloads contain duplicate global instanceId: " + id; break; }
                    identityBytes += bytes;
                }
                if (!result.diagnostic.empty()) break;
            }
            if (result.diagnostic.empty() && !ticket->canceled.load(std::memory_order_relaxed)) {
                for (const auto& id : expected) if (!all.contains(id)) { result.diagnostic = "Baked source is missing from near payloads: " + id; break; }
                if (result.diagnostic.empty()) result.objectIds.assign(all.begin(), all.end());
            }
            std::scoped_lock lock(queue->mutex);
            if (ticket->canceled.load(std::memory_order_relaxed)) { std::string{}.swap(result.text); std::vector<std::string>{}.swap(result.objectIds); }
            queue->pending.push_back(std::move(result));
        });
    }
    /// @note Extract references on the worker. The owner thread resolves paths against the live asset database.
    static void CollectCellInputs(Completion& result)
    {
        const auto document = toml::parse(result.text);
        const auto* objects = document ? document.table()["gameobjects"].as_array() : nullptr;
        if (!objects) return;
        for (size_t index = 0; index < objects->size(); ++index) {
            const auto* object = (*objects)[index].as_table();
            if (!object) continue;
            if (const auto* terrain = (*object)["TerrainComponent"].as_table(); terrain && !terrain->contains("inlineData")) {
                auto reference = (*terrain)["terrainAssetPath"].value_or(std::string{});
                if (!reference.empty()) result.terrainReferences.emplace_back(index, std::move(reference));
            }
            if (const auto* instances = (*object)["InstancedMeshComponent"].as_table()) {
                auto reference = (*instances)["instanceSetRef"].value_or(std::string{});
                if (!reference.empty()) result.instanceReferences.push_back(std::move(reference));
            }
        }
    }
    void SubmitCell(size_t cellIndex)
    {
        auto& cell = cells[cellIndex];
        const auto& path = cell.desired == WorldCellRepresentation::NEAR ? cell.spec.nearPath : cell.spec.hlodPath;
        std::string resolved;
        if (!ResolvePayload(projectRoot, path, resolved, cell.diagnostic)) { cell.state = WorldCellLoadState::FAILED; return; }
        cell.pendingRepresentation = cell.desired; cell.pendingPath = resolved; cell.state = WorldCellLoadState::READING;
        auto ticket = std::make_shared<Ticket>(); cell.ticket = ticket;
        const auto attempt = ++cell.attempt, revision = epoch, maximum = settings.maxCellBytes;
        auto queue = completions; ++jobs;
        executor([ticket, queue, revision, attempt, cellIndex, maximum, resolved] {
            Completion result; result.epoch = revision; result.attempt = attempt; result.cell = cellIndex;
            if (ReadBounded(resolved, maximum, ticket->canceled, result.text, result.diagnostic)
                && ValidateDocument(result.text, result.objectIds, result.diagnostic)) CollectCellInputs(result);
            std::scoped_lock lock(queue->mutex);
            if (ticket->canceled.load(std::memory_order_relaxed)) { std::string{}.swap(result.text); std::vector<std::string>{}.swap(result.objectIds); }
            queue->pending.push_back(std::move(result));
        });
    }
    void Fail(Cell& cell, std::string error)
    {
        Cancel(cell); cell.state = WorldCellLoadState::FAILED; cell.diagnostic = std::move(error);
    }
    bool SubmitTerrainReads(Completion& input)
    {
        if (input.terrainReferences.empty()) return false;
        const size_t cellIndex = input.cell;
        auto& cell = cells[cellIndex];
        std::vector<std::pair<size_t, std::string>> paths;
        paths.reserve(input.terrainReferences.size());
        for (const auto& [index, reference] : input.terrainReferences) {
            std::string path;
            if (asset::AssetDatabase::IsGuidRef(reference)) path = asset::AssetManager::ResolveAssetPath(reference);
            else if (reference.starts_with("Assets/")) {
                if (!ResolvePayload(projectRoot, reference, path, cell.diagnostic)) { Fail(cell, cell.diagnostic); return true; }
            } else {
                std::error_code error;
                const auto assets = std::filesystem::weakly_canonical(util::FileSystem::PathFromUtf8(projectRoot) / "Assets", error);
                const auto source = util::FileSystem::PathFromUtf8(cell.pendingPath).parent_path();
                const auto resolved = std::filesystem::weakly_canonical(source / util::FileSystem::PathFromUtf8(reference), error);
                if (error || !Inside(assets, resolved)) { Fail(cell, "Terrain reference escapes project Assets"); return true; }
                path = util::FileSystem::PathToUtf8(resolved);
            }
            if (path.empty()) { Fail(cell, "Terrain reference cannot be resolved: " + reference); return true; }
            paths.emplace_back(index, std::move(path));
        }
        auto ticket = std::make_shared<Ticket>(); cell.ticket = ticket; cell.state = WorldCellLoadState::READING;
        const auto attempt = cell.attempt, revision = epoch, maximum = settings.maxCellBytes;
        auto queue = completions; ++jobs;
        executor([ticket, queue, attempt, revision, maximum, cellIndex, paths = std::move(paths),
            text = std::move(input.text), instances = std::move(input.instanceReferences)]() mutable {
            Completion result; result.cell = cellIndex; result.attempt = attempt; result.epoch = revision;
            result.instanceReferences = std::move(instances);
            auto document = toml::parse(text);
            auto* objects = document ? document.table()["gameobjects"].as_array() : nullptr;
            uint64_t total = text.size();
            if (!objects) result.diagnostic = "Cell document changed during terrain preparation";
            for (const auto& [index, path] : paths) {
                if (!result.diagnostic.empty() || ticket->canceled.load(std::memory_order_relaxed)) break;
                std::string terrainText;
                if (total >= maximum) { result.diagnostic = "Combined terrain cell exceeds maxCellBytes"; break; }
                if (!ReadBounded(path, maximum - total, ticket->canceled, terrainText, result.diagnostic)) break;
                total += terrainText.size();
                auto terrainDocument = toml::parse(terrainText);
                if (!terrainDocument || !terrainDocument.table()["terrain"].is_table()) { result.diagnostic = "Referenced terrain is not a complete terrain document: " + path; break; }
                auto* owner = (*objects)[index].as_table();
                auto* terrain = owner ? (*owner)["TerrainComponent"].as_table() : nullptr;
                if (!terrain) { result.diagnostic = "Terrain owner changed during preparation"; break; }
                terrain->insert_or_assign("inlineData", std::move(terrainDocument.table()));
            }
            if (result.diagnostic.empty() && !ticket->canceled.load(std::memory_order_relaxed)) {
                std::ostringstream serialized; serialized << document.table(); result.text = serialized.str();
                if (result.text.size() > maximum) result.diagnostic = "Prepared terrain cell exceeds maxCellBytes";
                else (void)ValidateDocument(result.text, result.objectIds, result.diagnostic);
            }
            std::scoped_lock lock(queue->mutex);
            if (ticket->canceled.load(std::memory_order_relaxed)) { std::string{}.swap(result.text); std::vector<std::string>{}.swap(result.objectIds); }
            queue->pending.push_back(std::move(result));
        });
        return true;
    }
    bool Acquire(Cell& cell, const std::vector<std::string>& instanceReferences)
    {
        cell.pendingLeases = asset::AssetLeaseSet(*streamer);
        std::set<std::pair<std::string, std::string>> dependencies;
        for (const auto& item : cell.spec.dependencies) {
            const auto type = ChannelName(item.typeName);
            dependencies.emplace(type, item.reference);
            if (type == ChannelName("MeshInstancesAsset")) cell.pendingInstanceReferences.push_back(item.reference);
        }
        for (const auto& reference : instanceReferences) {
            const auto path = asset::AssetManager::ResolveAssetPath(reference);
            const auto extension = FoldPath(util::FileSystem::PathToUtf8(util::FileSystem::PathFromUtf8(path).extension()));
            if (extension == ".meshinstances") {
                dependencies.emplace(ChannelName("MeshInstancesAsset"), reference);
                cell.pendingInstanceReferences.push_back(reference);
            }
        }
        std::sort(cell.pendingInstanceReferences.begin(), cell.pendingInstanceReferences.end());
        cell.pendingInstanceReferences.erase(std::unique(cell.pendingInstanceReferences.begin(), cell.pendingInstanceReferences.end()), cell.pendingInstanceReferences.end());
        for (const auto& reference : cell.pendingInstanceReferences) {
            const auto path = asset::AssetManager::ResolveAssetPath(reference);
            std::error_code error;
            const auto bytes = path.empty() ? uintmax_t{0} : std::filesystem::file_size(util::FileSystem::PathFromUtf8(path), error);
            if (path.empty() || error || bytes == 0 || bytes > settings.maxCellBytes) {
                Fail(cell, "Placement asset is missing or exceeds maxCellBytes: " + reference); return false;
            }
        }
        for (const auto& item : CollectSceneAssetReferences(cell.pendingText, [](std::string_view reference) {
            return asset::AssetDatabase::PathFromGuid(asset::AssetDatabase::GuidFromRef(reference));
        })) dependencies.emplace(std::string(StreamTypeNameOf(item.kind)), item.reference);
        asset::AssetRequestOptions options;
        options.priority = cell.pendingRepresentation == WorldCellRepresentation::NEAR ? asset::AssetPriority::Visible : asset::AssetPriority::Prefetch;
        for (const auto& [type, reference] : dependencies) {
            const auto error = cell.pendingLeases.Add(type, reference, options);
            if (error != asset::AssetLoadError::None) { Fail(cell, "Asset dependency request refused: " + type + " " + reference); return false; }
        }
        cell.state = WorldCellLoadState::WAITING_ASSETS;
        return true;
    }
    void Drain()
    {
        std::vector<Completion> finished;
        { std::scoped_lock lock(completions->mutex); finished.swap(completions->pending); }
        for (auto& result : finished) {
            if (jobs > 0) --jobs;
            if (!Live() || result.epoch != epoch) { ++stale; continue; }
            if (result.sourcePreflight) {
                sourcePending = false;
                if (!sourceTicket || sourceTicket->canceled.load(std::memory_order_relaxed)) { ++stale; continue; }
                sourceTicket.reset();
                if (!result.diagnostic.empty()) { diagnostic = std::move(result.diagnostic); continue; }
                if (!SourceClosure(diagnostic)) continue;
                std::unordered_set<std::string> allowed(manifest.sourceInstanceIds.begin(), manifest.sourceInstanceIds.end());
                bool collision = false;
                std::unordered_set<std::string_view> persistentIds;
                for (const auto& object : scene->GameObjects())
                    if (!allowed.contains(object.instanceId)) persistentIds.insert(object.instanceId);
                for (const auto& id : result.objectIds) if (persistentIds.contains(id)) { diagnostic = "Cell ID collides with a persistent object: " + id; collision = true; break; }
                if (collision) continue;
                std::vector<EntityID> roots;
                for (const auto& [id, entity] : sourcesToRetire) {
                    auto* object = scene->GetGameObject(entity);
                    const auto* parent = object ? object->GetParent() : nullptr;
                    if (!parent || !sourcesToRetire.contains(parent->instanceId)) roots.push_back(entity);
                    retiredSources.insert(id);
                }
                scene->DestroyGameObjects(roots);
                if (!Live()) { ++stale; continue; }
                sourcesToRetire.clear(); sourcesReady = true; diagnostic.clear();
                continue;
            }
            if (result.cell >= cells.size()) { ++stale; continue; }
            auto& cell = cells[result.cell];
            if (result.attempt != cell.attempt || !cell.ticket || cell.ticket->canceled.load(std::memory_order_relaxed)) { ++stale; continue; }
            cell.ticket.reset();
            if (!result.diagnostic.empty() || result.text.empty()) { Fail(cell, result.diagnostic.empty() ? "Cell read was canceled" : std::move(result.diagnostic)); continue; }
            if (SubmitTerrainReads(result)) continue;
            const auto resident = ResidentBytes();
            if (result.text.size() > settings.maxResidentBytes || resident - cell.bytes > settings.maxResidentBytes - result.text.size()) { Fail(cell, "Cell payload exceeds resident byte budget"); continue; }
            cell.pendingText = std::move(result.text);
            (void)Acquire(cell, result.instanceReferences);
        }
    }
    bool Publish(Cell& cell, renderer::ResourceManager* resources)
    {
        if (!Live()) return false;
        if (cell.quarantined || !scene->CanDestroyOwnedGameObjects(cell.entities)) {
            Fail(cell, "Cell replacement refused: detach nonowned descendants before changing representation"); return false;
        }
        const auto progress = cell.pendingLeases.GetProgress();
        if (!progress.IsSettled()) return false;
        if (progress.failed != 0) { Fail(cell, "A required cell dependency failed to load"); return false; }
        uint64_t bytes = cell.pendingText.size();
        for (const auto& reference : cell.pendingInstanceReferences) {
            const auto placements = asset::MeshInstancesResidentBytes(reference);
            if (placements > settings.maxResidentBytes || bytes > settings.maxResidentBytes - placements) { Fail(cell, "Cell placements exceed resident byte budget"); return false; }
            bytes += placements;
        }
        const auto residentBytes = ResidentBytes();
        if (bytes > settings.maxResidentBytes || residentBytes - cell.bytes > settings.maxResidentBytes - bytes) { Fail(cell, "Cell publication exceeds resident byte budget"); return false; }

        /// @note Old representation remains live until its replacement has fully validated and appended. Reused saved IDs are hidden only during this transaction.
        std::vector<std::pair<EntityID, std::string>> hidden;
        for (const auto id : cell.entities) if (auto* object = scene->GetGameObject(id)) {
            hidden.emplace_back(id, object->instanceId);
            object->instanceId = "__world_partition_retiring_" + std::to_string(epoch) + "_" + std::to_string(cell.attempt) + "_" + std::to_string(id.index);
        }
        std::vector<EntityID> roots, entities;
        std::string error;
        const bool appended = AppendSceneForest(*scene, cell.pendingText, cell.pendingPath, nullptr, roots, entities, error);
        if (!appended || !Live()) {
            if (Live()) for (const auto& [id, guid] : hidden) if (auto* object = scene->GetGameObject(id)) object->instanceId = guid;
            Fail(cell, error.empty() ? "Scene changed while publishing a cell" : std::move(error)); return false;
        }
        if (!scene->DestroyOwnedGameObjects(cell.entities)) {
            if (!Live()) { Fail(cell, "Scene retired during cell replacement"); return false; }
            if (scene->DestroyOwnedGameObjects(entities)) {
                for (const auto& [id, guid] : hidden) if (auto* object = scene->GetGameObject(id)) object->instanceId = guid;
                Fail(cell, "Cell replacement refused because a callback attached a nonowned descendant");
            } else {
                for (const auto id : entities) if (auto* object = scene->GetGameObject(id)) {
                    object->instanceId = "__world_partition_quarantine_" + std::to_string(id.index) + "_" + std::to_string(id.generation);
                    object->SetActive(false);
                }
                for (const auto& [id, guid] : hidden) if (auto* object = scene->GetGameObject(id)) object->instanceId = guid;
                cell.entities.insert(cell.entities.end(), entities.begin(), entities.end());
                cell.roots.insert(cell.roots.end(), roots.begin(), roots.end());
                cell.quarantined = true; cell.state = WorldCellLoadState::FAILED;
                cell.diagnostic = "Publication rollback retained an edited forest; detach foreign descendants before retrying";
                std::string{}.swap(cell.pendingText);
            }
            return false;
        }
        if (!Live()) return false;
        BindResidentMeshes(*scene, entities, *streamer, resources);
        auto oldLeases = std::move(cell.leases);
        auto oldInstances = std::move(cell.instanceReferences);
        cell.roots = std::move(roots); cell.entities = std::move(entities); cell.bytes = bytes;
        cell.resident = cell.pendingRepresentation; cell.state = WorldCellLoadState::RESIDENT;
        cell.leases = std::move(cell.pendingLeases); std::string{}.swap(cell.pendingText); cell.pendingPath.clear(); cell.diagnostic.clear();
        cell.instanceReferences = std::move(cell.pendingInstanceReferences);
        cell.pendingRepresentation = WorldCellRepresentation::UNLOADED;
        oldLeases.Release();
        for (const auto& reference : oldInstances) EvictPlacements(reference);
        if (Live()) FlushWorldTransforms(*scene);
        return true;
    }
};

WorldPartitionRuntime::WorldPartitionRuntime(JobExecutor executor, asset::AssetStreamer* streamer) : m_impl(std::make_unique<Impl>(std::move(executor), streamer)) {}
WorldPartitionRuntime::~WorldPartitionRuntime()
{
    Reset();
    if (m_impl->Live() && !m_impl->cells.empty()) {
        auto* scene = m_impl->scene;
        auto keeper = std::make_shared<WorldPartitionRuntime>();
        keeper->m_impl = std::move(m_impl);
        Owners()[scene] = keeper.get();
        Services()[scene] = std::move(keeper);
    }
}

bool WorldPartitionRuntime::Configure(Scene& scene, const WorldPartitionManifest& manifest, const std::string& projectRoot,
    const WorldPartitionSettings& settings, std::string& diagnostic)
{
    const auto retained = RetainService(this); (void)retained;
    if (m_impl->inUpdate || m_impl->inReset) { diagnostic = "A partition cannot be reconfigured during its publication or retirement callbacks"; return false; }
    if (const auto found = Owners().find(&scene); found != Owners().end() && found->second != this) { diagnostic = "Another runtime already owns this Scene partition"; return false; }
    if (!ValidateWorldPartitionManifest(manifest, diagnostic) || !ValidateWorldPartitionSettings(settings, diagnostic)) return false;
    if (!manifest.sourceInstanceIds.empty() && !settings.retireSourceObjects) { diagnostic = "Authored source retirement is allowed only in an isolated Play scene"; return false; }
    std::error_code error;
    const auto suppliedRoot = util::FileSystem::PathFromUtf8(projectRoot);
    if (!suppliedRoot.is_absolute()) { diagnostic = "World partition requires an absolute projectRoot"; return false; }
    const auto root = std::filesystem::weakly_canonical(suppliedRoot, error);
    if (error || root.empty() || !root.is_absolute()) { diagnostic = "World partition requires an absolute projectRoot"; return false; }
    const auto rootText = util::FileSystem::PathToUtf8(root);
    for (const auto& cell : manifest.cells) {
        std::string resolved;
        if (!ResolvePayload(rootText, cell.nearPath, resolved, diagnostic) || (!cell.hlodPath.empty() && !ResolvePayload(rootText, cell.hlodPath, resolved, diagnostic))) return false;
    }
    std::unordered_map<std::string, EntityID> sources;
    const bool sameRetiredScene = m_impl->retiredScene == &scene && m_impl->retiredGeneration == scene.GetRenderSceneGeneration();
    for (const auto& id : manifest.sourceInstanceIds) {
        if (sameRetiredScene && m_impl->retiredSources.contains(id)) continue;
        sources.emplace(id, EntityID::INVALID);
    }
    if (!sources.empty()) for (const auto& object : scene.GameObjects()) {
        if (const auto source = sources.find(object.instanceId); source != sources.end() && !source->second.IsValid())
            source->second = object.GetID();
    }
    for (const auto& [id, entity] : sources) {
        auto* object = scene.GetGameObject(entity);
        if (!object) { diagnostic = "Missing authored source object: " + id; return false; }
        if (!CanRetireAuthoredSource(*object)) { diagnostic = "Source whitelist includes an always-loaded or scripted object"; return false; }
        for (int i = 0; i < object->GetChildCount(); ++i) {
            const auto* value = object->GetChild(i);
            if (value && !sources.contains(value->instanceId)) { diagnostic = "Source whitelist is not a closed descendant forest"; return false; }
        }
    }
    auto nextManifest = manifest;
    const auto nextSettings = settings;
    const auto expectedGeneration = scene.GetRenderSceneGeneration();
    Reset();
    if (m_impl->scene || !m_impl->cells.empty()) { diagnostic = "Previous cell ownership is unsafe; detach foreign descendants before reconfiguring"; return false; }
    if (scene.GetRenderSceneGeneration() != expectedGeneration) { diagnostic = "Scene changed while retiring the previous partition"; return false; }
    auto& state = *m_impl;
    if (!sameRetiredScene) state.retiredSources.clear();
    state.scene = &scene; state.generation = scene.GetRenderSceneGeneration(); state.retiredScene = &scene; state.retiredGeneration = state.generation;
    Owners()[&scene] = this;
    state.manifest = std::move(nextManifest); state.settings = nextSettings; state.projectRoot = rootText; state.diagnostic.clear(); state.hostDiagnostic = false;
    state.sourcesToRetire = std::move(sources); state.sourcesReady = state.manifest.sourceInstanceIds.empty() || state.sourcesToRetire.empty();
    for (const auto& spec : state.manifest.cells) {
        state.index.emplace(spec.id, state.cells.size()); state.cells.emplace_back(); state.cells.back().spec = spec;
    }
    if (!state.sourcesReady && state.jobs < state.settings.maxConcurrentLoads) state.SubmitSourcePreflight();
    diagnostic.clear(); return true;
}

bool WorldPartitionRuntime::Open(Scene& scene, const std::string& reference, const WorldPartitionSettings& settings, std::string& diagnostic)
{
    const auto retained = RetainService(this); (void)retained;
    const auto fail = [&] { m_impl->diagnostic = diagnostic; return false; };
    const std::string manifestReference = reference;
    const auto path = asset::AssetManager::ResolveAssetPath(manifestReference);
    if (path.empty()) { diagnostic = "Cannot resolve world manifest reference"; return fail(); }
    WorldPartitionManifest manifest;
    if (!LoadWorldPartitionManifest(path, manifest, diagnostic)) return fail();
    const auto root = asset::AssetDatabase::ProjectRoot();
    if (root.empty()) { diagnostic = "AssetDatabase project is not initialized"; return fail(); }
    if (!Configure(scene, manifest, root, settings, diagnostic)) return fail();
    m_impl->manifestPath = manifestReference;
    return true;
}

void WorldPartitionRuntime::Update(const math::Vector3& source, renderer::ResourceManager* resources)
{
    const auto retained = RetainService(this); (void)retained;
    auto& state = *m_impl;
    if (state.inUpdate || state.inReset) return;
    state.inUpdate = true;
    struct UpdateScope {
        WorldPartitionRuntime& owner;
        Impl& state;
        ~UpdateScope() { state.inUpdate = false; if (state.resetRequested) { state.resetRequested = false; owner.Reset(); } }
    } scope{*this, state};
    if (resources && state.Live()) state.scene->SetResourceManager(resources);
    state.Drain();
    if (!state.Live()) { DetachRetiredScene(); return; }
    if (!Finite(source)) { state.diagnostic = "Streaming source position must be finite"; return; }
    state.source = source;
    if (!state.sourcesReady) {
        if (!state.sourcePending && state.diagnostic.empty() && state.jobs < state.settings.maxConcurrentLoads) state.SubmitSourcePreflight();
        return;
    }
    std::vector<std::pair<float, size_t>> wanted;
    for (size_t i = 0; i < state.cells.size(); ++i) {
        auto& cell = state.cells[i];
        const float dx = (std::max)({cell.spec.bounds.min.x - source.x, source.x - cell.spec.bounds.max.x, 0.0f});
        const float dz = (std::max)({cell.spec.bounds.min.z - source.z, source.z - cell.spec.bounds.max.z, 0.0f});
        const float distance = (std::min)(WorldCellDistanceXZ(state.manifest.grid, cell.spec.id, source), std::hypot(dx, dz));
        auto target = WorldCellRepresentation::UNLOADED;
        if (cell.overrideRepresentation) target = *cell.overrideRepresentation;
        else {
            const float nearThreshold = state.settings.nearDistance + (cell.resident == WorldCellRepresentation::NEAR ? state.settings.hysteresis : cell.resident == WorldCellRepresentation::HLOD ? -state.settings.hysteresis : 0.0f);
            const float farThreshold = state.settings.hlodDistance + (cell.resident != WorldCellRepresentation::UNLOADED ? state.settings.hysteresis : 0.0f);
            if (distance <= nearThreshold) target = WorldCellRepresentation::NEAR;
            else if (distance <= farThreshold && !cell.spec.hlodPath.empty()) target = WorldCellRepresentation::HLOD;
        }
        if (target != cell.desired) {
            state.Cancel(cell); cell.desired = target; cell.diagnostic.clear();
            cell.state = cell.resident == WorldCellRepresentation::UNLOADED ? WorldCellLoadState::UNLOADED : WorldCellLoadState::RESIDENT;
        }
        if (cell.quarantined) { cell.state = WorldCellLoadState::FAILED; cell.diagnostic = "Detach foreign descendants from the retained forest before retrying"; }
        if (target != WorldCellRepresentation::UNLOADED) wanted.emplace_back(distance, i);
    }
    const size_t admitted = (std::min)(wanted.size(), static_cast<size_t>(state.settings.maxResidentCells));
    /// @note Cell index breaks distance ties in manifest order, matching the previous stable ordering.
    std::partial_sort(wanted.begin(), wanted.begin() + admitted, wanted.end());
    for (size_t i = admitted; i < wanted.size(); ++i) state.cells[wanted[i].second].desired = WorldCellRepresentation::UNLOADED;
    wanted.resize(admitted);
    for (auto& cell : state.cells) {
        if (cell.desired == WorldCellRepresentation::UNLOADED && (cell.resident != WorldCellRepresentation::UNLOADED || cell.ticket || !cell.pendingText.empty())) state.Unload(cell);
        if (!state.Live()) return;
    }
    uint32_t published = 0;
    auto preparing = static_cast<uint32_t>(std::count_if(state.cells.begin(), state.cells.end(), [](const auto& entry) {
        return entry.state == WorldCellLoadState::READING || entry.state == WorldCellLoadState::WAITING_ASSETS;
    }));
    for (const auto& [distance, cellIndex] : wanted) {
        (void)distance;
        auto& cell = state.cells[cellIndex];
        if (cell.desired == WorldCellRepresentation::UNLOADED) continue;
        if (cell.state == WorldCellLoadState::WAITING_ASSETS && published < state.settings.maxPublishesPerFrame) {
            if (state.Publish(cell, resources)) ++published;
            if (!state.Live()) return;
            if (cell.state != WorldCellLoadState::WAITING_ASSETS && preparing > 0) --preparing;
        }
        if (cell.resident != cell.desired && cell.state != WorldCellLoadState::FAILED && cell.state != WorldCellLoadState::READING && cell.state != WorldCellLoadState::WAITING_ASSETS) {
            cell.state = WorldCellLoadState::QUEUED;
            if (state.jobs < state.settings.maxConcurrentLoads && preparing < state.settings.maxConcurrentLoads) {
                state.SubmitCell(cellIndex);
                if (cell.state == WorldCellLoadState::READING) ++preparing;
            }
        }
        if (cell.resident != WorldCellRepresentation::UNLOADED) BindResidentMeshes(*state.scene, cell.entities, *state.streamer, resources);
    }
}

bool WorldPartitionRuntime::RequestCell(WorldCellId id, WorldCellRepresentation representation, std::string& diagnostic)
{
    const auto retained = RetainService(this); (void)retained;
    if (m_impl->inReset) { diagnostic = "Cell controls are unavailable during retirement callbacks"; return false; }
    const auto found = m_impl->index.find(id);
    if (found == m_impl->index.end()) { diagnostic = "Cell is not present in the manifest"; return false; }
    auto& cell = m_impl->cells[found->second];
    if (representation != WorldCellRepresentation::UNLOADED && representation != WorldCellRepresentation::NEAR && representation != WorldCellRepresentation::HLOD) { diagnostic = "Unknown cell representation"; return false; }
    if (representation == WorldCellRepresentation::HLOD && cell.spec.hlodPath.empty()) { diagnostic = "Cell has no HLOD payload"; return false; }
    cell.overrideRepresentation = representation;
    if (representation == WorldCellRepresentation::UNLOADED) {
        cell.desired = representation;
        if (!m_impl->inUpdate) {
            m_impl->inUpdate = true;
            m_impl->Unload(cell);
            m_impl->inUpdate = false;
            if (m_impl->resetRequested) { m_impl->resetRequested = false; Reset(); }
        }
    }
    diagnostic.clear(); return true;
}
bool WorldPartitionRuntime::ClearCellOverride(WorldCellId id)
{
    const auto found = m_impl->index.find(id); if (found == m_impl->index.end()) return false;
    m_impl->cells[found->second].overrideRepresentation.reset(); return true;
}
void WorldPartitionRuntime::RetryFailed()
{
    auto& state = *m_impl;
    if (state.inUpdate || state.inReset) return;
    for (auto& cell : state.cells) if (cell.state == WorldCellLoadState::FAILED) {
        if (cell.quarantined && !state.Unload(cell)) continue;
        cell.state = WorldCellLoadState::QUEUED; cell.diagnostic.clear();
    }
    if (!state.sourcesReady && !state.sourcePending && state.Live()) {
        state.diagnostic.clear();
        if (state.jobs < state.settings.maxConcurrentLoads) state.SubmitSourcePreflight();
    }
}
void WorldPartitionRuntime::Reset()
{
    auto& state = *m_impl;
    if (state.inUpdate) { state.resetRequested = true; return; }
    if (state.inReset) return;
    const auto retained = RetainService(this); (void)retained;
    state.inReset = true;
    auto* previousScene = state.scene;
    ++state.epoch;
    if (state.sourceTicket) state.sourceTicket->canceled.store(true, std::memory_order_relaxed);
    state.sourceTicket.reset(); state.sourcePending = false;
    for (auto& cell : state.cells) state.Cancel(cell);
    bool safe = true;
    if (state.Live()) for (auto& cell : state.cells) {
        if (!state.scene->CanDestroyOwnedGameObjects(cell.entities)) {
            cell.state = WorldCellLoadState::FAILED;
            cell.diagnostic = "Cell retirement refused: detach nonowned descendants first";
            state.diagnostic = cell.diagnostic; safe = false;
        }
    }
    if (safe && state.Live()) for (auto& cell : state.cells) {
        if (!state.Unload(cell)) { state.diagnostic = cell.diagnostic; safe = false; break; }
        if (!state.Live()) break;
    }
    if (safe || !state.Live()) {
        if (const auto found = Owners().find(previousScene); found != Owners().end() && found->second == this) Owners().erase(found);
        state.scene = nullptr;
        for (auto& cell : state.cells) {
            cell.leases.Release(); cell.pendingLeases.Release();
            for (const auto& reference : cell.instanceReferences) state.EvictPlacements(reference);
            for (const auto& reference : cell.pendingInstanceReferences) state.EvictPlacements(reference);
        }
        state.cells.clear(); state.index.clear(); state.sourcesToRetire.clear(); state.sourcesReady = true;
        state.manifest = {}; state.manifestPath.clear(); state.diagnostic.clear();
    }
    {
        std::scoped_lock lock(state.completions->mutex);
        for (const auto& completion : state.completions->pending) { (void)completion; if (state.jobs > 0) --state.jobs; ++state.stale; }
        state.completions->pending.clear();
    }
    state.inReset = false;
}

void WorldPartitionRuntime::DetachRetiredScene()
{
    auto& state = *m_impl;
    if (const auto found = Owners().find(state.scene); found != Owners().end() && found->second == this) Owners().erase(found);
    state.scene = nullptr;
    Reset(); state.retiredSources.clear(); state.retiredScene = nullptr; state.retiredGeneration = 0;
}
WorldPartitionStatus WorldPartitionRuntime::Status() const
{
    const auto& state = *m_impl; WorldPartitionStatus out;
    out.configured = state.scene != nullptr; out.manifestPath = state.manifestPath; out.diagnostic = state.diagnostic;
    out.grid = state.manifest.grid; out.settings = state.settings; out.revision = state.manifest.revision; out.source = state.source; out.loadsInFlight = state.jobs; out.staleCompletionsDropped = state.stale;
    for (const auto& cell : state.cells) {
        WorldPartitionCellStatus value; value.id = cell.spec.id; value.desired = cell.desired; value.resident = cell.resident; value.state = cell.state;
        value.revision = cell.spec.revision; value.residentBytes = cell.bytes; value.objectCount = static_cast<uint32_t>(cell.entities.size()); value.diagnostic = cell.diagnostic;
        out.cells.push_back(std::move(value)); out.residentBytes += cell.bytes;
        if (cell.resident == WorldCellRepresentation::NEAR) ++out.residentNear;
        if (cell.resident == WorldCellRepresentation::HLOD) ++out.residentHlod;
    }
    return out;
}
const WorldPartitionManifest& WorldPartitionRuntime::Manifest() const { return m_impl->manifest; }

WorldPartitionRuntime& GetWorldPartitionRuntime(Scene& scene)
{
    if (const auto found = Owners().find(&scene); found != Owners().end()) return *found->second;
    auto& service = Services()[&scene]; if (!service) service = std::make_shared<WorldPartitionRuntime>(); return *service;
}
const WorldPartitionRuntime* FindWorldPartitionRuntime(const Scene& scene)
{
    if (const auto found = Owners().find(&scene); found != Owners().end()) return found->second;
    const auto found = Services().find(&scene); return found == Services().end() ? nullptr : found->second.get();
}
void ForgetWorldPartitionRuntime(Scene& scene)
{
    WorldPartitionRuntime* owner = nullptr;
    if (const auto found = Owners().find(&scene); found != Owners().end()) { owner = found->second; Owners().erase(found); }
    std::shared_ptr<WorldPartitionRuntime> retained;
    if (const auto found = Services().find(&scene); found != Services().end()) { retained = std::move(found->second); Services().erase(found); }
    if (owner) owner->DetachRetiredScene();
    else if (retained) retained->DetachRetiredScene();
}
void TransferWorldPartitionRuntime(Scene& source, Scene& destination)
{
    const auto found = Owners().find(&source);
    if (found == Owners().end()) return;
    auto* runtime = found->second;
    Owners().erase(found); Owners()[&destination] = runtime;
    if (const auto service = Services().find(&source); service != Services().end()) {
        auto retained = std::move(service->second); Services().erase(service); Services()[&destination] = std::move(retained);
    }
    auto& state = *runtime->m_impl;
    state.scene = &destination; state.generation = destination.GetRenderSceneGeneration(); ++state.epoch;
    state.retiredScene = &destination; state.retiredGeneration = state.generation;
    if (state.sourceTicket) state.sourceTicket->canceled.store(true, std::memory_order_relaxed);
    state.sourceTicket.reset(); state.sourcePending = false;
    for (auto& cell : state.cells) { state.Cancel(cell); cell.state = cell.resident == WorldCellRepresentation::UNLOADED ? WorldCellLoadState::UNLOADED : WorldCellLoadState::RESIDENT; }
}
void UpdateWorldPartitionRuntime(Scene& scene, renderer::ResourceManager* resources, bool playing)
{
    WorldPartitionComponent* component = nullptr;
    for (const auto id : scene.GetEntities<WorldPartitionComponent>()) {
        auto* owner = scene.GetGameObject(id); auto* candidate = scene.GetComponent<WorldPartitionComponent>(id);
        if (!owner || !owner->activeInHierarchy() || !candidate || !candidate->enabled) continue;
        if (component) {
            auto& state = *GetWorldPartitionRuntime(scene).m_impl;
            state.diagnostic = "Only one active WorldPartitionComponent may control a Scene"; state.hostDiagnostic = true;
            return;
        }
        component = candidate;
    }
    if (!component || component->manifestPath.empty() || (!playing && !component->streamInEditor)) {
        const auto found = Services().find(&scene); if (found != Services().end()) found->second->Reset(); return;
    }
    auto componentSnapshot = *component;
    component = &componentSnapshot;
    auto& runtime = GetWorldPartitionRuntime(scene);
    const auto retained = RetainService(&runtime);
    (void)retained;
    if (runtime.m_impl->hostDiagnostic) { runtime.m_impl->diagnostic.clear(); runtime.m_impl->hostDiagnostic = false; }
    WorldPartitionSettings settings;
    settings.nearDistance = component->nearDistance; settings.hlodDistance = component->hlodDistance; settings.hysteresis = component->hysteresis;
    settings.maxConcurrentLoads = static_cast<uint32_t>(component->maxConcurrentLoads); settings.maxPublishesPerFrame = static_cast<uint32_t>(component->maxPublishesPerFrame);
    settings.maxResidentCells = static_cast<uint32_t>(component->maxResidentCells);
    settings.maxCellBytes = component->maxCellMegabytes > 0 ? static_cast<uint64_t>(component->maxCellMegabytes) * 1024ull * 1024ull : 0;
    settings.maxResidentBytes = component->maxResidentMegabytes > 0 ? static_cast<uint64_t>(component->maxResidentMegabytes) * 1024ull * 1024ull : 0;
    settings.retireSourceObjects = playing;
    const auto& current = *runtime.m_impl;
    std::string error;
    const auto matchesGrid = [&](const WorldCellGrid& grid) {
        return grid.cellSize == component->cellSize && grid.origin.x == component->gridOrigin.x
            && grid.origin.y == component->gridOrigin.y && grid.origin.z == component->gridOrigin.z;
    };
    if (!current.Live() || current.manifestPath != component->manifestPath || !(current.settings == settings) || !matchesGrid(current.manifest.grid)) {
        const auto path = asset::AssetManager::ResolveAssetPath(component->manifestPath);
        WorldPartitionManifest manifest;
        if (path.empty() || !LoadWorldPartitionManifest(path, manifest, error)) {
            runtime.m_impl->diagnostic = error.empty() ? "Cannot resolve world partition manifest" : error; runtime.m_impl->hostDiagnostic = true; return;
        }
        if (!matchesGrid(manifest.grid)) {
            runtime.m_impl->diagnostic = "World manifest grid differs from the authoring grid; rebake world cells before streaming"; runtime.m_impl->hostDiagnostic = true; return;
        }
        if (!runtime.Configure(scene, manifest, asset::AssetDatabase::ProjectRoot(), settings, error)) {
            runtime.m_impl->diagnostic = error; runtime.m_impl->hostDiagnostic = true; return;
        }
        runtime.m_impl->manifestPath = component->manifestPath;
    }
    FlushWorldTransforms(scene);
    auto source = component->sourcePosition;
    if (component->useMainCamera) {
        GameObject* sourceObject = component->sourceCamera.guid.empty() ? nullptr : scene.FindByGuid(component->sourceCamera.guid);
        if (!component->sourceCamera.guid.empty()) {
            const auto* camera = sourceObject ? sourceObject->GetComponent<CameraComponent>() : nullptr;
            if (!sourceObject || !sourceObject->activeInHierarchy() || !camera || !camera->enabled) {
                runtime.m_impl->diagnostic = "Configured streaming source must be an active CameraComponent"; runtime.m_impl->hostDiagnostic = true; return;
            }
        }
        if (!sourceObject) for (const auto id : scene.GetEntities<CameraComponent>()) {
            auto* owner = scene.GetGameObject(id); const auto* camera = scene.GetComponent<CameraComponent>(id);
            if (owner && owner->activeInHierarchy() && camera && camera->enabled && camera->isMain) { sourceObject = owner; break; }
        }
        if (sourceObject) source = sourceObject->transform.worldPosition;
    }
    runtime.Update(source, resources);
}
} /// @note namespace fbzz::scene
