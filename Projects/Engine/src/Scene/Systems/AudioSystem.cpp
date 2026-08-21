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

    const auto applyVoiceParameters = [&](uint32_t voiceId,
                                          const AudioSourceComponent& source,
                                          const Transform& sourceTransform,
                                          GameObject* sourceObject) {
        if (voiceId == 0) return;

        float attenuation = 1.0f;
        float pan = 0.0f;
        if (listener && listenerTransform && source.spatialBlend > 0.0f) {
            const math::Vector3 offset = sourceTransform.worldPosition - listenerTransform->worldPosition;
            const float distance = offset.Length();
            const float minDistance = (std::max)(source.minDistance, 0.0f);
            const float maxDistance = (std::max)(source.maxDistance, minDistance + 0.001f);
            const float normalized = (std::max)(0.0f, (std::min)(
                (distance - minDistance) / (maxDistance - minDistance), 1.0f));
            const float rolloff = (std::max)(source.rolloffFactor, 0.01f);
            const float distanceGain = std::pow(1.0f - normalized, rolloff);
            const float blend = (std::max)(0.0f, (std::min)(source.spatialBlend, 1.0f));
            attenuation = (1.0f - blend) + blend * distanceGain;

            if (distance > 0.0001f) {
                pan = math::Vector3::Dot(offset / distance, listenerTransform->Right()) * blend;
            }
        }

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
            voiceId, (std::max)(source.volume, 0.0f) * attenuation * listenerVolume * effectGain);
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

        // Pause は再開位置を保持しない。既存 API の意味を維持し、Resume 相当の Play で先頭から再生する。
        if (source.m_pendingPause) {
            if (source.m_isPlaying) {
                audioManager.StopVoice(source.m_voiceId);
                source.m_voiceId = 0;
                source.m_isPlaying = false;
                source.m_isPaused = true;
            }
            source.m_pendingPause = false;
        }

        const bool playOnAwake = source.playOnAwake && !source.m_played;
        if ((source.m_pendingPlay || playOnAwake) && !source.clipPath.empty()) {
            source.m_played = true;
            source.m_pendingPlay = false;
            source.m_isPaused = false;
            audioManager.StopVoice(source.m_voiceId);
            source.m_voiceId = audioManager.PlayVoice(source.clipPath, source.loop);
            source.m_isPlaying = source.m_voiceId != 0;
        }

        if (source.m_pendingOneShot) {
            source.m_pendingOneShot = false;
            if (!source.m_oneShotPath.empty()) {
                const uint32_t oneShot = audioManager.PlayVoice(source.m_oneShotPath, false);
                applyVoiceParameters(oneShot, source, transform, sourceObject);
            }
            source.m_oneShotPath.clear();
        }

        applyVoiceParameters(source.m_voiceId, source, transform, sourceObject);
    }
}

} // namespace fbzz::scene
