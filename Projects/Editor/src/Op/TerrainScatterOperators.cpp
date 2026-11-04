/// @file    TerrainScatterOperators.cpp
/// @brief   地形への配置を独立した InstanceSet に保存し、参照の変更を Undo へ記録する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include "Tools/TerrainScatter.hpp"
#include <Editor/EditorContext.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/DataAssetRegistry.hpp>
#include <Engine/Asset/InstanceSetAsset.hpp>
#include <Engine/Scene/Components/InstancedMeshComponent.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Scene/TomlReflector.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>

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

class ScatterCommand final : public ICommand {
public:
    ScatterCommand(EditorContext& context, std::string node, std::optional<scene::InstancedMeshComponent> before,
        scene::InstancedMeshComponent after, uint64_t lifetime, std::string diskPath)
        : m_context(context), m_scene(context.activeScene), m_sceneGeneration(context.activeScene->GetRenderSceneGeneration()),
          m_node(std::move(node)), m_before(std::move(before)),
          m_after(std::move(after)), m_lifetime(lifetime), m_diskPath(std::move(diskPath)),
          m_discardEpoch(AssetDirtyRegistry::DiscardEpoch()),
          m_beforeSnapshot(m_before ? m_before->Snapshot() : nullptr), m_afterSnapshot(m_after.Snapshot()) {}

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
        auto* node = m_scene->FindByGuid(m_node);
        const auto* component = node ? node->GetComponent<scene::InstancedMeshComponent>() : nullptr;
        const auto* expected = m_applied ? &m_after : (m_before ? &*m_before : nullptr);
        const auto& expectedSnapshot = m_applied ? m_afterSnapshot : m_beforeSnapshot;
        if (!node || (expected ? (!component || !SameComponent(*component, *expected, expectedSnapshot)
            || m_scene->GetComponentLifetime<scene::InstancedMeshComponent>(node->GetID()) != m_lifetime) : component != nullptr)) {
            reason = "配置先の Component または配置アセットが変更されています。"; return false;
        }
        return true;
    }
private:
    bool Apply(bool after, std::string& reason)
    {
        if (after == m_applied || !ValidateHistory(reason)) return false;
        if (after && (!util::FileSystem::Exists(util::FileSystem::PathFromUtf8(m_diskPath))
            || m_after.Snapshot() != m_afterSnapshot)) {
            reason = "生成した配置アセットが削除または変更されています。"; return false;
        }
        auto* node = m_scene->FindByGuid(m_node);
        const auto* value = after ? &m_after : (m_before ? &*m_before : nullptr);
        if (value) {
            if (auto* current = node->GetComponent<scene::InstancedMeshComponent>()) *current = *value;
            else node->AddComponent<scene::InstancedMeshComponent>(*value);
            m_lifetime = m_scene->GetComponentLifetime<scene::InstancedMeshComponent>(node->GetID());
        } else node->RemoveComponent<scene::InstancedMeshComponent>();
        m_applied = after;
        if (m_context.markSceneDirty) m_context.markSceneDirty();
        return true;
    }
    EditorContext& m_context;
    scene::Scene* m_scene;
    uint64_t m_sceneGeneration;
    std::string m_node;
    std::optional<scene::InstancedMeshComponent> m_before;
    scene::InstancedMeshComponent m_after;
    uint64_t m_lifetime;
    std::string m_diskPath;
    uint64_t m_discardEpoch;
    std::shared_ptr<const renderer::InstanceSetData> m_beforeSnapshot, m_afterSnapshot;
    bool m_applied = true;
};

