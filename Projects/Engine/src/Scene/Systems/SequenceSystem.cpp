/// @file    SequenceSystem.cpp
/// @brief   .sequence の評価 — 時刻で解き、開始時にスナップショットし、停止時に戻す
/// @author  Hasegawa Jin
/// @date    2026-08-26
#include <Engine/Scene/Systems/SequenceSystem.hpp>

#include <Engine/Asset/AnimationSampling.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/AnimationPropertyBinding.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/AudioSourceComponent.hpp>
#include <Engine/Scene/Components/BoneComponent.hpp>
#include <Engine/Scene/Components/SequencePlayerComponent.hpp>
#include <Engine/Scene/Components/VFXComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Engine/Scene/ScriptEvent.hpp>
#include <Engine/Scene/Systems/AudioSystem.hpp>
#include <Engine/Scene/Systems/ScriptSystem.hpp>
#include <Engine/Scene/Systems/VFXSystem.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace fbzz::scene {
namespace {

/// 半開区間 (tPrev, t] の下端を、再生開始時に「時刻 0 のキーも入る」よう少しだけ下げる。
constexpr double kEventEpsilon = 1.0e-6;

/// @name binding

GameObject* ResolveBinding(Scene& scene,
                           GameObject& owner,
                           const SequencePlayerComponent& player,
                           const asset::SequenceTrack& track)
{
    if (track.binding.empty()) return nullptr;
    /// @note 予約キー。単体オブジェクトの演出をバインド無しで書ける。
    if (track.binding == "$self") return &owner;
    for (const auto& binding : player.bindings) {
        if (binding.key != track.binding) continue;
        return binding.target.IsValid() ? binding.target.Resolve(scene) : nullptr;
    }
    return nullptr;
}

/// @brief 解決できないキーは、そのトラックだけを黙らせて警告を 1 回出す。
/// @note 演出の一部が欠けても、全体を止めるより被害が小さい。
GameObject* ResolveTrackTarget(Scene& scene,
                               GameObject& owner,
                               const SequencePlayerComponent& player,
                               const asset::SequenceTrack& track,
                               SequenceTrackRuntime& runtime)
{
    GameObject* target = ResolveBinding(scene, owner, player, track);
    if (target && target->IsValid()) {
        runtime.target = target->GetID();
        return target;
    }
    if (!runtime.warnedUnresolved) {
        runtime.warnedUnresolved = true;
        FBZZ_LOG_WARN("SequenceSystem: binding '%s' の実体が見つかりません "
                      "(track '%s' / player '%s')",
                      track.binding.c_str(), track.name.c_str(), owner.name.c_str());
    }
    runtime.target = EntityID::INVALID;
    return nullptr;
}

/// @name Transform

math::Vector3 DivideComponents(const math::Vector3& value, const math::Vector3& divisor)
{
    const auto safe = [](float a, float b) { return std::abs(b) > 1.0e-6f ? a / b : a; };
    return { safe(value.x, divisor.x), safe(value.y, divisor.y), safe(value.z, divisor.z) };
}

math::Vector3 MultiplyComponents(const math::Vector3& a, const math::Vector3& b)
{
    return { a.x * b.x, a.y * b.y, a.z * b.z };
}

/// @brief ワールド指定の姿勢を、親を考慮したローカル値へ落とす。
/// @note worldPosition へ直接書いても TransformLateUpdate が local から作り直すため、
///       書いた値は同じフレームのうちに捨てられる。
void ApplyWorldPose(GameObject& target,
                    const math::Vector3& worldPosition,
                    const math::Quaternion& worldRotation,
                    bool hasPosition,
                    bool hasRotation)
{
    GameObject* parent = target.GetParent();
    if (!parent) {
        if (hasPosition) target.transform.position = worldPosition;
        if (hasRotation) target.transform.rotation = worldRotation;
        return;
    }
    const math::Quaternion inverseParent = parent->transform.worldRotation.Inverse();
    if (hasPosition) {
        const math::Vector3 delta = worldPosition - parent->transform.worldPosition;
        target.transform.position =
            DivideComponents(inverseParent * delta, parent->transform.worldScale);
    }
    if (hasRotation)
        target.transform.rotation = inverseParent * worldRotation;
}

void ApplyTransformTrack(const asset::SequenceTrack& track,
                         const SequenceTrackRuntime& runtime,
                         GameObject& target,
                         double t)
{
    const bool hasPosition = !track.positions.empty();
    const bool hasRotation = !track.rotations.empty();
    const bool hasScale    = !track.scales.empty();
    if (!hasPosition && !hasRotation && !hasScale) return;

    const math::Vector3 position =
        asset::SampleVectorKeys(track.positions, t, math::Vector3::ZERO, track.interp);
    const math::Quaternion rotation =
        asset::SampleQuaternionKeys(track.rotations, t, math::Quaternion::Identity(), track.interp);
    const math::Vector3 scale =
        asset::SampleVectorKeys(track.scales, t, math::Vector3::ONE, track.interp);

    switch (track.space) {
    case asset::SequenceTransformSpace::Local:
        if (hasPosition) target.transform.position = position;
        if (hasRotation) target.transform.rotation = rotation;
        if (hasScale)    target.transform.scale    = scale;
        break;
    case asset::SequenceTransformSpace::World:
        ApplyWorldPose(target, position, rotation, hasPosition, hasRotation);
        if (hasScale) target.transform.scale = scale;
        break;
    case asset::SequenceTransformSpace::RelativeToStart:
        if (hasPosition) target.transform.position = runtime.basePosition + position;
        if (hasRotation) target.transform.rotation = runtime.baseRotation * rotation;
        if (hasScale)    target.transform.scale    = MultiplyComponents(runtime.scale, scale);
        break;
    }
}

/// @name Animation

const asset::AnimationClip* FindLoadedClip(const AnimatorComponent& animator,
                                           const std::string& sourcePath,
                                           const std::string& clipName)
{
    if (sourcePath.empty()) return nullptr;
    const asset::AnimationClip* firstFromSource = nullptr;
    for (size_t i = 0; i < animator.clips.size(); ++i) {
        if (i >= animator.clipSourcePaths.size() ||
            animator.clipSourcePaths[i] != sourcePath) continue;
        if (!firstFromSource) firstFromSource = &animator.clips[i];
        if (!clipName.empty() && animator.clips[i].name == clipName)
            return &animator.clips[i];
    }
    return firstFromSource;
}

/// @brief このトラックが参照するクリップを Animator のロード対象へ加える。
/// @note LoadClips はステートから辿れるクリップしか読まない。演出専用のクリップはどの
///       ステートからも参照されないため、宣言しておかないと Slot が空振りする。
void RegisterClipSources(AnimatorComponent& animator, const asset::SequenceTrack& track)
{
    bool added = false;
    for (const auto& clip : track.animationClips) {
        if (clip.sourcePath.empty()) continue;
        const auto& sources = animator.externalClipSources;
        if (std::find(sources.begin(), sources.end(), clip.sourcePath) != sources.end())
            continue;
        animator.externalClipSources.push_back(clip.sourcePath);
        added = true;
    }
    if (added) animator.clipsLoaded = false;
}

double ClipRegionEnd(const asset::SequenceTrack& track,
                     size_t index,
                     double clipLength,
                     double sequenceDuration)
{
    const auto& clip = track.animationClips[index];
    if (clip.duration > 0.0) return clip.start + clip.duration;
    if (index + 1 < track.animationClips.size())
        return track.animationClips[index + 1].start;
    if (clipLength > 0.0) {
        const double speed = clip.speed != 0.0f ? std::abs(static_cast<double>(clip.speed)) : 1.0;
        return clip.start + (clipLength - clip.clipIn) / speed;
    }
    return sequenceDuration;
}

void ReleaseSlot(AnimatorComponent& animator,
                 const std::string& layerName,
                 SequenceTrackRuntime& runtime,
                 bool fadeOut)
{
    AnimationLayer* layer = animator.FindLayer(layerName);
    if (layer) {
        layer->slot.driven = false;
        if (fadeOut && layer->slot.active) {
            layer->slot.stopping = true;
        } else {
            layer->slot.active   = false;
            layer->slot.stopping = false;
            layer->slot.weight   = 0.0f;
        }
    }
    runtime.slotActive = false;
    runtime.activeClip = -1;
}

void ApplyAnimationTrack(const asset::SequenceTrack& track,
                         SequenceTrackRuntime& runtime,
                         GameObject& target,
                         double t,
                         double sequenceDuration)
{
    AnimatorComponent* animator = target.GetComponent<AnimatorComponent>();
    if (!animator) return;
    RegisterClipSources(*animator, track);

    AnimationLayer* layer = animator->FindLayer(track.layerName);
    if (!layer) {
        if (!runtime.warnedContract) {
            runtime.warnedContract = true;
            /// @note Base Layer は animator.layers に入らない。Slot はレイヤーごとの機構なので、
            ///       演出でクリップを差し込むレイヤーは Controller 側に用意しておく必要がある。
            FBZZ_LOG_WARN("SequenceSystem: Animator にレイヤー '%s' がありません "
                          "(track '%s')", track.layerName.c_str(), track.name.c_str());
        }
        return;
    }

    int active = -1;
    double regionStart = 0.0;
    double regionEnd   = 0.0;
    double clipLength  = 0.0;
    for (size_t i = 0; i < track.animationClips.size(); ++i) {
        const auto& clip = track.animationClips[i];
        const asset::AnimationClip* loaded =
            FindLoadedClip(*animator, clip.sourcePath, clip.clipName);
        const double length = loaded ? loaded->GetDurationSeconds() : 0.0;
        const double end = ClipRegionEnd(track, i, length, sequenceDuration);
        if (t < clip.start || t >= end) continue;
        active      = static_cast<int>(i);
        regionStart = clip.start;
        regionEnd   = end;
        clipLength  = length;
        break;
    }

    if (active < 0) {
        if (runtime.slotActive) ReleaseSlot(*animator, track.layerName, runtime, false);
        return;
    }

    const auto& clip = track.animationClips[static_cast<size_t>(active)];
    if (runtime.activeClip != active) {
        layer->slot.sourcePath      = clip.sourcePath;
        layer->slot.clipName        = clip.clipName;
        layer->slot.speed           = clip.speed;
        layer->slot.loop            = clip.loop;
        layer->slot.fadeInDuration  = clip.blendIn;
        layer->slot.fadeOutDuration = clip.blendOut;
        layer->slot.stopping        = false;
        layer->slot.active          = true;
        runtime.activeClip = active;
        runtime.slotActive = true;
    }

    /// @note 時刻とフェードは演出が持つ。Animator に進めさせない。
    layer->slot.driven = true;

    double local = (t - regionStart) * static_cast<double>(clip.speed) + clip.clipIn;
    if (clipLength > 0.0) {
        if (clip.loop) {
            local = std::fmod(local, clipLength);
            if (local < 0.0) local += clipLength;
        } else {
            local = std::clamp(local, 0.0, clipLength);
        }
    }
    layer->slot.time = static_cast<float>(local);

    float weight = 1.0f;
    if (clip.blendIn > 0.0f)
        weight = (std::min)(weight, static_cast<float>((t - regionStart) / clip.blendIn));
    if (clip.blendOut > 0.0f)
        weight = (std::min)(weight, static_cast<float>((regionEnd - t) / clip.blendOut));
    layer->slot.weight = std::clamp(weight, 0.0f, 1.0f);
}

/// @name Audio

void PushOneShot(AudioSourceComponent& source, const asset::SequenceAudioClip& clip)
{
    if (source.m_pendingOneShots.size() >= AudioSourceComponent::MAX_PENDING_ONE_SHOTS) return;
    AudioSourceComponent::OneShotRequest request;
    request.path        = clip.clipPath;
    request.volumeScale = clip.volume;
    source.m_pendingOneShots.push_back(std::move(request));
}

/// @name スナップショットと復帰

/// 復帰に要るトラック情報を runtime へ写す。キーは持ち込まない (復帰には識別だけあればよい)。
void CaptureRestoreBinding(const asset::SequenceTrack& track, SequenceTrackRuntime& runtime)
{
    runtime.type          = track.type;
    runtime.restoreOnStop = track.restoreOnStop;
    runtime.layerName     = track.layerName;

    asset::PropertyAnimationTrack binding;
    binding.targetType    = track.property.targetType;
    binding.valueType     = track.property.valueType;
    binding.componentType = track.property.componentType;
    binding.propertyName  = track.property.propertyName;
    binding.materialSlot  = track.property.materialSlot;
    binding.meshIndex     = track.property.meshIndex;
    runtime.propertyBinding = std::move(binding);
}

void SnapshotTrack(const asset::SequenceTrack& track,
                   SequenceTrackRuntime& runtime,
                   GameObject* target)
{
    CaptureRestoreBinding(track, runtime);
    runtime.hasSnapshot = false;
    if (!target) return;

    switch (track.type) {
    case asset::SequenceTrackType::Transform:
        /// @note SequenceSystem は AnimatorSystem より前に走るため、ボーンへ書いても
        ///       同じフレームのうちに上書きされる。黙って効かないより言う。
        if (target->GetComponent<BoneComponent>() && !runtime.warnedContract) {
            runtime.warnedContract = true;
            FBZZ_LOG_WARN("SequenceSystem: TransformTrack '%s' の対象 '%s' はボーンです。"
                          "AnimatorSystem が同フレームで上書きするため効きません",
                          track.name.c_str(), target->name.c_str());
        }
        runtime.position     = target->transform.position;
        runtime.rotation     = target->transform.rotation;
        runtime.scale        = target->transform.scale;
        runtime.basePosition = target->transform.position;
        runtime.baseRotation = target->transform.rotation;
        runtime.hasSnapshot  = true;
        break;
    case asset::SequenceTrackType::Activation:
        runtime.active      = target->activeSelf();
        runtime.hasSnapshot = true;
        break;
    case asset::SequenceTrackType::Property:
        if (track.property.targetType == asset::AnimTargetType::MaterialProperty) {
            runtime.hadMaterialOverride =
                CaptureMaterialProperty(*target, track.property, runtime.materialValues);
        } else {
            const AnimationPropertySample sample =
                CaptureComponentProperty(*target, track.property);
            runtime.propertyFound = sample.found;
            runtime.propertyType  = sample.type;
            runtime.propertyInt   = sample.intValue;
            runtime.propertyBool  = sample.boolValue;
            for (int i = 0; i < 4; ++i) runtime.propertyValue[i] = sample.floats[i];
        }
        runtime.hasSnapshot = true;
        break;
    default:
        break;
    }
}

/// 復帰はアセットを一切見ない。runtime に撮ってある「元の値」と「戻す先」だけで完結する。
void RestoreTrack(SequenceTrackRuntime& runtime, GameObject* target)
{
    if (!target) { runtime.slotActive = false; runtime.loopingClip = -1; return; }

    switch (runtime.type) {
    case asset::SequenceTrackType::Animation:
        /// @note 差し込んだクリップはフェードアウトさせる。切ると姿勢が 1 フレームで飛ぶ。
        if (auto* animator = target->GetComponent<AnimatorComponent>())
            ReleaseSlot(*animator, runtime.layerName, runtime, true);
        break;
    case asset::SequenceTrackType::Transform:
        if (!runtime.restoreOnStop || !runtime.hasSnapshot) break;
        target->transform.position = runtime.position;
        target->transform.rotation = runtime.rotation;
        target->transform.scale    = runtime.scale;
        break;
    case asset::SequenceTrackType::Activation:
        if (!runtime.restoreOnStop || !runtime.hasSnapshot) break;
        target->SetActive(runtime.active);
        break;
    case asset::SequenceTrackType::Property: {
        if (!runtime.restoreOnStop || !runtime.hasSnapshot) break;
        if (runtime.propertyBinding.targetType == asset::AnimTargetType::MaterialProperty) {
            RestoreMaterialProperty(*target, runtime.propertyBinding,
                                    runtime.materialValues, runtime.hadMaterialOverride);
            break;
        }
        AnimationPropertySample sample;
        sample.found     = runtime.propertyFound;
        sample.type      = runtime.propertyType;
        sample.intValue  = runtime.propertyInt;
        sample.boolValue = runtime.propertyBool;
        for (int i = 0; i < 4; ++i) sample.floats[i] = runtime.propertyValue[i];
        RestoreComponentProperty(*target, runtime.propertyBinding, sample);
        break;
    }
    case asset::SequenceTrackType::Audio:
        /// @note 鳴らしっぱなしのループだけは責任があるので止める。
        ///       一度鳴った one-shot は戻せない出来事なので何もしない。
        if (runtime.loopingClip >= 0) {
            if (auto* source = target->GetComponent<AudioSourceComponent>()) {
                source->m_pendingStop = true;
                source->m_pendingPlay = false;
            }
            runtime.loopingClip = -1;
        }
        break;
    case asset::SequenceTrackType::VFX:
        if (auto* vfx = target->GetComponent<VFXComponent>()) {
            vfx->Stop();
            vfx->editorScrubTime = -1.0f;
        }
        break;
    case asset::SequenceTrackType::Event:
        break;
    }
}

/// @name イベント配送

void DispatchSequenceEvent(GameObject* target,
                           const asset::SequenceEventKey& key,
                           double t,
                           const std::string& sequenceName)
{
    if (target) {
        /// @note 無効な GameObject・無効なスクリプトには配らない (Update と同じ規則)。
        if (!target->activeInHierarchy()) return;
        const SequenceEventInfo info{
            key.name.c_str(), key.intParam, key.floatParam,
            static_cast<float>(t), sequenceName.c_str() };
        if (ScriptComponent* scripts = target->GetComponent<ScriptComponent>())
            for (auto& entry : scripts->scripts)
                if (entry.script && entry.script->enabled)
                    entry.script->ExecuteCallback(&Script::OnSequenceEvent, info);
        return;
    }
    const SequenceEvent event{
        key.name.c_str(), key.intParam, key.floatParam,
        static_cast<float>(t), sequenceName.c_str() };
    ScriptEventBus::PublishRaw(SequenceEvent::EVENT_NAME, &event);
}

void DispatchSequenceFinished(GameObject& owner, const std::string& sequenceName)
{
    if (!owner.activeInHierarchy()) return;
    if (ScriptComponent* scripts = owner.GetComponent<ScriptComponent>())
        for (auto& entry : scripts->scripts)
            if (entry.script && entry.script->enabled)
                entry.script->ExecuteCallback(&Script::OnSequenceFinished,
                                              sequenceName.c_str(), "OnSequenceFinished");
}

/// @name 1 プレイヤー分の評価

struct PlayerContext {
    Scene&                       scene;
    GameObject&                  owner;
    SequencePlayerComponent&     player;
    const asset::SequenceAsset&  sequence;
    double                       duration = 0.0;
    std::string                  displayName;
};

GameObject* TrackTarget(PlayerContext& pc, size_t index)
{
    const asset::SequenceTrack& track = pc.sequence.tracks[index];
    SequenceTrackRuntime& runtime = pc.player.trackRuntime[index];
    if (track.binding.empty()) return nullptr;
    /// @note 世代まで見る。破棄されたスロットが再利用されると、同じ index が別の
    ///       GameObject を指したまま「有効」になり、無関係なオブジェクトを動かす。
    if (runtime.target.IsValid() && pc.scene.IsValid(runtime.target)) {
        if (GameObject* cached = pc.scene.GetGameObject(runtime.target)) return cached;
    }
    return ResolveTrackTarget(pc.scene, pc.owner, pc.player, track, runtime);
}

/// 連続トラック (t だけの関数) を解く。
void EvaluateContinuous(PlayerContext& pc, double t, bool scrubbing)
{
    for (size_t i = 0; i < pc.sequence.tracks.size(); ++i) {
        const asset::SequenceTrack& track = pc.sequence.tracks[i];
        if (track.muted) continue;
        SequenceTrackRuntime& runtime = pc.player.trackRuntime[i];
        GameObject* target = TrackTarget(pc, i);
        if (!target) continue;

        switch (track.type) {
        case asset::SequenceTrackType::Transform:
            ApplyTransformTrack(track, runtime, *target, t);
            break;
        case asset::SequenceTrackType::Animation:
            ApplyAnimationTrack(track, runtime, *target, t, pc.duration);
            break;
        case asset::SequenceTrackType::Property:
            if (track.property.targetType == asset::AnimTargetType::MaterialProperty)
                ApplyMaterialProperty(*target, track.property, t);
            else
                ApplyComponentProperty(*target, track.property, t);
            break;
        case asset::SequenceTrackType::Activation: {
            /// @note 状態なので tPrev を見ない。区間に入っていれば表示、外れていれば非表示。
            bool inside = false;
            for (const auto& range : track.ranges) {
                if (t >= range.start && t < range.end) { inside = true; break; }
            }
            if (target->activeSelf() != inside) target->SetActive(inside);
            break;
        }
        case asset::SequenceTrackType::VFX:
            /// @note スクラブ中も絵を出したいので、区間内なら決定論スクラブへ書く。
            if (!scrubbing) break;
            if (auto* vfx = target->GetComponent<VFXComponent>()) {
                bool inside = false;
                for (const auto& clip : track.vfxClips) {
                    const double end = clip.end > 0.0 ? clip.end : pc.duration;
                    if (t < clip.start || t >= end) continue;
                    vfx->editorScrubTime = static_cast<float>(t - clip.start);
                    /// @note VFXSystem は «書き込みが途絶えたスクラブ» を手放すため、
                    ///       時刻と一緒に鮮度も更新しないと 2 フレームで通常再生へ戻る。
                    vfx->editorScrubFrame = Time::frameCount;
                    inside = true;
                    break;
                }
                if (!inside) vfx->editorScrubTime = -1.0f;
            }
            break;
        case asset::SequenceTrackType::Audio:
        case asset::SequenceTrackType::Event:
            break;
        }
    }
}

/// 離散トラックを半開区間 (from, to] で発火する。
void EvaluateDiscrete(PlayerContext& pc, double from, double to)
{
    if (to <= from) return;

    for (size_t i = 0; i < pc.sequence.tracks.size(); ++i) {
        const asset::SequenceTrack& track = pc.sequence.tracks[i];
        if (track.muted) continue;
        SequenceTrackRuntime& runtime = pc.player.trackRuntime[i];

        switch (track.type) {
        case asset::SequenceTrackType::Event: {
            GameObject* target = track.binding.empty() ? nullptr : TrackTarget(pc, i);
            if (!track.binding.empty() && !target) break;
            /// @note 1 フレームで複数キーをまたいだら全部発火する。落ちたフレームで
            ///       「戻す合図」だけが消えると、操作が返ってこない形で壊れる。
            for (const auto& key : track.eventKeys) {
                if (key.time <= from || key.time > to) continue;
                DispatchSequenceEvent(target, key, key.time, pc.displayName);
            }
            break;
        }
        case asset::SequenceTrackType::Audio: {
            GameObject* target = TrackTarget(pc, i);
            auto* source = target ? target->GetComponent<AudioSourceComponent>() : nullptr;
            if (!source) break;
            for (size_t clipIndex = 0; clipIndex < track.audioClips.size(); ++clipIndex) {
                const auto& clip = track.audioClips[clipIndex];
                if (clip.start > from && clip.start <= to) {
                    if (!clip.bus.empty()) source->busName = clip.bus;
                    if (clip.loop) {
                        source->clipPath      = clip.clipPath;
                        source->volume        = clip.volume;
                        source->enabled       = true;
                        source->m_pendingPlay = true;
                        source->m_pendingStop = false;
                        runtime.loopingClip   = static_cast<int>(clipIndex);
                    } else {
                        PushOneShot(*source, clip);
                    }
                }
                if (clip.loop && clip.end > 0.0 && clip.end > from && clip.end <= to &&
                    runtime.loopingClip == static_cast<int>(clipIndex)) {
                    source->m_pendingStop = true;
                    source->m_pendingPlay = false;
                    runtime.loopingClip   = -1;
                }
            }
            break;
        }
        case asset::SequenceTrackType::VFX: {
            GameObject* target = TrackTarget(pc, i);
            auto* vfx = target ? target->GetComponent<VFXComponent>() : nullptr;
            if (!vfx) break;
            for (const auto& clip : track.vfxClips) {
                if (clip.start > from && clip.start <= to) {
                    vfx->editorScrubTime = -1.0f;
                    if (clip.restart) vfx->Restart();
                    else              vfx->Resume();
                }
                if (clip.end > 0.0 && clip.end > from && clip.end <= to) vfx->Stop();
            }
            break;
        }
        default:
            break;
        }
    }
}

void SnapshotAll(PlayerContext& pc)
{
    for (size_t i = 0; i < pc.sequence.tracks.size(); ++i) {
        SequenceTrackRuntime& runtime = pc.player.trackRuntime[i];
        runtime.target           = EntityID::INVALID;
        runtime.warnedUnresolved = false;
        runtime.warnedContract   = false;
        runtime.activeClip       = -1;
        runtime.slotActive       = false;
        runtime.loopingClip      = -1;
        SnapshotTrack(pc.sequence.tracks[i], runtime, TrackTarget(pc, i));
    }
}

/// アセットを引数に取らない。差し替わった後でも触ったものを戻せる唯一の形。
void RestoreAll(Scene& scene, SequencePlayerComponent& player)
{
    for (auto& runtime : player.trackRuntime) {
        GameObject* target = (runtime.target.IsValid() && scene.IsValid(runtime.target))
            ? scene.GetGameObject(runtime.target)
            : nullptr;
        RestoreTrack(runtime, target);
    }
}

void StopPlayback(PlayerContext& pc)
{
    RestoreAll(pc.scene, pc.player);
    pc.player.playing  = false;
    pc.player.paused   = false;
    pc.player.time     = 0.0;
    pc.player.timePrev = 0.0;
}

void BeginPlayback(PlayerContext& pc)
{
    /// @note 前の再生が残っていたら、まず元へ戻してから撮り直す。
    if (pc.player.playing) RestoreAll(pc.scene, pc.player);
    SnapshotAll(pc);
    pc.player.playing  = true;
    pc.player.paused   = false;
    pc.player.time     = 0.0;
    /// @note 時刻 0 に置いたキーを (timePrev, t] へ含める。
    pc.player.timePrev = -kEventEpsilon;
}

void UpdatePlayer(SystemContext& ctx, GameObject& owner, SequencePlayerComponent& player)
{
    /// @note 未保存の編集があればそちらが正本。無ければディスクの .sequence を引く。
    const asset::SequenceAsset* sequence = player.authoringSequence.get();
    const int assetGeneration = asset::AssetManager::GetAssetGeneration();
    if (!sequence) {
        if (player.sequencePath.empty()) {
            /// @note パスも編集中の中身も無い。触ったものが残っていれば戻す。
            if (player.appliedSequence) {
                RestoreAll(ctx.scene, player);
                player.trackRuntime.clear();
                player.appliedSequence = nullptr;
                player.playing = false;
                player.editorSnapshot = false;
            }
            return;
        }
        const auto handle = asset::AssetManager::Load<asset::SequenceAsset>(player.sequencePath);
        sequence = asset::AssetManager::Get<asset::SequenceAsset>(handle);
        if (!sequence) return;
    }

    /// @note 参照先が入れ替わったら、まず「前の演出が触ったもの」を戻してから撮り直す。復帰は
    ///       アセットを見ないので差し替え後でも正しく戻せる。世代の比較は再生中には効かせない
    ///       (アセット世代は無関係な .mat の再読込でも進み、再生中に畳むとシェーダーを 1 枚
    ///       直しただけで演出が途中で止まる。世代は「編集中の取りこぼし防止」に限る)。
    const bool sequenceChanged =
        player.appliedSequence != static_cast<const void*>(sequence) ||
        player.appliedAuthoringRevision != player.authoringRevision ||
        player.trackRuntime.size() != sequence->tracks.size() ||
        (!player.playing && player.appliedAssetGeneration != assetGeneration);
    if (sequenceChanged) {
        RestoreAll(ctx.scene, player);
        player.trackRuntime.assign(sequence->tracks.size(), SequenceTrackRuntime{});
        player.appliedSequence          = sequence;
        player.appliedAuthoringRevision = player.authoringRevision;
        player.appliedAssetGeneration   = assetGeneration;
        player.playing        = false;
        player.editorSnapshot = false;
    }

    PlayerContext pc{ ctx.scene, owner, player, *sequence,
                      sequence->GetDurationSeconds(),
                      sequence->name.empty() ? player.sequencePath : sequence->name };

    const asset::SequenceWrapMode wrapMode =
        player.overrideAssetSettings ? player.wrapMode : sequence->wrapMode;
    const asset::SequenceTimeMode timeMode =
        player.overrideAssetSettings ? player.timeMode : sequence->timeMode;

    /// @name 編集中
    if (!ctx.playing) {
        player.awakeHandled = false;
        player.pendingPlay  = false;
        player.pendingStop  = false;
        if (player.editorScrubTime >= 0.0f) {
            /// @note スクラブは「見るだけ」。撮り直しは 1 回きりにする。毎フレーム撮り直すと、
            ///       2 フレーム目のスナップショットが「演出が書き替えた後の姿勢」になり、
            ///       編集中のシーンが戻せなくなる。
            if (!player.editorSnapshot) {
                SnapshotAll(pc);
                player.editorSnapshot = true;
            }
            const double t = std::clamp(static_cast<double>(player.editorScrubTime),
                                        0.0, pc.duration);
            EvaluateContinuous(pc, t, true);
            player.time = t;
        } else if (player.editorSnapshot) {
            RestoreAll(pc.scene, pc.player);
            player.editorSnapshot = false;
            player.time = 0.0;
        }
        return;
    }

    /// @name Play セッション
    if (!player.awakeHandled) {
        player.awakeHandled = true;
        if (player.playOnAwake) player.pendingPlay = true;
    }

    if (player.pendingStop) {
        player.pendingStop = false;
        if (player.playing) StopPlayback(pc);
    }
    if (player.pendingPlay) {
        player.pendingPlay = false;
        BeginPlayback(pc);
    }
    if (player.pendingSeek >= 0.0) {
        /// @note シークは「その時刻の絵」だけを出す。跨いだ合図は発火しない。
        player.time     = std::clamp(player.pendingSeek, 0.0, pc.duration);
        player.timePrev = player.time;
        player.pendingSeek = -1.0;
    }

    if (!player.playing) return;

    if (player.paused || !ctx.simulating) {
        /// @note 止まっている間も絵は保つ。他のシステムが上書きした姿勢を毎フレーム引き戻す。
        EvaluateContinuous(pc, player.time, false);
        return;
    }

    /// @note Unscaled は「止まっている画面の上で進めたい」演出専用。
    ///       シーケンスから timeScale は書かない — 書き手を 2 つにすると、どちらが最後に
    ///       書いたかで結果が決まる。スローをかけるのは TimeManager 側の仕事。
    const float rawDt = timeMode == asset::SequenceTimeMode::Unscaled
        ? Time::unscaledDeltaTime
        : ctx.dt;
    const double delta = static_cast<double>(rawDt) * static_cast<double>(player.speed);

    const double from = player.timePrev;
    double t = player.time + delta;
    bool wrapped  = false;
    bool finished = false;

    if (pc.duration <= 0.0) {
        t = 0.0;
    } else if (wrapMode == asset::SequenceWrapMode::Loop) {
        if (t >= pc.duration) {
            wrapped = true;
            t = std::fmod(t, pc.duration);
        } else if (t < 0.0) {
            t = std::fmod(t, pc.duration) + pc.duration;
        }
    } else if (t >= pc.duration) {
        t = pc.duration;
        finished = wrapMode == asset::SequenceWrapMode::Once;
    } else if (t < 0.0) {
        t = 0.0;
    }

    player.time = t;
    EvaluateContinuous(pc, t, false);

    /// @note 逆再生・巻き戻しでは離散トラックを発火しない (合図は巻き戻せない)。
    if (wrapped) {
        EvaluateDiscrete(pc, from, pc.duration);
        EvaluateDiscrete(pc, -kEventEpsilon, t);
        player.timePrev = t;
    } else if (t > from) {
        EvaluateDiscrete(pc, from, t);
        player.timePrev = t;
    } else {
        player.timePrev = t;
    }

    if (finished) {
        StopPlayback(pc);
        DispatchSequenceFinished(owner, pc.displayName);
    }
}

} // namespace

