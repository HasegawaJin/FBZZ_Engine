/// @file    TerrainScatterOperators.cpp
/// @brief   地形への配置を独立した InstanceSet に保存し、参照の変更を Undo へ記録する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include "Tools/TerrainScatter.hpp"
#include "Tools/TerrainPaintEditing.hpp"
#include "Tools/TerrainPaintMix.hpp"
#include "Tools/TerrainInstanceEditing.hpp"
#include "Tools/TerrainPaintClearance.hpp"
#include "Panels/MapToolCommon.hpp"
#include <Editor/EditorContext.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/Selection.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/DataAssetRegistry.hpp>
#include <Engine/Asset/InstanceSetAsset.hpp>
#include <Engine/Asset/MeshInstancesAsset.hpp>
#include <Engine/Asset/TerrainPaintAsset.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Scene/ComponentRegistry.hpp>
#include <Engine/Scene/Components/InstancedMeshComponent.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/MeshResolver.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Scene/WaterSurfaceQuery.hpp>
#include <Engine/Scene/TerrainSurfaceQuery.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Scene/TomlReflector.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace fbzz::editor {
namespace terrain_scatter_operators {
OpParam Parameter(const char* name, OpParamType type, OpValue fallback, const char* description)
{
    OpParam param;
    param.name = name; param.type = type; param.required = false;
    param.defaultValue = std::move(fallback); param.desc = description;
    return param;
}

const asset::InstanceSetAsset* ResolveSet(const std::string& reference)
{
    return asset::ResolveMeshInstanceDefinition(reference);
}

bool SameComponent(const scene::InstancedMeshComponent& current, const scene::InstancedMeshComponent& expected,
    const std::shared_ptr<const renderer::InstanceSetData>& expectedSnapshot)
{
    return current.m_enabled == expected.m_enabled && current.m_castShadows == expected.m_castShadows
        && current.m_maxDistance == expected.m_maxDistance && current.m_instanceSetRef.path == expected.m_instanceSetRef.path
        && current.m_instanceSetRef.type == expected.m_instanceSetRef.type && current.Snapshot() == expectedSnapshot;
}

/// @note 最初の配置を Redo で復元すると Component の寿命が変わる。後続ストロークにも復元した寿命を伝える。
auto& ScatterLifetimes()
{
    static std::unordered_map<uint64_t, std::weak_ptr<uint64_t>> lifetimes;
    return lifetimes;
}

std::shared_ptr<uint64_t> ScatterLifetime(uint64_t lifetime)
{
    auto& lifetimes = ScatterLifetimes();
    std::erase_if(lifetimes, [](const auto& entry) { return entry.second.expired(); });
    auto& entry = lifetimes[lifetime];
    if (auto shared = entry.lock()) return shared;
    auto shared = std::make_shared<uint64_t>(lifetime);
    entry = shared;
    return shared;
}

bool IsPaintOwnerTransform(const scene::GameObject& node)
{
    return !node.GetParent() && node.transform.position == math::Vector3::ZERO
        && node.transform.rotation == math::Quaternion::Identity() && node.transform.scale == math::Vector3::ONE;
}

struct ScatterChange {
    std::string node, newName, diskPath;
    std::optional<scene::InstancedMeshComponent> before;
    scene::InstancedMeshComponent after;
    std::shared_ptr<uint64_t> lifetime;
    std::shared_ptr<const renderer::InstanceSetData> beforeSnapshot, afterSnapshot;
    std::shared_ptr<asset::MeshInstancesAsset> beforeAsset, afterAsset;
};

asset::MeshInstancesAsset* ResolvePlacement(const std::string& path)
{
    if (util::FileSystem::GetExtension(asset::AssetManager::ResolveAssetPath(path)) != ".meshinstances") return nullptr;
    return asset::AssetManager::Get(asset::AssetManager::Load<asset::MeshInstancesAsset>(path));
}

void PublishPlacementDraft(const ScatterChange& change, const std::shared_ptr<asset::MeshInstancesAsset>& value)
{
    if (auto* current = ResolvePlacement(change.diskPath)) *current = *value;
    AssetDirtyRegistry::RegisterDraft(change.diskPath, change.after.m_instanceSetRef.path, "MESH INSTANCES", value,
        [path = change.diskPath](const asset::MeshInstancesAsset& draft) {
            const auto* current = ResolvePlacement(path);
            return current && asset::MeshInstancesAssetsEqual(*current, draft) && asset::SaveMeshInstancesAsset(path, draft);
        }, [path = change.diskPath] { (void)asset::AssetManager::ReloadPath(path); });
}

class ScatterCommand final : public ICommand {
public:
    ScatterCommand(EditorContext& context, std::vector<ScatterChange> changes)
        : m_context(context), m_scene(context.activeScene), m_sceneGeneration(m_scene->GetRenderSceneGeneration()),
          m_discardEpoch(AssetDirtyRegistry::DiscardEpoch()), m_changes(std::move(changes)) {}

