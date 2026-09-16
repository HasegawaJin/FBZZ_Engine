/// @file    EffectOperators.cpp
/// @brief   ParticleEmitter / VFXComponent の再生制御 Operator。
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// WHY: Inspector にはエフェクトを再生・停止・リセットする操作がある一方、AI からは
/// VFX のアセット編集やパラメーター変更しかできず、シーン上の実体を同じ手順で
/// 確認できなかった。コンポーネント側の共通 API を呼ぶ薄い Operator にすることで、
/// UI・Script・AI の再生状態リセット漏れを防ぎ、実体のプレビュー操作を一つへ集約する。
/// 再生状態はランタイム操作なので Undo には載せない。
#include <Editor/Op/OperatorGroups.hpp>

#include <Editor/EditorContext.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/VFXComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>

#include <string>
#include <utility>

namespace fbzz::editor {

namespace {

scene::GameObject* ResolveEffectNode(const OpContext& context, const std::string& nodeId)
{
    if (context.ctx.activeScene == nullptr) return nullptr;
    if (nodeId.empty()) return context.ctx.GetSelectedGO();
    return context.ctx.activeScene->FindByGuid(nodeId);
}

bool IsParticleAction(const std::string& action)
{
    return action == "play" || action == "pause" || action == "restart"
        || action == "stop" || action == "clear" || action == "burst";
}

bool IsVFXAction(const std::string& action)
{
    return action == "play" || action == "pause" || action == "resume"
        || action == "restart" || action == "stop" || action == "trigger";
}

} // namespace

void RegisterEffectOperators(OperatorRegistry& registry)
{
    EditorOperator op;
    op.id        = "effects.control";
    op.label     = "Control Effect";
    op.category  = "Effects";
    op.desc      = "シーン上の ParticleEmitter または VFXComponent を制御する。"
                   "Particle は play / pause / restart / stop / clear / burst、"
                   "VFX は play / pause / resume / restart / stop / trigger に対応する。";
    op.caution   = "再生状態とランタイム粒子を変更するが、シーン編集の Undo 履歴には載らない。";
    op.kind      = OpKind::Action;

    OpParam nodeParam;
    nodeParam.name = "node";
    nodeParam.type = OpParamType::NodeId;
    nodeParam.desc = "対象の Effect Component を持つ GameObject。省略時は選択中のもの";
    nodeParam.required = false;

    OpParam actionParam;
    actionParam.name = "action";
    actionParam.type = OpParamType::String;
    actionParam.desc = "play / pause / resume / restart / stop / clear / burst / trigger";

    OpParam countParam;
    countParam.name = "count";
    countParam.type = OpParamType::Int;
    countParam.desc = "Particle の burst 数 (既定 10、1〜100000)";
    countParam.required = false;
    countParam.defaultValue = 10;

    OpParam nameParam;
    nameParam.name = "name";
    nameParam.type = OpParamType::String;
    nameParam.desc = "VFX の Trigger 名 (VFXElement::trigger と一致させる)";
    nameParam.required = false;

    op.params = { nodeParam, actionParam, countParam, nameParam };
    op.poll = [](const OpContext& context, const OpArgs& args) {
        scene::GameObject* go = ResolveEffectNode(context, args.GetString("node"));
        if (go == nullptr) return false;
        const std::string action = args.GetString("action");
        const bool hasParticle = go->GetComponent<scene::ParticleEmitter>() != nullptr;
        const bool hasVFX = go->GetComponent<scene::VFXComponent>() != nullptr;
        const bool validBurst = args.GetInt("count", 10) >= 1
            && args.GetInt("count", 10) <= 100000;
        const bool validTrigger = !args.GetString("name").empty();
        const bool particleAvailable = hasParticle && IsParticleAction(action)
            && (action != "burst" || validBurst);
        const bool vfxAvailable = hasVFX && IsVFXAction(action)
            && (action != "trigger" || validTrigger);
        return particleAvailable || vfxAvailable;
    };

    op.exec = [](OpContext& context, const OpArgs& args) -> OpResult {
        scene::GameObject* go = ResolveEffectNode(context, args.GetString("node"));
        if (go == nullptr) return OpResult::Err("NO_NODE", "対象の GameObject が見つかりません");

        const std::string action = args.GetString("action");
        bool applied = false;
        if (auto* particle = go->GetComponent<scene::ParticleEmitter>();
            particle != nullptr && IsParticleAction(action)) {
            if (action == "play")         particle->Play();
            else if (action == "pause")   particle->Pause();
            else if (action == "restart") particle->Play(true);
            else if (action == "stop")    particle->Stop();
            else if (action == "clear")   particle->ClearParticles();
            else {
                const int count = args.GetInt("count", 10);
                if (count < 1 || count > 100000)
                    return OpResult::Err("BAD_ARG", "count は 1〜100000 です");
                particle->Burst(count);
            }
            applied = true;
        }

        if (auto* vfx = go->GetComponent<scene::VFXComponent>();
            vfx != nullptr && IsVFXAction(action)) {
            if (action == "play" || action == "resume") vfx->Resume();
            else if (action == "pause")                  vfx->Pause();
            else if (action == "restart")                vfx->Restart();
            else if (action == "stop")                   vfx->Stop();
            else {
                const std::string name = args.GetString("name");
                if (name.empty()) return OpResult::Err("BAD_ARG", "trigger には name が必要です");
                vfx->Trigger(name);
            }
            applied = true;
        }

        if (applied) return OpResult::Ok();
        if (go->GetComponent<scene::ParticleEmitter>() != nullptr
            || go->GetComponent<scene::VFXComponent>() != nullptr)
            return OpResult::Err("BAD_ARG", "対象コンポーネントに対応しない action です");
        return OpResult::Err("NOT_PRESENT", "ParticleEmitter または VFXComponent がありません");
    };

    registry.Register(std::move(op));
}

} // namespace fbzz::editor
