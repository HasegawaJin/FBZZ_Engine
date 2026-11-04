/// @file    ScriptAnimationProxies.cpp
/// @brief   Animation 領域の Script 転送と関連する内部ヘルパー。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#include "ScriptProxyCommon.hpp"
#include <Engine/Scene/Script.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Input/Input.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/TargetPointComponent.hpp>
#include <Engine/Scene/Components/MotionWarpComponent.hpp>
#include <Engine/Scene/Components/RagdollComponent.hpp>
#include <Engine/Scene/Components/SequencePlayerComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/SpringBoneComponent.hpp>
#include <Engine/Scene/EntityRef.hpp>
#include <limits>
#include <type_traits>
#include <string_view>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cmath>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <Engine/Scene/IKTargetReferences.hpp>
#include <Engine/Scene/PresentationControlPoints.hpp>
namespace fbzz::scene {
using namespace script_proxy_detail;
namespace {

IKSolverComponent* SelfIK(const Script* script)
{
    return SelfComponent<IKSolverComponent>(script);
}


IKChain* FindChainByTarget(IKSolverComponent* ik, std::string_view targetName)
{
    if (!ik) return nullptr;
    for (IKChain& c : ik->chains)
        if (c.targetName == targetName) return &c;
    return nullptr;
}


MotionWarpComponent* SelfMotionWarp(const Script* script)
{
    return SelfComponent<MotionWarpComponent>(script);
}



/// @brief Player は「演出の再生窓口」であって、事前に置いておくものとは限らない。
/// @brief sequence.Play(path) の 1 行で始められるよう、無ければその場で足す。
SequencePlayerComponent* EnsureSequencePlayer(GameObject* object)
{
    if (!object || !object->IsValid()) return nullptr;
    if (auto* player = object->GetComponent<SequencePlayerComponent>()) return player;
    return &object->AddComponent<SequencePlayerComponent>();
}


SequencePlayerComponent* SelfSequencePlayer(const Script* script, bool ensure)
{
    if (!script) return nullptr;
    /// @note m_gameObject は protected で、friend なのは Proxy 型だけ。free function から
    /// @note 直接は引けないため、公開されている scene プロキシ経由で取り出す。
    GameObject* object = script->scene.Self();
    if (!object || !object->IsValid()) return nullptr;
    if (auto* player = object->GetComponent<SequencePlayerComponent>()) return player;
    return ensure ? EnsureSequencePlayer(object) : nullptr;
}


const asset::SequenceAsset* ResolveSequenceAsset(const SequencePlayerComponent& player)
{
    /// @note SequenceSystem と同じ順序で引く。エディタが編集中なら尺もそちらのもの。
    if (player.authoringSequence) return player.authoringSequence.get();
    if (player.sequencePath.empty()) return nullptr;
    const auto handle = asset::AssetManager::Load<asset::SequenceAsset>(player.sequencePath);
    return asset::AssetManager::Get<asset::SequenceAsset>(handle);
}


void BindSequenceKey(SequencePlayerComponent* player,
                     std::string_view key,
                     GameObject* target)
{
    if (!player || key.empty()) return;
    player->Bind(key, EntityRef{ target && target->IsValid() ? target->GetID()
                                                             : EntityID::INVALID });
}



SpringBoneChain* FindSpringChain(const Script* script, std::string_view rootBoneName)
{
    auto* spring = SelfComponent<SpringBoneComponent>(script);
    if (!spring) return nullptr;
    for (SpringBoneChain& chain : spring->chains)
        if (chain.rootBoneName == rootBoneName) return &chain;
    return nullptr;
}


RagdollComponent* EnsureRagdoll(const Script* script)
{
    if (!script) return nullptr;
    /// @note m_gameObject は protected で、friend なのは Proxy 型だけ。free function から
    /// @note 直接は引けないため、公開されている scene プロキシ経由で取り出す。
    GameObject* object = script->scene.Self();
    if (!object || !object->IsValid()) return nullptr;
    if (auto* ragdoll = object->GetComponent<RagdollComponent>()) return ragdoll;
    return &object->AddComponent<RagdollComponent>();
}

} /// @note namespace
void ScriptAnimatorProxy::SetFloat(std::string_view name, float v) const
{
    if (auto* a = SelfComponent<AnimatorComponent>(script)) a->SetFloat(name, v);
}


void ScriptAnimatorProxy::SetInt(std::string_view name, int v) const
{
    if (auto* a = SelfComponent<AnimatorComponent>(script)) a->SetInt(name, v);
}


void ScriptAnimatorProxy::SetBool(std::string_view name, bool v) const
{
    if (auto* a = SelfComponent<AnimatorComponent>(script)) a->SetBool(name, v);
}


void ScriptAnimatorProxy::SetTrigger(std::string_view name) const
{
    if (auto* a = SelfComponent<AnimatorComponent>(script)) a->SetTrigger(name);
}


bool ScriptAnimatorProxy::IsInState(std::string_view name) const
{
    const auto* a = SelfComponent<AnimatorComponent>(script);
    return a && a->IsInState(name);
}


void ScriptAnimatorProxy::SetFloat(GameObject* go, std::string_view name, float v) const
{
    if (auto* a = ObjectComponent<AnimatorComponent>(go)) a->SetFloat(name, v);
}


void ScriptAnimatorProxy::SetInt(GameObject* go, std::string_view name, int v) const
{
    if (auto* a = ObjectComponent<AnimatorComponent>(go)) a->SetInt(name, v);
}


void ScriptAnimatorProxy::SetBool(GameObject* go, std::string_view name, bool v) const
{
    if (auto* a = ObjectComponent<AnimatorComponent>(go)) a->SetBool(name, v);
}


void ScriptAnimatorProxy::SetTrigger(GameObject* go, std::string_view name) const
{
    if (auto* a = ObjectComponent<AnimatorComponent>(go)) a->SetTrigger(name);
}


bool ScriptAnimatorProxy::IsInState(GameObject* go, std::string_view name) const
{
    const auto* a = ObjectComponent<AnimatorComponent>(go);
    return a && a->IsInState(name);
}


float ScriptAnimatorProxy::GetFloat(std::string_view name) const
{
    const auto* a = SelfComponent<AnimatorComponent>(script);
    return a ? a->GetFloat(name) : 0.0f;
}


int ScriptAnimatorProxy::GetInt(std::string_view name) const
{
    const auto* a = SelfComponent<AnimatorComponent>(script);
    return a ? a->GetInt(name) : 0;
}


bool ScriptAnimatorProxy::GetBool(std::string_view name) const
{
    const auto* a = SelfComponent<AnimatorComponent>(script);
    return a && a->GetBool(name);
}


float ScriptAnimatorProxy::GetNormalizedTime() const
{
    const auto* a = SelfComponent<AnimatorComponent>(script);
    return a ? a->GetNormalizedTime() : 0.0f;
}


float ScriptAnimatorProxy::GetFloat(GameObject* go, std::string_view name) const
{
    const auto* a = ObjectComponent<AnimatorComponent>(go);
    return a ? a->GetFloat(name) : 0.0f;
}


int ScriptAnimatorProxy::GetInt(GameObject* go, std::string_view name) const
{
    const auto* a = ObjectComponent<AnimatorComponent>(go);
    return a ? a->GetInt(name) : 0;
}


bool ScriptAnimatorProxy::GetBool(GameObject* go, std::string_view name) const
{
    const auto* a = ObjectComponent<AnimatorComponent>(go);
    return a && a->GetBool(name);
}


float ScriptAnimatorProxy::GetNormalizedTime(GameObject* go) const
{
    const auto* a = ObjectComponent<AnimatorComponent>(go);
    return a ? a->GetNormalizedTime() : 0.0f;
}


std::vector<std::pair<std::string, float>>
ScriptAnimatorProxy::GetCurrentBlendWeights() const
{
    const auto* a = SelfComponent<AnimatorComponent>(script);
    return a ? a->currentBlendWeights
             : std::vector<std::pair<std::string, float>>{};
}


std::string ScriptAnimatorProxy::GetCurrentState() const
{
    const auto* a = SelfComponent<AnimatorComponent>(script);
    return a ? a->currentStateName : std::string{};
}


std::string ScriptAnimatorProxy::GetCurrentState(GameObject* go) const
{
    const auto* a = ObjectComponent<AnimatorComponent>(go);
    return a ? a->currentStateName : std::string{};
}


std::string ScriptAnimatorProxy::GetBlendToState() const
{
    const auto* a = SelfComponent<AnimatorComponent>(script);
    return a ? a->GetBlendToState() : std::string{};
}


std::string ScriptAnimatorProxy::GetBlendToState(GameObject* go) const
{
    const auto* a = ObjectComponent<AnimatorComponent>(go);
    return a ? a->GetBlendToState() : std::string{};
}


float ScriptAnimatorProxy::GetBlendToNormalizedTime() const
{
    const auto* a = SelfComponent<AnimatorComponent>(script);
    return a ? a->GetBlendToNormalizedTime() : 0.0f;
}


float ScriptAnimatorProxy::GetBlendToNormalizedTime(GameObject* go) const
{
    const auto* a = ObjectComponent<AnimatorComponent>(go);
    return a ? a->GetBlendToNormalizedTime() : 0.0f;
}


void ScriptAnimatorProxy::SetSpeed(float speed) const
{
    if (auto* a = SelfComponent<AnimatorComponent>(script)) a->speed = speed;
}


void ScriptAnimatorProxy::SetLocalTimeScale(float scale) const
{
    if (auto* a = SelfComponent<AnimatorComponent>(script))
        a->localTimeScale = std::clamp(scale, 0.0f, 8.0f);
}


float ScriptAnimatorProxy::GetSpeed() const
{
    const auto* a = SelfComponent<AnimatorComponent>(script);
    return a ? a->speed : 1.0f;
}


void ScriptAnimatorProxy::Play(std::string_view stateName) const
{
    if (auto* a = SelfComponent<AnimatorComponent>(script)) {
        a->currentStateName = std::string(stateName);
        a->stateTime        = 0.0f;
        a->blendToState.clear();
        a->blendWeight      = 0.0f;
    }
}


void ScriptAnimatorProxy::SetSpeed(GameObject* go, float speed) const
{
    if (auto* a = ObjectComponent<AnimatorComponent>(go)) a->speed = speed;
}


float ScriptAnimatorProxy::GetSpeed(GameObject* go) const
{
    const auto* a = ObjectComponent<AnimatorComponent>(go);
    return a ? a->speed : 1.0f;
}


void ScriptAnimatorProxy::SetPlaying(bool playing) const
{
    if (auto* a = SelfComponent<AnimatorComponent>(script)) a->playing = playing;
}


bool ScriptAnimatorProxy::IsPlaying() const
{
    const auto* a = SelfComponent<AnimatorComponent>(script);
    return a && a->playing;
}


void ScriptAnimatorProxy::Play(GameObject* go, std::string_view stateName) const
{
    if (auto* a = ObjectComponent<AnimatorComponent>(go)) {
        a->currentStateName = std::string(stateName);
        a->stateTime        = 0.0f;
        a->blendToState.clear();
        a->blendWeight      = 0.0f;
    }
}


void ScriptAnimatorProxy::SetLayerWeight(std::string_view layerName, float weight) const
{
    /// @note 名前引きと診断は AnimatorComponent 側に集約してある (このファイルの方針どおり
    /// @note «Script から自 GameObject の Animator を引くだけ» に留める)。以前ここだけ
    /// @note 探索を書き写していたため、レイヤー名を間違えたときの報告が漏れていた。
    if (auto* animator = SelfComponent<AnimatorComponent>(script))
        animator->SetLayerWeight(layerName, weight);
}


float ScriptAnimatorProxy::GetLayerWeight(std::string_view layerName) const
{
    if (const auto* animator = SelfComponent<AnimatorComponent>(script))
        for (const auto& layer : animator->layers)
            if (layer.name == layerName) return layer.weight;
    return 0.0f;
}

/// @name レイヤー制御 / Slot
/// @note 判定と状態遷移は AnimatorComponent 側のメソッドに集約済み。ここは Script から
/// @note 自 GameObject の Animator を引くだけの薄い委譲に留める。


std::string ScriptAnimatorProxy::GetLayerState(std::string_view layerName) const
{
    if (const auto* animator = SelfComponent<AnimatorComponent>(script))
        return animator->GetLayerState(layerName);
    return {};
}


bool ScriptAnimatorProxy::IsLayerInState(
    std::string_view layerName, std::string_view stateName) const
{
    if (const auto* animator = SelfComponent<AnimatorComponent>(script))
        return animator->IsLayerInState(layerName, stateName);
    return false;
}


void ScriptAnimatorProxy::PlayLayerState(
    std::string_view layerName, std::string_view stateName) const
{
    if (auto* animator = SelfComponent<AnimatorComponent>(script))
        animator->PlayLayerState(layerName, stateName);
}


void ScriptAnimatorProxy::SetLayerMask(
    std::string_view layerName, std::string_view maskPath) const
{
    auto* animator = SelfComponent<AnimatorComponent>(script);
    if (!animator) return;
    AnimationLayer* layer = animator->FindLayer(layerName);
    if (!layer) { ReportUnknownAnimationLayer("SetLayerMask", layerName, animator->layers); return; }
    layer->mask.path = std::string(maskPath);
    /// @note ロード済みキャッシュを落とし、次フレームの AnimatorSystem に読み直させる。
    layer->mask.Invalidate();
}


std::string ScriptAnimatorProxy::GetLayerMask(std::string_view layerName) const
{
    auto* animator = SelfComponent<AnimatorComponent>(script);
    if (!animator) return {};
    const AnimationLayer* layer = animator->FindLayer(layerName);
    return layer ? layer->mask.path : std::string{};
}


void ScriptAnimatorProxy::PlaySlot(std::string_view layerName,
                                   std::string_view sourcePath,
                                   std::string_view clipName,
                                   float fadeIn, float fadeOut,
                                   float speed, bool loop) const
{
    if (auto* animator = SelfComponent<AnimatorComponent>(script))
        animator->PlaySlot(layerName, sourcePath, clipName, fadeIn, fadeOut, speed, loop);
}


void ScriptAnimatorProxy::StopSlot(std::string_view layerName, float fadeOut) const
{
    if (auto* animator = SelfComponent<AnimatorComponent>(script))
        animator->StopSlot(layerName, fadeOut);
}


bool ScriptAnimatorProxy::IsSlotPlaying(std::string_view layerName) const
{
    if (const auto* animator = SelfComponent<AnimatorComponent>(script))
        return animator->IsSlotPlaying(layerName);
    return false;
}


float ScriptAnimatorProxy::GetSlotWeight(std::string_view layerName) const
{
    if (const auto* animator = SelfComponent<AnimatorComponent>(script))
        return animator->GetSlotWeight(layerName);
    return 0.0f;
}


void ScriptAnimatorProxy::SetSlotSpeed(std::string_view layerName, float speed) const
{
    auto* animator = SelfComponent<AnimatorComponent>(script);
    if (!animator) return;
    AnimationLayer* layer = animator->FindLayer(layerName);
    if (!layer) { ReportUnknownAnimationLayer("SetSlotSpeed", layerName, animator->layers); return; }
    if (layer->slot.active) layer->slot.speed = speed;
}


float ScriptAnimatorProxy::GetSlotTime(std::string_view layerName) const
{
    const auto* animator = SelfComponent<AnimatorComponent>(script);
    if (!animator) return 0.0f;
    const AnimationLayer* layer = animator->FindLayer(layerName);
    return layer && layer->slot.active ? layer->slot.time : 0.0f;
}


void ScriptAnimatorProxy::SetStateSpeed(std::string_view stateName, float speed) const
{
    auto* animator = SelfComponent<AnimatorComponent>(script);
    if (!animator) return;
    for (AnimationState& state : animator->states)
        if (state.name == stateName) { state.speed = speed; return; }
}


float ScriptAnimatorProxy::GetStateSpeed(std::string_view stateName) const
{
    if (const auto* animator = SelfComponent<AnimatorComponent>(script))
        for (const AnimationState& state : animator->states)
            if (state.name == stateName) return state.speed;
    return 1.0f;
}


void ScriptAnimatorProxy::SetMorphWeight(std::string_view morphName, float weight) const
{
    if (!script || !script->m_gameObject) return;
    GameObject* owner = script->m_gameObject;
    SkinnedMeshRenderer* renderer = owner->GetComponent<SkinnedMeshRenderer>();
    if (!renderer) {
        for (int i = 0; i < owner->GetChildCount(); ++i) {
            GameObject* child = owner->GetChild(i);
            if (child && (renderer = child->GetComponent<SkinnedMeshRenderer>())) break;
        }
    }
    if (renderer) renderer->morphWeights[std::string(morphName)] = weight;
}


float ScriptAnimatorProxy::GetMorphWeight(std::string_view morphName) const
{
    if (!script || !script->m_gameObject) return 0.0f;
    GameObject* owner = script->m_gameObject;
    SkinnedMeshRenderer* renderer = owner->GetComponent<SkinnedMeshRenderer>();
    if (!renderer) {
        for (int i = 0; i < owner->GetChildCount(); ++i) {
            GameObject* child = owner->GetChild(i);
            if (child && (renderer = child->GetComponent<SkinnedMeshRenderer>())) break;
        }
    }
    if (!renderer) return 0.0f;
    const auto it = renderer->morphWeights.find(std::string(morphName));
    return it != renderer->morphWeights.end() ? it->second : 0.0f;
}


math::Vector3 ScriptAnimatorProxy::GetRootMotionDeltaPosition() const
{
    const auto* animator = SelfComponent<AnimatorComponent>(script);
    return animator ? animator->rootMotionDeltaPosition : math::Vector3::ZERO;
}


math::Quaternion ScriptAnimatorProxy::GetRootMotionDeltaRotation() const
{
    const auto* animator = SelfComponent<AnimatorComponent>(script);
    return animator ? animator->rootMotionDeltaRotation : math::Quaternion::Identity();
}


math::Vector3 ScriptAnimatorProxy::GetRootMotionWorldDelta() const
{
    const auto* animator = SelfComponent<AnimatorComponent>(script);
    return animator ? animator->rootMotionWorldDelta : math::Vector3::ZERO;
}


math::Vector3 ScriptAnimatorProxy::GetRootMotionWorldVelocity() const
{
    const auto* animator = SelfComponent<AnimatorComponent>(script);
    return animator ? animator->rootMotionWorldVelocity : math::Vector3::ZERO;
}


float ScriptAnimatorProxy::GetRootMotionDeltaTime() const
{
    const auto* animator = SelfComponent<AnimatorComponent>(script);
    return animator ? animator->rootMotionDeltaTime : 0.0f;
}


bool ScriptAnimatorProxy::IsRootMotionAppliedByEngine() const
{
    const auto* animator = SelfComponent<AnimatorComponent>(script);
    return animator && animator->rootMotionAppliedByEngine;
}


void ScriptAnimatorProxy::SetRootMotionMode(int mode) const
{
    auto* animator = SelfComponent<AnimatorComponent>(script);
    if (!animator) return;
    if (mode < static_cast<int>(RootMotionMode::None) ||
        mode > static_cast<int>(RootMotionMode::ApplyToRigidBody)) return;
    animator->rootMotion.mode = static_cast<RootMotionMode>(mode);
}


int ScriptAnimatorProxy::GetRootMotionMode() const
{
    const auto* animator = SelfComponent<AnimatorComponent>(script);
    return animator ? static_cast<int>(animator->rootMotion.mode)
                    : static_cast<int>(RootMotionMode::None);
}


void ScriptAnimatorProxy::SetRootMotionPositionScale(float scale) const
{
    if (auto* animator = SelfComponent<AnimatorComponent>(script))
        animator->rootMotion.positionScale = scale;
}


float ScriptAnimatorProxy::GetRootMotionPositionScale() const
{
    const auto* animator = SelfComponent<AnimatorComponent>(script);
    return animator ? animator->rootMotion.positionScale : 1.0f;
}


void ScriptAnimatorProxy::SetRootMotionRotationScale(float scale) const
{
    if (auto* animator = SelfComponent<AnimatorComponent>(script))
        animator->rootMotion.rotationScale = scale;
}


float ScriptAnimatorProxy::GetRootMotionRotationScale() const
{
    const auto* animator = SelfComponent<AnimatorComponent>(script);
    return animator ? animator->rootMotion.rotationScale : 1.0f;
}


std::string ScriptAnimatorProxy::GetRootMotionNodeName() const
{
    const auto* animator = SelfComponent<AnimatorComponent>(script);
    return animator ? animator->rootMotion.nodeName : std::string{};
}


void ScriptAnimatorProxy::SetRootMotionNodeName(std::string_view nodeName) const
{
    auto* animator = SelfComponent<AnimatorComponent>(script);
    if (!animator) return;
    animator->rootMotion.nodeName = nodeName;
    /// @note 名前を指定したら解決方法も NodeName へ切り替える。空なら クリップ指定へ戻す。
    animator->rootMotion.source = nodeName.empty()
        ? RootMotionSource::ClipDefined
        : RootMotionSource::NodeName;
    /// @note トラックが変わるとサンプル位置の連続性が失われるため、キャッシュを捨てる。
    animator->rootMotionSamples.clear();
}


void ScriptIKProxy::SetChainEnabled(std::string_view targetName, bool enabled) const
{
    if (auto* c = FindChainByTarget(SelfIK(script), targetName)) c->enabled = enabled;
}


bool ScriptIKProxy::IsChainEnabled(std::string_view targetName) const
{
    const auto* c = FindChainByTarget(SelfIK(script), targetName);
    return c && c->enabled;
}


bool ScriptIKProxy::HasChain(std::string_view targetName) const
{
    return FindChainByTarget(SelfIK(script), targetName) != nullptr;
}


void ScriptIKProxy::SetAllEnabled(bool enabled) const
{
    auto* ik = SelfIK(script);
    if (!ik) return;
    for (IKChain& c : ik->chains) c.enabled = enabled;
}


void ScriptIKProxy::SetChainWeight(std::string_view targetName, float weight) const
{
    if (auto* c = FindChainByTarget(SelfIK(script), targetName))
        c->weight = weight < 0.0f ? 0.0f : (weight > 1.0f ? 1.0f : weight);
}


float ScriptIKProxy::GetChainWeight(std::string_view targetName) const
{
    auto* c = FindChainByTarget(SelfIK(script), targetName);
    return c ? c->weight : 0.0f;
}


void ScriptIKProxy::SetChainTarget(std::string_view targetName, EntityID target) const
{
    if (!script || !script->m_scene) return;
    if (auto* c = FindChainByTarget(SelfIK(script), targetName))
        (void)SetIKTargetReference(*script->m_scene, *c, target);
}


void ScriptIKProxy::SetChainTarget(std::string_view targetName, const GameObject& target) const
{
    if (script && script->m_scene && target.GetScene() == script->m_scene
        && script->m_scene->GetGameObject(target.GetID()) == &target)
        SetChainTarget(targetName, target.GetID());
}


void ScriptIKProxy::SetChainPole(std::string_view targetName, EntityID target) const
{
    if (!script || !script->m_scene) return;
    if (auto* chain = FindChainByTarget(SelfIK(script), targetName))
        (void)SetIKPoleReference(*script->m_scene, *chain, target);
}


void ScriptIKProxy::SetChainPole(std::string_view targetName, const GameObject& target) const
{
    if (script && script->m_scene && target.GetScene() == script->m_scene
        && script->m_scene->GetGameObject(target.GetID()) == &target)
        SetChainPole(targetName, target.GetID());
}


void ScriptIKProxy::SetEnabled(bool enabled) const
{
    if (auto* ik = SelfIK(script)) ik->enabled = enabled;
}


bool ScriptIKProxy::IsEnabled() const
{
    const auto* ik = SelfIK(script);
    return ik && ik->enabled;
}

/// @brief ScriptWaterProxy

void ScriptMotionWarpProxy::WarpTo(const math::Vector3& position, float duration) const
{
    auto* warp = SelfMotionWarp(script);
    if (!warp || !std::isfinite(duration) || duration <= 0.0f
        || !std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) return;
    warp->target.active       = true;
    warp->target.position     = position;
    warp->target.warpPosition = true;
    warp->target.warpRotation = false;
    warp->target.duration     = duration;
    warp->target.remaining    = duration;
    warp->runtimeTargetSource = MotionWarpTargetSource::NUMERIC;
    ++warp->runtimeWarpCount;
}


void ScriptMotionWarpProxy::WarpToPose(const math::Vector3& position,
                                       const math::Quaternion& rotation,
                                       float duration) const
{
    auto* warp = SelfMotionWarp(script);
    const float length = rotation.Length();
    if (!warp || !std::isfinite(duration) || duration <= 0.0f
        || !std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)
        || !std::isfinite(length) || length <= math::EPSILON) return;
    warp->target.active       = true;
    warp->target.position     = position;
    warp->target.rotation     = rotation.Normalized();
    warp->target.warpPosition = true;
    warp->target.warpRotation = true;
    warp->target.duration     = duration;
    warp->target.remaining    = duration;
    warp->runtimeTargetSource = MotionWarpTargetSource::NUMERIC;
    ++warp->runtimeWarpCount;
}


