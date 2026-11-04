/// @file    VectorFieldOperators.cpp
/// @brief   速度場の照会と手続きベイクを直接APIへ公開する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Op/OperatorGroups.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/PlayModeController.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/VectorFieldFile.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <cmath>
#include <filesystem>
namespace fbzz::editor {
void RegisterVectorFieldOperators(OperatorRegistry& registry)
{
    for (const bool bake : {false, true}) {
        EditorOperator op; op.id = bake ? "asset.vector_field.bake" : "asset.vector_field.inspect"; op.label = op.id; op.category = "Assets";
        op.kind = bake ? OpKind::Action : OpKind::Query;
        op.desc = "Assets内の速度場PNG/FGAを読む。bakeはCurl/Vortex/Tornado/Sphere/TurbulenceからPNGとmetaを保存する。既存速度場を焼き直すにはoverwrite=true。";
        OpParam path; path.name = "path"; op.params.push_back(path);
        if (bake) {
            OpParam p; p.name = "recipe"; p.enumValues = {"Curl", "Vortex", "Tornado", "Sphere", "Turbulence"}; op.params.push_back(p);
            for (const char* name : {"resolution", "seed"}) { p = {}; p.name = name; p.type = OpParamType::Int; p.required = false; op.params.push_back(p); }
            p = {}; p.name = "extents"; p.type = OpParamType::Vec3; p.required = false; op.params.push_back(p);
            p = {}; p.name = "strength"; p.type = OpParamType::Float; p.required = false; op.params.push_back(p);
            p = {}; p.name = "overwrite"; p.type = OpParamType::Bool; p.required = false; op.params.push_back(p);
        }
        op.poll = [bake](const OpContext& context, const OpArgs&) { return !bake || !context.ctx.playMode || context.ctx.playMode->IsInEditor(); };
        op.exec = [bake](OpContext& context, const OpArgs& args) {
            auto& ctx = context.ctx; auto requested = args.GetString("path");
            if (requested.starts_with("guid:")) requested = asset::AssetDatabase::PathFromGuid(asset::AssetDatabase::GuidFromRef(requested));
            if (ctx.projectRoot.empty() || requested.empty()) return OpResult::Err("BAD_PATH", "Assets内の速度場を指定してください。");
            const auto root = util::FileSystem::PathFromUtf8(ctx.projectRoot); auto file = util::FileSystem::PathFromUtf8(requested);
            if (file.is_relative()) file = root / file;
            std::error_code error; file = std::filesystem::weakly_canonical(file, error);
            const auto assets = std::filesystem::weakly_canonical(root / "Assets", error), relative = file.lexically_relative(assets);
            if (error || relative.empty() || relative.is_absolute() || *relative.begin() == ".." || (file.extension() != ".png" && (!bake && file.extension() != ".fga"))) return OpResult::Err("BAD_PATH", "Assets内のPNG/FGAを指定してください。");
            if (bake && file.extension() != ".png") return OpResult::Err("BAD_PATH", "出力先は.pngです。");
            const auto path = util::FileSystem::PathToUtf8(file); fluid::VectorFieldAsset field;
            if (bake) {
                const int resolution = args.GetInt("resolution", 32), seed = args.GetInt("seed", 1); const float strength = args.GetFloat("strength", 1);
                const auto extents = args.GetVec3("extents", {5, 5, 5});
                const auto valid = [](float value) { return std::isfinite(value) && value >= 0.01f && value <= 1000; };
                if (resolution < 8 || resolution > 64 || seed < 1 || seed > 100000 || !std::isfinite(strength) || strength < -100 || strength > 100 || !valid(extents.x) || !valid(extents.y) || !valid(extents.z)) return OpResult::Err("BAD_ARG", "解像度8..64、seed1..100000、extents0.01..1000、strength-100..100です。");
                if (util::FileSystem::Exists(file) && (!args.GetBool("overwrite") || !asset::IsVectorFieldPng(path))) return OpResult::Err("EXISTS", "既存の速度場だけoverwrite=trueで焼き直せます。");
                if (!util::FileSystem::Exists(file) && util::FileSystem::Exists(path + ".meta")) return OpResult::Err("EXISTS", "出力先にmetaが残っています。");
                const auto recipe = args.GetString("recipe"); fluid::VectorFieldRecipe selected = fluid::VectorFieldRecipe::Curl;
                for (int i = 0; i < 5; ++i) if (recipe == fluid::VectorFieldRecipeName(static_cast<fluid::VectorFieldRecipe>(i))) selected = static_cast<fluid::VectorFieldRecipe>(i);
                fluid::BakeVectorField(selected, static_cast<std::uint32_t>(resolution), extents, static_cast<std::uint32_t>(seed), strength, field);
                if (!util::FileSystem::EnsureDirectory(file.parent_path()) || !asset::SaveVectorField(path, field)) return OpResult::Err("SAVE_FAILED", "速度場を保存できません。");
                asset::AssetManager::Unload<fluid::VectorFieldAsset>(path); asset::AssetManager::BumpAssetGeneration(); ctx.requestAssetBrowserRefresh = true;
            } else if (!(file.extension() == ".fga" ? asset::ImportFgaFile(path, field) : asset::LoadVectorFieldFile(path, field))) return OpResult::Err("READ_FAILED", "速度場を読み取れません。");
            const auto guid = bake ? asset::AssetDatabase::GuidFromPath(path) : asset::AssetDatabase::TryGetGuidFromPath(path);
            OpData data = OpData::MakeObject(); data.Set("path", OpData(path)); data.Set("reference", OpData(guid.empty() ? "" : "guid:" + guid));
            data.Set("sizeX", OpData(field.sizeX)); data.Set("sizeY", OpData(field.sizeY)); data.Set("sizeZ", OpData(field.sizeZ));
            data.Set("boundsMin", OpData(field.boundsMin)); data.Set("boundsMax", OpData(field.boundsMax)); data.Set("maxMagnitude", OpData(field.maxMagnitude));
            data.Set("sampleCount", OpData(static_cast<std::int64_t>(field.data.size()))); return OpResult::Data(std::move(data));
        }; registry.Register(std::move(op));
    }
}
} /// @note namespace fbzz::editor