    void Execute() override { std::string reason; (void)TryExecute(reason); }
    void Undo() override { std::string reason; (void)TryUndo(reason); }
    bool TryExecute(std::string& reason) override { return Apply(true, reason); }
    bool TryUndo(std::string& reason) override { return Apply(false, reason); }
    bool SupportsLegacyComposition() const override { return false; }
    std::string GetDescription() const override { return "Scatter Terrain Instances"; }
    bool ValidateHistory(std::string& reason) const override
    {
        if (m_context.activeScene != m_scene || m_scene->GetRenderSceneGeneration() != m_sceneGeneration
            || AssetDirtyRegistry::DiscardEpoch() != m_discardEpoch) {
            reason = "配置したシーンまたは未保存データの世代が変わりました。"; return false;
        }
        for (const auto& change : m_changes) {
            const auto* currentAsset = ResolvePlacement(change.diskPath);
            const auto& expectedAsset = m_applied || !change.beforeAsset ? change.afterAsset : change.beforeAsset;
            if (!currentAsset || !expectedAsset || !asset::MeshInstancesAssetsEqual(*currentAsset, *expectedAsset)) {
                reason = "配置アセットの内容が変更されています。"; return false;
            }
            auto* node = m_scene->FindByGuid(change.node);
            if (!m_applied && !change.newName.empty() && !node) continue;
            const auto* component = node ? node->GetComponent<scene::InstancedMeshComponent>() : nullptr;
            const auto* expected = m_applied ? &change.after : (change.before ? &*change.before : nullptr);
            const auto& snapshot = m_applied ? change.afterSnapshot : change.beforeSnapshot;
            if (!node || (!m_applied && !change.newName.empty()) || (expected
                    ? (!component || !SameComponent(*component, *expected, snapshot)
                        || m_scene->GetComponentLifetime<scene::InstancedMeshComponent>(node->GetID()) != *change.lifetime)
                    : component != nullptr)) {
                reason = "配置先の Component または配置アセットが変更されています。"; return false;
            }
        }
        return true;
    }
private:
    bool Apply(bool after, std::string& reason)
    {
        if (after == m_applied || !ValidateHistory(reason)) return false;
        size_t createCount = 0;
        for (const auto& change : m_changes) {
            if (after) {
                if (!util::FileSystem::Exists(util::FileSystem::PathFromUtf8(change.diskPath))
                    || (!change.beforeAsset && change.after.Snapshot() != change.afterSnapshot)) {
                    reason = "生成した配置アセットが削除または変更されています。"; return false;
                }
                if (!change.newName.empty()) ++createCount;
            } else if (!change.newName.empty()) {
                auto* node = m_scene->FindByGuid(change.node);
                bool edited = node->GetChildCount() != 0 || !IsPaintOwnerTransform(*node)
                    || node->name != change.newName || node->tag != "Untagged" || node->layer != 0 || !node->activeSelf();
                scene::ForEachRegisteredComponent([&]<typename T, typename Registration>() {
                    if constexpr (!std::is_same_v<T, scene::InstancedMeshComponent>)
                        edited = edited || node->GetComponent<T>() != nullptr;
                });
                if (edited) {
                    reason = "自動生成した配置先が編集されているため削除できません。"; return false;
                }
            }
        }
        if (createCount && !m_scene->CanCreateGameObjects(createCount)) {
            reason = "配置先の GameObject 上限を超えます。"; return false;
        }
        for (const auto& change : m_changes) {
            if (after) PublishPlacementDraft(change, change.afterAsset);
            else if (change.beforeAsset) PublishPlacementDraft(change, change.beforeAsset);
            else AssetDirtyRegistry::MarkClean(change.diskPath);
        }
        for (const auto& change : m_changes) {
            auto* node = m_scene->FindByGuid(change.node);
            if (after && !node) {
                node = &m_scene->CreateGameObject(change.newName);
                node->instanceId = change.node;
            }
            if (!after && !change.newName.empty()) {
                m_scene->DestroyGameObject(node->GetID());
                continue;
            }
            const auto* value = after ? &change.after : (change.before ? &*change.before : nullptr);
            if (value) {
                if (auto* current = node->GetComponent<scene::InstancedMeshComponent>()) *current = *value;
                else {
                    node->AddComponent<scene::InstancedMeshComponent>(*value);
                    if (*change.lifetime) ScatterLifetimes().erase(*change.lifetime);
                    *change.lifetime = m_scene->GetComponentLifetime<scene::InstancedMeshComponent>(node->GetID());
                    ScatterLifetimes()[*change.lifetime] = change.lifetime;
                }
            } else node->RemoveComponent<scene::InstancedMeshComponent>();
        }
        m_applied = after;
        if (!after) PruneSelection(m_context);
        if (m_context.markSceneDirty) m_context.markSceneDirty();
        return true;
    }
    EditorContext& m_context;
    scene::Scene* m_scene;
    uint64_t m_sceneGeneration, m_discardEpoch;
    std::vector<ScatterChange> m_changes;
    bool m_applied = false;
};

bool ReadBrushPoints(const OpData& stroke, std::vector<math::Vector3>& points)
{
    if (!stroke.IsArray() || stroke.AsArray().empty() || stroke.AsArray().size() > TERRAIN_SCATTER_STROKE_LIMIT) return false;
    points.reserve(stroke.AsArray().size());
    for (const auto& value : stroke.AsArray()) {
        if (value.IsVec3()) {
            points.push_back(value.AsVec3());
            continue;
        }
        if (!value.IsArray() || value.AsArray().size() != 3) return false;
        const auto& xyz = value.AsArray();
        float coordinates[3]{};
        for (size_t i = 0; i < 3; ++i) {
            if (xyz[i].IsInt()) coordinates[i] = static_cast<float>(xyz[i].AsInt64());
            else if (!xyz[i].TryGetFloat(coordinates[i])) return false;
        }
        points.emplace_back(coordinates[0], coordinates[1], coordinates[2]);
    }
    return true;
}

bool ReadSettings(const OpArgs& args, TerrainScatterSettings& settings)
{
    settings.spacing = args.GetFloat("spacing", settings.spacing);
    settings.density = args.GetFloat("density", settings.density);
    const int seed = args.GetInt("seed", 1), maxInstances = args.GetInt("maxInstances", 100000);
    if (seed < 0 || maxInstances < 1) return false;
    settings.seed = static_cast<uint32_t>(seed); settings.maxInstances = static_cast<uint32_t>(maxInstances);
    settings.minHeight = args.GetFloat("minHeight", settings.minHeight); settings.maxHeight = args.GetFloat("maxHeight", settings.maxHeight);
    settings.minSlope = args.GetFloat("minSlope", settings.minSlope); settings.maxSlope = args.GetFloat("maxSlope", settings.maxSlope);
    settings.layer = args.GetInt("layer", settings.layer); settings.layerThreshold = args.GetFloat("layerThreshold", settings.layerThreshold);
    settings.minScale = args.GetFloat("minScale", settings.minScale); settings.maxScale = args.GetFloat("maxScale", settings.maxScale);
    settings.minYaw = args.GetFloat("minYaw", settings.minYaw); settings.maxYaw = args.GetFloat("maxYaw", settings.maxYaw);
    settings.alignToNormal = args.GetBool("alignToNormal", settings.alignToNormal);
    settings.alignDownslope = args.GetBool("alignDownslope", settings.alignDownslope);
    settings.downslopeJitter = args.GetFloat("downslopeJitter", settings.downslopeJitter);
    settings.embedDepth = args.GetFloat("embedDepth", settings.embedDepth);
    settings.minimumDistance = args.GetFloat("minimumDistance", settings.minimumDistance);
    return true;
}

std::optional<OpResult> StageSet(EditorContext& ctx, const asset::InstanceSetAsset& definition,
    std::span<const renderer::InstancePlacement> placements, const std::string& folder,
    const scene::InstancedMeshComponent* previous, ScatterChange& change, const std::string& source = {}, bool reusable = true)
{
    asset::MeshInstancesAsset staged;
    bool reuse = reusable && previous && util::FileSystem::IsChildPathText(
            asset::AssetManager::ResolveAssetPath(previous->m_instanceSetRef.path), ctx.projectRoot + "/Assets")
        && util::FileSystem::GetExtension(asset::AssetManager::ResolveAssetPath(previous->m_instanceSetRef.path)) == ".meshinstances";
    if (reuse) {
        for (auto id : ctx.activeScene->GetEntities<scene::InstancedMeshComponent>()) {
            const auto* node = ctx.activeScene->GetGameObject(id);
            const auto* other = ctx.activeScene->GetComponent<scene::InstancedMeshComponent>(id);
            if (node && node->instanceId != change.node && other
                && asset::AssetManager::ResolveAssetPath(other->m_instanceSetRef.path)
                    == asset::AssetManager::ResolveAssetPath(previous->m_instanceSetRef.path)) reuse = false;
        }
    }
    std::string reference;
    if (reuse) {
        change.diskPath = asset::AssetManager::ResolveAssetPath(previous->m_instanceSetRef.path);
        const auto* current = ResolvePlacement(change.diskPath);
        if (!current) return OpResult::Err("LOAD_FAILED", "現在の配置を読み込めません。");
        change.beforeAsset = std::make_shared<asset::MeshInstancesAsset>(*current);
        staged = *current;
        reference = previous->m_instanceSetRef.path;
    }
    if (!staged.instances.Replace(definition.Parts(), definition.LodHeights(), placements))
        return OpResult::Err("INVALID_TEMPLATE", "配置またはテンプレートの mesh・材質・LOD が不正です。");
    staged.terrainPaintRef = source;
    if (!reuse) {
        const auto relative = folder + asset::AssetDatabase::GenerateGuid() + ".meshinstances";
        change.diskPath = util::FileSystem::PathToUtf8(util::FileSystem::PathFromUtf8(ctx.projectRoot) / relative);
        if (!asset::SaveMeshInstancesAsset(change.diskPath, staged))
            return OpResult::Err("WRITE_FAILED", "生成した配置を保存できませんでした: " + change.diskPath);
        const auto guid = asset::AssetDatabase::GuidFromPath(change.diskPath);
        if (guid.empty()) return OpResult::Err("META_FAILED", "配置の .meta を作成できませんでした: " + change.diskPath);
        reference = "guid:" + guid + "|" + relative;
        const auto* loaded = ResolvePlacement(change.diskPath);
        if (!loaded) return OpResult::Err("LOAD_FAILED", "生成した配置を読み込めませんでした: " + change.diskPath);
        staged = *loaded;
    }
    change.afterAsset = std::make_shared<asset::MeshInstancesAsset>(std::move(staged));
    if (previous) change.before = *previous;
    change.beforeSnapshot = previous ? previous->Snapshot() : nullptr;
    change.after = previous ? *previous : scene::InstancedMeshComponent{};
    change.after.m_instanceSetRef = {reference, asset::InstanceSetAsset::TYPE_NAME};
    change.after.ResetOverrides();
    change.afterSnapshot = change.afterAsset->instances.Snapshot();
    if (!change.afterSnapshot)
        return OpResult::Err("LOAD_FAILED", "生成した配置を読み込めませんでした: " + change.diskPath);
    return std::nullopt;
}

OpResult PublishStroke(EditorContext& ctx, std::vector<ScatterChange> changes, size_t count, size_t changed, bool erase)
{
    OpResult result;
    if (changes.empty()) {
        result.noChange = true;
        result.message = "ブラシ範囲の配置は変わりませんでした。";
        return result;
    }
    const std::string reference = changes.front().after.m_instanceSetRef.path;
    auto command = std::make_unique<ScatterCommand>(ctx, std::move(changes));
    std::string reason;
    if (!command->TryExecute(reason)) return OpResult::Err("SCATTER_FAILED", reason);
    ctx.requestAssetBrowserRefresh = true;
    result.command = std::move(command);
    result.data = OpData::MakeObject();
    result.data.Set("count", OpData(static_cast<uint32_t>(count)));
    result.data.Set("assetPath", OpData(reference));
    result.message = std::to_string(changed) + (erase ? " 個を消去しました。" : " 個を配置しました。")
        + "シーンを保存してください。";
    return result;
}

OpResult Scatter(OpContext& context, const OpArgs& args)
{
    auto& ctx = context.ctx;
    auto* terrainNode = ctx.activeScene->FindByGuid(args.GetString("terrain"));
    auto* targetNode = ctx.activeScene->FindByGuid(args.GetString("target"));
    auto* terrain = terrainNode ? terrainNode->GetComponent<scene::TerrainComponent>() : nullptr;
    if (!terrain || !terrain->enabled || !targetNode)
        return OpResult::Err("NO_TARGET", "有効な Terrain と配置先 GameObject を指定してください。");
    const auto* previous = targetNode->GetComponent<scene::InstancedMeshComponent>();
    const auto* stroke = args.GetData("stroke");
    std::vector<math::Vector3> points;
    if (stroke && !ReadBrushPoints(*stroke, points))
        return OpResult::Err("BAD_ARG", "stroke は 1〜512 個の XYZ 座標で指定してください。");
    const bool erase = args.GetBool("erase");
    if (erase && !stroke) return OpResult::Err("BAD_ARG", "消去には stroke が必要です。");
    const auto previousReference = previous ? previous->m_instanceSetRef.path : std::string{};
    if (erase && previousReference.empty()) {
        OpResult result;
        result.noChange = true;
        result.message = "消去する配置がありません。";
        return result;
    }
    const std::string reference = args.GetString("source");
    const std::string source = (reference.empty() || (stroke && !previousReference.empty())) ? previousReference : reference;
    auto* definition = source.empty() ? nullptr : ResolveSet(source);
    if (!definition || !definition->Snapshot())
        return OpResult::Err("NO_TEMPLATE", "有効な .terrainpaint / .meshinstances または旧配置素材を指定してください。");
    TerrainScatterSettings settings;
    if (!ReadSettings(args, settings)) return OpResult::Err("BAD_ARG", "seed または maxInstances が範囲外です。");
    std::vector<renderer::InstancePlacement> placements;
    if (stroke && !previousReference.empty()) {
        const auto snapshot = previous->Snapshot();
        if (!snapshot) return OpResult::Err("INVALID_TEMPLATE", "現在の配置を読み込めません。");
        for (const auto& placement : snapshot->placements)
            if (placement.active) placements.push_back(placement);
    }
    const size_t oldCount = placements.size();
    const TerrainScatterBrush brush{points, args.GetFloat("brushRadius", 5.0f), erase};
    std::string reason;
    scene::FlushWorldTransforms(*ctx.activeScene);
    if (!BuildTerrainScatter(*terrain, terrainNode->transform, targetNode->transform, settings, placements, reason,
            stroke ? &brush : nullptr))
        return OpResult::Err("INVALID_SCATTER", reason);
    if (stroke && oldCount == placements.size()) return PublishStroke(ctx, {}, 0, 0, erase);
    ScatterChange change;
    change.node = targetNode->instanceId;
    const auto lifetime = ctx.activeScene->GetComponentLifetime<scene::InstancedMeshComponent>(targetNode->GetID());
    change.lifetime = lifetime ? ScatterLifetime(lifetime) : std::make_shared<uint64_t>(0);
    if (auto failure = StageSet(ctx, *definition, placements, "Assets/Terrain/Scatter/", previous, change, reference, stroke != nullptr))
        return std::move(*failure);
    const size_t changed = erase ? oldCount - placements.size() : placements.size() - oldCount;
    std::vector<ScatterChange> changes;
    changes.push_back(std::move(change));
    return PublishStroke(ctx, std::move(changes), placements.size(), changed, erase);
}

struct PaintSet {
    std::string guid, name, ownerId, reference;
    const asset::InstanceSetAsset* definition = nullptr;
    scene::GameObject* owner = nullptr;
    std::vector<renderer::InstancePlacement> placements;
    size_t beforeCount = 0;
    size_t sourceIndex = 0;
    float weight = 1.0f;
    const asset::TerrainPaintAsset* paintAsset = nullptr;
    std::vector<renderer::InstancePlacement> removed;
};

struct PaintPlan {
    std::string folder;
    std::vector<PaintSet> sets;
    bool erase = false;
    TerrainPaintDiagnostics diagnostics;
};

OpResult PlanPaint(EditorContext& ctx, const OpArgs& args, PaintPlan& plan)
{
    if (!ctx.activeScene) return OpResult::Err("NO_TARGET", "シーンがありません。");
    auto& scene = *ctx.activeScene;
    auto* terrainNode = scene.FindByGuid(args.GetString("terrain"));
    const auto* terrain = terrainNode ? terrainNode->GetComponent<scene::TerrainComponent>() : nullptr;
    if (!terrain || !terrain->enabled) return OpResult::Err("NO_TARGET", "有効な Terrain を指定してください。");
    const auto* sources = args.GetData("sources");
    const auto* stroke = args.GetData("stroke");
    std::vector<math::Vector3> points;
    if (!sources || !sources->IsArray() || sources->AsArray().empty()
        || sources->AsArray().size() > TERRAIN_SCATTER_PALETTE_LIMIT || !stroke || !ReadBrushPoints(*stroke, points))
        return OpResult::Err("BAD_ARG", "素材とストロークを指定してください。");
    TerrainScatterSettings settings;
    if (!ReadSettings(args, settings)) return OpResult::Err("BAD_ARG", "seed または maxInstances が範囲外です。");
    const auto* weights = args.GetData("weights");
    if (weights && (!weights->IsArray() || weights->AsArray().size() != sources->AsArray().size()))
        return OpResult::Err("BAD_ARG", "weights は素材と同じ件数で指定してください。");
    plan.erase = args.GetBool("erase");
    const TerrainScatterBrush brush{points, args.GetFloat("brushRadius", 5.0f), plan.erase, args.GetBool("polygon"), args.GetFloat("feather", 0)};
    plan.folder = "Assets/Terrain/Scatter/Painted/" + asset::AssetDatabase::DeriveGuid(terrainNode->instanceId, "Terrain Paint") + "/";
    auto& palette = plan.sets;
    for (size_t index = 0; index < sources->AsArray().size(); ++index) {
        float weight = 1.0f;
        if (weights) {
            const auto& value = weights->AsArray()[index];
            if (value.IsInt()) weight = static_cast<float>(value.AsInt64());
            else if (!value.TryGetFloat(weight)) return OpResult::Err("BAD_ARG", "配合比は数値で指定してください。");
        }
        if (!std::isfinite(weight) || weight < 0.0f || weight > 100.0f)
            return OpResult::Err("BAD_ARG", "配合比は 0〜100 で指定してください。");
        const auto reference = sources->AsArray()[index].AsString();
        const auto guid = asset::AssetDatabase::GuidFromRef(reference);
        const auto sourcePath = asset::AssetDatabase::PathFromGuid(guid);
        const auto* definition = sourcePath.empty() ? nullptr : ResolveSet(reference);
        if (!definition || !definition->Snapshot()
            || util::FileSystem::IsChildPathText(sourcePath, ctx.projectRoot + "/Assets/Terrain/Scatter/Painted"))
            return OpResult::Err("NO_TEMPLATE", "有効な Terrain Paint 素材を選んでください。");
        if (std::any_of(palette.begin(), palette.end(), [&](const PaintSet& set) { return set.guid == guid; })) continue;
        PaintSet set;
        set.guid = guid;
        set.reference = reference;
        if (util::FileSystem::GetExtension(sourcePath) == ".terrainpaint")
            set.paintAsset = asset::AssetManager::Get(asset::AssetManager::Load<asset::TerrainPaintAsset>(reference));
        set.sourceIndex = index;
        set.weight = weight;
        set.name = util::FileSystem::PathToUtf8(util::FileSystem::PathFromUtf8(sourcePath).stem());
        if (set.paintAsset) set.name = set.paintAsset->name;
        set.definition = definition;
        set.ownerId = asset::AssetDatabase::DeriveGuid(terrainNode->instanceId, "Terrain Paint/" + guid);
        set.owner = scene.FindByGuid(set.ownerId);
        if (set.owner) {
            const auto* current = set.owner->GetComponent<scene::InstancedMeshComponent>();
            const auto snapshot = current ? current->Snapshot() : nullptr;
            const auto* placement = current ? ResolvePlacement(current->m_instanceSetRef.path) : nullptr;
            if (!snapshot || !placement || asset::AssetDatabase::GuidFromRef(placement->terrainPaintRef) != guid
                || (!plan.erase && !IsPaintOwnerTransform(*set.owner)))
                return OpResult::Err("PLACEMENT_CHANGED", "配置先が編集されています。配置先の参照と Transform を確認してください。");
            set.definition = ResolveSet(current->m_instanceSetRef.path);
            if (!set.definition) return OpResult::Err("NO_TEMPLATE", "現在の配置定義を読み込めません。");
            for (const auto& item : snapshot->placements)
                if (item.active) set.placements.push_back(item);
        }
        set.beforeCount = set.placements.size();
        palette.push_back(std::move(set));
    }
    std::sort(palette.begin(), palette.end(), [](const PaintSet& a, const PaintSet& b) { return a.guid < b.guid; });
    scene::FlushWorldTransforms(scene);
    std::string reason;
    std::vector<renderer::InstancePlacement> validation;
    auto validationSettings=settings; validationSettings.density=0;
    if(!BuildTerrainScatter(*terrain,terrainNode->transform,scene::Transform{},validationSettings,validation,reason,&brush))
        return OpResult::Err("INVALID_SCATTER",reason);
    if (plan.erase) {
        /// @note 移行済み配置は来歴か部品の一致で選ぶ。葉と幹を分けた旧配置も同じ素材で編集できる。
        const auto selected = palette;
        for (const auto id : scene.GetEntities<scene::InstancedMeshComponent>()) {
            auto* owner = scene.GetGameObject(id);
            const auto* component = scene.GetComponent<scene::InstancedMeshComponent>(id);
            if (!owner || !owner->activeInHierarchy() || !component || !component->m_enabled
                || std::any_of(palette.begin(), palette.end(), [&](const auto& set) { return set.owner == owner; })) continue;
            const auto* definition = ResolveSet(component->m_instanceSetRef.path);
            const auto snapshot = component->Snapshot();
            if (!definition || !snapshot) continue;
            const auto* placement = ResolvePlacement(component->m_instanceSetRef.path);
            for (const auto& source : selected) {
                const bool provenance = placement && asset::AssetDatabase::GuidFromRef(placement->terrainPaintRef) == source.guid;
                const auto parts = definition->Parts();
                const auto expected = source.definition->Parts();
                const bool matching = !parts.empty() && std::all_of(parts.begin(), parts.end(), [&](const auto& part) {
                    return std::any_of(expected.begin(), expected.end(), [&](const auto& other) {
                        return part.meshPath == other.meshPath && part.materialPath == other.materialPath && part.lod == other.lod;
                    });
                });
                const bool knownSource = placement && !asset::AssetDatabase::GuidFromRef(placement->terrainPaintRef).empty();
                if (!provenance && (knownSource || !matching)) continue;
                auto set = source;
                set.owner = owner; set.ownerId = owner->instanceId; set.definition = definition;
                set.placements.clear();
                for (const auto& item : snapshot->placements) if (item.active) set.placements.push_back(item);
                set.beforeCount = set.placements.size();
                palette.push_back(std::move(set));
                break;
            }
        }
        for (auto& set : palette) {
            if (!set.owner) continue;
            const auto before = set.placements;
            if (!BuildTerrainScatter(*terrain, terrainNode->transform, set.owner->transform, settings, set.placements, reason, &brush))
                return OpResult::Err("INVALID_SCATTER", reason);
            std::unordered_set<uint32_t> retained;
            for (const auto& item : set.placements) retained.insert(item.id);
            for (const auto& item : before) if (!retained.contains(item.id)) set.removed.push_back(item);
        }
    } else {
        double totalWeight = 0.0;
        for (const auto& set : palette) totalWeight += set.weight;
        if (totalWeight == 0.0) return OpResult::Ok();
        const bool equalWeights = std::all_of(palette.begin(), palette.end(), [&](const PaintSet& set) {
            return set.weight == palette.front().weight;
        });
        std::vector<renderer::InstancePlacement> candidates;
        std::vector<bool> groundCover;
        float margin = std::max(settings.spacing, settings.minimumDistance);
        for (const auto& set : palette) if (set.paintAsset) margin = std::max(margin, set.paintAsset->placement.spacing);
        const auto inverseTerrainRotation = terrainNode->transform.worldRotation.Inverse();
        const auto project = [&](const math::Vector3& p) { return inverseTerrainRotation * (p - terrainNode->transform.worldPosition); };
        math::Vector3 low{INFINITY, 0, INFINITY}, high{-INFINITY, 0, -INFINITY};
        for (const auto& point : points) {
            const auto p = project(point);
            low.x = std::min(low.x, p.x); low.z = std::min(low.z, p.z);
            high.x = std::max(high.x, p.x); high.z = std::max(high.z, p.z);
        }
        margin += brush.radius;
        for (const auto id : scene.GetEntities<scene::InstancedMeshComponent>()) {
            const auto* owner = scene.GetGameObject(id);
            const auto* component = scene.GetComponent<scene::InstancedMeshComponent>(id);
            if (!owner || !owner->activeInHierarchy() || !component || !component->m_enabled) continue;
            const auto snapshot = component->Snapshot();
            if (!snapshot) continue;
            const auto* stored = ResolvePlacement(component->m_instanceSetRef.path);
            const auto* paint = stored && util::FileSystem::GetExtension(asset::AssetManager::ResolveAssetPath(stored->terrainPaintRef)) == ".terrainpaint"
                ? asset::AssetManager::Get(asset::AssetManager::Load<asset::TerrainPaintAsset>(stored->terrainPaintRef)) : nullptr;
            const bool grass = paint && paint->category == asset::TerrainPaintCategory::GRASS;
            const auto world = owner->transform.GetWorldMatrix();
            for (const auto& item : snapshot->placements) {
                if (!item.active) continue;
                auto obstacle = item;
                const auto position = world * math::Vector4{item.position.x, item.position.y, item.position.z, 1.0f};
                obstacle.position = {position.x, position.y, position.z};
                const auto p = project(obstacle.position);
                if (p.x < low.x - margin || p.x > high.x + margin || p.z < low.z - margin || p.z > high.z + margin) continue;
                candidates.push_back(obstacle);
                groundCover.push_back(grass);
            }
        }
        const size_t existingCount = candidates.size();
        std::vector<std::pair<const scene::WaterComponent*, const scene::Transform*>> water;
        if (args.GetBool("avoidWater", true)) for (const auto id : scene.GetEntities<scene::WaterComponent>()) {
            const auto* owner = scene.GetGameObject(id);
            const auto* component = scene.GetComponent<scene::WaterComponent>(id);
            if (owner && owner->activeInHierarchy() && component && component->enabled)
                water.emplace_back(component, &owner->transform);
        }
        const int excludedLayer = args.GetInt("excludedLayer", -1);
        if (excludedLayer < -1 || excludedLayer >= terrain->LayerCount())
            return OpResult::Err("BAD_ARG", "除外する地形レイヤーが範囲外です。");
        std::vector<int> excludedLayers;
        if (excludedLayer >= 0) excludedLayers.push_back(excludedLayer);
        if (args.GetBool("avoidPaths", true)) for (int i = 0; i < terrain->LayerCount(); ++i) {
            auto name = asset::AssetManager::ResolveAssetPath(terrain->layerMaterials[i]);
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const auto stem = util::FileSystem::PathToUtf8(util::FileSystem::PathFromUtf8(name).stem());
            if (stem.find("path") != std::string::npos || stem.find("road") != std::string::npos || stem.find("trail") != std::string::npos)
                excludedLayers.push_back(i);
        }
        if(const auto* materials=args.GetData("excludedMaterials")) {
            if(!materials->IsArray()||materials->AsArray().size()>256)return OpResult::Err("BAD_ARG","除外する地表素材を確認してください。");
            for(const auto& ref:materials->AsArray()) {
                if(!ref.IsString()||ref.AsString().empty())return OpResult::Err("BAD_ARG","除外する地表素材が不正です。");
                const auto guid=asset::AssetDatabase::GuidFromRef(ref.AsString());
                const auto path=asset::AssetManager::ResolveAssetPath(ref.AsString());
                for(int i=0;i<terrain->LayerCount();++i) {
                    if((!guid.empty()&&asset::AssetDatabase::GuidFromRef(terrain->layerMaterials[i])==guid)
                        ||(!path.empty()&&util::FileSystem::SamePathText(asset::AssetManager::ResolveAssetPath(terrain->layerMaterials[i]),path)))excludedLayers.push_back(i);
                }
            }
        }
        std::sort(excludedLayers.begin(),excludedLayers.end());excludedLayers.erase(std::unique(excludedLayers.begin(),excludedLayers.end()),excludedLayers.end());
        float maxWater=0,maxPath=0,sourceWater=0,sourcePath=0;
        for(const auto& set:palette)if(set.paintAsset){maxWater=std::max(maxWater,set.paintAsset->placement.waterClearance);maxPath=std::max(maxPath,set.paintAsset->placement.pathClearance);}
        const auto clearance=BuildTerrainPaintClearance(*terrain,terrainNode->transform,brush,excludedLayers,water,maxWater,maxPath);
        struct Obstacle { math::Matrix4 inverse; math::Vector3 center, extents; };
        std::vector<Obstacle> obstacles;
        for (const auto id : scene.GetEntities<scene::MeshRenderer>()) {
            const auto* owner = scene.GetGameObject(id);
            const auto* meshRenderer = scene.GetComponent<scene::MeshRenderer>(id);
            if (!owner || !owner->activeInHierarchy() || !meshRenderer || !meshRenderer->enabled) continue;
            if (const auto* material = scene.GetComponent<scene::MaterialComponent>(id)) {
                if (!material->enabled || !material->IsVisible()) continue;
                const auto snapshot = material->ReadAuthoring();
                if (!snapshot) continue;
                const auto& values = snapshot->Data();
                const auto* definition = asset::AssetManager::Get(material->GetMaterialAsset());
                if (!definition && !material->GetMaterialPath().empty())
                    definition = asset::AssetManager::Get(asset::AssetManager::Load<asset::MaterialAsset>(material->GetMaterialPath()));
                const auto blend = values.hasBlendModeOverride ? values.blendModeOverride
                    : definition ? definition->Authoring().blendMode : renderer::BlendMode::OPAQUE_BLEND;
                if (blend != renderer::BlendMode::OPAQUE_BLEND) continue;
            }
            const auto scale = owner->transform.worldScale;
            if (!std::isfinite(scale.x) || !std::isfinite(scale.y) || !std::isfinite(scale.z)
                || std::abs(scale.x) < 0.00001f || std::abs(scale.y) < 0.00001f || std::abs(scale.z) < 0.00001f) continue;
            const auto* mesh = meshRenderer->mesh;
            if (!mesh && ctx.resources) mesh = scene::ResolveMeshPath(meshRenderer->meshPath, *ctx.resources);
            if (!mesh || mesh->boundsRadius <= 0.0f) continue;
            obstacles.push_back({math::Matrix4::Inverse(owner->transform.GetWorldMatrix()), mesh->boundsCenter, mesh->boundsExtents});
        }
        const auto classify = [&](const math::Vector3& position) {
            for (const auto& [surface, transform] : water) {
                scene::WaterSurfaceHeightSample sample;
                if (scene::TrySampleWaterSurfaceHeightWorld(*surface, *transform, position, 0.0f, sample)
                    && sample.insideSurface && !sample.excluded && position.y <= sample.position.y + 0.15f) return TerrainPaintReason::WATER;
            }
            if(clearance.NearWater(position,sourceWater))return TerrainPaintReason::WATER;
            if(clearance.NearPath(position,sourcePath))return TerrainPaintReason::PATH;
            for (const auto& obstacle : obstacles) {
                const auto local = obstacle.inverse * math::Vector4{position + math::Vector3{0, 0.5f, 0}, 1};
                const auto delta = math::Vector3{local.x, local.y, local.z} - obstacle.center;
                if (std::abs(delta.x) <= obstacle.extents.x && std::abs(delta.z) <= obstacle.extents.z
                    && std::abs(delta.y) <= obstacle.extents.y) return TerrainPaintReason::OBSTACLE;
            }
            if (!excludedLayers.empty()) {
                const auto point = terrainNode->transform.worldRotation.Inverse() * (position - terrainNode->transform.worldPosition);
                const math::Vector3 local{point.x / terrainNode->transform.worldScale.x,
                    point.y / terrainNode->transform.worldScale.y, point.z / terrainNode->transform.worldScale.z};
                const float gx = local.x / terrain->cellSize, gz = local.z / terrain->cellSize;
                const int x = std::clamp(static_cast<int>(gx), 0, terrain->columns - 2);
                const int z = std::clamp(static_cast<int>(gz), 0, terrain->rows - 2);
                const float fx = std::clamp(gx - x, 0.0f, 1.0f), fz = std::clamp(gz - z, 0.0f, 1.0f);
                for (int layer : excludedLayers) {
                    const auto weight = [&](int dx, int dz) { return terrain->GetLayerWeightAtGrid(x + dx, z + dz, layer); };
                    if (std::lerp(std::lerp(weight(0, 0), weight(1, 0), fx), std::lerp(weight(0, 1), weight(1, 1), fx), fz) >= 0.35f) return TerrainPaintReason::PATH;
                }
            }
            return TerrainPaintReason::ACCEPTED;
        };
        const bool assetSettings = std::any_of(palette.begin(), palette.end(), [](const PaintSet& set) { return set.paintAsset != nullptr; });
        if (assetSettings) {
            for (auto& set : palette) {
                if (set.weight == 0.0f) continue;
                sourceWater=sourcePath=0;
                auto sourceSettings = settings;
                if (set.paintAsset) {
                    const auto& p = set.paintAsset->placement;
                    sourceWater=p.waterClearance;sourcePath=p.pathClearance;
                    sourceSettings.spacing = p.spacing;
                    sourceSettings.density *= p.density;
                    sourceSettings.minScale = p.minScale; sourceSettings.maxScale = p.maxScale;
                    sourceSettings.minYaw = p.minYaw; sourceSettings.maxYaw = p.maxYaw;
                    sourceSettings.minSlope = p.minSlope; sourceSettings.maxSlope = p.maxSlope;
                    sourceSettings.alignToNormal = p.alignToNormal; sourceSettings.alignDownslope = p.alignDownslope;
                    sourceSettings.downslopeJitter = p.downslopeJitter; sourceSettings.embedDepth = p.embedDepth;
                }
                if (sourceSettings.minSlope > sourceSettings.maxSlope) continue;
                sourceSettings.density *= static_cast<float>(set.weight / totalWeight);
                const bool grass = set.paintAsset && set.paintAsset->category == asset::TerrainPaintCategory::GRASS;
                if (set.paintAsset && set.paintAsset->category == asset::TerrainPaintCategory::TREE)
                    sourceSettings.minimumDistance = std::max(sourceSettings.minimumDistance, sourceSettings.spacing * 0.45f);
                for (unsigned char character : set.guid) sourceSettings.seed = sourceSettings.seed * 16777619u ^ character;
                std::vector<renderer::InstancePlacement> sourceCandidates;
                for (size_t i = 0; i < candidates.size(); ++i) if (groundCover[i] == grass) sourceCandidates.push_back(candidates[i]);
                const size_t before = sourceCandidates.size();
                if (!BuildTerrainScatter(*terrain, terrainNode->transform, scene::Transform{}, sourceSettings, sourceCandidates, reason, &brush, {}, &plan.diagnostics, classify))
                    return OpResult::Err("INVALID_SCATTER", reason);
                if (set.placements.size() + sourceCandidates.size() - before > settings.maxInstances)
                    return OpResult::Err("INVALID_SCATTER", "配置数が上限を超えます。上限を増やしてください。");
                std::unordered_set<uint32_t> usedIds;
                for (const auto& item : set.placements) usedIds.insert(item.id);
                uint32_t nextId = 1;
                for (size_t i = before; i < sourceCandidates.size(); ++i) {
                    if (usedIds.insert(sourceCandidates[i].id).second) continue;
                    while (!usedIds.insert(nextId).second) ++nextId;
                    sourceCandidates[i].id = nextId++;
                }
                plan.diagnostics.sources[set.sourceIndex]+=sourceCandidates.size()-before;
                set.placements.insert(set.placements.end(), sourceCandidates.begin() + before, sourceCandidates.end());
                candidates.insert(candidates.end(), sourceCandidates.begin() + before, sourceCandidates.end());
                groundCover.insert(groundCover.end(), sourceCandidates.size() - before, grass);
            }
            return OpResult::Ok();
        }
        if (!BuildTerrainScatter(*terrain, terrainNode->transform, scene::Transform{}, settings, candidates, reason, &brush, {}, &plan.diagnostics, classify))
            return OpResult::Err("INVALID_SCATTER", reason);
        for (size_t index = existingCount; index < candidates.size(); ++index) {
            const auto& item = candidates[index];
            /// @note 配合を変えても候補位置と総密度は同じ。GUID 順の累積比だけで素材を割り当てる。
            uint32_t key = item.id;
            key ^= key >> 16;
            key *= 0x7feb352du;
            key ^= key >> 15;
            if (equalWeights) {
                ++plan.diagnostics.sources[palette[key % palette.size()].sourceIndex];
                palette[key % palette.size()].placements.push_back(item);
                continue;
            }
            key *= 0x846ca68bu;
            key ^= key >> 16;
            double choice = (static_cast<double>(key) / 4294967296.0) * totalWeight;
            for (auto& set : palette) {
                if (choice < set.weight) {
                    ++plan.diagnostics.sources[set.sourceIndex];
                    set.placements.push_back(item);
                    break;
                }
                choice -= set.weight;
            }
        }
    }
    return OpResult::Ok();
}

OpResult PaintPalette(OpContext& context, const OpArgs& args)
{
    auto& ctx = context.ctx;
    auto& scene = *ctx.activeScene;
    PaintPlan plan;
    if (auto result = PlanPaint(ctx, args, plan); !result.ok) return result;
    std::vector<ScatterChange> changes;
    size_t count = 0, changed = 0;
    for (const auto& set : plan.sets) {
        count += set.placements.size();
        if (set.placements.size() == set.beforeCount) continue;
        changed += plan.erase ? set.beforeCount - set.placements.size() : set.placements.size() - set.beforeCount;
        ScatterChange change;
        change.node = set.ownerId;
        const auto* previous = set.owner ? set.owner->GetComponent<scene::InstancedMeshComponent>() : nullptr;
        if (!set.owner) change.newName = set.name + " (Terrain Paint)";
        change.lifetime = set.owner
            ? ScatterLifetime(scene.GetComponentLifetime<scene::InstancedMeshComponent>(set.owner->GetID()))
            : std::make_shared<uint64_t>(0);
        if (auto failure = StageSet(ctx, *set.definition, set.placements, plan.folder + set.guid + "/", previous, change, set.reference))
            return std::move(*failure);
        changes.push_back(std::move(change));
    }
    return PublishStroke(ctx, std::move(changes), count, changed, plan.erase);
}
} /// @note namespace terrain_scatter_operators