void ScriptMotionWarpProxy::SetAxisWeight(const math::Vector3& weight) const
{
    if (auto* warp = SelfMotionWarp(script)) warp->target.positionAxisWeight = weight;
}


bool ScriptMotionWarpProxy::SetTargetPoint(EntityID targetId, MotionWarpPointMode mode) const
{
    auto* warp = SelfMotionWarp(script);
    if (!warp || !script || !script->m_scene
        || (mode != MotionWarpPointMode::CAPTURED && mode != MotionWarpPointMode::LIVE)) return false;
    GameObject* target = targetId.IsValid() ? script->m_scene->GetGameObject(targetId) : nullptr;
    if (targetId.IsValid() && (!target || !target->GetComponent<TargetPointComponent>())) return false;
    if (target && mode == MotionWarpPointMode::LIVE
        && (target == script->m_gameObject || (script->m_gameObject && target->IsDescendantOf(*script->m_gameObject)))) return false;
    warp->targetReference = target ? ObjectReference{target->instanceId, target->GetID(), script->m_scene->GetRenderSceneGeneration()} : ObjectReference{};
    warp->targetPointMode = mode;
    return true;
}


bool ScriptMotionWarpProxy::SetTargetPoint(const GameObject& target, MotionWarpPointMode mode) const
{
    return script && script->m_scene && target.GetScene() == script->m_scene
        && script->m_scene->GetGameObject(target.GetID()) == &target && SetTargetPoint(target.GetID(), mode);
}


