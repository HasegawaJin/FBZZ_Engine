/// @file    AudioSystem.cpp
/// @brief   AudioSource と AudioListener を突き合わせ、空間の効きを voice へ転送する。
/// @author  Hasegawa Jin
/// @date    2025-01-01
#include "Engine/Scene/Systems/AudioSystem.hpp"
#include "Engine/Core/Scheduler/SystemContext.hpp"
#include "Engine/Scene/Systems/ScriptSystem.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/GameObject.hpp"
#include "Engine/Scene/Components/AudioListenerComponent.hpp"
#include "Engine/Scene/Components/AudioSourceComponent.hpp"
#include "Engine/Scene/Components/AudioSpatialComponents.hpp"
#include "Engine/Audio/AudioManager.hpp"
#include <Physics/Layer.hpp>
#include <Physics/World.hpp>
#include <algorithm>
#include <cmath>
#include <limits>

namespace fbzz::scene {
namespace {

/// @note 乾いた 20°C の空気での音速 (m/s)。Doppler の基準。
constexpr float kSpeedOfSound = 343.0f;

/// @note 背後の音を曇らせる量。左右のパンだけでは前後が区別できない。
/// @note 音量を変えないのは、振り向くたびに大きさが変わると距離を見誤るため。
constexpr float kBehindDamping = 0.2f;

/// @note 音源に付いたコンポーネントから決まる補正。
struct SourceEffects {
    float gain    = 1.0f;
    float lowPass = 1.0f;
    std::string_view sendBus;
    float sendLevel = 0.0f;
};

} /// @note namespace

ComponentAccess AudioSystem::GetAccess() const
{
    return ComponentAccess{}
        .Reads<Transform>()
        .Writes<AudioSourceComponent, AudioListenerComponent>();
}

OrderingHints AudioSystem::GetOrder() const
{
    return OrderingHints{}.After<LateScriptSystem>();
}

void AudioSystem::Update(SystemContext& ctx)
{
    if (!ctx.audioManager) return;
    audio::AudioManager& audioManager = *ctx.audioManager;

    /// @note priority 最大の有効な Listener を受聴点にする。同値なら Scene 登録順。
    AudioListenerComponent* listener = nullptr;
    const Transform* listenerTransform = nullptr;
    int bestPriority = (std::numeric_limits<int>::min)();
    for (EntityID id : ctx.scene.GetEntities<AudioListenerComponent>()) {
        auto* candidate = ctx.scene.GetComponent<AudioListenerComponent>(id);
        const auto* go = ctx.scene.GetGameObject(id);
        if (!candidate || !candidate->enabled || !go || !go->activeInHierarchy()
            || candidate->priority <= bestPriority) continue;
        listener = candidate;
        listenerTransform = &go->transform;
        bestPriority = candidate->priority;
    }

    /// @note Doppler は相対速度で決まるので、受聴点側の速度も要る。
    math::Vector3 listenerVelocity = math::Vector3::ZERO;
    if (listener && listenerTransform) {
        if (listener->m_hasPreviousPosition && ctx.dt > 0.0f)
            listenerVelocity =
                (listenerTransform->worldPosition - listener->m_previousPosition) / ctx.dt;
        listener->m_previousPosition    = listenerTransform->worldPosition;
        listener->m_hasPreviousPosition = true;
    }

    /// @note 残響は「聴いている部屋」の性質。重なるゾーンは最も wet が強い 1 つが勝つ。
    float zoneWet       = 0.0f;
    float zoneDecay     = 1.5f;
    float zoneHfRatio   = 1.0f;
    if (listenerTransform) {
        /// @note View は GameObject を返さないので、親ごと無効化されたゾーンを外すために実体から引く。
        for (const EntityID id : ctx.scene.GetEntities<AudioReverbZoneComponent>()) {
            const GameObject* zoneObject = ctx.scene.GetGameObject(id);
            const auto* zonePtr = ctx.scene.GetComponent<AudioReverbZoneComponent>(id);
            if (!zoneObject || !zonePtr || !zonePtr->enabled || !zoneObject->activeInHierarchy())
                continue;
            const auto& zone = *zonePtr;
            const float distance = (listenerTransform->worldPosition - zoneObject->transform.worldPosition).Length();
            if (distance > zone.outerRadius)
                continue;
            const float blend = distance <= zone.innerRadius ? 1.0f
                : 1.0f - (distance - zone.innerRadius)
                    / (std::max)(zone.outerRadius - zone.innerRadius, 0.001f);
            if (zone.wetLevel * blend > zoneWet) {
                zoneWet     = zone.wetLevel * blend;
                zoneDecay   = zone.decayTime;
                zoneHfRatio = zone.highFrequencyRatio;
            }
        }
    }
    audioManager.SetEnvironmentReverb(zoneWet, zoneDecay, zoneHfRatio);

    /// @note 減衰・パン・音色。AudioSource 経由と PlayAtPoint の両方がここを通る。
    const auto spatialize = [&](const math::Vector3& worldPosition, float spatialBlend,
                                float minDistanceIn, float maxDistanceIn, float rolloffIn,
                                float airAbsorption,
                                float& outAttenuation, float& outPan, float& outLowPass) {
        outAttenuation = 1.0f;
        outPan         = 0.0f;
        outLowPass     = 1.0f;
        if (!listener || !listenerTransform || spatialBlend <= 0.0f) return;

        const math::Vector3 offset = worldPosition - listenerTransform->worldPosition;
        const float distance = offset.Length();
        const float minDistance = (std::max)(minDistanceIn, 0.0f);
        const float maxDistance = (std::max)(maxDistanceIn, minDistance + 0.001f);
        const float normalized = std::clamp(
            (distance - minDistance) / (maxDistance - minDistance), 0.0f, 1.0f);
        const float rolloff = (std::max)(rolloffIn, 0.01f);
        const float distanceGain = std::pow(1.0f - normalized, rolloff);
        const float blend = std::clamp(spatialBlend, 0.0f, 1.0f);
        outAttenuation = (1.0f - blend) + blend * distanceGain;

        if (distance <= 0.0001f) return;
        const math::Vector3 direction = offset / distance;
        outPan = math::Vector3::Dot(direction, listenerTransform->Right()) * blend;

        /// @note 空気吸収。距離そのものは rolloff が受け持つ。
        float cutoff = 1.0f - std::clamp(airAbsorption, 0.0f, 1.0f) * normalized * blend;
        const float behind = (std::max)(
            0.0f, -math::Vector3::Dot(direction, listenerTransform->Forward()));
        cutoff -= kBehindDamping * behind * blend;
        outLowPass = (std::max)(cutoff, 0.05f);
    };

    /// @note 相対速度からピッチ倍率を出す。
    const auto dopplerRatio = [&](const math::Vector3& worldPosition,
                                  const math::Vector3& sourceVelocity, float level) -> float {
        if (level <= 0.0f || !listenerTransform) return 1.0f;
        const math::Vector3 offset = worldPosition - listenerTransform->worldPosition;
        const float distance = offset.Length();
        if (distance < 0.0001f) return 1.0f;

        /// @note direction は受聴点から音源へ向く。音源側の正は「遠ざかる」、受聴点側の正は「近づく」。
        const math::Vector3 direction = offset / distance;
        /// @note 座標差分ゆえテレポートで音速を超え、分母が 0 を跨いで暴れる。
        const float limit   = kSpeedOfSound * 0.5f;
        const float away    = std::clamp(math::Vector3::Dot(sourceVelocity, direction), -limit, limit);
        const float closing = std::clamp(math::Vector3::Dot(listenerVelocity, direction), -limit, limit);
        const float ratio   = (kSpeedOfSound + closing) / (kSpeedOfSound + away);
        return 1.0f + (std::clamp(ratio, 0.5f, 2.0f) - 1.0f) * std::clamp(level, 0.0f, 1.0f);
    };

    /// @note 遮蔽とセンド。レイキャストを含むので音源ごとに 1 フレーム 1 回だけ呼ぶ。
    const auto evaluateEffects = [&](GameObject* sourceObject,
                                     const Transform& sourceTransform) -> SourceEffects {
        SourceEffects effects;
        if (!sourceObject) return effects;

        if (const auto* send = sourceObject->GetComponent<AudioMixerSendComponent>();
            send && send->enabled) {
            effects.sendBus = send->busName;
            effects.sendLevel = send->sendLevel;
        }

        auto* occlusion = sourceObject->GetComponent<AudioOcclusionComponent>();
        if (!occlusion || !occlusion->enabled || !listenerTransform) return effects;

        occlusion->updateTimer -= ctx.dt;
        if (occlusion->updateTimer <= 0.0f) {
            const math::Vector3 offset =
                listenerTransform->worldPosition - sourceTransform.worldPosition;
            const float distance = offset.Length();
            const LayerMask mask = static_cast<LayerMask>(occlusion->obstacleLayerMask);
            physics::World::RaycastHit hit{};
            const bool blocked = distance > 0.001f
                && ctx.world.Raycast(sourceTransform.worldPosition, offset / distance,
                                     distance - 0.001f, hit,
                                     [mask](const physics::ColliderInstance& instance) {
                                         /// @note トリガーは通り抜ける形状なので音も遮らない。
                                         return !instance.isTrigger
                                             && Layer::Contains(mask, instance.layer);
                                     });
            occlusion->m_targetOcclusion = blocked ? 1.0f : 0.0f;
            occlusion->updateTimer = (std::max)(occlusion->updateInterval, 0.01f);
        }

        /// @note レイキャストは 0/1 の二値。そのまま当てると物陰を横切るたびに音が跳ぶ。
        const float transition = (std::max)(occlusion->transitionTime, 0.0f);
        const float step = transition > 0.0f ? (std::min)(ctx.dt / transition, 1.0f) : 1.0f;
        occlusion->currentOcclusion +=
            (occlusion->m_targetOcclusion - occlusion->currentOcclusion) * step;

        effects.gain *= 1.0f - occlusion->currentOcclusion
            * std::clamp(occlusion->volumeAttenuation, 0.0f, 1.0f);
        effects.lowPass = (std::min)(effects.lowPass, 1.0f - occlusion->currentOcclusion
            * std::clamp(occlusion->lowPass, 0.0f, 1.0f));
        return effects;
    };

    const float listenerVolume = listener ? (std::max)(listener->volume, 0.0f) : 1.0f;

    const auto applyVoiceParameters = [&](uint32_t voiceId,
                                          const AudioSourceComponent& source,
                                          const Transform& sourceTransform,
                                          const SourceEffects& effects,
                                          const math::Vector3& sourceVelocity,
                                          float volumeScale = 1.0f) {
        if (voiceId == 0) return;

        float attenuation = 1.0f;
        float pan         = 0.0f;
        float lowPass     = 1.0f;
        spatialize(sourceTransform.worldPosition, source.spatialBlend,
                   source.minDistance, source.maxDistance, source.rolloffFactor,
                   source.airAbsorption, attenuation, pan, lowPass);

        const float pitch = (std::max)(source.pitch, 0.01f)
            * dopplerRatio(sourceTransform.worldPosition, sourceVelocity, source.dopplerLevel);

        audioManager.SetVoiceVolume(
            voiceId, (std::max)(source.volume, 0.0f) * (std::max)(volumeScale, 0.0f)
                         * attenuation * listenerVolume * effects.gain);
        audioManager.SetVoicePitch(voiceId, pitch);
        audioManager.SetVoicePan(voiceId, pan);
        audioManager.SetVoiceSend(voiceId, effects.sendBus, effects.sendLevel);
        audioManager.SetVoiceLowPass(voiceId, (std::min)(lowPass, effects.lowPass));
    };

    for (EntityID id : ctx.scene.GetEntities<AudioSourceComponent>()) {
        auto* sourcePointer = ctx.scene.GetComponent<AudioSourceComponent>(id);
        GameObject* sourceObject = ctx.scene.GetGameObject(id);
        if (!sourcePointer || !sourceObject)
            continue;
        AudioSourceComponent& source = *sourcePointer;
        if (source.m_scriptLoopLifetime && !source.m_scriptLoopLifetime->active) {
            audioManager.StopVoice(source.m_voiceId);
            source.m_voiceId = 0;
            GameObject::Destroy(*sourceObject);
            continue;
        }
        Transform& transform = sourceObject->transform;
        if (source.m_voiceId != 0 && !audioManager.IsVoicePlaying(source.m_voiceId)) {
            source.m_voiceId = 0;
            source.m_isPlaying = false;
        }

        if (!source.enabled || !sourceObject->activeInHierarchy()) {
            audioManager.StopVoice(source.m_voiceId);
            source.m_voiceId = 0;
            source.m_isPlaying = false;
            source.m_hasPreviousPosition = false;
            continue;
        }

        /// @note 無効の間は履歴を捨てる。残すと再有効化した最初のフレームが巨大な速度になる。
        math::Vector3 sourceVelocity = math::Vector3::ZERO;
        if (source.dopplerLevel > 0.0f) {
            if (source.m_hasPreviousPosition && ctx.dt > 0.0f)
                sourceVelocity = (transform.worldPosition - source.m_previousPosition) / ctx.dt;
            source.m_previousPosition    = transform.worldPosition;
            source.m_hasPreviousPosition = true;
        } else {
            source.m_hasPreviousPosition = false;
        }

        if (source.m_pendingStop) {
            audioManager.StopVoice(source.m_voiceId);
            source.m_voiceId = 0;
            source.m_isPlaying = false;
            source.m_isPaused = false;
            source.m_pendingStop = false;
            source.m_pendingPlay = false;
            source.m_pendingPause = false;
        }

        /// @note voice は破棄せず止めるだけなので、Resume で続きから鳴る。
        if (source.m_pendingPause) {
            if (source.m_isPlaying && source.m_voiceId != 0) {
                audioManager.PauseVoice(source.m_voiceId);
                source.m_isPlaying = false;
                source.m_isPaused = true;
            }
            source.m_pendingPause = false;
        }

        if (source.m_pendingResume) {
            if (source.m_isPaused && source.m_voiceId != 0) {
                audioManager.ResumeVoice(source.m_voiceId);
                source.m_isPaused = false;
                source.m_isPlaying = true;
            }
            source.m_pendingResume = false;
        }

        const audio::BusIndex bus = audioManager.FindBus(source.busName);
        audio::AudioManager::PlayParams params;
        params.priority = source.priority;
        params.streaming = source.streaming;

        /// @note 生成クリップの要求は clipPath より優先。要求が握った参照は起動失敗時も手放す。
        const bool playOnAwake = source.playOnAwake && !source.m_played;
        if (source.m_pendingClipId != 0) {
            source.m_played = true;
            source.m_pendingPlay = false;
            source.m_isPaused = false;
            audioManager.StopVoice(source.m_voiceId);
            source.m_voiceId =
                audioManager.PlayClipVoice(source.m_pendingClipId, source.loop, bus, params);
            source.m_isPlaying = source.m_voiceId != 0;
            audioManager.ReleaseClip(source.m_pendingClipId);
            source.m_pendingClipId = 0;
        } else if ((source.m_pendingPlay || playOnAwake) && !source.clipPath.empty()) {
            source.m_played = true;
            source.m_pendingPlay = false;
            source.m_isPaused = false;
            audioManager.StopVoice(source.m_voiceId);
            source.m_voiceId = audioManager.PlayVoice(source.clipPath, source.loop, bus, params);
            source.m_isPlaying = source.m_voiceId != 0;
        }

        if (source.m_voiceId != 0 && source.m_scriptLoopLifetime)
            audioManager.BindVoiceLifetime(source.m_voiceId, source.m_scriptLoopLifetime);

        const SourceEffects effects = evaluateEffects(sourceObject, transform);

        for (const auto& request : source.m_pendingOneShots) {
            const uint32_t oneShot = request.clipId != 0
                ? audioManager.PlayClipVoice(request.clipId, false, bus, params)
                : audioManager.PlayVoice(request.path, false, bus, params);
            applyVoiceParameters(oneShot, source, transform, effects, sourceVelocity,
                                 request.volumeScale);
            /// @note 要求が握っていた参照を返す。再生中は voice 側が実体を押さえる。
            if (request.clipId != 0) audioManager.ReleaseClip(request.clipId);
        }
        source.m_pendingOneShots.clear();

        applyVoiceParameters(source.m_voiceId, source, transform, effects, sourceVelocity);
    }

    /// @note AudioSource を持たない使い捨て再生 (PlayAtPoint)。
    /// @note 減衰とパンは一度だけ焼き込むので、鳴っている間に音像は動かない。
    for (auto& request : audioManager.TakePositional()) {
        audio::AudioManager::PlayParams params;
        params.priority = request.priority;
        const uint32_t voiceId = request.clip != 0
            ? audioManager.PlayClipVoice(request.clip, false, request.bus, params)
            : audioManager.PlayVoice(request.path, false, request.bus, params);
        if (request.clip != 0) audioManager.ReleaseClip(request.clip);
        if (voiceId == 0) continue;

        float attenuation = 1.0f;
        float pan         = 0.0f;
        float lowPass     = 1.0f;
        spatialize({ request.x, request.y, request.z }, 1.0f,
                   request.minDistance, request.maxDistance, request.rolloff,
                   request.airAbsorption, attenuation, pan, lowPass);

        audioManager.SetVoiceVolume(
            voiceId, (std::max)(request.volume, 0.0f) * attenuation * listenerVolume);
        audioManager.SetVoicePan(voiceId, pan);
        audioManager.SetVoiceLowPass(voiceId, lowPass);
    }
}

} /// @note namespace fbzz::scene
