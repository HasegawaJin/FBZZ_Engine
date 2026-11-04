/// @file    ModelImportOperators.cpp
/// @brief   モデル原本の検査・設定・変換をMCPとパレットへ公開する。
/// @author  Hasegawa Jin
/// @date    2026-11-03
#include <Editor/Op/OperatorGroups.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Import/FbxMetaSerializer.hpp>
#include <Editor/Import/ModelImport.hpp>
#include <Editor/PlayModeController.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/ModelAsset.hpp>
#include <Engine/Asset/ModelSource.hpp>
#include <Engine/Core/Concurrency/TaskSystem.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace fbzz::editor {
namespace {

constexpr const char* SOURCE_DCC[] = {"Auto", "Maya", "Blender"};
constexpr const char* UP_AXIS[] = {"Auto", "YUp", "ZUp"};
constexpr const char* NORMAL_CONVENTION[] = {"DirectX", "OpenGL"};
constexpr const char* COMPRESSION[] = {"Auto", "BC1", "BC3", "BC4", "BC5", "BC6H", "BC7", "None"};

OpData Strings(const std::vector<std::string>& values)
{
    OpData result = OpData::MakeArray();
    for (const auto& value : values) result.Push(OpData(value));
    return result;
}

template<std::size_t N, typename T>
bool ReadEnum(const OpData& data, T& output, const char* const (&values)[N])
{
    if (!data.IsString()) return false;
    for (std::size_t i = 0; i < N; ++i) {
        if (data.AsString() != values[i]) continue;
        output = static_cast<T>(i);
        return true;
    }
    return false;
}

bool ReadStrings(const OpData& data, std::vector<std::string>& output)
{
    if (!data.IsArray() || data.AsArray().size() > 1024) return false;
    std::vector<std::string> result;
    for (const auto& item : data.AsArray()) {
        if (!item.IsString() || item.AsString().empty()) return false;
        if (std::find(result.begin(), result.end(), item.AsString()) != result.end()) return false;
        result.push_back(item.AsString());
    }
    output = std::move(result);
    return true;
}

bool ReadNumber(const OpData& data, double& output)
{
    if (!data.IsInt() && !data.IsFloat()) return false;
    const double value = data.IsInt() ? static_cast<double>(data.AsInt64()) : data.AsDouble();
    if (!std::isfinite(value)) return false;
    output = value;
    return true;
}

bool ReadClips(const OpData& data, std::vector<AnimationClipImportSettings>& output)
{
    if (!data.IsArray() || data.AsArray().size() > 1024) return false;
    std::vector<AnimationClipImportSettings> result;
    for (const auto& row : data.AsArray()) {
        if (!row.IsObject()) return false;
        AnimationClipImportSettings clip;
        for (const auto& [key, value] : row.AsObject()) {
            if (key == "name") {
                if (!value.IsString() || value.AsString().empty()) return false;
                clip.name = value.AsString();
            } else if (key == "loop") {
                if (!value.IsBool()) return false;
                clip.loop = value.AsBool();
            } else if (key == "startFrame") {
                if (!ReadNumber(value, clip.startFrame) || clip.startFrame < 0.0) return false;
            } else if (key == "endFrame") {
                if (!ReadNumber(value, clip.endFrame) || clip.endFrame < -1.0) return false;
            } else if (key == "outputName") {
                if (!value.IsString()) return false;
                clip.outputName = value.AsString();
                if (clip.outputName.find_first_of("/\\:") != std::string::npos || clip.outputName == "." || clip.outputName == "..") return false;
            } else return false;
        }
        if (clip.name.empty() || (clip.endFrame >= 0.0 && clip.endFrame <= clip.startFrame)) return false;
        for (const auto& previous : result)
            if (previous.name == clip.name || (!clip.outputName.empty() && previous.outputName == clip.outputName)) return false;
        result.push_back(std::move(clip));
    }
    output = std::move(result);
    return true;
}

/// @note 未指定項目は保存済み設定を保持し、不正入力は原本・設定・生成物へ触れる前に拒否する。
bool PatchOptions(const OpData& data, FbxImportOptions& options)
{
    if (!data.IsObject()) return false;
    FbxImportOptions staged = options;
    for (const auto& [key, value] : data.AsObject()) {
        if (key == "sourceDcc") { if (!ReadEnum(value, staged.sourceDcc, SOURCE_DCC)) return false; }
        else if (key == "upAxis") { if (!ReadEnum(value, staged.upAxis, UP_AXIS)) return false; }
        else if (key == "normalMapConvention") { if (!ReadEnum(value, staged.normalMapConvention, NORMAL_CONVENTION)) return false; }
        else if (key == "defaultCompression") { if (!ReadEnum(value, staged.defaultCompression, COMPRESSION)) return false; }
        else if (key == "unitScaleMultiplier") {
            double scale = 0.0;
            if (!ReadNumber(value, scale) || scale <= 0.0 || scale > 10000.0) return false;
            staged.unitScaleMultiplier = static_cast<float>(scale);
        } else if (key == "generateNormals" || key == "generateTangents" || key == "generateTexDescriptors") {
            if (!value.IsBool()) return false;
            if (key == "generateNormals") staged.generateNormals = value.AsBool();
            else if (key == "generateTangents") staged.generateTangents = value.AsBool();
            else staged.generateTexDescriptors = value.AsBool();
        } else if (key == "rootMotionNodeName") {
            if (!value.IsString()) return false;
            staged.rootMotionNodeName = value.AsString();
        } else if (key == "selectedMeshNames") { if (!ReadStrings(value, staged.selectedMeshNames)) return false; }
        else if (key == "selectedAnimNames") { if (!ReadStrings(value, staged.selectedAnimNames)) return false; }
        else if (key == "clipSettings") { if (!ReadClips(value, staged.clipSettings)) return false; }
        else return false;
    }
    options = std::move(staged);
    return true;
}

OpData DescribeOptions(const FbxImportOptions& options)
{
    OpData data = OpData::MakeObject();
    data.Set("sourceDcc", OpData(SOURCE_DCC[static_cast<int>(options.sourceDcc)]));
    data.Set("upAxis", OpData(UP_AXIS[static_cast<int>(options.upAxis)]));
    data.Set("normalMapConvention", OpData(NORMAL_CONVENTION[static_cast<int>(options.normalMapConvention)]));
    data.Set("defaultCompression", OpData(COMPRESSION[static_cast<int>(options.defaultCompression)]));
    data.Set("unitScaleMultiplier", OpData(options.unitScaleMultiplier));
    data.Set("generateNormals", OpData(options.generateNormals));
    data.Set("generateTangents", OpData(options.generateTangents));
    data.Set("generateTexDescriptors", OpData(options.generateTexDescriptors));
    data.Set("rootMotionNodeName", OpData(options.rootMotionNodeName));
    data.Set("selectedMeshNames", Strings(options.selectedMeshNames));
    data.Set("selectedAnimNames", Strings(options.selectedAnimNames));
    OpData clips = OpData::MakeArray();
    for (const auto& clip : options.clipSettings) {
        OpData row = OpData::MakeObject();
        row.Set("name", OpData(clip.name)); row.Set("loop", OpData(clip.loop));
        row.Set("startFrame", OpData(clip.startFrame)); row.Set("endFrame", OpData(clip.endFrame));
        row.Set("outputName", OpData(clip.outputName)); clips.Push(std::move(row));
    }
    data.Set("clipSettings", std::move(clips));
    return data;
}

/// @note canonicalなAssets配下の原本だけを受け、Library派生物と外部SDKを書き換えない。
std::string ResolveModelSource(const EditorContext& ctx, const std::string& input)
{
    namespace fs = std::filesystem;
    if (input.empty() || ctx.projectRoot.empty()) return {};
    std::string path = input;
    if (path.starts_with("guid:")) path = asset::AssetDatabase::PathFromGuid(asset::AssetDatabase::GuidFromRef(path));
    if (path.empty()) return {};
    std::error_code error;
    fs::path source = util::FileSystem::PathFromUtf8(path);
    const fs::path root = fs::weakly_canonical(util::FileSystem::PathFromUtf8(ctx.projectRoot) / "Assets", error);
    if (error) return {};
    if (source.is_relative()) source = util::FileSystem::PathFromUtf8(ctx.projectRoot) / source;
    source = fs::weakly_canonical(source, error);
    if (error || !fs::is_regular_file(source, error) || error) return {};
    const fs::path relative = fs::relative(source, root, error);
    if (error || relative.empty() || relative.is_absolute()) return {};
    for (const auto& part : relative) if (part == "..") return {};
    if (!asset::IsModelSourcePath(util::FileSystem::PathToUtf8(source))) return {};
    return util::FileSystem::PathToUtf8(source);
}

OpData Outputs(const std::string& projectRoot, const std::string& sourceGuid)
{
    namespace fs = std::filesystem;
    OpData outputs = OpData::MakeArray();
    const fs::path root = util::FileSystem::PathFromUtf8(projectRoot) / "Library" / "Baked" / sourceGuid;
    std::error_code error;
    std::vector<std::string> keys;
    for (fs::recursive_directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
        if (!it->is_regular_file(error) || error) continue;
        const fs::path relative = fs::relative(it->path(), root, error);
        if (error) break;
        keys.push_back(util::FileSystem::PathToUtf8(relative));
    }
    std::sort(keys.begin(), keys.end());
    for (const auto& key : keys) {
        OpData row = OpData::MakeObject();
        row.Set("path", OpData(util::FileSystem::PathToUtf8(root / util::FileSystem::PathFromUtf8(key))));
        row.Set("subKey", OpData(key));
        const std::string extension = fs::path(key).extension().string();
        if (extension == ".anim" || extension == ".mat" || extension == ".png")
            row.Set("reference", OpData("guid:" + asset::AssetDatabase::DeriveGuid(sourceGuid, key)));
        outputs.Push(std::move(row));
    }
    return outputs;
}

struct ModelImportJob {
    int id = 0;
    std::string projectRoot;
    std::string path;
    std::string sourceGuid;
    std::future<ModelImportResult> future;
    OpData status;
    bool finished = false;
};

/// @note 登録簿の実行と状態照会が共有所有し、ワーカーはリクエスト値だけを保持する。
struct ModelImportJobs {
    int nextId = 1;
    std::vector<ModelImportJob> jobs;
    ~ModelImportJobs()
    {
        for (auto& job : jobs) if (job.future.valid()) job.future.wait();
    }
};

} /// @note namespace