bool ScriptMotionWarpProxy::WarpToTarget(float duration) const
{
    auto* warp = SelfMotionWarp(script);
    return warp && script && script->m_scene && script->m_gameObject
        && BeginMotionWarpToTarget(*script->m_scene, script->m_gameObject->GetID(), *warp, duration);
}


GameObject* ScriptMotionWarpProxy::GetTargetPoint() const
{
    const auto* warp = SelfMotionWarp(script);
    auto* target = warp && script && script->m_scene && !warp->targetReference.guid.empty()
        ? script->m_scene->FindByGuid(warp->targetReference.guid) : nullptr;
    return target && target->GetComponent<TargetPointComponent>() ? target : nullptr;
}


MotionWarpPointMode ScriptMotionWarpProxy::GetTargetPointMode() const
{
    const auto* warp = SelfMotionWarp(script);
    return warp ? warp->targetPointMode : MotionWarpPointMode::CAPTURED;
}


void ScriptMotionWarpProxy::SetMaxSpeed(float metersPerSecond) const
{
    if (auto* warp = SelfMotionWarp(script)) warp->target.maxSpeed = metersPerSecond;
}


void ScriptMotionWarpProxy::Cancel() const
{
    if (auto* warp = SelfMotionWarp(script)) {
        warp->target.active    = false;
        warp->target.remaining = 0.0f;
    }
}


