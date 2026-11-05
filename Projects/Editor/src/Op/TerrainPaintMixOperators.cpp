/// @file    TerrainPaintMixOperators.cpp
/// @brief   配合の保存と呼び出しを共通の操作登録簿へ公開する。
/// @author  Hasegawa Jin
/// @date    2026-11-05
#include "Tools/TerrainPaintMix.hpp"
#include "Tools/TerrainTool.hpp"
#include <Editor/EditorContext.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/AssetSearch.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/MeshInstancesAsset.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <filesystem>

namespace fbzz::editor {
void RegisterTerrainPaintMixOperators(OperatorRegistry& registry)
{
    for (bool save : {true, false}) {
        EditorOperator op;
        op.id = save ? "terrain.paint_mix.save" : "terrain.paint_mix.use";
        op.label = save ? "Save Terrain Paint Mix" : "Use Terrain Paint Mix";
        op.category = "Terrain"; op.kind = OpKind::Action;
        OpParam path; path.name = "path"; path.type = OpParamType::String; path.required = true;
        OpParam name; name.name = "name"; name.type = OpParamType::String; name.required = false; name.defaultValue = std::string{"New Mix"};
        op.params = {path, name};
        op.poll = [](const OpContext& context, const OpArgs&) {
            return context.ctx.terrainTool && !context.ctx.projectRoot.empty()
                && (!context.ctx.playMode || context.ctx.playMode->IsInEditor());
        };
        op.exec = [save](OpContext& context, const OpArgs& args) {
            auto& ctx = context.ctx;
            auto path = util::FileSystem::PathFromUtf8(args.GetString("path"));
            if (path.is_relative()) path = util::FileSystem::PathFromUtf8(ctx.projectRoot) / path;
            path = path.lexically_normal();
            const auto absolute = util::FileSystem::PathToUtf8(path);
            if (path.extension() != ".terrainmix" || !util::FileSystem::IsChildPathText(absolute, ctx.projectRoot + "/Assets"))
                return OpResult::Err("BAD_PATH", "配合はプロジェクトの Assets 内の .terrainmix を指定してください。");
            if (save) {
                const auto mix = ctx.terrainTool->CapturePaintMix(args.GetString("name", "New Mix"));
                if (!SaveTerrainPaintMix(absolute, mix)) return OpResult::Err("WRITE_FAILED", "配合または保存先を確認してください。");
                if (asset::AssetDatabase::GuidFromPath(absolute).empty())
                    return OpResult::Err("META_FAILED", "配合の .meta を作成できませんでした。");
                AssetSearch::Rebuild(); ctx.requestAssetBrowserRefresh = true;
            } else {
                TerrainPaintMix mix;
                if (!LoadTerrainPaintMix(absolute, mix)) return OpResult::Err("INVALID_MIX", "配合を読み込めません。");
                for (const auto& source : mix.sources)
                    if (!asset::ResolveMeshInstanceDefinition(source.reference))
                        return OpResult::Err("NO_TEMPLATE", "配合に含まれる素材を解決できません。現在の配合は保持しています。");
                ctx.terrainTool->ApplyPaintMix(mix);
            }
            auto result = OpResult::Ok(); result.message = save ? "配合を保存しました。" : "配合を読み込みました。";
            return result;
        };
        registry.Register(std::move(op));
    }
}
} /// @note namespace fbzz::editor
