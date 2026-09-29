/// @file    ScriptAudioLoops.cpp
/// @brief   Script が所有する独立ループ音を AudioSource と寿命トークンへ変換する。
/// @author  Hasegawa Jin
/// @date    2026-09-29
#include <Engine/Scene/ScriptProxy/ScriptAudioProxy.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/AudioSourceComponent.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::scene {
namespace {
AudioSourceComponent* ResolveLoop(Script* owner, EntityID id,
                                  const std::shared_ptr<audio::VoiceLifetime>& lifetime)
{
    if (!owner || !lifetime || !lifetime->active) return nullptr;
    auto* object = owner->scene.GetGameObject(id);
    if (!object) return nullptr;
    auto* source = object->GetComponent<AudioSourceComponent>();
    return source && source->m_scriptLoopLifetime == lifetime ? source : nullptr;
}
} /// @note namespace

bool AudioLoop::IsValid() const
{
    const auto lifetime = m_lifetime.lock();
    return lifetime && lifetime->active;
}

bool ScriptAudioProxy::UpdateLoop(AudioLoop& loop, std::string_view clipPath, float volume,
                                  float pitch, const AudioLoopSettings& settings) const
{
    if (!script || (loop.m_owner && loop.m_owner != script)) return false;
    if (!std::isfinite(volume) || !std::isfinite(pitch) || !std::isfinite(settings.spatialBlend)
        || !std::isfinite(settings.minDistance) || !std::isfinite(settings.maxDistance)) return false;
    if (clipPath.empty() || volume <= 0.001f) {
        StopLoop(loop);
        return true;
    }
    if (!script->m_scene || !script->m_gameObject || !script->scene.IsActiveAndEnabled()) return false;

    auto lifetime = loop.m_lifetime.lock();
    auto* source = ResolveLoop(script, loop.m_source, lifetime);
    if (!source) {
        /// @note 名前は表示だけに使う。同じラベルの複数個体が同じ音源を取得することはない。
        auto& object = script->m_scene->CreateGameObject("SFX_Loop_" + settings.label);
        object.runtimeGenerated = true;
        object.SetParent(*script->m_gameObject);
        object.transform.position = math::Vector3::ZERO;
        source = &object.AddComponent<AudioSourceComponent>();
        lifetime = std::make_shared<audio::VoiceLifetime>();
        source->m_scriptLoopLifetime = lifetime;
        loop.m_source = object.GetID();
        loop.m_owner = script;
        loop.m_lifetime = lifetime;
        std::erase_if(script->m_ownedAudioLoops, [](const auto& weak) {
            const auto value = weak.lock();
            return !value || !value->active;
        });
        script->m_ownedAudioLoops.push_back(lifetime);
    }

    /// @note 更新ごとに再生要求を出すとループ先頭へ巻き戻る。停止後・クリップやバス変更時だけ開始する。
    const bool restart = source->clipPath != clipPath || source->busName != settings.bus
        || (!source->m_isPlaying && !source->m_pendingPlay) || source->m_pendingStop;
    source->clipPath = std::string(clipPath);
    source->busName = settings.bus;
    source->loop = true;
    source->playOnAwake = false;
    source->enabled = true;
    source->volume = std::clamp(volume, 0.0f, 1.0f);
    source->pitch = std::clamp(pitch, 0.01f, 4.0f);
    source->spatialBlend = std::clamp(settings.spatialBlend, 0.0f, 1.0f);
    source->minDistance = (std::max)(settings.minDistance, 0.0f);
    source->maxDistance = (std::max)(settings.maxDistance, source->minDistance + 0.001f);
    if (restart) {
        source->m_pendingPlay = true;
        source->m_pendingStop = false;
        source->m_pendingPause = false;
        source->m_pendingResume = false;
    }
    return true;
}

bool ScriptAudioProxy::UpdateLoop(AudioLoop& loop, const AudioClipRef& clip, float volume,
                                  float pitch, const AudioLoopSettings& settings) const
{
    return UpdateLoop(loop, clip.ResolvePath(), volume, pitch, settings);
}

bool ScriptAudioProxy::StopLoop(const AudioLoop& loop) const
{
    if (loop.m_owner != script) return false;
    auto* source = ResolveLoop(script, loop.m_source, loop.m_lifetime.lock());
    if (!source) return false;
    source->m_pendingStop = true;
    source->m_pendingPlay = false;
    source->m_pendingPause = false;
    source->m_pendingResume = false;
    return true;
}

void ScriptAudioProxy::ReleaseLoop(AudioLoop& loop) const
{
    if (loop.m_owner != script) return;
    if (const auto lifetime = loop.m_lifetime.lock()) lifetime->active = false;
    loop = {};
}
} /// @note namespace fbzz::scene