OpResult Scatter(OpContext& context, const OpArgs& args)
{
    auto& ctx = context.ctx;
    auto* terrainNode = ctx.activeScene->FindByGuid(args.GetString("terrain"));
    auto* targetNode = ctx.activeScene->FindByGuid(args.GetString("target"));
    auto* terrain = terrainNode ? terrainNode->GetComponent<scene::TerrainComponent>() : nullptr;
    if (!terrain || !terrain->enabled || !targetNode)
        return OpResult::Err("NO_TARGET", "有効な Terrain と配置先 GameObject を指定してください。");
    const auto* previous = targetNode->GetComponent<scene::InstancedMeshComponent>();
    const std::string reference = args.GetString("source", previous ? previous->m_instanceSetRef.path : std::string{});
    const std::string source = reference.empty() && previous ? previous->m_instanceSetRef.path : reference;
    auto* definition = source.empty() ? nullptr : ResolveSet(source);
    if (!definition || !definition->Snapshot())
        return OpResult::Err("NO_TEMPLATE", "source に有効な InstanceSetAsset (.fzdata) を指定してください。");
    TerrainScatterSettings settings;
    settings.spacing = args.GetFloat("spacing", settings.spacing);
    settings.density = args.GetFloat("density", settings.density);
    const int seed = args.GetInt("seed", 1), maxInstances = args.GetInt("maxInstances", 100000);
    if (seed < 0 || maxInstances < 1) return OpResult::Err("BAD_ARG", "seed は 0 以上、maxInstances は 1 以上です。");
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
    std::vector<renderer::InstancePlacement> placements;
    std::string reason;
    scene::FlushWorldTransforms(*ctx.activeScene);
    if (!BuildTerrainScatter(*terrain, terrainNode->transform, targetNode->transform, settings, placements, reason))
        return OpResult::Err("INVALID_SCATTER", reason);
    auto staged = std::make_unique<asset::InstanceSetAsset>();
    if (!staged->Replace(definition->Parts(), definition->LodHeights(), placements))
        return OpResult::Err("INVALID_TEMPLATE", "配置またはテンプレートの mesh・材質・LOD が不正です。");
    /// @note 元アセットと他の参照個体は変更せず、新規の検証済みファイルだけを公開する。
    toml::table table;
    table.insert("type", std::string(asset::InstanceSetAsset::TYPE_NAME));
    util::TomlWriteReflector writer(table);
    staged->Reflect(writer);
    std::ostringstream serialized;
    serialized << table;
    const auto relative = "Assets/Terrain/Scatter/" + asset::AssetDatabase::GenerateGuid() + ".fzdata";
    const auto disk = util::FileSystem::PathToUtf8(util::FileSystem::PathFromUtf8(ctx.projectRoot) / relative);
    if (!util::FileSystem::WriteTextAtomic(disk, serialized.str()))
        return OpResult::Err("WRITE_FAILED", "生成した配置を保存できませんでした。シーンは未変更です。");
    const auto guid = asset::AssetDatabase::GuidFromPath(disk);
    if (guid.empty()) return OpResult::Err("META_FAILED", "配置は保存済みですが .meta を作成できませんでした: " + disk);
    const std::string generatedReference = "guid:" + guid + "|" + relative;
    const auto* generated = ResolveSet(generatedReference);
    if (!generated || !generated->Snapshot())
        return OpResult::Err("LOAD_FAILED", "生成した配置を読み込めませんでした: " + disk);
    std::optional<scene::InstancedMeshComponent> before;
    if (previous) before = *previous;
    scene::InstancedMeshComponent after = before ? *before : scene::InstancedMeshComponent{};
    after.m_instanceSetRef = {generatedReference, asset::InstanceSetAsset::TYPE_NAME};
    after.ResetOverrides();
    if (auto* current = targetNode->GetComponent<scene::InstancedMeshComponent>()) *current = after;
    else targetNode->AddComponent<scene::InstancedMeshComponent>(after);
    const auto lifetime = ctx.activeScene->GetComponentLifetime<scene::InstancedMeshComponent>(targetNode->GetID());
    if (ctx.markSceneDirty) ctx.markSceneDirty();
    ctx.requestAssetBrowserRefresh = true;
    OpResult result;
    result.command = std::make_unique<ScatterCommand>(ctx, targetNode->instanceId, std::move(before), after, lifetime, disk);
    result.data = OpData::MakeObject();
    result.data.Set("count", OpData(static_cast<uint32_t>(placements.size())));
    result.data.Set("assetPath", OpData(generatedReference));
    result.message = std::to_string(placements.size()) + " 個を配置しました。シーンを保存してください。";
    return result;
}
} /// @note namespace terrain_scatter_operators

void RegisterTerrainScatterOperators(OperatorRegistry& registry)
{
    using namespace terrain_scatter_operators;
    EditorOperator op;
    op.id = "terrain.scatter"; op.label = "Scatter Trees and Rocks"; op.category = "Terrain";
    op.desc = "地形の標高・傾斜・保存レイヤーから木や岩を配置し、独立した配置アセットへ確定する。";
    op.caution = "配置先の既存配置を全置換する。Undo で戻せる。生成アセットは Undo 後も残る。";
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
        Parameter("maxInstances", OpParamType::Int, 100000, "配置上限。1〜1000000。超過時は全体を拒否。")};
    op.params[0].required = true; op.params[1].required = true;
    op.poll = [](const OpContext& context, const OpArgs&) {
        return context.ctx.activeScene && !context.ctx.projectRoot.empty()
            && (!context.ctx.playMode || context.ctx.playMode->IsInEditor());
    };
    op.exec = Scatter;
    registry.Register(std::move(op));
}
} /// @note namespace fbzz::editor
