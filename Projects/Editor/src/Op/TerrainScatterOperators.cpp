/// @file    TerrainScatterOperators.cpp
/// @brief   地形への配置を独立した InstanceSet に保存し、参照の変更を Undo へ記録する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include "Tools/TerrainScatter.hpp"
#include "Panels/MapToolCommon.hpp"
#include <Editor/EditorContext.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/Selection.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/DataAssetRegistry.hpp>
#include <Engine/Asset/InstanceSetAsset.hpp>
#include <Engine/Scene/ComponentRegistry.hpp>
#include <Engine/Scene/Components/InstancedMeshComponent.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Scene/TomlReflector.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>

namespace fbzz::editor {
namespace terrain_scatter_operators {
OpParam Parameter(const char* name, OpParamType type, OpValue fallback, const char* description)
{
    OpParam param;
    param.name = name; param.type = type; param.required = false;
    param.defaultValue = std::move(fallback); param.desc = description;
    return param;
}

asset::InstanceSetAsset* ResolveSet(const std::string& reference)
{
    auto* value = asset::DataAssetRegistry::Resolve(reference);
    return value && std::string_view(value->GetTypeName()) == asset::InstanceSetAsset::TYPE_NAME
        ? static_cast<asset::InstanceSetAsset*>(value) : nullptr;
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
};

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
                    || change.after.Snapshot() != change.afterSnapshot) {
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
    return true;
}

std::optional<OpResult> StageSet(EditorContext& ctx, const asset::InstanceSetAsset& definition,
    std::span<const renderer::InstancePlacement> placements, const std::string& folder,
    const scene::InstancedMeshComponent* previous, ScatterChange& change)
{
    asset::InstanceSetAsset staged;
    if (!staged.Replace(definition.Parts(), definition.LodHeights(), placements))
        return OpResult::Err("INVALID_TEMPLATE", "配置またはテンプレートの mesh・材質・LOD が不正です。");
    toml::table table;
    util::TomlWriteReflector writer(table);
    staged.Reflect(writer);
    std::ostringstream serialized;
    /// @note 型を先頭に置くと Asset Browser は個体配列を読まずに分類できる。
    serialized << "type = '" << asset::InstanceSetAsset::TYPE_NAME << "'\n" << table;
    const auto relative = folder + asset::AssetDatabase::GenerateGuid() + ".fzdata";
    change.diskPath = util::FileSystem::PathToUtf8(util::FileSystem::PathFromUtf8(ctx.projectRoot) / relative);
    if (!util::FileSystem::WriteTextAtomic(change.diskPath, serialized.str()))
        return OpResult::Err("WRITE_FAILED", "生成した配置を保存できませんでした: " + change.diskPath);
    const auto guid = asset::AssetDatabase::GuidFromPath(change.diskPath);
    if (guid.empty())
        return OpResult::Err("META_FAILED", "配置の .meta を作成できませんでした: " + change.diskPath);
    const std::string reference = "guid:" + guid + "|" + relative;
    if (previous) change.before = *previous;
    change.beforeSnapshot = previous ? previous->Snapshot() : nullptr;
    change.after = previous ? *previous : scene::InstancedMeshComponent{};
    change.after.m_instanceSetRef = {reference, asset::InstanceSetAsset::TYPE_NAME};
    change.after.ResetOverrides();
    change.afterSnapshot = change.after.Snapshot();
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
        return OpResult::Err("NO_TEMPLATE", "source に有効な InstanceSetAsset (.fzdata) を指定してください。");
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
    if (auto failure = StageSet(ctx, *definition, placements, "Assets/Terrain/Scatter/", previous, change))
        return std::move(*failure);
    const size_t changed = erase ? oldCount - placements.size() : placements.size() - oldCount;
    std::vector<ScatterChange> changes;
    changes.push_back(std::move(change));
    return PublishStroke(ctx, std::move(changes), placements.size(), changed, erase);
}

struct PaintSet {
    std::string guid, name, ownerId;
    const asset::InstanceSetAsset* definition = nullptr;
    scene::GameObject* owner = nullptr;
    std::vector<renderer::InstancePlacement> placements;
    size_t beforeCount = 0;
    size_t sourceIndex = 0;
    float weight = 1.0f;
};

struct PaintPlan {
    std::string folder;
    std::vector<PaintSet> sets;
    bool erase = false;
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
    const TerrainScatterBrush brush{points, args.GetFloat("brushRadius", 5.0f), plan.erase};
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
            return OpResult::Err("NO_TEMPLATE", "配置用の Instance Set を選んでください。");
        if (std::any_of(palette.begin(), palette.end(), [&](const PaintSet& set) { return set.guid == guid; })) continue;
        PaintSet set;
        set.guid = guid;
        set.sourceIndex = index;
        set.weight = weight;
        set.name = util::FileSystem::PathToUtf8(util::FileSystem::PathFromUtf8(sourcePath).stem());
        set.definition = definition;
        set.ownerId = asset::AssetDatabase::DeriveGuid(terrainNode->instanceId, "Terrain Paint/" + guid);
        set.owner = scene.FindByGuid(set.ownerId);
        if (set.owner) {
            const auto* current = set.owner->GetComponent<scene::InstancedMeshComponent>();
            const auto snapshot = current ? current->Snapshot() : nullptr;
            if (!snapshot || !asset::AssetDatabase::HintFromRef(current->m_instanceSetRef.path).starts_with(plan.folder + guid + "/")
                || !IsPaintOwnerTransform(*set.owner))
                return OpResult::Err("PLACEMENT_CHANGED", "配置先が編集されています。配置先の参照と Transform を確認してください。");
            for (const auto& item : snapshot->placements)
                if (item.active) set.placements.push_back(item);
        }
        set.beforeCount = set.placements.size();
        palette.push_back(std::move(set));
    }
    std::sort(palette.begin(), palette.end(), [](const PaintSet& a, const PaintSet& b) { return a.guid < b.guid; });
    scene::FlushWorldTransforms(scene);
    std::string reason;
    if (plan.erase) {
        for (auto& set : palette) {
            if (set.owner && !BuildTerrainScatter(*terrain, terrainNode->transform, scene::Transform{}, settings, set.placements, reason, &brush))
                return OpResult::Err("INVALID_SCATTER", reason);
        }
    } else {
        double totalWeight = 0.0;
        for (const auto& set : palette) totalWeight += set.weight;
        if (totalWeight == 0.0) return OpResult::Ok();
        const bool equalWeights = std::all_of(palette.begin(), palette.end(), [&](const PaintSet& set) {
            return set.weight == palette.front().weight;
        });
        std::vector<renderer::InstancePlacement> candidates;
        for (const auto id : scene.GetEntities<scene::InstancedMeshComponent>()) {
            const auto* owner = scene.GetGameObject(id);
            const auto* component = scene.GetComponent<scene::InstancedMeshComponent>(id);
            if (!owner || !component || !asset::AssetDatabase::HintFromRef(component->m_instanceSetRef.path).starts_with(plan.folder)) continue;
            const auto snapshot = component->Snapshot();
            if (!snapshot) continue;
            const auto world = owner->transform.GetWorldMatrix();
            for (const auto& item : snapshot->placements) {
                if (!item.active) continue;
                auto obstacle = item;
                const auto position = world * math::Vector4{item.position.x, item.position.y, item.position.z, 1.0f};
                obstacle.position = {position.x, position.y, position.z};
                candidates.push_back(obstacle);
            }
        }
        const size_t existingCount = candidates.size();
        if (!BuildTerrainScatter(*terrain, terrainNode->transform, scene::Transform{}, settings, candidates, reason, &brush))
            return OpResult::Err("INVALID_SCATTER", reason);
        for (size_t index = existingCount; index < candidates.size(); ++index) {
            const auto& item = candidates[index];
            /// @note 配合を変えても候補位置と総密度は同じ。GUID 順の累積比だけで素材を割り当てる。
            uint32_t key = item.id;
            key ^= key >> 16;
            key *= 0x7feb352du;
            key ^= key >> 15;
            if (equalWeights) {
                palette[key % palette.size()].placements.push_back(item);
                continue;
            }
            key *= 0x846ca68bu;
            key ^= key >> 16;
            double choice = (static_cast<double>(key) / 4294967296.0) * totalWeight;
            for (auto& set : palette) {
                if (choice < set.weight) {
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
        if (auto failure = StageSet(ctx, *set.definition, set.placements, plan.folder + set.guid + "/", previous, change))
            return std::move(*failure);
        changes.push_back(std::move(change));
    }
    return PublishStroke(ctx, std::move(changes), count, changed, plan.erase);
}
} /// @note namespace terrain_scatter_operators

bool BuildTerrainPaintPreview(EditorContext& ctx, const OpArgs& args, std::vector<TerrainPaintPreviewPoint>& points,
    size_t& total, std::string& reason)
{
    points.clear();
    total = 0;
    terrain_scatter_operators::PaintPlan plan;
    const auto result = terrain_scatter_operators::PlanPaint(ctx, args, plan);
    reason = result.message;
    if (!result.ok) return false;
    if (plan.erase) return true;
    for (const auto& set : plan.sets) total += set.placements.size() - set.beforeCount;
    /// @note プレビューの描画数は一定に保ち、ストローク全体から等間隔に拾う。
    constexpr size_t DISPLAY_LIMIT = 384;
    const size_t stride = std::max<size_t>(1, (total + DISPLAY_LIMIT - 1) / DISPLAY_LIMIT);
    size_t index = 0;
    for (const auto& set : plan.sets) {
        for (size_t i = set.beforeCount; i < set.placements.size(); ++i) {
            if (index++ % stride == 0) points.push_back({set.placements[i].position, set.sourceIndex});
        }
    }
    return true;
}

void RegisterTerrainScatterOperators(OperatorRegistry& registry)
{
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
    paint.desc = "選択した配置素材を同じブラシの候補へ割り当て、1 回の Undo に記録する。";
    paint.caution = "生成アセットは Undo 後も残る。";
    paint.params.erase(paint.params.begin() + 1, paint.params.begin() + 3);
    auto sources = Parameter("sources", OpParamType::Data, std::monostate{}, "Instance Set の GUID 参照の配列 (1〜16)。");
    sources.required = true;
    paint.params.push_back(sources);
    paint.params.push_back(Parameter("weights", OpParamType::Data, std::monostate{}, "素材順の相対配合比 (0〜100)。省略すると等配合。"));
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
