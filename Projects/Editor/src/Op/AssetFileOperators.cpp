/// @file    AssetFileOperators.cpp
/// @brief   アセットの作成・複写・移動・ごみ箱退避を直接APIへ公開する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include <Editor/Op/OperatorGroups.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/AssetCreation.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/AssetFileOperations.hpp>
#include <Editor/Util/ScriptCodeGen.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/DataAssetFactory.hpp>
#include <Engine/Asset/ModelSource.hpp>
#include <Engine/Audio/Synth.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <Physics/PhysicsMaterial.hpp>
#include <filesystem>

namespace fbzz::editor {
namespace {
struct AssetKind { const char* name; const char* extension; };
constexpr AssetKind KINDS[] = {{"folder", ""}, {"scene", ".scene"}, {"material", ".mat"}, {"vector_field", ".png"},
    {"fluid", ".fluid"}, {"post_process_profile", ".fzdata"}, {"animator_controller", ".animcontroller"},
    {"sequence", ".sequence"}, {"avatar_mask", ".mask"}, {"behavior_tree", ".behaviortree"},
    {"physics_material", ".physmat"}, {"synth", ".synth"}, {"data_asset", ".fzdata"}};
std::string ResolvePath(EditorContext& ctx, const std::string& input, bool missing, bool baked = false)
{
    if (input.empty() || ctx.projectRoot.empty()) return {};
    const std::string resolved = input.starts_with("guid:")
        ? asset::AssetDatabase::PathFromGuid(asset::AssetDatabase::GuidFromRef(input)) : input;
    if (resolved.empty()) return {};
    auto path = util::FileSystem::PathFromUtf8(resolved);
    if (path.is_relative()) path = util::FileSystem::PathFromUtf8(ctx.projectRoot) / path;
    std::error_code error;
    path = std::filesystem::weakly_canonical(path, error);
    if (error || path.extension() == ".meta") return {};
    const auto root = std::filesystem::weakly_canonical(util::FileSystem::PathFromUtf8(ctx.projectRoot) / (baked ? "Library/Baked" : "Assets"), error);
    if (error) return {};
    const auto relative = path.lexically_relative(root);
    if (relative.empty() || relative == "." || relative.is_absolute() || *relative.begin() == "..") return {};
    if (!missing && !std::filesystem::exists(path, error)) return {};
    if (missing && (!std::filesystem::is_directory(path.parent_path(), error) || std::filesystem::exists(path, error)
        || std::filesystem::exists(path.string() + ".meta", error))) return {};
    return error ? std::string{} : util::FileSystem::PathToUtf8(path);
}
bool HasDirtyAssets(const std::string& path)
{
    for (const auto& entry : AssetDirtyRegistry::GetAll())
        if (util::FileSystem::SamePathText(path, entry.path) || util::FileSystem::IsChildPathText(entry.path, path)) return true;
    return false;
}
OpData ResultPath(const std::string& path)
{
    OpData result = OpData::MakeObject(); result.Set("path", OpData(path));
    const std::string guid = asset::AssetDatabase::TryGetGuidFromPath(path);
    result.Set("reference", OpData(guid.empty() ? "" : "guid:" + guid)); return result;
}
} /// @note namespace
void RegisterAssetFileOperators(OperatorRegistry& registry)
{
    {
        EditorOperator op; op.id = "asset.creation_catalog"; op.label = op.id; op.category = "Assets"; op.kind = OpKind::Query;
        op.exec = [](OpContext&, const OpArgs&) {
            OpData data = OpData::MakeObject(); OpData kinds = OpData::MakeArray();
            for (const auto& kind : KINDS) { OpData item = OpData::MakeObject(); item.Set("kind", OpData(kind.name)); item.Set("extension", OpData(kind.extension)); kinds.Push(item); }
            data.Set("kinds", std::move(kinds)); OpData physics = OpData::MakeArray(), synth = OpData::MakeArray(), types = OpData::MakeArray();
            for (int i = 0; i < physics::PhysicsMaterial::PRESET_COUNT; ++i) physics.Push(OpData(physics::PhysicsMaterial::PresetName(i)));
            for (int i = 0; i < static_cast<int>(audio::SynthPreset::Count); ++i) synth.Push(OpData(audio::PresetToString(static_cast<audio::SynthPreset>(i))));
            for (const auto& type : asset::DataAssetFactory::RegisteredTypeNames()) types.Push(OpData(type));
            data.Set("physicsPresets", std::move(physics)); data.Set("synthPresets", std::move(synth)); data.Set("dataAssetTypes", std::move(types)); return OpResult::Data(std::move(data));
        }; registry.Register(std::move(op));
    }
    {
        EditorOperator op; op.id = "asset.hlsl.create"; op.label = op.id; op.category = "Assets"; op.kind = OpKind::Action;
        op.desc = "プロジェクトのAssets/ShadersへHLSLテンプレートを生成する。既存ファイルは上書きしない。";
        OpParam name; name.name = "name"; op.params.push_back(name);
        OpParam kind; kind.name = "kind"; kind.enumValues = {"surface", "particle", "post_process", "compute"}; op.params.push_back(kind);
        op.poll = [](const OpContext& context, const OpArgs&) { return !context.ctx.projectRoot.empty() && (!context.ctx.playMode || context.ctx.playMode->IsInEditor()); };
        op.exec = [](OpContext& context, const OpArgs& args) {
            const auto name = args.GetString("name"), kind = args.GetString("kind");
            if (!ScriptCodeGen::IsValidCSharpName(name) || name.size() > 128) return OpResult::Err("BAD_ARG", "nameはASCIIの識別子です。");
            auto type = ScriptCodeGen::HlslKind::SurfaceVSPS;
            if (kind == "particle") type = ScriptCodeGen::HlslKind::ParticlePS;
            else if (kind == "post_process") type = ScriptCodeGen::HlslKind::PostProcessVSPS;
            else if (kind == "compute") type = ScriptCodeGen::HlslKind::ComputeCS;
            const std::string directory = context.ctx.projectRoot + "/Assets/Shaders";
            const auto root = util::FileSystem::PathFromUtf8(context.ctx.projectRoot);
            std::error_code error;
            const auto destination = std::filesystem::weakly_canonical(util::FileSystem::PathFromUtf8(directory), error);
            const auto assets = std::filesystem::weakly_canonical(root / "Assets", error);
            const auto relative = destination.lexically_relative(assets);
            if (error || relative.empty() || relative.is_absolute() || *relative.begin() == "..") return OpResult::Err("BAD_PATH", "Assets/Shadersを確認してください。");
            const auto subdirectory = kind == "surface" || kind == "particle" ? "Material/Custom" : "PostProcess/Custom";
            const auto candidate = std::filesystem::weakly_canonical(destination / subdirectory / (name + (kind == "compute" ? ".cs.hlsl" : ".hlsl")), error);
            const auto outputRelative = candidate.lexically_relative(assets);
            if (error || outputRelative.empty() || outputRelative.is_absolute() || *outputRelative.begin() == ".."
                || util::FileSystem::Exists(candidate.string() + ".meta")) return OpResult::Err("BAD_PATH", "出力先と既存metaを確認してください。");
            const auto path = ScriptCodeGen::CreateHlsl(name, directory, type);
            if (path.empty()) return OpResult::Err("CREATE_FAILED", "HLSLを作成できません。既存ファイルと書込先を確認してください。");
            static_cast<void>(asset::AssetDatabase::GuidFromPath(path)); context.ctx.requestAssetBrowserRefresh = true;
            return OpResult::Data(ResultPath(path));
        }; registry.Register(std::move(op));
    }
    for (const char* action : {"create", "move", "rename", "duplicate", "extract", "trash"}) {
        EditorOperator op; const std::string name = action;
        op.id = "asset." + name; op.label = op.id; op.category = "Assets"; op.kind = OpKind::Action;
        op.desc = "Assets配下のファイル操作。既存出力は上書きしない。move/renameはGUIDを保ち、duplicateは新GUID、trashはmetaと一緒に.fbzz/Trashへ退避する。未保存アセットは先に保存が必要。";
        OpParam path; path.name = "path"; path.type = OpParamType::String; op.params.push_back(std::move(path));
        if (name == "create") {
            OpParam kind; kind.name = "kind"; kind.type = OpParamType::String; for (const auto& entry : KINDS) kind.enumValues.push_back(entry.name); op.params.push_back(std::move(kind));
            OpParam preset; preset.name = "preset"; preset.type = OpParamType::String; preset.required = false; preset.desc = "asset.creation_catalogのプリセット名/型名"; op.params.push_back(std::move(preset));
        }
        if (name == "move" || name == "duplicate" || name == "extract") {
            OpParam destination; destination.name = "destination"; destination.type = OpParamType::String;
            destination.required = name != "duplicate"; destination.desc = "Assets起点の完全な出力パス。親フォルダは必要。"; op.params.push_back(std::move(destination));
        }
        if (name == "rename") { OpParam value; value.name = "name"; value.type = OpParamType::String; value.desc = "拡張子を除く新しい名前"; op.params.push_back(std::move(value)); }
        op.poll = [](const OpContext& context, const OpArgs&) { return !context.ctx.projectRoot.empty() && (!context.ctx.playMode || context.ctx.playMode->IsInEditor()); };
        op.exec = [name](OpContext& context, const OpArgs& args) {
            auto& ctx = context.ctx;
            if (ctx.modelImportBusy && ctx.modelImportBusy()) return OpResult::Err("MODEL_IMPORT_BUSY", "モデル変換の完了後に操作してください。");
            const std::string source = ResolvePath(ctx, args.GetString("path"), name == "create", name == "extract");
            if (source.empty()) return OpResult::Err("BAD_PATH", "Assets内の有効なパスを指定してください。extractの原本はLibrary/Baked内です。");
            if (name != "create" && HasDirtyAssets(source)) return OpResult::Err("UNSAVED_ASSET", "未保存アセットを先に保存してください。");
            std::string destination;
            if (name == "create") {
                const std::string kind = args.GetString("kind"); const std::string extension = util::StringUtils::ToLower(util::FileSystem::GetExtension(source));
                for (const auto& entry : KINDS) if (kind == entry.name && extension != entry.extension && kind != "folder")
                    return OpResult::Err("BAD_PATH", "kindと拡張子が一致しません。");
                if (!CreateEditorAsset(kind, source, args.GetString("preset"))) return OpResult::Err("CREATE_FAILED", "アセットを作成できません。presetと型名を確認してください。");
                destination = source;
            } else if (name == "trash") {
                std::string trash; if (TrashAssets({source}, ctx, &trash) != 1) return OpResult::Err("TRASH_FAILED", "ごみ箱へ退避できません。");
                OpData data = OpData::MakeObject(); data.Set("source", OpData(source)); data.Set("trashRoot", OpData(trash)); return OpResult::Data(std::move(data));
            } else {
                std::string requested = args.GetString("destination");
                const bool directory = util::FileSystem::IsDirectory(source);
                if (name == "rename") {
                    const std::string stem = args.GetString("name");
                    if (stem.empty() || stem == "." || stem == ".." || stem.find_first_of("/\\:*?\"<>|") != std::string::npos)
                        return OpResult::Err("BAD_ARG", "名前が不正です。");
                    const auto original = util::FileSystem::PathFromUtf8(source);
                    requested = util::FileSystem::PathToUtf8(original.parent_path() / util::FileSystem::PathFromUtf8(stem + (directory ? "" : util::FileSystem::PathToUtf8(original.extension()))));
                } else if (name == "duplicate" && requested.empty()) requested = UniqueDuplicatePath(source, directory);
                destination = ResolvePath(ctx, requested, true);
                if (destination.empty() || (directory && util::FileSystem::IsChildPathText(destination, source))) return OpResult::Err("BAD_PATH", "出力先が不正、存在済み、または元フォルダ内です。");
                if (name == "move" || name == "rename") {
                    if (!directory && util::FileSystem::PathFromUtf8(source).extension() != util::FileSystem::PathFromUtf8(destination).extension()) return OpResult::Err("BAD_PATH", "移動でアセット形式は変更できません。");
                    if (!MoveAssetWithSidecar(source, destination)) return OpResult::Err("MOVE_FAILED", "本体とmetaを移動できません。");
                } else if (!CopyAssetPath(source, destination, directory)) return OpResult::Err("COPY_FAILED", "アセットを複写できません。");
            }
            if (!asset::IsModelSourcePath(destination)) static_cast<void>(asset::AssetDatabase::GuidFromPath(destination));
            asset::AssetManager::FlushFailed(); ctx.requestAssetBrowserRefresh = true;
            return OpResult::Data(ResultPath(destination));
        }; registry.Register(std::move(op));
    }
}
} /// @note namespace fbzz::editor