ComponentAccess SequenceSystem::GetAccess() const
{
    /// @note 触る先はアセットが決めるため静的には宣言できない。PropertyTrack は任意の
    ///       コンポーネントへ Reflect 経由で書くため、書き込み型を並べ切れない以上、
    ///       並列バッチから外すのが正しい。
    return ComponentAccess{}.Unrestricted();
}

OrderingHints SequenceSystem::GetOrder() const
{
    return OrderingHints{}
        .After<LateScriptSystem>()
        .Before<VFXSystem>()
        .Before<AudioSystem>();
}

void SequenceSystem::Update(SystemContext& ctx)
{
    /// @note EventTrack の配送はスクリプトを呼び、その中で GameObject が増減しうる。
    ///       ComponentArray の span を握ったまま回すと、途中で無効化された領域を読む。
    const auto entities = ctx.scene.GetEntities<SequencePlayerComponent>();
    if (entities.empty()) return;
    std::vector<EntityID> owners(entities.begin(), entities.end());

    for (const EntityID id : owners) {
        GameObject* owner = ctx.scene.GetGameObject(id);
        /// @note 親ごと無効化されたプレイヤーは止める (Unity の activeInHierarchy と同じ)。回し続けると対象の表示・音・VFX を裏で切り替え続ける。
        if (!owner || !owner->activeInHierarchy()) continue;
        auto* player = ctx.scene.GetComponent<SequencePlayerComponent>(id);
        if (!player || !player->enabled) continue;
        UpdatePlayer(ctx, *owner, *player);
    }
}

} // namespace fbzz::scene
