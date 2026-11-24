/// @file    AssetFileOperators.cpp
/// @brief   アセットの作成・複写・移動・ごみ箱退避を直接APIへ公開する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Editor/Op/OperatorGroups.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Play/PlayModeController.hpp>
#include <Editor/Asset/Storage/AssetCreation.hpp>
#include <Editor/Asset/Storage/AssetDirtyRegistry.hpp>
#include <Editor/Asset/Storage/AssetFileOperations.hpp>
#include <Editor/Script/Authoring/ScriptCodeGen.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/Data/DataAssetFactory.hpp>
#include <Engine/Asset/Model/Import/ModelSource.hpp>
#include <Engine/Audio/Synthesis/Synth.hpp>
#include <Engine/Util/Files/FileSystem.hpp>
#include <Engine/Util/Text/StringUtils.hpp>
#include <Engine/Util/Types/Uuid.hpp>
#include <Physics/Dynamics/PhysicsMaterial.hpp>
#include <toml++/toml.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>

namespace fbzz::editor {
namespace {
namespace fs = std::filesystem;
using util::FileSystem;

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

constexpr std::size_t MAX_BATCH_MOVES = 32;
constexpr std::size_t MAX_META_BYTES = 1024 * 1024;

struct ReviewedAssetMove {
    fs::path source;
    fs::path destination;
    std::string guid;
    std::string meta;
    uintmax_t sourceSize = 0;
    fs::file_time_type sourceWriteTime;

