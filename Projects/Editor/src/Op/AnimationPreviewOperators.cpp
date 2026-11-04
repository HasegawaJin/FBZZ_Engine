/// @file    AnimationPreviewOperators.cpp
/// @brief   アニメーションプレビューの対象・再生・解析・画像取得を直接APIへ公開する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include "../Panels/AnimationPreview/AnimationPreviewInternal.hpp"
#include <Editor/Op/OperatorGroups.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <algorithm>
#include <filesystem>
#include <memory>

namespace fbzz::editor {
void RegisterAnimationMaskPreviewOperators(OperatorRegistry& registry);

namespace {
using namespace animpreview;
struct PreviewJob { int id = 0; std::string project; std::string filename; OpData result; bool finished = false; bool rendered = false; renderer::ResourceHandle<renderer::RenderTargetTag> target; };
struct PreviewJobs { int nextId = 1; std::vector<PreviewJob> jobs; };
std::weak_ptr<PreviewJobs> activePreviewJobs;
std::string PreviewPath(EditorContext& ctx, const std::string& input)
{
    if (input.empty()) return {};
    std::string path = input.starts_with("guid:") ? asset::AssetDatabase::PathFromGuid(asset::AssetDatabase::GuidFromRef(input)) : input;
    auto absolute = util::FileSystem::PathFromUtf8(path);
    if (absolute.is_relative()) absolute = util::FileSystem::PathFromUtf8(ctx.projectRoot) / absolute;
    std::error_code error;
    if (!std::filesystem::is_regular_file(absolute, error)) return {};
    return util::FileSystem::PathToUtf8(absolute);
}
const asset::AnimationClip* CurrentClip(const asset::Model*& model)
{
    model = g_state.target.modelPath.empty() ? nullptr : asset::AssetManager::LoadAndGet<asset::Model>(g_state.target.modelPath);
    return ResolveClip(model, g_state.target.clipName, g_state.target.animAssetPath);
}
OpData State()
{
    OpData data = OpData::MakeObject(); const auto& target = g_state.target;
    data.Set("mode", OpData(target.mode == PreviewTarget::Mode::None ? "none" : target.mode == PreviewTarget::Mode::Clip ? "clip" : "transition"));
    data.Set("model", OpData(target.modelPath)); data.Set("clip", OpData(target.clipName)); data.Set("animation", OpData(target.animAssetPath));
    data.Set("toClip", OpData(target.toClipName)); data.Set("toSource", OpData(target.toSourcePath)); data.Set("toAnimation", OpData(target.toAnimAssetPath));
    data.Set("blendSeconds", OpData(target.blendSeconds)); data.Set("transitionStart", OpData(target.transitionStartSeconds));
    data.Set("time", OpData(g_state.time)); data.Set("duration", OpData(g_state.timelineLength)); data.Set("playing", OpData(g_state.playing));
    data.Set("speed", OpData(g_state.speed)); data.Set("loop", OpData(g_state.loop)); data.Set("selectedBone", OpData(g_state.selectedBoneNode));
    data.Set("cameraYaw", OpData(g_state.yaw)); data.Set("cameraPitch", OpData(g_state.pitch)); data.Set("cameraDistance", OpData(g_state.distance));
    return data;
}
bool SelectClip(PreviewTarget& target, const std::string& name)
{
    if (name.empty()) return true;
    const auto* model = asset::AssetManager::LoadAndGet<asset::Model>(target.modelPath);
    if (!model) return false;
    for (const auto& clip : model->clips) if (clip.name == name) { target.clipName = name; target.animAssetPath.clear(); return true; }
    return false;
}
} /// @note namespace
namespace {
void FinishJob(PreviewJob& job, OpResult result)
{
    job.result = std::move(result.data); job.result.Set("job", OpData(job.id));
    job.result.Set("state", OpData(result.ok ? "done" : "failed"));
    if (!result.ok) { job.result.Set("errorCode", OpData(result.errorCode)); job.result.Set("message", OpData(result.message)); }
    job.finished = true;
}
} /// @note namespace
void CompleteAnimationPreviewRequests(EditorContext& ctx)
{
    const auto jobs = activePreviewJobs.lock(); if (!jobs) return;
    for (auto& job : jobs->jobs) {
        if (job.finished || !job.rendered) continue;
        const auto readback = [&]() -> OpResult {
            if (ctx.projectRoot != job.project) return OpResult::Err("PROJECT_CHANGED", "要求後にプロジェクトが変わりました。");
            if (!ctx.renderer || !ctx.resources) return OpResult::Err("NO_RENDERER", "描画デバイスがありません。");
            std::vector<uint8_t> png; uint32_t width = 0, height = 0;
            if (!ctx.renderer->CaptureRenderTargetToPng(job.target, *ctx.resources, png, width, height) || png.empty()) return OpResult::Err("CAPTURE_FAILED", "画像を読み戻せません。");
            const auto directory = util::FileSystem::PathFromUtf8(job.project) / "Library/Captures";
            if (!util::FileSystem::EnsureDirectory(directory) || !util::FileSystem::WriteBinaryAtomic(directory / util::FileSystem::PathFromUtf8(job.filename), png.data(), png.size())) return OpResult::Err("CAPTURE_WRITE_FAILED", "PNGを保存できません。");
            OpData data = job.result; data.Set("path", OpData(util::FileSystem::PathToUtf8(directory / util::FileSystem::PathFromUtf8(job.filename))));
            data.Set("width", OpData(width)); data.Set("height", OpData(height)); return OpResult::Data(std::move(data));
        };
        FinishJob(job, readback());
    }
}
void ProcessAnimationPreviewRequests(EditorContext& ctx)
{
    const auto jobs = activePreviewJobs.lock(); if (!jobs) return;
    for (auto& job : jobs->jobs) {
        if (job.finished || job.rendered) continue;
        if (ctx.projectRoot != job.project) { FinishJob(job, OpResult::Err("PROJECT_CHANGED", "要求後にプロジェクトが変わりました。")); continue; }
        if (g_state.target.mode == PreviewTarget::Mode::None) { FinishJob(job, OpResult::Err("NO_PREVIEW_TARGET", "プレビュー対象がありません。")); continue; }
        g_state.lastRenderFrame = -1;
        if (!RenderPreviewFrame(ctx, 1.0f)) { FinishJob(job, OpResult::Err("PREVIEW_RENDER_FAILED", "プレビューを描画できません。")); continue; }
        job.result = State(); job.result.Set("job", OpData(job.id)); job.result.Set("state", OpData("reading"));
        job.target = g_gpu.renderTarget; job.rendered = true; break;
    }
}
void RegisterAnimationPreviewOperators(OperatorRegistry& registry)
{
    using namespace animpreview;
    RegisterAnimationMaskPreviewOperators(registry);
    const auto jobs = std::make_shared<PreviewJobs>(); activePreviewJobs = jobs;
    for (const char* action : {"inspect", "open", "control", "analyze", "capture", "status"}) {
        EditorOperator op; const std::string name = action;
        op.id = "animation.preview." + name; op.label = op.id; op.category = "Animation Preview";
        op.kind = name == "inspect" || name == "analyze" || name == "status" ? OpKind::Query : OpKind::Action;
        op.desc = "独立Animation Previewを画面なしで操作する。openはpath/clip/modelと任意のtoPath/toClipで遷移を指定する。表示設定はworkspace.settings.configureのanimation_preview。captureは次の描画フレームをLibrary/Capturesへ保存するjobを返し、statusで完了を確認する。";
        if (name == "open") {
            for (const char* field : {"path", "clip", "model", "toPath", "toClip"}) {
                OpParam value; value.name = field; value.type = OpParamType::String; value.required = std::string(field) == "path"; op.params.push_back(std::move(value));
            }
            for (const char* field : {"blendSeconds", "transitionStart"}) {
                OpParam value; value.name = field; value.type = OpParamType::Float; value.required = false;
                value.hasRange = true; value.minValue = 0; value.maxValue = 86400; op.params.push_back(std::move(value));
            }
        } else if (name == "control") {
            OpParam actionParam; actionParam.name = "action"; actionParam.type = OpParamType::String;
            actionParam.enumValues = {"play", "pause", "stop", "seek", "step", "reset_camera", "focus_bone"}; op.params.push_back(std::move(actionParam));
            for (const char* field : {"time", "step", "speed", "cameraYaw", "cameraPitch", "cameraDistance"}) {
                OpParam value; value.name = field; value.type = OpParamType::Float; value.required = false;
                value.hasRange = true; const std::string key = field;
                value.minValue = key == "step" || key == "cameraYaw" ? -86400.0f : key == "cameraPitch" ? -1.35f : key == "speed" ? 0.05f : key == "cameraDistance" ? 0.01f : 0.0f;
                value.maxValue = key == "speed" ? 4.0f : key == "cameraPitch" ? 1.35f : 86400.0f; op.params.push_back(std::move(value));
            }
            OpParam loop; loop.name = "loop"; loop.type = OpParamType::Bool; loop.required = false; op.params.push_back(std::move(loop));
            OpParam bone; bone.name = "bone"; bone.type = OpParamType::Int; bone.required = false; op.params.push_back(std::move(bone));
        } else if (name == "status") { OpParam job; job.name = "job"; job.type = OpParamType::Int; op.params.push_back(std::move(job)); }
        else if (name == "capture") { OpParam file; file.name = "name"; file.type = OpParamType::String; file.required = false; op.params.push_back(std::move(file)); }
        op.exec = [name, jobs](OpContext& context, const OpArgs& args) {
            auto& ctx = context.ctx;
            if (name == "status") {
                for (const auto& job : jobs->jobs) if (job.id == args.GetInt("job")) return OpResult::Data(job.result);
                return OpResult::Err("PREVIEW_JOB_NOT_FOUND", "そのキャプチャジョブはありません。");
            }
            if (name == "open") {
                const std::string path = PreviewPath(ctx, args.GetString("path"));
                PreviewTarget target;
                if (path.empty() || !ResolvePathTarget(path, target)) return OpResult::Err("BAD_PATH", "プレビュー可能なモデルまたは.animを指定してください。");
                if (args.Has("model")) {
                    const std::string model = PreviewPath(ctx, args.GetString("model"));
                    if (model.empty() || !LoadsAsPreviewableGeometry(model)) return OpResult::Err("BAD_MODEL", "ジオメトリを読み取れません。");
                    target.modelPath = model;
                }
                if (!SelectClip(target, args.GetString("clip"))) return OpResult::Err("UNKNOWN_CLIP", "そのクリップはモデルにありません。");
                const auto* model = target.modelPath.empty() ? nullptr : asset::AssetManager::LoadAndGet<asset::Model>(target.modelPath);
                const auto* clip = ResolveClip(model, target.clipName, target.animAssetPath);
                if (!target.animAssetPath.empty() && !clip) return OpResult::Err("BAD_CLIP", ".animを読み取れません。");
                float duration = ClipDurationSeconds(clip, 1.0f);
                if (args.Has("toPath")) {
                    PreviewTarget to; const std::string toPath = PreviewPath(ctx, args.GetString("toPath"));
                    if (toPath.empty() || !ResolvePathTarget(toPath, to) || !SelectClip(to, args.GetString("toClip"))) return OpResult::Err("BAD_CLIP", "遷移先を読み取れません。");
                    const auto* toModel = to.modelPath.empty() ? nullptr : asset::AssetManager::LoadAndGet<asset::Model>(to.modelPath);
                    if (!ResolveClip(toModel, to.clipName, to.animAssetPath)) return OpResult::Err("BAD_CLIP", "遷移先クリップを読み取れません。");
                    target.mode = PreviewTarget::Mode::Transition; target.toClipName = to.clipName;
                    target.toSourcePath = to.modelPath; target.toAnimAssetPath = to.animAssetPath;
                    target.blendSeconds = args.GetFloat("blendSeconds", 0.25f); target.transitionStartSeconds = args.GetFloat("transitionStart", duration * 0.5f);
                    duration = std::max(duration, target.transitionStartSeconds + target.blendSeconds + 0.5f);
                }
                AdoptManualTarget(target, !target.modelPath.empty()); g_state.timelineLength = duration;
                g_state.debugPoseValid = false; g_state.jointPositions.clear(); g_state.selectedBoneNode = -1;
            } else if (name != "inspect" && g_state.target.mode == PreviewTarget::Mode::None) return OpResult::Err("NO_PREVIEW_TARGET", "プレビュー対象がありません。");
            if (name == "control") {
                const std::string action = args.GetString("action");
                if (args.Has("time") && args.GetFloat("time") > g_state.timelineLength) return OpResult::Err("BAD_ARG", "timeがクリップ終端を超えています。");
                const int bone = args.GetInt("bone", g_state.selectedBoneNode);
                const asset::Model* model = nullptr; static_cast<void>(CurrentClip(model));
                if (args.Has("bone") && (bone < -1 || !model || !model->skeleton || bone >= static_cast<int>(model->skeleton->nodes.size()))) return OpResult::Err("BAD_ARG", "boneが範囲外です。");
                if (action == "focus_bone" && (bone < 0 || bone >= static_cast<int>(g_state.jointPositions.size()))) return OpResult::Err("PREVIEW_NOT_READY", "ボーン位置を取得するため先にcaptureしてください。");
                if (args.Has("time")) g_state.time = args.GetFloat("time");
                if (args.Has("speed")) g_state.speed = args.GetFloat("speed"); if (args.Has("loop")) g_state.loop = args.GetBool("loop");
                if (args.Has("cameraYaw")) g_state.yaw = args.GetFloat("cameraYaw"); if (args.Has("cameraPitch")) g_state.pitch = args.GetFloat("cameraPitch");
                if (args.Has("cameraDistance")) { g_state.distance = std::max(0.01f, args.GetFloat("cameraDistance")); g_state.needsFraming = false; }
                if (args.Has("bone")) g_state.selectedBoneNode = bone;
                if (action == "play") g_state.playing = true; else if (action == "pause") g_state.playing = false;
                else if (action == "stop") { g_state.playing = false; g_state.time = 0; }
                else if (action == "seek") g_state.playing = false;
                else if (action == "step") { g_state.playing = false; g_state.time = std::clamp(g_state.time + args.GetFloat("step", 1.0f / 30.0f), 0.0f, g_state.timelineLength); }
                else if (action == "reset_camera") { g_state.needsFraming = true; g_state.yaw = 2.55f; g_state.pitch = 0.30f; }
                else if (action == "focus_bone") { g_state.pendingFocus = g_state.jointPositions[static_cast<std::size_t>(bone)]; g_state.hasPendingFocus = true; g_state.pendingDistance = std::max(0.1f, g_state.boundsRadius * 0.5f); }
                g_state.lastRenderFrame = -1;
            } else if (name == "analyze") {
                const asset::Model* model = nullptr; const auto* clip = CurrentClip(model);
                if (!model || !model->skeleton || !clip) return OpResult::Err("NO_SKELETAL_CLIP", "スケルトンとクリップが必要です。");
                AnimationTrackTable tracks; tracks.Update(*model->skeleton, clip, 0); RootMotionAnalysis analysis;
                AnalyzeRootMotion(*model->skeleton, *clip, tracks, analysis);
                OpData data = State(); OpData metrics = OpData::MakeObject(); metrics.Set("valid", OpData(analysis.valid));
                metrics.Set("duration", OpData(analysis.duration)); metrics.Set("pathLength", OpData(analysis.pathLength)); metrics.Set("netDistance", OpData(analysis.netDistance));
                metrics.Set("averageSpeed", OpData(analysis.averageSpeed)); metrics.Set("verticalRange", OpData(analysis.verticalRange)); metrics.Set("loopPositionGap", OpData(analysis.loopPositionGap));
                metrics.Set("loopRotationGap", OpData(analysis.loopRotationGap)); metrics.Set("loopWorstBone", OpData(analysis.loopWorstBone)); metrics.Set("plantedSlide", OpData(analysis.plantedSlide));
                data.Set("analysis", std::move(metrics)); return OpResult::Data(std::move(data));
            } else if (name == "capture") {
                if (!ctx.renderer || !ctx.resources || ctx.projectRoot.empty()) return OpResult::Err("NO_RENDERER", "描画デバイスがありません。");
                const std::string filename = args.GetString("name", "AnimationPreview.png");
                if (filename.empty() || filename.size() > 128 || filename.find_first_of("/\\:*?\"<>|") != std::string::npos || util::FileSystem::PathFromUtf8(filename).extension() != ".png") return OpResult::Err("BAD_ARG", "nameはPNGファイル名です。");
                for (const auto& pending : jobs->jobs) if (!pending.finished) return OpResult::Err("PREVIEW_CAPTURE_BUSY", "前のキャプチャが実行待ちです。");
                if (jobs->jobs.size() >= 32) jobs->jobs.erase(jobs->jobs.begin());
                PreviewJob job; job.id = jobs->nextId++; job.project = ctx.projectRoot; job.filename = filename;
                job.result = OpData::MakeObject(); job.result.Set("job", OpData(job.id)); job.result.Set("state", OpData("queued"));
                OpData result = job.result; jobs->jobs.push_back(std::move(job)); return OpResult::Data(std::move(result));
            }
            return OpResult::Data(State());
        }; registry.Register(std::move(op));
    }
}
} /// @note namespace fbzz::editor