void RegisterModelImportOperators(OperatorRegistry& registry)
{
    const auto jobs = std::make_shared<ModelImportJobs>();
    for (const char* id : {"asset.model.inspect", "asset.model.configure", "asset.model.import"}) {
        EditorOperator op;
        op.id = id; op.category = "AssetBrowser"; op.label = id;
        const bool inspect = op.id == "asset.model.inspect";
        const bool configure = op.id == "asset.model.configure";
        op.kind = inspect ? OpKind::Query : OpKind::Action;
        op.desc = inspect ? "モデル原本のメッシュ・クリップ・保存済み変換設定・生成物とGUIDを読む。"
            : configure ? "モデル変換設定の指定項目を保存する。変換はasset.model.importで実行する。"
            : "保存済み設定とoptionsでモデル原本を変換し、成功後に設定・キャッシュ・索引を更新する。";
        OpParam path; path.name = "path"; path.type = OpParamType::String;
        path.desc = "Assets起点パス、プロジェクト内絶対パス、または原本のguid:参照";
        op.params.push_back(std::move(path));
        if (!inspect) {
            OpParam options; options.name = "options"; options.type = OpParamType::Data;
            options.required = configure;
            options.desc = "inspectのoptionsと同じキーを部分指定。sourceDcc=Auto/Maya/Blender、upAxis=Auto/YUp/ZUp、normalMapConvention=DirectX/OpenGL。clipSettingsはname/loop/startFrame/endFrame/outputNameの配列。";
            op.params.push_back(std::move(options));
            op.caution = "原本のmetaと再生成可能なLibrary/Bakedを更新する。ファイル生成はUndo対象外。";
        }
        op.poll = [inspect](const OpContext& context, const OpArgs&) {
            return !context.ctx.projectRoot.empty() && (inspect || !context.ctx.playMode || context.ctx.playMode->IsInEditor());
        };
        op.exec = [inspect, configure, jobs](OpContext& context, const OpArgs& args) {
            if (!inspect) for (const auto& job : jobs->jobs)
                if (!job.finished) return OpResult::Err("MODEL_IMPORT_BUSY", "前のモデル変換をasset.model.statusで完了確認してください。");
            const std::string source = ResolveModelSource(context.ctx, args.GetString("path"));
            if (source.empty()) return OpResult::Err("MODEL_SOURCE_NOT_FOUND", "Assets配下のFBX/glTF/GLB/OBJ原本を指定してください。");
            FbxImportOptions options;
            const bool savedSettings = FbxMetaSerializer::LoadOptions(source, options);
            if (const OpData* patch = args.GetData("options"); patch && !PatchOptions(*patch, options))
                return OpResult::Err("BAD_ARG", "optionsのキー・型・値・クリップ範囲が不正です。asset.model.inspectで現在設定を確認してください。");
            const FbxScanResult scan = FbxImportTool::Scan(source);
            if (!scan.valid) return OpResult::Err("MODEL_SCAN_FAILED", "モデル原本を解析できませんでした。");
            for (const auto& name : options.selectedMeshNames)
                if (std::find(scan.meshNames.begin(), scan.meshNames.end(), name) == scan.meshNames.end())
                    return OpResult::Err("BAD_ARG", "存在しないメッシュ: " + name);
            for (const auto& name : options.selectedAnimNames)
                if (std::find(scan.animNames.begin(), scan.animNames.end(), name) == scan.animNames.end())
                    return OpResult::Err("BAD_ARG", "存在しないクリップ: " + name);
            for (const auto& clip : options.clipSettings)
                if (std::find(scan.animNames.begin(), scan.animNames.end(), clip.name) == scan.animNames.end())
                    return OpResult::Err("BAD_ARG", "存在しないクリップ設定: " + clip.name);
            if (configure) {
                const auto saved = SaveModelImportOptions(source, options);
                if (saved == ModelSettingsWriteResult::BUSY) return OpResult::Err("MODEL_IMPORT_BUSY", "モデル変換が実行中です。");
                if (saved == ModelSettingsWriteResult::FAILED) return OpResult::Err("MODEL_SETTINGS_WRITE_FAILED", "モデル設定を保存できませんでした。");
            }
            if (!inspect && !configure) {
                for (const auto& job : jobs->jobs)
                    if (!job.finished) return OpResult::Err("MODEL_IMPORT_BUSY", "前のモデル変換をasset.model.statusで完了確認してください。");
                if (jobs->jobs.size() >= 32) jobs->jobs.erase(jobs->jobs.begin());
                static_cast<void>(asset::AssetDatabase::GuidFromPath(source));
                ModelImportJob job;
                job.id = jobs->nextId++;
                job.projectRoot = context.ctx.projectRoot;
                job.path = source;
                job.sourceGuid = asset::AssetDatabase::TryGetGuidFromPath(source);
                job.status = OpData::MakeObject();
                job.status.Set("job", OpData(job.id)); job.status.Set("path", OpData(source));
                job.status.Set("state", OpData("running"));
                context.ctx.modelImportBusy = [weak = std::weak_ptr<ModelImportJobs>(jobs)] {
                    if (const auto state = weak.lock())
                        for (const auto& active : state->jobs)
                            if (active.future.valid() && active.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return true;
                    return false;
                };
                context.ctx.waitForModelImports = [weak = std::weak_ptr<ModelImportJobs>(jobs)] {
                    if (const auto state = weak.lock())
                        for (auto& active : state->jobs) if (active.future.valid()) active.future.wait();
                };
                job.future = fbzz::TaskSystem::Submit([source, options] { return ImportModelSource(source, options); });
                OpData status = job.status;
                jobs->jobs.push_back(std::move(job));
                return OpResult::Data(std::move(status));
            }
            if (!inspect) context.ctx.requestAssetBrowserRefresh = true;
            const std::string guid = asset::AssetDatabase::TryGetGuidFromPath(source);
            OpData data = OpData::MakeObject();
            data.Set("path", OpData(source)); data.Set("savedSettings", OpData(savedSettings || !inspect));
            data.Set("options", DescribeOptions(options));
            data.Set("meshNames", Strings(scan.meshNames)); data.Set("animNames", Strings(scan.animNames));
            data.Set("detectedSourceDcc", OpData(SOURCE_DCC[static_cast<int>(scan.detectedSourceDcc)]));
            data.Set("reference", OpData(guid.empty() ? "" : "guid:" + guid));
            data.Set("outputs", guid.empty() ? OpData::MakeArray() : Outputs(context.ctx.projectRoot, guid));
            data.Set("imported", OpData(!inspect && !configure));
            return OpResult::Data(std::move(data));
        };
        registry.Register(std::move(op));
    }
    EditorOperator status;
    status.id = "asset.model.status"; status.label = "Model Import Status";
    status.category = "AssetBrowser"; status.kind = OpKind::Query;
    status.desc = "変換ジョブの完了・失敗・生成物のGUIDを返す。直近32件を保持する。";
    OpParam jobParam; jobParam.name = "job"; jobParam.type = OpParamType::Int;
    jobParam.desc = "asset.model.importが返したjob番号";
    status.params.push_back(std::move(jobParam));
    status.exec = [jobs](OpContext& context, const OpArgs& args) {
        for (auto& job : jobs->jobs) {
            if (job.id != args.GetInt("job")) continue;
            if (!job.finished && job.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                const ModelImportResult result = job.future.get();
                const bool success = result.imported && result.optionsSaved && result.cacheSaved;
                job.status.Set("state", OpData(success ? "done" : "failed"));
                job.status.Set("imported", OpData(result.imported));
                job.status.Set("optionsSaved", OpData(result.optionsSaved));
                job.status.Set("cacheSaved", OpData(result.cacheSaved));
                if (!success) job.status.Set("errorCode", OpData(result.imported ? "MODEL_METADATA_WRITE_FAILED" : "MODEL_IMPORT_FAILED"));
                if (result.imported && context.ctx.projectRoot == job.projectRoot) {
                    asset::AssetDatabase::IndexBakedLibrary(job.projectRoot + "/Library/Baked");
                    asset::AssetManager::FlushFailed();
                    asset::AssetManager::Unload<asset::ModelAsset>(job.path);
                    context.ctx.requestAssetBrowserRefresh = true;
                }
                const std::string& guid = job.sourceGuid;
                job.status.Set("outputs", guid.empty() ? OpData::MakeArray() : Outputs(job.projectRoot, guid));
                job.finished = true;
            }
            return OpResult::Data(job.status);
        }
        return OpResult::Err("MODEL_IMPORT_JOB_NOT_FOUND", "そのモデル変換ジョブはありません。");
    };
    registry.Register(std::move(status));
}

} /// @note namespace fbzz::editor