void ScriptMotionWarpProxy::SetEnabled(bool enabled) const
{
    if (auto* warp = SelfMotionWarp(script)) warp->enabled = enabled;
}


bool ScriptMotionWarpProxy::IsWarping() const
{
    const auto* warp = SelfMotionWarp(script);
    return warp && warp->enabled && warp->target.active;
}


float ScriptMotionWarpProxy::GetRemainingTime() const
{
    const auto* warp = SelfMotionWarp(script);
    return warp && warp->target.active ? warp->target.remaining : 0.0f;
}


float ScriptMotionWarpProxy::GetRemainingDistance() const
{
    const auto* warp = SelfMotionWarp(script);
    return warp ? warp->runtimeRemainingDistance : 0.0f;
}

/// @brief ScriptLifetimeProxy

void ScriptSequenceProxy::Play(std::string_view path) const
{
    auto* player = SelfSequencePlayer(script, true);
    if (!player) return;
    if (!path.empty()) player->sequencePath = std::string(path);
    player->Play();
}


void ScriptSequenceProxy::Play() const
{
    if (auto* player = SelfSequencePlayer(script, true)) player->Play();
}


void ScriptSequenceProxy::Stop() const
{
    if (auto* player = SelfSequencePlayer(script, false)) player->Stop();
}