OpResult CommitTerrainInstanceEdits(EditorContext& ctx, std::vector<TerrainInstanceEdit> edits, size_t affected)
{
    using namespace terrain_scatter_operators;
    if (!ctx.activeScene) return OpResult::Err("NO_TARGET", "シーンがありません。");
    std::vector<ScatterChange> changes;
    size_t count = 0;
    for (auto& edit : edits) {
        auto* owner = ctx.activeScene->FindByGuid(edit.owner);
        const auto* previous = owner ? owner->GetComponent<scene::InstancedMeshComponent>() : nullptr;
        if ((!previous && edit.newName.empty()) || (owner && !edit.newName.empty()))
            return OpResult::Err("PLACEMENT_CHANGED", "配置先が変更されています。");
        const auto* definition = ResolveSet(edit.definition);
        if (!definition) return OpResult::Err("NO_TEMPLATE", "配置素材を解決できません。");
        ScatterChange change; change.node = edit.owner; change.newName = edit.newName;
        change.lifetime = previous ? ScatterLifetime(ctx.activeScene->GetComponentLifetime<scene::InstancedMeshComponent>(owner->GetID()))
            : std::make_shared<uint64_t>(0);
        if (auto failure = StageSet(ctx, *definition, edit.placements, "Assets/Terrain/Scatter/Edited/", previous, change, edit.provenance))
            return std::move(*failure);
        count += edit.placements.size(); changes.push_back(std::move(change));
    }
    auto result = PublishStroke(ctx, std::move(changes), count, affected, false);
    if (result.ok && !result.noChange) result.message = std::to_string(affected) + " 個の配置を編集しました。保存で確定します。";
    return result;
}

