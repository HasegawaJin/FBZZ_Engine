/// @file    VFXTimelineOperators.cpp
/// @brief   VFXタイムラインのトラック照会と非表示時のスクラブを提供する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include <Editor/Panels/VFXTimelinePanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Components/VFXComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <cmath>
namespace fbzz::editor {
void VFXTimelinePanel::OnUpdate(EditorContext& ctx, float dt)
{
    static_cast<void>(dt);
    if (m_apiScrubTime < 0) return;
    auto* root = ctx.projectRoot == m_apiProject && ctx.activeScene ? ctx.activeScene->FindByGuid(m_apiRoot) : nullptr;
    auto* vfx = root ? root->GetComponent<scene::VFXComponent>() : nullptr;
    if (!vfx) { m_apiScrubTime = -1; return; }
    vfx->editorScrubTime = m_apiScrubTime; vfx->editorScrubFrame = Time::frameCount;
}
void VFXTimelinePanel::OnRegisterOperators(OperatorRegistry& registry)
{
    for (const char* action : {"inspect", "control"}) {
        EditorOperator op; const std::string name = action;
        op.id = "vfx.timeline." + name; op.label = op.id; op.category = "VFX";
        op.kind = name == "inspect" ? OpKind::Query : OpKind::Action;
        op.desc = "VFXルートの全トラックと再生状態。controlはplay/pause/restart/stop/seek/release。seekはreleaseまで非表示でも時刻を固定する。トラックの編集はcomponent.setでParticleEmitter/VFXElementを指定する。";
        OpParam node; node.name = "node"; node.type = OpParamType::NodeId; node.required = false; op.params.push_back(node);
        if (name == "control") {
            OpParam command; command.name = "action"; command.enumValues = {"play", "pause", "restart", "stop", "seek", "release"}; op.params.push_back(command);
            OpParam time; time.name = "time"; time.type = OpParamType::Float; time.required = false; op.params.push_back(time);
        }
        op.exec = [this, name](OpContext& context, const OpArgs& args) {
            auto& ctx = context.ctx; if (!ctx.activeScene) return OpResult::Err("NO_SCENE", "シーンがありません。");
            const auto id = args.GetString("node"); auto* root = id.empty() ? ResolveRoot(ctx) : ctx.activeScene->FindByGuid(id);
            auto* vfx = root ? root->GetComponent<scene::VFXComponent>() : nullptr;
            if (!vfx) return OpResult::Err("NO_VFX", "VFXルートを指定してください。");
            if (name == "control") {
                const auto action = args.GetString("action"); const float time = args.GetFloat("time", 0);
                if (action == "seek" && (!std::isfinite(time) || time < 0 || time > 86400)) return OpResult::Err("BAD_ARG", "timeは有限の秒数0..86400です。");
                m_apiScrubTime = -1; m_scrubTime = -1; vfx->editorScrubTime = -1;
                if (action == "play") vfx->Resume(); else if (action == "pause") vfx->Pause();
                else if (action == "restart") vfx->Restart(); else if (action == "stop") vfx->Stop();
                else if (action == "seek") { vfx->Pause(); m_apiScrubTime = time; m_apiRoot = root->instanceId; m_apiProject = ctx.projectRoot; OnUpdate(ctx, 0); }
            }
            m_tracks.clear(); CollectTracks(*ctx.activeScene, *root, 0);
            OpData data = OpData::MakeObject(), tracks = OpData::MakeArray();
            for (const auto& track : m_tracks) {
                OpData row = OpData::MakeObject(); auto* go = ctx.activeScene->GetGameObject(track.entity);
                row.Set("node", OpData(go ? go->instanceId : "")); row.Set("name", OpData(track.label)); row.Set("depth", OpData(track.depth));
                row.Set("group", OpData(track.isGroup)); row.Set("component", OpData(track.fromEmitter ? "ParticleEmitter" : "VFXElement"));
                row.Set("startDelay", OpData(track.start)); row.Set("duration", OpData(track.duration)); row.Set("loop", OpData(track.loop)); row.Set("trigger", OpData(track.trigger)); tracks.Push(std::move(row));
            }
            data.Set("node", OpData(root->instanceId)); data.Set("tracks", std::move(tracks)); data.Set("time", OpData(vfx->time));
            data.Set("playing", OpData(vfx->playing)); data.Set("scrubTime", OpData(m_apiScrubTime)); data.Set("duration", OpData(vfx->resolvedDuration));
            return OpResult::Data(std::move(data));
        }; registry.Register(std::move(op));
    }
}
} /// @note namespace fbzz::editor