void ScriptSequenceProxy::Pause() const
{
    if (auto* player = SelfSequencePlayer(script, false)) player->Pause();
}


void ScriptSequenceProxy::Resume() const
{
    if (auto* player = SelfSequencePlayer(script, false)) player->Resume();
}


void ScriptSequenceProxy::SetTime(float seconds) const
{
    if (auto* player = SelfSequencePlayer(script, false))
        player->SetTime(static_cast<double>(seconds));
}


void ScriptSequenceProxy::SetSpeed(float speed) const
{
    if (auto* player = SelfSequencePlayer(script, false)) player->speed = speed;
}


void ScriptSequenceProxy::Bind(std::string_view key, GameObject* target) const
{
    BindSequenceKey(SelfSequencePlayer(script, true), key, target);
}


void ScriptSequenceProxy::ClearBindings() const
{
    if (auto* player = SelfSequencePlayer(script, false)) player->bindings.clear();
}


bool ScriptSequenceProxy::IsPlaying() const
{
    const auto* player = SelfSequencePlayer(script, false);
    return player && player->playing;
}


float ScriptSequenceProxy::Time() const
{
    const auto* player = SelfSequencePlayer(script, false);
    return player ? static_cast<float>(player->time) : 0.0f;
}


float ScriptSequenceProxy::Duration() const
{
    const auto* player = SelfSequencePlayer(script, false);
    if (!player) return 0.0f;
    const asset::SequenceAsset* sequence = ResolveSequenceAsset(*player);
    return sequence ? static_cast<float>(sequence->GetDurationSeconds()) : 0.0f;
}