bool BuildTerrainPaintPreview(EditorContext& ctx, const OpArgs& args, std::vector<TerrainPaintPreviewPoint>& points,
    size_t& total, std::string& reason, TerrainPaintDiagnostics* diagnostics)
{
    if(diagnostics)*diagnostics={};
    points.clear();
    total = 0;
    terrain_scatter_operators::PaintPlan plan;
    const auto result = terrain_scatter_operators::PlanPaint(ctx, args, plan);
    reason = result.message;
    if (!result.ok) return false;
    if(plan.erase)for(const auto& set:plan.sets)plan.diagnostics.sources[set.sourceIndex]+=set.removed.size();
    if(diagnostics)*diagnostics=std::move(plan.diagnostics);
    for (const auto& set : plan.sets) total += plan.erase ? set.removed.size() : set.placements.size() - set.beforeCount;
    /// @note プレビューの描画数は一定に保ち、ストローク全体から等間隔に拾う。
    constexpr size_t DISPLAY_LIMIT = 384;
    const size_t stride = std::max<size_t>(1, (total + DISPLAY_LIMIT - 1) / DISPLAY_LIMIT);
    size_t index = 0;
    for (const auto& set : plan.sets) {
        const auto& placements = plan.erase ? set.removed : set.placements;
        for (size_t i = plan.erase ? 0 : set.beforeCount; i < placements.size(); ++i) {
            if (index++ % stride != 0) continue;
            const auto world = set.owner ? set.owner->transform.GetWorldMatrix() : math::Matrix4::Identity();
            const auto p = world * math::Vector4{placements[i].position, 1.0f};
            points.push_back({{p.x, p.y, p.z}, set.sourceIndex, placements[i], world,
                set.owner ? set.owner->GetComponent<scene::InstancedMeshComponent>()->m_instanceSetRef.path : set.reference, plan.erase});
        }
    }
    return true;
}