    bool operator==(const ReviewedAssetMove&) const = default;
};

struct AssetMoveReview {
    fs::path project;
    OpData request;
    std::vector<ReviewedAssetMove> moves;
    std::string receipt;
};

bool CanMoveAssetBatch(const OpContext& context, const OpArgs&)
{
    const auto& ctx = context.ctx;
    return !ctx.projectRoot.empty() && !ctx.InPrefabEditMode() && (!ctx.playMode || ctx.playMode->IsInEditor());
}

bool SameFilePath(const fs::path& left, const fs::path& right)
{
    const auto normalizedLeft = fs::path(left).make_preferred();
    const auto normalizedRight = fs::path(right).make_preferred();
    return ::CompareStringOrdinal(normalizedLeft.c_str(), -1, normalizedRight.c_str(), -1, TRUE) == CSTR_EQUAL;
}

fs::path WithMeta(fs::path path)
{
    return path += L".meta";
}

fs::path ResolveBatchPath(const fs::path& project, std::string input, bool isSource)
{
    if (input.empty() || input.size() > 1024) return {};
    if (isSource && asset::AssetDatabase::IsGuidRef(input)) {
        const auto indexed = FileSystem::PathFromUtf8(asset::AssetDatabase::PathFromGuid(asset::AssetDatabase::GuidFromRef(input)));
        if (indexed.empty()) return {};
        std::error_code error;
        const auto relative = fs::relative(indexed, project, error);
        if (error) return {};
        input = FileSystem::PathToUtf8(relative);
    }

    if (input.find_first_of(":*?\"<>|") != std::string::npos ||
        std::any_of(input.begin(), input.end(), [](unsigned char value) { return value < 32; })) return {};
    const auto relative = FileSystem::PathFromUtf8(input);
    if (relative.has_root_path() || !relative.has_filename() || relative.parent_path().empty() ||
        !SameFilePath(*relative.begin(), L"Assets")) return {};
    for (const auto& component : relative) {
        const auto& name = component.native();
        if (name.empty() || name.back() == L'.' || name.back() == L' ') return {};
    }

    /// @note Assets 配下のジャンクション等を通過すると、レビュー後に別の保管場所を変更するおそれがある。
    auto parent = project;
    for (const auto& component : relative.parent_path()) {
        parent /= component;
        const DWORD attributes = ::GetFileAttributesW(parent.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return {};
    }
    return fs::path(parent / relative.filename()).make_preferred();
}

bool IsPlainFile(const fs::path& path)
{
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}

bool IsVacant(const fs::path& path)
{
    return ::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES && ::GetLastError() == ERROR_FILE_NOT_FOUND;
}

bool ReadMoveSnapshot(ReviewedAssetMove& move)
{
    const auto sidecar = WithMeta(move.source);
    if (!IsPlainFile(move.source) || !IsPlainFile(sidecar)) return false;
    std::error_code error;
    move.sourceSize = fs::file_size(move.source, error);
    if (error) return false;
    move.sourceWriteTime = fs::last_write_time(move.source, error);
    if (error) return false;
    const auto metaSize = fs::file_size(sidecar, error);
    if (error || metaSize == 0 || metaSize > MAX_META_BYTES) return false;

    std::ifstream input(sidecar, std::ios::binary);
    move.meta.resize(static_cast<std::size_t>(metaSize));
    if (!input.read(move.meta.data(), static_cast<std::streamsize>(metaSize)) ||
        input.peek() != std::char_traits<char>::eof() || input.bad()) return false;
    const auto metadata = toml::parse(std::string_view(move.meta));
    if (!metadata) return false;
    const auto guid = metadata.table()["meta"]["guid"].value<std::string>();
    if (!guid || guid->empty()) return false;
    move.guid = util::StringUtils::ToLowerCopy(*guid);

    /// @note キャッシュの GUID とディスク上の所有者が一致することを確認する。照会時に GUID は発行しない。
    const auto indexed = asset::AssetDatabase::PathFromGuid(move.guid);
    if (indexed.empty()) return false;
    const auto owner = fs::weakly_canonical(FileSystem::PathFromUtf8(indexed), error);
    return !error && SameFilePath(owner, move.source);
}

OpResult BuildMoveReview(EditorContext& ctx, const OpData* request, AssetMoveReview& review)
{
    if (!request || !request->IsArray() || request->AsArray().empty() || request->AsArray().size() > MAX_BATCH_MOVES)
        return OpResult::Err("BAD_ARG", "moves must contain between 1 and 32 objects with path and destination.");
    if (ctx.modelImportBusy && ctx.modelImportBusy())
        return OpResult::Err("MODEL_IMPORT_BUSY", "Wait for the model import to finish.");

    std::error_code error;
    review.project = fs::canonical(FileSystem::PathFromUtf8(ctx.projectRoot), error);
    if (error) return OpResult::Err("BAD_PATH", "Cannot open the project directory.");
    fs::path openScene;
    if (!ctx.currentScenePath.empty()) {
        const auto current = FileSystem::PathFromUtf8(ctx.currentScenePath);
        openScene = fs::weakly_canonical(current.is_absolute() ? current : review.project / current, error);
        if (error) return OpResult::Err("BAD_PATH", "Cannot resolve the open scene.");
    }

    for (const auto& item : request->AsArray()) {
        const auto* source = item.Find("path");
        const auto* destination = item.Find("destination");
        if (!source || !source->IsString() || !destination || !destination->IsString())
            return OpResult::Err("BAD_ARG", "Each move requires a path and destination string.");

        ReviewedAssetMove move;
        move.source = ResolveBatchPath(review.project, source->AsString(), true);
        move.destination = ResolveBatchPath(review.project, destination->AsString(), false);
        if (move.source.empty() || move.destination.empty() ||
            !SameFilePath(move.source.extension(), move.destination.extension()))
            return OpResult::Err("BAD_PATH", "Use paths under Assets with existing parent folders and the same extension: " +
                source->AsString() + " -> " + destination->AsString());
        const auto sourceName = FileSystem::PathToUtf8(move.source);
        if (!asset::AssetDatabase::ShouldHaveMeta(util::StringUtils::ToLowerCopy(FileSystem::PathToUtf8(move.source.filename()))) ||
            asset::IsModelSourcePath(sourceName) || SameFilePath(move.source, openScene))
            return OpResult::Err("UNSUPPORTED_ASSET", "Use a registered asset file outside the open scene and model import pipeline: " + source->AsString());
        if (HasDirtyAssets(sourceName))
            return OpResult::Err("UNSAVED_ASSET", "Save the asset before moving it: " + source->AsString());
        if (!IsVacant(move.destination) || !IsVacant(WithMeta(move.destination)))
            return OpResult::Err("DESTINATION_OCCUPIED", "The destination or its sidecar is unavailable: " + destination->AsString());
        if (!ReadMoveSnapshot(move))
            return OpResult::Err("ASSET_GUID_UNAVAILABLE", "Cannot verify an indexed GUID and its sidecar: " + source->AsString());
        if (std::any_of(review.moves.begin(), review.moves.end(), [&](const auto& previous) {
                return previous.guid == move.guid || SameFilePath(previous.destination, move.destination);
            }))
            return OpResult::Err("DUPLICATE_MOVE", "The batch repeats a source GUID or destination.");
        review.moves.push_back(std::move(move));
    }
    review.request = *request;
    return OpResult::Ok();
}

OpData MoveReviewData(const AssetMoveReview& review)
{
    const auto relative = [&review](const fs::path& path) {
        return FileSystem::PathToUtf8(path.lexically_relative(review.project));
    };
    OpData moves = OpData::MakeArray();
    for (const auto& move : review.moves) {
        OpData item = OpData::MakeObject();
        item.Set("guid", move.guid);
        item.Set("source", relative(move.source));
        item.Set("destination", relative(move.destination));
        item.Set("sourceMeta", relative(WithMeta(move.source)));
        item.Set("destinationMeta", relative(WithMeta(move.destination)));
        moves.Push(std::move(item));
    }
    OpData data = OpData::MakeObject();
    data.Set("moves", std::move(moves));
    return data;
}

OpResult ApplyMoveReview(EditorContext& ctx, const AssetMoveReview& review)
{
    std::vector<AssetFileMove> files;
    files.reserve(review.moves.size() * 2);
    for (const auto& move : review.moves) {
        files.push_back({move.source, move.destination});
        files.push_back({WithMeta(move.source), WithMeta(move.destination)});
    }

    const auto result = MoveAssetFilesWithRollback(files);
    if (result.systemError != 0) {
        std::string message = "Move failed after " + std::to_string(result.movedCount / 2) + " prior pair(s). ";
        message += result.unrestored.empty() ? "All moved files were restored. " : "Some files could not be restored. ";
        message += "Failed file: " + FileSystem::PathToUtf8(files[result.movedCount].source) +
            " (Windows error " + std::to_string(result.systemError) + ").";
        for (const auto index : result.unrestored) {
            message += " Restore " + FileSystem::PathToUtf8(files[index].destination) +
                " to " + FileSystem::PathToUtf8(files[index].source) + ".";
        }
        ctx.requestAssetBrowserRefresh = true;
        return OpResult::Err(result.unrestored.empty() ? "MOVE_FAILED" : "ROLLBACK_INCOMPLETE", std::move(message));
    }

    for (const auto& move : review.moves)
        asset::AssetDatabase::OnAssetMoved(FileSystem::PathToUtf8(move.source), FileSystem::PathToUtf8(move.destination));
    asset::AssetManager::FlushFailed();
    ctx.requestAssetBrowserRefresh = true;
    return OpResult::Data(MoveReviewData(review));
}

void RegisterAssetMoveBatch(OperatorRegistry& registry)
{
    const auto review = std::make_shared<std::optional<AssetMoveReview>>();
    EditorOperator preview;
    preview.id = "asset.move_batch.preview";
    preview.label = "Preview Asset Moves";
    preview.category = "Assets";
    preview.desc = "List the original and destination files, their .meta sidecars, and unchanged GUIDs before a batch move.";
    preview.kind = OpKind::Query;
    preview.dialog = OpDialog::NONE;
    preview.poll = CanMoveAssetBatch;
    OpParam moves;
    moves.name = "moves";
    moves.type = OpParamType::Data;
    moves.desc = "Array of 1–32 {path, destination} objects. Source can be an Assets/... path or guid: reference. Destination is an Assets/... path.";
    preview.params.push_back(std::move(moves));
    preview.exec = [review](OpContext& context, const OpArgs& args) -> OpResult {
        AssetMoveReview candidate;
        auto result = BuildMoveReview(context.ctx, args.GetData("moves"), candidate);
        if (!result.ok) return result;
        candidate.receipt = util::GenerateUUID();
        OpData data = MoveReviewData(candidate);
        data.Set("receipt", candidate.receipt);
        *review = std::move(candidate);
        return OpResult::Data(std::move(data));
    };
    registry.Register(std::move(preview));

    EditorOperator apply;
    apply.id = "asset.move_batch.apply";
    apply.label = "Apply Reviewed Asset Moves";
    apply.category = "Assets";
    apply.desc = "Apply the reviewed file and .meta moves without overwriting; restore completed moves if a move fails.";
    apply.kind = OpKind::Action;
    apply.dialog = OpDialog::NONE;
    apply.poll = CanMoveAssetBatch;
    OpParam receipt;
    receipt.name = "receipt";
    receipt.type = OpParamType::String;
    receipt.desc = "Single-use receipt from the latest asset.move_batch.preview.";
    apply.params.push_back(std::move(receipt));
    apply.exec = [review](OpContext& context, const OpArgs& args) -> OpResult {
        if (!*review || args.GetString("receipt") != (*review)->receipt)
            return OpResult::Err("STALE_REVIEW", "Review the moves again before applying.");

        AssetMoveReview reviewed = std::move(**review);
        review->reset();
        AssetMoveReview current;
        const auto validation = BuildMoveReview(context.ctx, &reviewed.request, current);
        if (!validation.ok || current.project != reviewed.project || current.moves != reviewed.moves)
            return OpResult::Err("STALE_REVIEW", "The project or files changed. Review the moves again. " + validation.message);
        return ApplyMoveReview(context.ctx, reviewed);
    };
    registry.Register(std::move(apply));
}
} /// @note namespace
void RegisterAssetFileOperators(OperatorRegistry& registry)
{
    RegisterAssetMoveBatch(registry);
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