void ScriptSequenceProxy::PlayOn(GameObject& owner, std::string_view path) const
{
    auto* player = EnsureSequencePlayer(&owner);
    if (!player) return;
    if (!path.empty()) player->sequencePath = std::string(path);
    player->Play();
}


void ScriptSequenceProxy::StopOn(GameObject& owner) const
{
    if (!owner.IsValid()) return;
    if (auto* player = owner.GetComponent<SequencePlayerComponent>()) player->Stop();
}


void ScriptSequenceProxy::BindOn(GameObject& owner,
                                 std::string_view key,
                                 GameObject* target) const
{
    BindSequenceKey(EnsureSequencePlayer(&owner), key, target);
}

/// @brief ScriptObjectMaskProxy
/// @brief 触る先は ScriptGraphicsProxy と同じ「実行中の RenderSettings」。編集中は
/// @brief 登録されていないので、ExecuteAlways の Script から呼んでも何も起きない。

void ScriptSpringBoneProxy::EnsureChain(std::string_view rootBoneName, int maxDepth) const
{
    if (!script || !script->m_gameObject || rootBoneName.empty()) return;

    auto* spring = script->m_gameObject->GetComponent<SpringBoneComponent>();
    if (!spring) spring = &script->m_gameObject->AddComponent<SpringBoneComponent>();

    for (SpringBoneChain& chain : spring->chains) {
        if (chain.rootBoneName != rootBoneName) continue;
        /// @note 段数が変わったら組み直させる。nodes を空にするのが «組み直せ» の合図。
        if (chain.maxDepth != maxDepth) {
            chain.maxDepth = maxDepth;
            chain.nodes.clear();
        }
        return;
    }

    SpringBoneChain chain{};
    chain.rootBoneName = std::string(rootBoneName);
    chain.maxDepth     = maxDepth;
    /// @note 既定は «動かない»。Script が作るチェーンは、使う瞬間に weight を上げて使う。
    chain.weight       = 0.0f;
    chain.gravityPower = 0.0f;
    spring->chains.push_back(std::move(chain));
}


bool ScriptSpringBoneProxy::HasChain(std::string_view rootBoneName) const
{
    return FindSpringChain(script, rootBoneName) != nullptr;
}


void ScriptSpringBoneProxy::SetChainEnabled(std::string_view rootBoneName, bool enabled) const
{
    if (auto* chain = FindSpringChain(script, rootBoneName)) chain->enabled = enabled;
}


bool ScriptSpringBoneProxy::IsChainEnabled(std::string_view rootBoneName) const
{
    const auto* chain = FindSpringChain(script, rootBoneName);
    return chain && chain->enabled;
}


void ScriptSpringBoneProxy::SetWeight(std::string_view rootBoneName, float weight) const
{
    if (auto* chain = FindSpringChain(script, rootBoneName))
        chain->weight = math::Clamp01(weight);
}


float ScriptSpringBoneProxy::GetWeight(std::string_view rootBoneName) const
{
    const auto* chain = FindSpringChain(script, rootBoneName);
    return chain ? chain->weight : 0.0f;
}


void ScriptSpringBoneProxy::SetSpring(std::string_view rootBoneName,
                                      float stiffness, float damping) const
{
    if (auto* chain = FindSpringChain(script, rootBoneName)) {
        chain->stiffness = math::Clamp01(stiffness);
        chain->damping   = math::Clamp01(damping);
    }
}


void ScriptSpringBoneProxy::SetLimitAngle(std::string_view rootBoneName, float degrees) const
{
    if (auto* chain = FindSpringChain(script, rootBoneName))
        chain->limitAngle = math::Clamp(degrees, 0.0f, 180.0f);
}


void ScriptSpringBoneProxy::SetRadius(std::string_view rootBoneName, float radius) const
{
    if (auto* chain = FindSpringChain(script, rootBoneName))
        chain->radius = radius < 0.0f ? 0.0f : radius;
}


void ScriptSpringBoneProxy::SetForce(std::string_view rootBoneName,
                                     const math::Vector3& direction,
                                     float power) const
{
    auto* chain = FindSpringChain(script, rootBoneName);
    if (!chain) return;
    chain->gravityDirection = direction;
    chain->gravityPower     = power;
}


void ScriptSpringBoneProxy::ClearForce(std::string_view rootBoneName) const
{
    if (auto* chain = FindSpringChain(script, rootBoneName)) chain->gravityPower = 0.0f;
}


void ScriptSpringBoneProxy::ResetAll() const
{
    /// @note hasLastOwnerPosition を落とすと、次の更新で全チェーンが静止姿勢へ戻る
    /// @note (SpringBoneSystem のテレポート復帰と同じ経路)。
    if (auto* spring = SelfComponent<SpringBoneComponent>(script))
        spring->hasLastOwnerPosition = false;
}


void ScriptSpringBoneProxy::SetEnabled(bool enabled) const
{
    if (auto* spring = SelfComponent<SpringBoneComponent>(script)) spring->enabled = enabled;
}


bool ScriptSpringBoneProxy::IsEnabled() const
{
    const auto* spring = SelfComponent<SpringBoneComponent>(script);
    return spring && spring->enabled;
}

/// @brief ScriptRagdollProxy

void ScriptRagdollProxy::Begin(float holdSeconds, float maxWeight, float gravityScale) const
{
    auto* ragdoll = EnsureRagdoll(script);
    if (!ragdoll) return;
    ragdoll->beginRequested  = true;
    ragdoll->activeRequested = false;
    ragdoll->endRequested    = false;
    ragdoll->holdRemaining   = holdSeconds > 0.0f ? holdSeconds : 0.0f;
    ragdoll->activationWeight  = math::Clamp01(maxWeight);
    ragdoll->activationGravity = gravityScale < 0.0f ? 0.0f : gravityScale;
}


