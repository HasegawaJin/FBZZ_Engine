/// @file    SequenceOperators.cpp
/// @brief   タイムラインの編集・保存・履歴・プレビューを直接APIへ公開する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include "SettingsDocument.hpp"
#include <Editor/Panels/SequencePanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>

namespace fbzz::editor {
namespace {
bool ValidDocument(const OpData& data, const std::string& key = {})
{
    if (data.IsFloat() && !std::isfinite(data.AsDouble())) return false;
    if ((key == "time" || key == "start" || key == "end" || key == "duration"
        || key == "clipIn" || key == "blendIn" || key == "blendOut")
        && ((data.IsFloat() && data.AsDouble() < 0.0) || (data.IsInt() && data.AsInt64() < 0))) return false;
    if (data.IsArray()) {
        if (data.AsArray().size() > 65536) return false;
        for (const auto& value : data.AsArray()) if (!ValidDocument(value)) return false;
    }
    if (data.IsObject()) for (const auto& [name, value] : data.AsObject()) if (!ValidDocument(value, name)) return false;
    return true;
}
std::string ResolveSequence(EditorContext& ctx, const std::string& input)
{
    const std::string path = input.starts_with("guid:") ? asset::AssetDatabase::PathFromGuid(asset::AssetDatabase::GuidFromRef(input)) : input;
    if (path.empty() || ctx.projectRoot.empty()) return {};
    auto absolute = util::FileSystem::PathFromUtf8(path);
    if (absolute.is_relative()) absolute = util::FileSystem::PathFromUtf8(ctx.projectRoot) / absolute;
    std::error_code error;
    absolute = std::filesystem::weakly_canonical(absolute, error);
    if (error || absolute.extension() != ".sequence" || !std::filesystem::is_regular_file(absolute, error)) return {};
    const auto root = std::filesystem::weakly_canonical(util::FileSystem::PathFromUtf8(ctx.projectRoot) / "Assets", error);
    const auto relative = absolute.lexically_relative(root);
    if (error || relative.empty() || relative.is_absolute() || *relative.begin() == "..") return {};
    return util::FileSystem::PathToUtf8(absolute);
}
} /// @note namespace
void SequencePanel::OnRegisterOperators(OperatorRegistry& registry)
{
    EditorOperator curveInspect;
    curveInspect.id = "sequence.curve.inspect";
    curveInspect.label = "Inspect Sequence Curve View";
    curveInspect.category = "Sequence";
    curveInspect.kind = OpKind::Query;
    curveInspect.desc = "選択中のトラック・キーとカーブの表示範囲を返す。";
    curveInspect.exec = [this](OpContext&, const OpArgs&) {
        auto data = OpData::MakeObject();
        data.Set("track", m_selection.track);
        data.Set("key", m_selection.index);
        data.Set("curveTrack", m_curveTrack);
        data.Set("end", m_curveEnd);
        data.Set("min", m_curveMin);
        data.Set("max", m_curveMax);
        return OpResult::Data(std::move(data));
    };
    registry.Register(std::move(curveInspect));

    EditorOperator curveConfigure;
    curveConfigure.id = "sequence.curve.configure";
    curveConfigure.label = "Configure Sequence Curve View";
    curveConfigure.category = "Sequence";
    curveConfigure.kind = OpKind::Action;
    curveConfigure.desc = "Floatカーブのトラック・キーを選択し、表示範囲を設定または自動調整する。";
    curveConfigure.params = {
        {"track", OpParamType::Int, "Float property track index", true, {}},
        {"key", OpParamType::Int, "Key index or -1", false, {}},
        {"fit", OpParamType::Bool, "Fit the curve", false, {}},
        {"end", OpParamType::Float, "End of visible time range in seconds", false, {}},
        {"min", OpParamType::Float, "Lower value bound", false, {}},
        {"max", OpParamType::Float, "Upper value bound", false, {}}
    };
    curveConfigure.poll = [this](const OpContext& context, const OpArgs&) {
        return !m_path.empty() && m_projectRoot == context.ctx.projectRoot;
    };
    curveConfigure.exec = [this](OpContext&, const OpArgs& args) {
        const int index = args.GetInt("track", -1);
        if (index < 0 || static_cast<size_t>(index) >= m_asset.tracks.size())
            return OpResult::Err("BAD_ARG", "track");
        const auto& track = m_asset.tracks[static_cast<size_t>(index)];
        if (track.type != asset::SequenceTrackType::Property || track.property.valueType != asset::AnimValueType::Float)
            return OpResult::Err("BAD_ARG", "track");
        const int key = args.GetInt("key", m_selection.track == index ? m_selection.index : -1);
        if (key < -1 || (key >= 0 && static_cast<size_t>(key) >= track.property.floatKeys.size()))
            return OpResult::Err("BAD_ARG", "key");
        const bool custom = args.Has("end") || args.Has("min") || args.Has("max");
        const float end = args.GetFloat("end");
        const float low = args.GetFloat("min");
        const float high = args.GetFloat("max");
        if (custom && (!args.Has("end") || !args.Has("min") || !args.Has("max") || args.GetBool("fit")
            || !std::isfinite(end) || !std::isfinite(low) || !std::isfinite(high)
            || end <= 0 || low >= high || low < -1.0e30f || high > 1.0e30f))
            return OpResult::Err("BAD_ARG", "end/min/max");
        m_selection.track = index;
        m_selection.channel = 0;
        m_selection.index = key;
        if (custom) {
            m_curveTrack = index;
            m_curveEnd = end;
            m_curveMin = low;
            m_curveMax = high;
        } else if (m_curveTrack != index || args.GetBool("fit")) {
            FitFloatCurve(track);
        }
        return OpResult::Ok();
    };
    registry.Register(std::move(curveConfigure));

    for (const char* action : {"inspect", "open", "configure", "save", "reload", "close", "undo", "redo", "control"}) {
        EditorOperator op;
        const std::string name = action;
        op.id = "sequence." + name; op.label = op.id; op.category = "Sequence";
        op.kind = name == "inspect" ? OpKind::Query : OpKind::Action;
        op.desc = "Sequenceの作業ドキュメントを画面なしで操作する。configureはdocumentの部分更新、tracks配列は全置換。履歴はsequence.undo/redo。未保存文書の破棄はdiscard=trueが必要。";
        if (name == "open") { OpParam path; path.name = "path"; path.type = OpParamType::String; op.params.push_back(std::move(path)); }
        if (name == "open" || name == "reload" || name == "close") {
            OpParam discard; discard.name = "discard"; discard.type = OpParamType::Bool; discard.required = false;
            discard.defaultValue = false; op.params.push_back(std::move(discard));
        }
        if (name == "configure") { OpParam patch; patch.name = "document"; patch.type = OpParamType::Data; op.params.push_back(std::move(patch)); }
        if (name == "control") {
            OpParam actionParam; actionParam.name = "action"; actionParam.type = OpParamType::String;
            actionParam.enumValues = {"play", "pause", "stop", "seek", "step", "release"}; op.params.push_back(std::move(actionParam));
            for (const char* field : {"time", "speed", "step"}) {
                OpParam value; value.name = field; value.type = OpParamType::Float; value.required = false;
                value.hasRange = true; value.minValue = std::string(field) == "step" ? -3600.0f : std::string(field) == "speed" ? 0.05f : 0.0f;
                value.maxValue = std::string(field) == "speed" ? 4.0f : 86400.0f; op.params.push_back(std::move(value));
            }
            OpParam preview; preview.name = "preview"; preview.type = OpParamType::Bool; preview.required = false;
            preview.desc = "trueはSequencePlayerへ姿勢を反映。Event/Audio/VFXは発火しない。"; op.params.push_back(std::move(preview));
            OpParam target; target.name = "target"; target.type = OpParamType::NodeId; target.required = false;
            target.desc = "プレビュー先SequencePlayerのinstanceId。空なら選択/同じアセットの先頭。"; op.params.push_back(std::move(target));
        }
        op.poll = [this, name](const OpContext& context, const OpArgs&) {
            return !context.ctx.projectRoot.empty() && (name == "inspect" || !context.ctx.playMode || context.ctx.playMode->IsInEditor());
        };
        op.exec = [this, name](OpContext& context, const OpArgs& args) {
            EditorContext& ctx = context.ctx;
            m_projectRoot = ctx.projectRoot;
            if ((name == "open" || name == "reload" || name == "close") && m_dirty && !args.GetBool("discard"))
                return OpResult::Err("UNSAVED_DOCUMENT", "保存するかdiscard=trueを明示してください。");
            if (name == "open") {
                const std::string path = ResolveSequence(ctx, args.GetString("path"));
                if (path.empty()) return OpResult::Err("BAD_PATH", "Assets配下の.sequenceを指定してください。");
                asset::SequenceAsset candidate;
                if (!asset::LoadSequenceAsset(path, candidate)) return OpResult::Err("LOAD_FAILED", "Sequenceを解析できません。");
                const std::string previousPath = m_path;
                ReleasePreview(ctx);
                if (!Load(path)) return OpResult::Err("LOAD_FAILED", m_error);
                AssetDirtyRegistry::MarkClean(previousPath);
            } else if (name != "inspect" && m_path.empty()) return OpResult::Err("NO_DOCUMENT", "Sequenceを開いてください。");
            if (name == "configure") {
                std::string before, text, canonical;
                const OpData* patch = args.GetData("document");
                asset::SequenceAsset after;
                if (!patch || !ValidDocument(*patch) || !asset::SerializeSequenceAsset(m_asset, before)
                    || !settingsdocument::Patch(before, *patch, text) || !asset::LoadSequenceAssetFromToml(text, after)
                    || !asset::SerializeSequenceAsset(after, canonical) || !settingsdocument::Preserves(canonical, *patch))
                    return OpResult::Err("BAD_ARG", "Sequenceのキー・型・値・時刻順が不正です。");
                if (before != canonical) {
                    PushUndo(); m_asset = std::move(after); MarkEdited(); m_selection = {}; m_curveTrack = -1;
                    AssetDirtyRegistry::Register(m_path, m_assetPath, "Sequence", [this]() { return Save(); });
                    if (m_previewing) PushPreview(ctx);
                }
            } else if (name == "save") {
                if (!Save()) return OpResult::Err("SAVE_FAILED", m_error);
                ctx.requestAssetBrowserRefresh = true;
            } else if (name == "reload") {
                ReleasePreview(ctx);
                if (!Load(m_path)) return OpResult::Err("LOAD_FAILED", m_error);
            } else if (name == "close") { AssetDirtyRegistry::MarkClean(m_path); CloseDocument(ctx); }
            else if (name == "undo") { if (m_undoStack.empty()) return OpResult::Err("NO_HISTORY", "Undo履歴がありません。"); Undo(); }
            else if (name == "redo") { if (m_redoStack.empty()) return OpResult::Err("NO_HISTORY", "Redo履歴がありません。"); Redo(); }
            else if (name == "control") {
                const std::string action = args.GetString("action");
                scene::EntityID target = m_pinnedEntity;
                if (args.Has("target")) {
                    target = scene::EntityID::INVALID;
                    if (!args.GetString("target").empty()) {
                        auto* go = ctx.activeScene ? ctx.activeScene->FindByGuid(args.GetString("target")) : nullptr;
                        if (!go) return OpResult::Err("NO_TARGET", "プレビュー先がありません。");
                        target = go->GetID();
                    }
                }
                const bool preview = args.GetBool("preview", m_previewing);
                if (args.Has("time") && args.GetFloat("time") > Duration()) return OpResult::Err("BAD_ARG", "timeがSequenceの終端を超えています。");
                if (preview && action != "release") {
                    const auto previous = m_pinnedEntity; m_pinnedEntity = target;
                    scene::EntityID resolved;
                    const bool found = ResolvePreviewPlayer(ctx, resolved) != nullptr; m_pinnedEntity = previous;
                    if (!found) return OpResult::Err("NO_SEQUENCE_PLAYER", "プレビュー先SequencePlayerがありません。");
                }
                m_pinnedEntity = target; m_previewing = preview; m_apiPreview = true;
                if (args.Has("speed")) m_playSpeed = args.GetFloat("speed");
                if (args.Has("time")) m_playhead = args.GetFloat("time");
                if (action == "play") m_playing = true;
                else if (action == "pause") m_playing = false;
                else if (action == "stop") { m_playing = false; m_playhead = 0.0; }
                else if (action == "step") { m_playing = false; m_playhead = std::clamp(m_playhead + args.GetFloat("step", 1.0f / 60.0f), 0.0, Duration()); }
                else if (action == "release") { m_apiPreview = false; m_playing = false; m_previewing = false; ReleasePreview(ctx); }
                if (m_previewing) PushPreview(ctx); else ReleasePreview(ctx);
            }
            if (m_dirty) AssetDirtyRegistry::Register(m_path, m_assetPath, "Sequence", [this]() { return Save(); });
            std::string text;
            if (!asset::SerializeSequenceAsset(m_asset, text)) return OpResult::Err("INVALID_DOCUMENT", "Sequenceをシリアライズできません。");
            OpData data = OpData::MakeObject(); data.Set("path", OpData(m_path)); data.Set("dirty", OpData(m_dirty));
            data.Set("document", settingsdocument::Inspect(text)); data.Set("time", OpData(m_playhead));
            data.Set("duration", OpData(Duration())); data.Set("playing", OpData(m_playing)); data.Set("preview", OpData(m_previewing));
            data.Set("undoCount", OpData(static_cast<int>(m_undoStack.size()))); data.Set("redoCount", OpData(static_cast<int>(m_redoStack.size())));
            return OpResult::Data(std::move(data));
        };
        registry.Register(std::move(op));
    }
}
} /// @note namespace fbzz::editor
