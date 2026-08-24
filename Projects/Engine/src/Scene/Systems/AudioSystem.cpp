// FBZZ Engine
// AudioSystem.cpp | fbzz::scene
// AudioSourceComponent と AudioListenerComponent を同期する 2D / 3D 音響 System
// 距離減衰・左右パン・pitch を voice 単位で AudioManager へ転送する。
#include "Engine/Scene/Systems/AudioSystem.hpp"
#include "Engine/Core/Scheduler/SystemContext.hpp"
#include "Engine/Scene/Systems/ScriptSystem.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/GameObject.hpp"
#include "Engine/Scene/Components/AudioListenerComponent.hpp"
#include "Engine/Scene/Components/AudioSourceComponent.hpp"
#include "Engine/Scene/Components/AudioSpatialComponents.hpp"
#include "Engine/Audio/AudioManager.hpp"
#include <Physics/World.hpp>
#include <algorithm>
#include <cmath>
#include <limits>

namespace fbzz::scene {

ComponentAccess AudioSystem::GetAccess() const
{
    return ComponentAccess{}
        .Reads<AudioListenerComponent, Transform>()
        .Writes<AudioSourceComponent>();
}

OrderingHints AudioSystem::GetOrder() const
{
    return OrderingHints{}.After<LateScriptSystem>();
}

void AudioSystem::Update(SystemContext& ctx)
{
    if (!ctx.audioManager) return;
    audio::AudioManager& audioManager = *ctx.audioManager;

    // WHAT: priority 最大の有効な Listener を受聴点にする。同値なら Scene 登録順を維持する。
    const AudioListenerComponent* listener = nullptr;
    const Transform* listenerTransform = nullptr;
    int bestPriority = (std::numeric_limits<int>::min)();
    for (EntityID id : ctx.scene.GetEntities<AudioListenerComponent>()) {
        const auto* candidate = ctx.scene.GetComponent<AudioListenerComponent>(id);
        const auto* go = ctx.scene.GetGameObject(id);
        if (!candidate || !candidate->enabled || !go || !go->activeInHierarchy()
            || candidate->priority <= bestPriority) continue;
        listener = candidate;
        listenerTransform = &go->transform;
        bestPriority = candidate->priority;
    }

    float zoneWet = 0.0f;
    float zoneHighFrequency = 1.0f;
    if (listenerTransform) {
        for (auto [zone, transform] : ctx.scene.View<AudioReverbZoneComponent, Transform>()) {
            if (!zone.enabled)
                continue;
            const float distance = (listenerTransform->worldPosition - transform.worldPosition).Length();
            if (distance > zone.outerRadius)
                continue;
            const float blend = distance <= zone.innerRadius ? 1.0f
                : 1.0f - (distance - zone.innerRadius)
                    / (std::max)(zone.outerRadius - zone.innerRadius, 0.001f);
            if (zone.wetLevel * blend > zoneWet) {
                zoneWet = zone.wetLevel * blend;
                zoneHighFrequency = zone.highFrequencyRatio;
            }
        }
    }

    // 位置と 3D 設定から減衰とパンを出す。AudioSource を持つ音源と、
    // 位置だけ指定された使い捨て再生 (PlayAtPoint) の両方がここを通る。
    const auto spatialize = [&](const math::Vector3& worldPosition, float spatialBlend,
                                float minDistanceIn, float maxDistanceIn, float rolloffIn,
                                float& outAttenuation, float& outPan) {
        outAttenuation = 1.0f;
        outPan = 0.0f;
        if (!listener || !listenerTransform || spatialBlend <= 0.0f) return;

        const math::Vector3 offset = worldPosition - listenerTransform->worldPosition;
        const float distance = offset.Length();
        const float minDistance = (std::max)(minDistanceIn, 0.0f);
        const float maxDistance = (std::max)(maxDistanceIn, minDistance + 0.001f);
        const float normalized = (std::max)(0.0f, (std::min)(
            (distance - minDistance) / (maxDistance - minDistance), 1.0f));
        const float rolloff = (std::max)(rolloffIn, 0.01f);
        const float distanceGain = std::pow(1.0f - normalized, rolloff);
        const float blend = (std::max)(0.0f, (std::min)(spatialBlend, 1.0f));
        outAttenuation = (1.0f - blend) + blend * distanceGain;

        if (distance > 0.0001f)
            outPan = math::Vector3::Dot(offset / distance, listenerTransform->Right()) * blend;
    };

    const auto applyVoiceParameters = [&](uint32_t voiceId,
                                          const AudioSourceComponent& source,
                                          const Transform& sourceTransform,
                                          GameObject* sourceObject,
                                          float volumeScale = 1.0f) {
        if (voiceId == 0) return;

        float attenuation = 1.0f;
        float pan = 0.0f;
        spatialize(sourceTransform.worldPosition, source.spatialBlend,
                   source.minDistance, source.maxDistance, source.rolloffFactor,
                   attenuation, pan);

        float effectGain = 1.0f;
        float lowPass = 1.0f - zoneWet * (1.0f - zoneHighFrequency);
        if (sourceObject) {
            if (const auto* send = sourceObject->GetComponent<AudioMixerSendComponent>();
                send && send->enabled)
                effectGain *= std::clamp(send->sendLevel, 0.0f, 1.0f);
            if (auto* occlusion = sourceObject->GetComponent<AudioOcclusionComponent>();
                occlusion && occlusion->enabled && listenerTransform) {
                occlusion->updateTimer -= ctx.dt;
                if (occlusion->updateTimer <= 0.0f) {
                    const math::Vector3 offset =
                        listenerTransform->worldPosition - sourceTransform.worldPosition;
                    physics::World::RaycastHit hit{};
                    const float distance = offset.Length();
                    const bool blocked = distance > 0.001f
                        && ctx.world.Raycast(sourceTransform.worldPosition, offset / distance,
                                             distance - 0.001f, hit);
                    occlusion->currentOcclusion = blocked ? 1.0f : 0.0f;
                    occlusion->updateTimer = (std::max)(occlusion->updateInterval, 0.01f);
                }
                effectGain *= 1.0f - occlusion->currentOcclusion
                    * std::clamp(occlusion->volumeAttenuation, 0.0f, 1.0f);
                lowPass = (std::min)(lowPass, 1.0f - occlusion->currentOcclusion
                    * std::clamp(occlusion->lowPass, 0.0f, 1.0f));
            }
        }
        const float listenerVolume = listener ? (std::max)(listener->volume, 0.0f) : 1.0f;
        audioManager.SetVoiceVolume(
            voiceId, (std::max)(source.volume, 0.0f) * (std::max)(volumeScale, 0.0f)
                         * attenuation * listenerVolume * effectGain);
        audioManager.SetVoicePitch(voiceId, (std::max)(source.pitch, 0.01f));
        audioManager.SetVoicePan(voiceId, pan);
        audioManager.SetVoiceLowPass(voiceId, lowPass);
    };

    for (EntityID id : ctx.scene.GetEntities<AudioSourceComponent>()) {
        auto* sourcePointer = ctx.scene.GetComponent<AudioSourceComponent>(id);
        GameObject* sourceObject = ctx.scene.GetGameObject(id);
        if (!sourcePointer || !sourceObject)
            continue;
        AudioSourceComponent& source = *sourcePointer;
        Transform& transform = sourceObject->transform;
        if (source.m_voiceId != 0 && !audioManager.IsVoicePlaying(source.m_voiceId)) {
            source.m_voiceId = 0;
            source.m_isPlaying = false;
        }

        if (!source.enabled || !sourceObject->activeInHierarchy()) {
            audioManager.StopVoice(source.m_voiceId);
            source.m_voiceId = 0;
            source.m_isPlaying = false;
            continue;
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

        // voice は破棄せず止めるだけなので、Resume で続きから鳴る。
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

        // 生成クリップの要求は clipPath より優先する。要求フィールドが参照を 1 つ
        // 握っているので、voice を起こしたあとに必ず手放す (起動失敗時も同じ)。
        const bool playOnAwake = source.playOnAwake && !source.m_played;
        if (source.m_pendingClipId != 0) {
            source.m_played = true;
            source.m_pendingPlay = false;
            source.m_isPaused = false;
            audioManager.StopVoice(source.m_voiceId);
            source.m_voiceId = audioManager.PlayClipVoice(source.m_pendingClipId, source.loop, bus);
            source.m_isPlaying = source.m_voiceId != 0;
            audioManager.ReleaseClip(source.m_pendingClipId);
            source.m_pendingClipId = 0;
        } else if ((source.m_pendingPlay || playOnAwake) && !source.clipPath.empty()) {
            source.m_played = true;
            source.m_pendingPlay = false;
            source.m_isPaused = false;
            audioManager.StopVoice(source.m_voiceId);
            source.m_voiceId = audioManager.PlayVoice(source.clipPath, source.loop, bus);
            source.m_isPlaying = source.m_voiceId != 0;
        }

        for (const auto& request : source.m_pendingOneShots) {
            const uint32_t oneShot = request.clipId != 0
                ? audioManager.PlayClipVoice(request.clipId, false, bus)
                : audioManager.PlayVoice(request.path, false, bus);
            applyVoiceParameters(oneShot, source, transform, sourceObject, request.volumeScale);
            // 要求が握っていた参照を返す。再生中は voice 側が実体を押さえる。
            if (request.clipId != 0) audioManager.ReleaseClip(request.clipId);
        }
        source.m_pendingOneShots.clear();

        applyVoiceParameters(source.m_voiceId, source, transform, sourceObject);
    }

    // AudioSource を持たない使い捨て再生 (PlayAtPoint)。
    // 減衰とパンはここで一度だけ焼き込む。短い効果音が前提なので、
    // 鳴っている間に受聴点が動いても音像は追従しない。
    for (auto& request : audioManager.TakePositional()) {
        const uint32_t voiceId = request.clip != 0
            ? audioManager.PlayClipVoice(request.clip, false, request.bus)
            : audioManager.PlayVoice(request.path, false, request.bus);
        if (request.clip != 0) audioManager.ReleaseClip(request.clip);
        if (voiceId == 0) continue;

        float attenuation = 1.0f;
        float pan = 0.0f;
        spatialize({ request.x, request.y, request.z }, 1.0f,
                   request.minDistance, request.maxDistance, request.rolloff,
                   attenuation, pan);

        const float listenerVolume = listener ? (std::max)(listener->volume, 0.0f) : 1.0f;
        audioManager.SetVoiceVolume(
            voiceId, (std::max)(request.volume, 0.0f) * attenuation * listenerVolume);
        audioManager.SetVoicePan(voiceId, pan);
        audioManager.SetVoiceLowPass(voiceId, 1.0f - zoneWet * (1.0f - zoneHighFrequency));
    }
}

} // namespace fbzz::scene