void ScriptRagdollProxy::SetStandingGuard(bool enabled, float maxDistance, float maxDegrees) const
{
    auto* ragdoll = EnsureRagdoll(script);
    if (!ragdoll) return;
    ragdoll->standingGuard = enabled;
    ragdoll->standingMaxDistance = std::max(maxDistance, 0.0f);
    ragdoll->standingMaxDegrees = std::clamp(maxDegrees, 0.0f, 180.0f);
}


void ScriptRagdollProxy::SetProfile(ScriptRagdollProfile profile) const
{
    if (auto* ragdoll = EnsureRagdoll(script))
        ragdoll->profile = profile == ScriptRagdollProfile::Humanoid
            ? RagdollProfileKind::Humanoid : RagdollProfileKind::Mech;
}


void ScriptRagdollProxy::SetExcludedBranches(const std::vector<std::string>& rootBoneNames) const
{
    if (auto* ragdoll = EnsureRagdoll(script)) ragdoll->excludedRootBones = rootBoneNames;
}


void ScriptRagdollProxy::SetRootAnchor(float scale, float sag, float tiltDegrees) const
{
    if (auto* ragdoll = EnsureRagdoll(script)) {
        ragdoll->rootAnchor = std::max(scale, 0.0f);
        ragdoll->rootAnchorSag = std::max(sag, 0.0f);
        ragdoll->rootAnchorTilt = std::max(tiltDegrees, 0.0f) * math::PI / 180.0f;
    }
}


void ScriptRagdollProxy::SetContacts(bool world, bool dynamic, bool self, bool whileActive) const
{
    if (auto* ragdoll = EnsureRagdoll(script)) {
        ragdoll->contactWorld = world;
        ragdoll->contactDynamic = dynamic;
        ragdoll->contactSelf = self;
        ragdoll->contactWhileActive = whileActive;
    }
}


void ScriptRagdollProxy::SetGroundPlane(bool enabled, float offset) const
{
    if (auto* ragdoll = EnsureRagdoll(script)) {
        ragdoll->groundPlane = enabled;
        ragdoll->groundOffset = offset;
    }
}


void ScriptRagdollProxy::BeginActive(float holdSeconds) const
{
    auto* ragdoll = EnsureRagdoll(script);
    if (!ragdoll) return;
    /// @note 既に立って走っているなら捕獲し直さない。目標はどのみち毎フレーム取り直すので
    /// @note 呼び直す意味が無く、Begin すると押されて沈んでいた勢いだけが消える。
    if (ragdoll->IsStanding()) {
        ragdoll->endRequested  = false;
        ragdoll->holdRemaining = holdSeconds > 0.0f ? holdSeconds : 0.0f;
        /// @note 戻りかけの適用率を始点にして、連撃でも姿勢が跳ねないようにする。
        if (ragdoll->phase == RagdollPhase::BlendOut) {
            ragdoll->phaseStartWeight = math::Clamp01(ragdoll->weight);
            ragdoll->phase      = RagdollPhase::BlendIn;
            ragdoll->phaseTimer = 0.0f;
        }
        return;
    }
    ragdoll->beginRequested  = true;
    ragdoll->activeRequested = true;
    ragdoll->endRequested    = false;
    ragdoll->holdRemaining   = holdSeconds > 0.0f ? holdSeconds : 0.0f;
    /// @note 立っている絵はクリップと一致するので、薄く乗せる理由が無い。
    ragdoll->activationWeight  = 1.0f;
    ragdoll->activationGravity = 1.0f;
}


void ScriptRagdollProxy::End() const
{
    auto* ragdoll = SelfComponent<RagdollComponent>(script);
    if (!ragdoll) return;
    ragdoll->beginRequested = false;
    ragdoll->activeRequested = false;
    ragdoll->startTriggered = true;
    if (ragdoll->phase == RagdollPhase::Idle) {
        ragdoll->pendingImpulses.clear();
        return;
    }
    ragdoll->endRequested  = true;
    ragdoll->holdRemaining = 0.0f;
}


void ScriptRagdollProxy::SetRoots(const std::vector<std::string>& rootBoneNames,
                                  int maxDepth) const
{
    auto* ragdoll = EnsureRagdoll(script);
    if (!ragdoll) return;

    /// @note 先頭を rootBoneName、残りを extraRootBones へ分ける。呼ぶ側にこの非対称を
    /// @note 見せないための口なので、ここで畳む。空なら «骨格の根から» (＝全身) に戻る。
    ragdoll->rootBoneName = rootBoneNames.empty() ? std::string{} : rootBoneNames.front();
    ragdoll->extraRootBones.clear();
    for (std::size_t i = 1; i < rootBoneNames.size(); ++i)
        ragdoll->extraRootBones.push_back(rootBoneNames[i]);
    ragdoll->maxDepth = maxDepth;
}


int ScriptRagdollProxy::GetRootCount() const
{
    const auto* ragdoll = SelfComponent<RagdollComponent>(script);
    if (!ragdoll) return 0;
    if (ragdoll->rootBoneName.empty() && ragdoll->extraRootBones.empty()) return 0;
    return 1 + static_cast<int>(ragdoll->extraRootBones.size());
}


void ScriptRagdollProxy::Push(const math::Vector3& velocity) const
{
    auto* ragdoll = SelfComponent<RagdollComponent>(script);
    if (!ragdoll) return;
    ragdoll->pendingImpulses.push_back(RagdollImpulse{ math::Vector3::ZERO, velocity, 0.0f });
}


void ScriptRagdollProxy::PushAt(const math::Vector3& origin,
                                const math::Vector3& velocity,
                                float radius) const
{
    auto* ragdoll = SelfComponent<RagdollComponent>(script);
    if (!ragdoll) return;
    ragdoll->pendingImpulses.push_back(
        RagdollImpulse{ origin, velocity, radius < 0.0f ? 0.0f : radius });
}


void ScriptRagdollProxy::PushAngular(const math::Vector3& angularVelocity) const
{
    PushAngularAt(math::Vector3::ZERO, angularVelocity, 0.0f);
}


