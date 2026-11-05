/// @file    TerrainPaintAssetOperators.cpp
/// @brief   専用 Paint 素材の作成・更新と検証付きファイル Undo。
/// @author  Hasegawa Jin
/// @date    2026-11-05
#include "Tools/TerrainPaintEditing.hpp"
#include "Tools/TerrainTool.hpp"
#include "Panels/MapToolCommon.hpp"
#include <Editor/EditorContext.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/AssetSearch.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <algorithm>
#include <filesystem>
#include <bit>

namespace fbzz::editor {
namespace {

struct PlacementField { const char* name; float asset::TerrainPaintPlacement::* value; };
constexpr PlacementField PLACEMENT_FIELDS[]{
    {"spacing", &asset::TerrainPaintPlacement::spacing}, {"density", &asset::TerrainPaintPlacement::density},
    {"minScale", &asset::TerrainPaintPlacement::minScale}, {"maxScale", &asset::TerrainPaintPlacement::maxScale},
    {"minYaw", &asset::TerrainPaintPlacement::minYaw}, {"maxYaw", &asset::TerrainPaintPlacement::maxYaw},
    {"minSlope", &asset::TerrainPaintPlacement::minSlope}, {"maxSlope", &asset::TerrainPaintPlacement::maxSlope},
    {"downslopeJitter", &asset::TerrainPaintPlacement::downslopeJitter}, {"embedDepth", &asset::TerrainPaintPlacement::embedDepth}};
constexpr const char* CATEGORIES[]{"grass", "tree", "rock", "other"};

OpParam Parameter(const char* name, OpParamType type, OpValue value = {})
{
    OpParam result;
    result.name = name; result.type = type; result.required = std::holds_alternative<std::monostate>(value);
    result.defaultValue = std::move(value);
    return result;
}

bool ReadDefinition(const OpArgs& args, asset::TerrainPaintAsset& out)
{
    out.name = args.GetString("name");
    const auto category = args.GetString("category", "other");
    const auto match = std::find(std::begin(CATEGORIES), std::end(CATEGORIES), category);
    if (match == std::end(CATEGORIES)) return false;
    out.category = static_cast<asset::TerrainPaintCategory>(match - std::begin(CATEGORIES));
    const auto* parts = args.GetData("parts"), *lods = args.GetData("lodHeights");
    if (!parts || !parts->IsArray() || parts->AsArray().empty() || parts->AsArray().size() > 64
        || !lods || !lods->IsArray() || lods->AsArray().empty() || lods->AsArray().size() > 4) return false;
    std::vector<asset::InstanceMeshPart> meshParts;
    std::vector<float> heights;
    for (const auto& part : parts->AsArray()) {
        const auto* mesh = part.Find("mesh"), *material = part.Find("material"), *lod = part.Find("lod");
        if (!mesh || !mesh->IsString() || !material || !material->IsString() || !lod || !lod->IsInt() || lod->AsInt64() < 0 || lod->AsInt64() > 3) return false;
        meshParts.push_back({mesh->AsString(), material->AsString(), static_cast<uint32_t>(lod->AsInt64())});
    }
    for (const auto& lod : lods->AsArray()) {
        float height = 0;
        if (lod.IsInt()) height = static_cast<float>(lod.AsInt64());
        else if (!lod.TryGetFloat(height)) return false;
        heights.push_back(height);
    }
    if (!out.definition.Replace(meshParts, heights, {})) return false;
    for (const auto& field : PLACEMENT_FIELDS) out.placement.*field.value = args.GetFloat(field.name, out.placement.*field.value);
    out.placement.alignToNormal = args.GetBool("alignToNormal");
    out.placement.alignDownslope = args.GetBool("alignDownslope");
    return asset::ValidateTerrainPaintAsset(out);
}

bool Store(const std::string& path, const asset::TerrainPaintAsset& value)
{
    if (!asset::SaveTerrainPaintAsset(path, value)) return false;
    (void)asset::AssetManager::ReloadPath(path);
    return true;
}

OpResult WriteAsset(OpContext& context, const OpArgs& args, bool create)
{
    const auto path = ToProjectAssetDiskPath(context.ctx.projectRoot, args.GetString("path"));
    const auto assets = util::FileSystem::PathToUtf8(util::FileSystem::PathFromUtf8(context.ctx.projectRoot) / "Assets");
    if (path.empty() || !util::FileSystem::IsChildPathText(path, assets)
        || util::FileSystem::GetExtension(path) != ".terrainpaint")
        return OpResult::Err("BAD_PATH", "プロジェクト Assets 内の .terrainpaint 保存先を指定してください。");
    std::error_code error;
    const bool exists = std::filesystem::exists(util::FileSystem::PathFromUtf8(path), error);
    if (error || exists == create)
        return OpResult::Err("BAD_PATH", create ? "同名の素材が存在します。別の保存名を指定してください。" : "編集する素材がありません。");
    asset::TerrainPaintAsset after;
    if (!ReadDefinition(args, after)) return OpResult::Err("BAD_ARG", "モデル・材質・LOD または配置規則が不正です。");
    asset::TerrainPaintAsset before;
    if (!create && !asset::LoadTerrainPaintAsset(path, before)) return OpResult::Err("READ_FAILED", "元の素材を読み込めません。");
    if (!create && args.GetString("expected") != TerrainPaintAssetFingerprint(before))
        return OpResult::Err("CONFLICT", "素材が別の操作で変更されています。読み直してください。");
    if (!create && asset::TerrainPaintAssetsEqual(before, after)) {
        auto result = OpResult::Ok(); result.noChange = true; return result;
    }
    if (!Store(path, after)) return OpResult::Err("WRITE_FAILED", "素材を保存できませんでした。");
    const auto guid = asset::AssetDatabase::GuidFromPath(path);
    if (guid.empty()) return OpResult::Err("META_FAILED", ".meta を発行できませんでした: " + path);
    context.ctx.requestAssetBrowserRefresh = true;
    AssetSearch::SetProjectRoot(context.ctx.projectRoot);
    AssetSearch::Rebuild();
    auto result = OpResult::Ok();
    result.data = OpData::MakeObject();
    result.data.Set("reference", OpData("guid:" + guid + "|" + NormalizeAssetPath(path)));
    result.message = create ? "Paint 素材を作成しました。" : "Paint 素材を保存しました。";
    if (!create) {
        const auto project = context.ctx.projectRoot;
        auto* owner = &context.ctx;
        const auto apply = [path, project, owner](const asset::TerrainPaintAsset& expected,
            const asset::TerrainPaintAsset& value, std::string& reason) {
            asset::TerrainPaintAsset current;
            if (owner->projectRoot != project || !asset::LoadTerrainPaintAsset(path, current)
                || !asset::TerrainPaintAssetsEqual(current, expected)) {
                reason = "素材が別の操作で変更されています。"; return false;
            }
            if (!Store(path, value)) { reason = "素材を保存できません。"; return false; }
            reason.clear(); return true;
        };
        result.command = std::make_unique<CheckedLambdaCommand>("Edit Terrain Paint Asset",
            [apply, before, after](std::string& reason) { return apply(before, after, reason); },
            [apply, before, after](std::string& reason) { return apply(after, before, reason); });
    }
    return result;
}
} /// @note namespace

OpArgs TerrainPaintAssetArguments(const std::string& path, const asset::TerrainPaintAsset& asset)
{
    OpArgs args;
    args.Set("path", path); args.Set("name", asset.name);
    args.Set("category", std::string(CATEGORIES[std::clamp(static_cast<int>(asset.category), 0, 3)]));
    auto parts = OpData::MakeArray(), lods = OpData::MakeArray();
    for (const auto& part : asset.definition.Parts()) {
        auto row = OpData::MakeObject();
        row.Set("mesh", OpData(part.meshPath)); row.Set("material", OpData(part.materialPath)); row.Set("lod", OpData(part.lod));
        parts.Push(std::move(row));
    }
    for (float height : asset.definition.LodHeights()) lods.Push(OpData(height));
    args.Set("parts", std::move(parts)); args.Set("lodHeights", std::move(lods));
    for (const auto& field : PLACEMENT_FIELDS) args.Set(field.name, asset.placement.*field.value);
    args.Set("alignToNormal", asset.placement.alignToNormal); args.Set("alignDownslope", asset.placement.alignDownslope);
    args.Set("expected", TerrainPaintAssetFingerprint(asset));
    return args;
}

std::string TerrainPaintAssetFingerprint(const asset::TerrainPaintAsset& asset)
{
    uint64_t hash = 1469598103934665603ull;
    const auto add = [&](uint64_t value) { hash = (hash ^ value) * 1099511628211ull; };
    const auto text = [&](const std::string& value) { add(value.size()); for (unsigned char character : value) add(character); };
    text(asset.name); add(static_cast<uint64_t>(asset.category));
    for (const auto& part : asset.definition.Parts()) { text(part.meshPath); text(part.materialPath); add(part.lod); }
    for (float height : asset.definition.LodHeights()) add(std::bit_cast<uint32_t>(height));
    for (const auto& field : PLACEMENT_FIELDS) add(std::bit_cast<uint32_t>(asset.placement.*field.value));
    add(asset.placement.alignToNormal); add(asset.placement.alignDownslope);
    return std::to_string(hash);
}

void RegisterTerrainPaintAssetOperators(OperatorRegistry& registry)
{
    EditorOperator open;
    open.id = "terrain.paint_asset.new"; open.label = "New Terrain Paint Asset";
    open.category = "Terrain"; open.kind = OpKind::Action;
    open.params = {Parameter("directory", OpParamType::String, std::string{"Assets/Terrain/Paint"})};
    open.poll = [](const OpContext& ctx, const OpArgs&) { return ctx.ctx.terrainTool && (!ctx.ctx.playMode || ctx.ctx.playMode->IsInEditor()); };
    open.exec = [](OpContext& ctx, const OpArgs& args) {
        ctx.ctx.terrainTool->OpenPaintAssetCreation(args.GetString("directory", "Assets/Terrain/Paint"));
        if (ctx.ctx.mapEditingMode) ActivateMapTool(ctx.ctx, EditorContext::MapTool::TerrainPaint);
        else ctx.ctx.showTerrainTool = true;
        OpArgs panel; panel.Set("panel", std::string{"Scene"});
        (void)InvokeOperator(ctx.ctx, "panel.focus", panel);
        return OpResult::Ok();
    };
    registry.Register(std::move(open));
    for (bool create : {true, false}) {
        EditorOperator op;
        op.id = create ? "terrain.paint_asset.create" : "terrain.paint_asset.save";
        op.label = create ? "Create Terrain Paint Asset" : "Save Terrain Paint Asset";
        op.category = "Terrain"; op.kind = create ? OpKind::Action : OpKind::Mutation;
        op.undoLabel = "Edit Terrain Paint Asset";
        op.params = {Parameter("path", OpParamType::String), Parameter("name", OpParamType::String),
            Parameter("category", OpParamType::String, std::string{"other"}), Parameter("parts", OpParamType::Data), Parameter("lodHeights", OpParamType::Data)};
        op.params[2].enumValues = {"grass", "tree", "rock", "other"};
        auto expected = Parameter("expected", OpParamType::String, std::string{});
        expected.required = !create;
        expected.desc = "編集開始時の素材 fingerprint。作成時は不要。";
        op.params.push_back(std::move(expected));
        const asset::TerrainPaintPlacement defaults;
        for (const auto& field : PLACEMENT_FIELDS) op.params.push_back(Parameter(field.name, OpParamType::Float, defaults.*field.value));
        op.params.push_back(Parameter("alignToNormal", OpParamType::Bool, false));
        op.params.push_back(Parameter("alignDownslope", OpParamType::Bool, false));
        op.poll = [](const OpContext& ctx, const OpArgs&) {
            return !ctx.ctx.projectRoot.empty() && (!ctx.ctx.playMode || ctx.ctx.playMode->IsInEditor());
        };
        op.exec = [create](OpContext& ctx, const OpArgs& args) { return WriteAsset(ctx, args, create); };
        registry.Register(std::move(op));
    }
}
} /// @note namespace fbzz::editor