void RegisterTerrainScatterOperators(OperatorRegistry& registry)
{
    RegisterTerrainPaintAssetOperators(registry);
    RegisterTerrainPaintMixOperators(registry);
    RegisterTerrainInstanceEditingOperators(registry);
    using namespace terrain_scatter_operators;
    EditorOperator op;
    op.id = "terrain.scatter"; op.label = "Scatter Trees and Rocks"; op.category = "Terrain";
    op.desc = "地形から木や岩を配置し、独立した配置アセットへ確定する。stroke 指定時は範囲を追加または消去する。";
    op.caution = "stroke 省略時は配置先を全置換する。Undo で戻せる。生成アセットは Undo 後も残る。";
    op.kind = OpKind::Mutation; op.undoLabel = "Scatter Terrain Instances";
    op.params = {
        Parameter("terrain", OpParamType::NodeId, std::string{}, "配置する Terrain の instanceId。"),
        Parameter("target", OpParamType::NodeId, std::string{}, "配置先の instanceId。空の GameObject にも追加可能。"),
        Parameter("source", OpParamType::String, std::string{}, "InstanceSetAsset の参照。空なら配置先の現在のアセット。"),
        Parameter("spacing", OpParamType::Float, 6.0f, "地形軸に沿った候補格子の幅 [world m]。0.25〜10000。"),
        Parameter("density", OpParamType::Float, 1.0f, "候補採用率 [0,1]。指定層の重みも乗算。"),
        Parameter("seed", OpParamType::Int, 1, "0 以上の再現可能な配置 seed。"),
        Parameter("minHeight", OpParamType::Float, -100000.0f, "最低標高 [world m]。"),
        Parameter("maxHeight", OpParamType::Float, 100000.0f, "最高標高 [world m]。"),
        Parameter("minSlope", OpParamType::Float, 0.0f, "最小傾斜 [度]。"),
        Parameter("maxSlope", OpParamType::Float, 40.0f, "最大傾斜 [度]。0〜90。"),
        Parameter("layer", OpParamType::Int, -1, "地形の保存レイヤー番号。-1 は全レイヤー。"),
        Parameter("layerThreshold", OpParamType::Float, 0.25f, "指定層の最小重み [0,1]。"),
        Parameter("minScale", OpParamType::Float, 0.8f, "モデルに掛ける最小 world scale。0.01〜100。"),
        Parameter("maxScale", OpParamType::Float, 1.2f, "モデルに掛ける最大 world scale。0.01〜100。"),
        Parameter("minYaw", OpParamType::Float, 0.0f, "最小回転 [度]。-360〜360。"),
        Parameter("maxYaw", OpParamType::Float, 360.0f, "最大回転 [度]。-360〜360。"),
        Parameter("alignToNormal", OpParamType::Bool, false, "true は地形の world 法線へモデルの上方向を揃える。"),
        Parameter("alignDownslope", OpParamType::Bool, false, "モデルの +Y を地表法線、+Z を下り方向へ揃える。平地では通常の yaw を使う。"),
        Parameter("downslopeJitter", OpParamType::Float, 0.0f, "下り方向を基準にした左右の回転幅 [度]。0〜180。"),
        Parameter("embedDepth", OpParamType::Float, 0.0f, "地表法線の逆向きに埋め込む距離 [world m]。0〜100。"),
        Parameter("stroke", OpParamType::Data, std::monostate{}, "省略時は全置換。指定時は world XYZ の円の中心列 (1〜512 点)。"),
        Parameter("brushRadius", OpParamType::Float, 5.0f, "stroke の各円の半径 [world m]。0.25〜500。"),
        Parameter("erase", OpParamType::Bool, false, "true は stroke の円内の既存配置を消去する。"),
        Parameter("maxInstances", OpParamType::Int, 100000, "配置上限。1〜1000000。超過時は全体を拒否。")};
    op.params[0].required = true; op.params[1].required = true;
    op.poll = [](const OpContext& context, const OpArgs&) {
        return context.ctx.activeScene && !context.ctx.projectRoot.empty()
            && (!context.ctx.playMode || context.ctx.playMode->IsInEditor());
    };
    op.exec = Scatter;
    auto paint = op;
    paint.id = "terrain.paint_instances";
    paint.label = "Paint Terrain Instances";
    paint.desc = "選択した配置素材の規則でメッシュ草・木・石を塗り、1 回の Undo に記録する。";
    paint.caution = "生成アセットは Undo 後も残る。";
    paint.params.erase(paint.params.begin() + 1, paint.params.begin() + 3);
    auto sources = Parameter("sources", OpParamType::Data, std::monostate{}, "Instance Set の GUID 参照の配列 (1〜16)。");
    sources.required = true;
    paint.params.push_back(sources);
    paint.params.push_back(Parameter("weights", OpParamType::Data, std::monostate{}, "素材順の相対配合比 (0〜100)。省略すると等配合。"));
    paint.params.push_back(Parameter("avoidWater", OpParamType::Bool, true, "水面下の候補を除外する。"));
    paint.params.push_back(Parameter("avoidPaths", OpParamType::Bool, true, "Path / Road / Trail の材質名を持つ保存層を避ける。"));
    paint.params.push_back(Parameter("excludedLayer", OpParamType::Int, -1, "追加の除外レイヤー番号。-1 は指定なし。"));
    paint.params.push_back(Parameter("polygon", OpParamType::Bool, false, "stroke を閉じた多角形として塗る。"));
    paint.params.push_back(Parameter("feather", OpParamType::Float, 0.0f, "半径に対する境界の密度減衰幅 [0,1]。"));
    paint.params.push_back(Parameter("excludedMaterials", OpParamType::Data, std::monostate{}, "除外する地表材質の GUID 参照配列。"));
    paint.params.push_back(Parameter("minimumDistance", OpParamType::Float, 0.0f, "既存と新規の配置の最小間隔 [world m]。"));
    for (auto& parameter : paint.params) {
        if (parameter.name != "stroke") continue;
        parameter.required = true;
        parameter.desc = "world XYZ の円の中心列 (1〜512 点)。";
    }
    paint.exec = PaintPalette;

    EditorOperator palette;
    palette.id = "terrain.select_paint_palette";
    palette.label = "Paint on Terrain";
    palette.category = "Terrain";
    palette.desc = "Asset Browser の選択を Terrain Paint の配置パレットに渡す。";
    palette.kind = OpKind::Action;
    palette.params = {std::move(sources)};
    palette.poll = [canScatter = op.poll](const OpContext& context, const OpArgs& args) {
        return context.ctx.terrainTool && canScatter(context, args);
    };
    palette.exec = [](OpContext& context, const OpArgs& args) {
        const auto* values = args.GetData("sources");
        if (!values || !values->IsArray() || values->AsArray().empty() || values->AsArray().size() > TERRAIN_SCATTER_PALETTE_LIMIT)
            return OpResult::Err("BAD_ARG", "配置素材を 1〜16 件選んでください。");
        std::vector<std::string> references;
        for (const auto& value : values->AsArray()) {
            const auto reference = value.AsString();
            if (asset::AssetDatabase::GuidFromRef(reference).empty())
                return OpResult::Err("NO_TEMPLATE", "配置素材の GUID 参照を指定してください。");
            references.push_back(reference);
        }
        auto& ctx = context.ctx;
        if (ctx.mapEditingMode) ActivateMapTool(ctx, EditorContext::MapTool::TerrainPaint);
        else ctx.showTerrainTool = true;
        ctx.terrainTool->SetPaintPalette(std::move(references), !ctx.mapEditingMode);
        OpArgs panel;
        panel.Set("panel", std::string("Scene"));
        (void)InvokeOperator(ctx, "panel.focus", panel);
        return OpResult::Ok();
    };
    registry.Register(std::move(op));
    registry.Register(std::move(paint));
    registry.Register(std::move(palette));
}
} /// @note namespace fbzz::editor