void ScriptRagdollProxy::PushAngularAt(const math::Vector3& origin,
                                      const math::Vector3& angularVelocity, float radius) const
{
    if (auto* ragdoll = SelfComponent<RagdollComponent>(script))
        ragdoll->pendingImpulses.push_back(RagdollImpulse{
            origin, math::Vector3::ZERO, std::max(radius, 0.0f), angularVelocity});
}


bool ScriptRagdollProxy::IsActive() const
{
    const auto* ragdoll = SelfComponent<RagdollComponent>(script);
    return ragdoll && ragdoll->IsActive();
}


bool ScriptRagdollProxy::IsStanding() const
{
    const auto* ragdoll = SelfComponent<RagdollComponent>(script);
    return ragdoll && ragdoll->IsStanding();
}


float ScriptRagdollProxy::GetWeight() const
{
    const auto* ragdoll = SelfComponent<RagdollComponent>(script);
    return ragdoll ? ragdoll->weight : 0.0f;
}


float ScriptRagdollProxy::GetDeviation() const
{
    const auto* ragdoll = SelfComponent<RagdollComponent>(script);
    return ragdoll ? ragdoll->runtimeDeviation : 0.0f;
}


const char* ScriptRagdollProxy::GetStatus() const
{
    const auto* ragdoll = SelfComponent<RagdollComponent>(script);
    /// @note «コンポーネントがそもそも無い» は «止まっている» と区別が付かないと調べようがない。
    if (!ragdoll) return "NoComponent";
    switch (ragdoll->runtimeStatus) {
    case RagdollStatus::Idle:          return "Idle";
    case RagdollStatus::Disabled:      return "Disabled";
    case RagdollStatus::NotPlaying:    return "NotPlaying";
    case RagdollStatus::NoAnimator:    return "NoAnimator";
    case RagdollStatus::NoSkinnedMesh: return "NoSkinnedMesh";
    case RagdollStatus::NoParticles:   return "NoParticles";
    case RagdollStatus::NoBones:       return "NoBones";
    case RagdollStatus::Running:       return "Running";
    case RagdollStatus::InvalidPhysicsAsset: return "InvalidPhysicsAsset";
    }
    return "Unknown";
}


int ScriptRagdollProxy::GetParticleCount() const
{
    const auto* ragdoll = SelfComponent<RagdollComponent>(script);
    return ragdoll ? ragdoll->runtimeBodyCount : 0;
}


int ScriptRagdollProxy::GetSaturatedJointCount() const
{
    const auto* ragdoll = SelfComponent<RagdollComponent>(script);
    return ragdoll ? ragdoll->runtimeSaturated : 0;
}


int ScriptRagdollProxy::GetLimitedJointCount() const
{
    const auto* ragdoll = SelfComponent<RagdollComponent>(script);
    return ragdoll ? ragdoll->runtimeLimited : 0;
}


int ScriptRagdollProxy::GetContactCount() const
{
    const auto* ragdoll = SelfComponent<RagdollComponent>(script);
    return ragdoll ? ragdoll->runtimeContacts : 0;
}


int ScriptRagdollProxy::GetUnmatchedBoneCount() const
{
    const auto* ragdoll = SelfComponent<RagdollComponent>(script);
    return ragdoll ? ragdoll->runtimeUnmatched : 0;
}


void ScriptRagdollProxy::SetRoot(const char* rootBoneName, int maxDepth) const
{
    auto* ragdoll = EnsureRagdoll(script);
    if (!ragdoll) return;
    const std::string next = rootBoneName ? rootBoneName : "";
    if (ragdoll->rootBoneName == next && ragdoll->maxDepth == maxDepth) return;
    /// @note 骨の並びが変わるので剛体も組み直しになるが、ここで壊す必要はない。
    /// @note RagdollSystem が «組んだときの root / maxDepth» と突き合わせて自分で組み直す。
    ragdoll->rootBoneName = next;
    ragdoll->maxDepth     = maxDepth;
}


void ScriptRagdollProxy::SetGravity(float gravity) const
{
    if (auto* ragdoll = EnsureRagdoll(script))
        ragdoll->gravity = gravity < 0.0f ? 0.0f : gravity;
}


void ScriptRagdollProxy::SetBlend(float blendIn, float blendOut) const
{
    auto* ragdoll = EnsureRagdoll(script);
    if (!ragdoll) return;
    ragdoll->blendIn  = blendIn  < 0.0f ? 0.0f : blendIn;
    ragdoll->blendOut = blendOut < 0.0f ? 0.0f : blendOut;
}


void ScriptRagdollProxy::SetMuscle(float stiffness, float falloff, float damping) const
{
    auto* ragdoll = EnsureRagdoll(script);
    if (!ragdoll) return;
    /// @note どれもプロファイルが与えた値への «倍率»。1 でプロファイルどおり。
    ragdoll->driveScale   = stiffness < 0.0f ? 0.0f : stiffness;
    ragdoll->driveFalloff = math::Clamp01(falloff);
    ragdoll->driveDamping = damping < 0.0f ? 0.0f : damping;
}


void ScriptRagdollProxy::SetRecovery(float impactSlack, float recoverySeconds) const
{
    auto* ragdoll = EnsureRagdoll(script);
    if (!ragdoll) return;
    ragdoll->impactSlack     = math::Clamp01(impactSlack);
    ragdoll->recoverySeconds = recoverySeconds < 0.0f ? 0.0f : recoverySeconds;
}


void ScriptRagdollProxy::SetCollapse(float distance) const
{
    if (auto* ragdoll = EnsureRagdoll(script))
        ragdoll->collapseDistance = distance < 0.0f ? 0.0f : distance;
}


void ScriptRagdollProxy::SetEnabled(bool enabled) const
{
    if (auto* ragdoll = EnsureRagdoll(script)) ragdoll->enabled = enabled;
}


bool ScriptRagdollProxy::IsEnabled() const
{
    const auto* ragdoll = SelfComponent<RagdollComponent>(script);
    return ragdoll && ragdoll->enabled;
}

/// @brief ScriptJointProxy

} /// @note namespace fbzz::scene
