/// @file    FluidEditorOperators.cpp
/// @brief   未保存のFluid文書と再生・表示状態を直接APIへ公開する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include <Editor/Panels/FluidEditorPanel.hpp>
#include "../Panels/FluidEditor/FluidEditorInternal.hpp"
#include "../Panels/FluidEditor/FluidEditorExtras.hpp"
#include "SettingsDocument.hpp"
#include <Editor/EditorContext.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/FluidBakeService.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/FluidRecipeCodec.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <cmath>
#include <filesystem>
namespace fbzz::editor {
namespace {
std::string ResolveFluid(const EditorContext& ctx, std::string path, bool create = false)
{
    if (ctx.projectRoot.empty() || path.empty()) return {};
    if (path.starts_with("guid:")) path = asset::AssetDatabase::PathFromGuid(asset::AssetDatabase::GuidFromRef(path));
    if (path.empty()) return {};
    auto file = util::FileSystem::PathFromUtf8(path); const auto root = util::FileSystem::PathFromUtf8(ctx.projectRoot);
    if (file.is_relative()) file = root / file;
    std::error_code error; file = std::filesystem::weakly_canonical(file, error);
    if (error || file.extension() != ".fluid" || (!create && !std::filesystem::is_regular_file(file, error))) return {};
    if (create && (util::FileSystem::Exists(file) || util::FileSystem::Exists(file.string() + ".meta"))) return {};
    const auto assets = std::filesystem::weakly_canonical(root / "Assets", error); const auto relative = file.lexically_relative(assets);
    if (error || relative.empty() || relative.is_absolute() || *relative.begin() == "..") return {};
    return util::FileSystem::PathToUtf8(file);
}
FluidSelectionKind SelectionKind(const std::string& name)
{
    if (name == "source") return FluidSelectionKind::Source; if (name == "force") return FluidSelectionKind::Force;
    if (name == "collider") return FluidSelectionKind::Collider; if (name == "simulation") return FluidSelectionKind::Simulation;
    if (name == "look") return FluidSelectionKind::Look; if (name == "output") return FluidSelectionKind::Output;
    if (name == "bake") return FluidSelectionKind::Bake; return FluidSelectionKind::None;
}
} /// @note namespace
void FluidEditorPanel::OnRenderFrame(EditorContext& ctx)
{
    m_context = &ctx; m_resources = ctx.resources;
    auto& state = *m_state;
    if (!m_apiProject.empty() && m_apiProject != ctx.projectRoot) {
        if (!m_registeredDirtyPath.empty()) AssetDirtyRegistry::MarkClean(m_registeredDirtyPath);
        m_registeredDirtyPath.clear(); state.document.Close(); m_apiProject.clear(); m_apiPreview = false;
    }
    PollBakeJob(ctx);
    if (!state.document.IsOpen()) return;
    state.document.PollExternalChange();
    if (fluideditor::TickFluidAutoSave(ctx, state.document)) m_autoSaveNotice = 2.5f;
    SyncDirtyRegistry(ctx);
    if (!m_apiPreview && (!visible || !WasContentRendered())) return;
    SyncPreview(); state.preview.Tick(ctx.resources, ctx.imguiRenderer); AdvancePlayback();
    fluideditor::TickVolumePreview(ctx, state);
}
void FluidEditorPanel::OnRegisterOperators(OperatorRegistry& registry)
{
    for (const char* action : {"inspect", "open", "configure", "save", "save_as", "reload", "keep_local", "close", "control", "part", "bake"}) {
        EditorOperator op; const std::string name = action;
        op.id = "fluid.editor." + name; op.label = op.id; op.category = "Fluid Editor";
        op.kind = name == "inspect" ? OpKind::Query : OpKind::Action;
        op.desc = "Fluid Editorの未保存文書を直接扱う。configureはinspect.recipeと同形のTOMLキーで部分指定し、配列は置換する。全体Undoに対応。open/reload/closeで未保存変更を破棄するにはdiscard=true。controlは再生・時刻・表示・カメラ。";
        if (name == "open" || name == "save_as") { OpParam p; p.name = "path"; op.params.push_back(p); }
        if (name == "open" || name == "reload" || name == "close") { OpParam p; p.name = "discard"; p.type = OpParamType::Bool; p.required = false; op.params.push_back(p); }
        if (name == "configure" || name == "control") { OpParam p; p.name = "settings"; p.type = OpParamType::Data; op.params.push_back(p); }
        if (name == "bake") { for (const char* key : {"draft", "makeVfx"}) { OpParam p; p.name = key; p.type = OpParamType::Bool; p.required = false; op.params.push_back(p); } }
        if (name == "part") {
            OpParam p; p.name = "kind"; p.enumValues = {"source", "force", "collider", "simulation", "look", "output", "bake"}; op.params.push_back(p);
            p = {}; p.name = "action"; p.enumValues = {"select", "hide", "show", "solo", "remove", "insert_motion_key"}; op.params.push_back(p);
            p = {}; p.name = "index"; p.type = OpParamType::Int; p.required = false; op.params.push_back(p);
        }
        op.poll = [name](const OpContext& context, const OpArgs&) { return name == "inspect" || !context.ctx.playMode || context.ctx.playMode->IsInEditor(); };
        op.exec = [this, name](OpContext& context, const OpArgs& args) {
            auto& ctx = context.ctx; auto& state = *m_state; auto& document = state.document;
            if (name == "open" || name == "reload" || name == "close") {
                if (document.IsDirty() && !args.GetBool("discard")) return OpResult::Err("UNSAVED_ASSET", "先に保存するかdiscard=trueを指定してください。");
                if (name == "close") { if (document.IsOpen()) AssetDirtyRegistry::MarkClean(document.Path()); m_registeredDirtyPath.clear(); document.Close(); state.playing = false; m_apiPreview = false; }
                else {
                    const auto path = ResolveFluid(ctx, name == "open" ? args.GetString("path") : document.Path());
                    if (path.empty()) return OpResult::Err("BAD_PATH", "Assets配下の.fluidを指定してください。");
                    fluid::FluidRecipe staged; std::string error; if (!asset::LoadFluidRecipe(path, staged, &error)) return OpResult::Err("FLUID_READ_FAILED", error);
                    if (document.IsOpen()) AssetDirtyRegistry::MarkClean(document.Path()); m_registeredDirtyPath.clear();
                    OpenNow(ctx, path); if (!document.IsOpen() || document.Path() != path) return OpResult::Err("FLUID_OPEN_FAILED", state.status);
                    m_apiProject = ctx.projectRoot;
                }
            } else if (name != "inspect") {
                if (!document.IsOpen()) return OpResult::Err("NO_DOCUMENT", "fluid.editor.openで文書を開いてください。");
                if (document.InInteractiveEdit()) return OpResult::Err("INTERACTIVE_EDIT", "画面での操作の完了を待ってください。");
                if (name == "configure") {
                    const auto* patch = args.GetData("settings"); std::string text; fluid::FluidRecipe next;
                    if (!patch || !patch->IsObject() || !settingsdocument::Patch(asset::SerializeFluidRecipe(document.Recipe(), true), *patch, text)
                        || text.size() > 1048576 || !asset::LoadFluidRecipeFromToml(text, next)
                        || !settingsdocument::Preserves(asset::SerializeFluidRecipe(next, true), *patch)) return OpResult::Err("BAD_ARG", "設定の型・配列・フィールド名を確認してください。");
                    document.Edit(ctx, "Edit Fluid Recipe", [&next](fluid::FluidRecipe& recipe) { recipe = next; });
                    SyncDirtyRegistry(ctx); state.hasSent = false;
                } else if (name == "save_as") {
                    const auto path = ResolveFluid(ctx, args.GetString("path"), true);
                    if (path.empty()) return OpResult::Err("BAD_PATH", "Assets内の未使用.fluidを指定してください。");
                    const auto file = util::FileSystem::PathFromUtf8(path);
                    if (!util::FileSystem::EnsureDirectory(file.parent_path()) || !util::FileSystem::WriteTextAtomic(path, asset::SerializeFluidRecipe(document.Recipe()))) return OpResult::Err("SAVE_FAILED", "コピーを保存できません。");
                    static_cast<void>(asset::AssetDatabase::GuidFromPath(path));
                    fluideditor::FluidSaveAsResult saved; saved.action = fluideditor::FluidSaveAsAction::SavedCopy; saved.path = path;
                    FinishSaveAs(ctx, saved); m_apiProject = ctx.projectRoot;
                } else if (name == "keep_local") document.KeepLocal();
                else if (name == "save") {
                    if (document.HasExternalConflict()) return OpResult::Err("EXTERNAL_CONFLICT", "reloadまたはkeep_localで競合を解決してください。");
                    if (ResolveFluid(ctx, document.Path()).empty() || !WriteDocumentToDisk(ctx)) return OpResult::Err("SAVE_FAILED", state.status);
                } else if (name == "control") {
                    const auto* patch = args.GetData("settings"); if (!patch || !patch->IsObject()) return OpResult::Err("BAD_ARG", "settings");
                    bool enabled = m_apiPreview, playing = state.playing, loop = state.loop, checker = state.checkerBackground, overlays = state.showOverlays, compare = state.compareBaked, plane = state.showVolumeCenterPlane;
                    float time = state.playhead, zoom = state.zoom, panX = state.pan.x, panY = state.pan.y, yaw = state.volumeOrbit.yawDegrees, pitch = state.volumeOrbit.pitchDegrees;
                    int mode = static_cast<int>(state.viewMode), quality = static_cast<int>(state.preview.Quality()), center = static_cast<int>(state.volumeCenterPlane), gizmo = static_cast<int>(state.gizmoOp);
                    for (const auto& [key, value] : patch->AsObject()) {
                        if (key == "enabled" || key == "playing" || key == "loop" || key == "checkerBackground" || key == "showOverlays" || key == "compareBaked" || key == "showVolumeCenterPlane") {
                            if (!value.IsBool()) return OpResult::Err("BAD_ARG", key);
                            if (key == "enabled") enabled = value.AsBool(); else if (key == "playing") playing = value.AsBool(); else if (key == "loop") loop = value.AsBool();
                            else if (key == "checkerBackground") checker = value.AsBool(); else if (key == "showOverlays") overlays = value.AsBool(); else if (key == "compareBaked") compare = value.AsBool(); else plane = value.AsBool();
                        } else {
                            const double number = value.IsFloat() ? value.AsDouble() : value.IsInt() ? static_cast<double>(value.AsInt64()) : NAN;
                            if (!std::isfinite(number)) return OpResult::Err("BAD_ARG", key);
                            if (key == "time" && number >= 0 && number <= fluideditor::TimelineDuration(document.Recipe())) time = static_cast<float>(number);
                            else if (key == "zoom" && number >= 0.1 && number <= 20) zoom = static_cast<float>(number);
                            else if (key == "panX" && std::abs(number) <= 100000) panX = static_cast<float>(number);
                            else if (key == "panY" && std::abs(number) <= 100000) panY = static_cast<float>(number);
                            else if (key == "yaw" && std::abs(number) <= 360) yaw = static_cast<float>(number);
                            else if (key == "pitch" && std::abs(number) <= 89) pitch = static_cast<float>(number);
                            else if (std::floor(number) == number && key == "viewMode" && number >= 0 && number <= 1) mode = static_cast<int>(number);
                            else if (std::floor(number) == number && key == "quality" && number >= 0 && number <= 2) quality = static_cast<int>(number);
                            else if (std::floor(number) == number && key == "centerPlane" && number >= 0 && number <= 2) center = static_cast<int>(number);
                            else if (std::floor(number) == number && key == "gizmo" && number >= 0 && number <= 2) gizmo = static_cast<int>(number);
                            else return OpResult::Err("BAD_ARG", key);
                        }
                    }
                    m_apiPreview = enabled; state.playing = playing; state.loop = loop; state.playhead = time; state.zoom = zoom; state.pan = {panX, panY};
                    state.checkerBackground = checker; state.preview.SetCheckerBackground(checker); state.showOverlays = overlays; state.compareBaked = compare;
                    state.viewMode = static_cast<fluideditor::ViewportMode>(mode); state.viewModeChosen = true; state.preview.SetQuality(static_cast<FluidPreviewQuality>(quality));
                    state.volumeCenterPlane = static_cast<fluideditor::VolumeCenterPlane>(center); state.showVolumeCenterPlane = plane; state.gizmoOp = static_cast<fluideditor::GizmoOp>(gizmo);
                    state.volumeOrbit.yawDegrees = yaw; state.volumeOrbit.pitchDegrees = pitch; state.volumeOrbit.initialized = true; state.volumeOrbit.animActive = false;
                } else if (name == "part") {
                    FluidSelection selection{SelectionKind(args.GetString("kind")), args.GetInt("index", -1)}; const auto action = args.GetString("action");
                    if (selection.IsPart() && (selection.index < 0 || selection.index >= CountFluidParts(document.Recipe(), selection.kind))) return OpResult::Err("BAD_ARG", "部品indexが範囲外です。");
                    if (!selection.IsPart() && action != "select") return OpResult::Err("BAD_ARG", "部品を指定してください。");
                    document.selection = selection;
                    if (action == "hide" || action == "show") { document.SetHidden(selection.kind, selection.index, action == "hide"); ++state.visibilityGeneration; }
                    else if (action == "solo") { document.ToggleSolo(selection.kind, selection.index); ++state.visibilityGeneration; }
                    else if (action == "remove" && !fluideditor::RemoveSelectedPart(ctx, state)) return OpResult::Err("EDIT_FAILED", "部品を削除できません。");
                    else if (action == "insert_motion_key" && !fluideditor::InsertMotionKeyAtPlayhead(ctx, state)) return OpResult::Err("EDIT_FAILED", "キーを追加できません。");
                    SyncDirtyRegistry(ctx);
                } else if (name == "bake") {
                    if (!ctx.fluidBake || !ctx.renderer || !ctx.resources) return OpResult::Err("NO_RENDERER", "描画デバイスが必要です。");
                    if (ctx.fluidBake->IsBusy(document.Path())) return OpResult::Err("FLUID_BUSY", "ベイク中です。");
                    if (document.HasExternalConflict()) return OpResult::Err("EXTERNAL_CONFLICT", "reloadまたはkeep_localで競合を解決してください。");
                    if (!args.GetBool("draft") && document.IsDirty() && !WriteDocumentToDisk(ctx)) return OpResult::Err("SAVE_FAILED", state.status);
                    m_bakeJob = 0;
                    StartBake(ctx, args.GetBool("makeVfx"), args.GetBool("draft")); if (state.statusIsError || m_bakeJob == 0) return OpResult::Err("BAKE_FAILED", state.status);
                }
            }
            OpData data = OpData::MakeObject(); data.Set("open", OpData(document.IsOpen())); data.Set("path", OpData(document.Path()));
            data.Set("dirty", OpData(document.IsDirty())); data.Set("conflict", OpData(document.HasExternalConflict()));
            data.Set("revision", OpData(static_cast<std::int64_t>(document.Revision()))); data.Set("job", OpData(m_bakeJob));
            data.Set("recipe", document.IsOpen() ? settingsdocument::Inspect(asset::SerializeFluidRecipe(document.Recipe(), true)) : OpData{});
            OpData playback = OpData::MakeObject(); playback.Set("enabled", OpData(m_apiPreview)); playback.Set("time", OpData(state.playhead));
            playback.Set("playing", OpData(state.playing)); playback.Set("loop", OpData(state.loop)); playback.Set("solvedUntil", OpData(state.preview.SolvedUntil()));
            playback.Set("quality", OpData(static_cast<int>(state.preview.Quality()))); playback.Set("viewMode", OpData(static_cast<int>(state.viewMode)));
            playback.Set("zoom", OpData(state.zoom)); playback.Set("panX", OpData(state.pan.x)); playback.Set("panY", OpData(state.pan.y));
            playback.Set("yaw", OpData(state.volumeOrbit.yawDegrees)); playback.Set("pitch", OpData(state.volumeOrbit.pitchDegrees)); data.Set("playback", std::move(playback));
            return OpResult::Data(std::move(data));
        }; registry.Register(std::move(op));
    }
}
} /// @note namespace fbzz::editor
