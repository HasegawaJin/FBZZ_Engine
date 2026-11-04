/// @file    ScriptAudioProxies.cpp
/// @brief   Audio 領域の Script 転送と関連する内部ヘルパー。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#include "ScriptProxyCommon.hpp"
#include <Engine/Scene/Script.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/SynthAsset.hpp>
#include <Engine/Audio/AudioManager.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Input/Input.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/AudioSourceComponent.hpp>
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
#include <Engine/Audio/Synth.hpp>
#include <Engine/Core/Logger.hpp>
namespace fbzz::scene {
using namespace script_proxy_detail;
namespace {


audio::AudioManager* ActiveAudioManager()
{
    return core::Application::Get().GetAudioManager();
}


bool LoadSynthSpecFromPath(std::string_view path, audio::SynthSpec& out)
{
    if (path.empty()) return false;
    const std::string requested(path);
    const std::string resolved = asset::AssetManager::ResolveAssetPath(requested);
    asset::SynthAsset synth;
    if (!asset::LoadSynthAssetFromFile(resolved.empty() ? requested : resolved, synth))
        return false;
    out = synth.spec;
    return true;
}

/// @brief 手続きクリップの再生要求は「フィールドが参照を 1 つ握る」規約で動く。
/// @brief 同じフレームに 2 回積まれたら、置き換えられる側の参照をここで返す。

void HandOffPendingClip(audio::AudioManager& manager, uint32_t& slot, uint32_t clip)
{
    if (slot != 0) manager.ReleaseClip(slot);
    slot = clip;
}

/// @brief AudioSource が無い GameObject で音声 API を呼んだことを一度だけ報告する。
/// @note 音声は「鳴らない」以外の症状が出ずコンポーネントの付け忘れに気付きにくいため警告する。
/// @note OnUpdate から呼ばれても Console が埋まらないよう (オブジェクト名, API 名) で重複を 1 回に絞る。

void WarnMissingAudioSource(const Script* script, const char* api)
{
    static std::unordered_set<std::string> reported;
    /// @note m_gameObject は protected で、friend なのは Proxy 型だけ。free function から
    /// @note 直接は引けないため、公開されている scene プロキシ経由で取り出す。
    const GameObject* go = script ? script->scene.Self() : nullptr;
    std::string key = go ? go->name : std::string("<no object>");
    key += '/';
    key += api;
    if (!reported.insert(key).second) return;
    FBZZ_LOG_WARN("audio.%s: '%s' に AudioSourceComponent がありません — この呼び出しは無視されます",
                  api, go ? go->name.c_str() : "<no object>");
}

/// @brief 書き込み系の音声 API 用。見つからなければ警告してから nullptr を返す。

AudioSourceComponent* SelfAudioSource(const Script* script, const char* api)
{
    auto* source = SelfComponent<AudioSourceComponent>(script);
    if (!source) WarnMissingAudioSource(script, api);
    return source;
}

/// @brief one-shot 要求を積む。上限を超えたぶんは捨て、握っていた参照を返す。

void PushOneShot(AudioSourceComponent& source, AudioSourceComponent::OneShotRequest request)
{
    if (source.m_pendingOneShots.size() >= AudioSourceComponent::MAX_PENDING_ONE_SHOTS) {
        if (request.clipId != 0) {
            if (auto* manager = core::Application::Get().GetAudioManager())
                manager->ReleaseClip(request.clipId);
        }
        return;
    }
    source.m_pendingOneShots.push_back(std::move(request));
}


} /// @note namespace
bool ScriptAudioProxy::Preload(std::string_view clipPath) const
{
    auto* manager = ActiveAudioManager();
    if (!manager || clipPath.empty()) {
        FBZZ_LOG_WARN("Audio Preload: %s", manager ? "empty clip path" : "audio manager is unavailable");
        return false;
    }
    return manager->AcquireClip(std::string(clipPath)) != 0;
}


bool ScriptAudioProxy::Preload(const AudioClipRef& clip) const
{
    return Preload(clip.ResolvePath());
}


void ScriptAudioProxy::Play() const
{
    auto* audio = SelfAudioSource(script, "Play");
    if (!audio) return;
    if (audio->clipPath.empty()) {
        FBZZ_LOG_WARN("audio.Play(): Clip Path が未設定です (Inspector で指定するか "
                      "Play(path) を使ってください)");
        return;
    }
    audio->enabled        = true;
    audio->m_pendingPlay  = true;
    audio->m_pendingStop  = false;
    audio->m_pendingPause = false;
}


void ScriptAudioProxy::Play(std::string_view clipPath) const
{
    auto* audio = SelfAudioSource(script, "Play");
    if (!audio) return;
    audio->clipPath       = std::string(clipPath);
    audio->enabled        = true;
    audio->m_pendingPlay  = true;
    audio->m_pendingStop  = false;
    audio->m_pendingPause = false;
}


void ScriptAudioProxy::Play(const AudioClipRef& clip) const
{
    Play(clip.ResolvePath());
}


void ScriptAudioProxy::Stop() const
{
    auto* audio = SelfAudioSource(script, "Stop");
    if (!audio) return;
    audio->m_pendingStop   = true;
    audio->m_pendingPlay   = false;
    audio->m_pendingPause  = false;
    audio->m_pendingResume = false;
}


void ScriptAudioProxy::Pause() const
{
    auto* audio = SelfAudioSource(script, "Pause");
    if (!audio) return;
    audio->m_pendingPause  = true;
    audio->m_pendingPlay   = false;
    audio->m_pendingResume = false;
}


void ScriptAudioProxy::Resume() const
{
    auto* audio = SelfAudioSource(script, "Resume");
    if (!audio) return;
    audio->m_pendingResume = true;
    audio->m_pendingPause  = false;
}


void ScriptAudioProxy::SetVolume(float v) const
{
    if (auto* audio = SelfAudioSource(script, "SetVolume"))
        audio->volume = std::clamp(v, 0.0f, 1.0f);
}


void ScriptAudioProxy::SetLoop(bool loop) const
{
    if (auto* audio = SelfAudioSource(script, "SetLoop"))
        audio->loop = loop;
}


bool ScriptAudioProxy::IsPlaying() const
{
    const auto* audio = SelfComponent<AudioSourceComponent>(script);
    return audio && audio->m_isPlaying;
}


float ScriptAudioProxy::GetVolume() const
{
    const auto* audio = SelfComponent<AudioSourceComponent>(script);
    return audio ? audio->volume : 0.0f;
}


void ScriptAudioProxy::SetPitch(float pitch) const
{
    if (auto* audio = SelfAudioSource(script, "SetPitch"))
        audio->pitch = std::clamp(pitch, 0.01f, 4.0f);
}


float ScriptAudioProxy::GetPitch() const
{
    const auto* audio = SelfComponent<AudioSourceComponent>(script);
    return audio ? audio->pitch : 1.0f;
}


bool ScriptAudioProxy::GetLoop() const
{
    const auto* audio = SelfComponent<AudioSourceComponent>(script);
    return audio && audio->loop;
}


bool ScriptAudioProxy::IsPaused() const
{
    const auto* audio = SelfComponent<AudioSourceComponent>(script);
    return audio && audio->m_isPaused;
}


void ScriptAudioProxy::SetSpatialBlend(float blend) const
{
    if (auto* audio = SelfAudioSource(script, "SetSpatialBlend"))
        audio->spatialBlend = std::clamp(blend, 0.0f, 1.0f);
}


float ScriptAudioProxy::GetSpatialBlend() const
{
    const auto* audio = SelfComponent<AudioSourceComponent>(script);
    return audio ? audio->spatialBlend : 0.0f;
}


void ScriptAudioProxy::Set3DDistances(float minDistance, float maxDistance, float rolloff) const
{
    if (auto* audio = SelfAudioSource(script, "Set3DDistances")) {
        audio->minDistance = (std::max)(minDistance, 0.0f);
        audio->maxDistance = (std::max)(maxDistance, audio->minDistance + 0.001f);
        audio->rolloffFactor = (std::max)(rolloff, 0.01f);
    }
}


void ScriptAudioProxy::PlayOneShot(std::string_view clipPath, float volumeScale) const
{
    auto* audio = SelfAudioSource(script, "PlayOneShot");
    if (!audio || clipPath.empty()) return;
    AudioSourceComponent::OneShotRequest request;
    request.path        = std::string(clipPath);
    request.volumeScale = (std::max)(volumeScale, 0.0f);
    PushOneShot(*audio, std::move(request));
}


void ScriptAudioProxy::PlayOneShot(const AudioClipRef& clip, float volumeScale) const
{
    PlayOneShot(clip.ResolvePath(), volumeScale);
}


audio::SynthSpec ScriptAudioProxy::MakeSpec(audio::SynthPreset preset, uint32_t seed) const
{
    return audio::MakePreset(preset, seed);
}


audio::SynthSpec ScriptAudioProxy::MutateSpec(const audio::SynthSpec& base,
                                              float amount, uint32_t seed) const
{
    return audio::Mutate(base, amount, seed);
}


bool ScriptAudioProxy::LoadSpec(const AudioClipRef& synthAsset, audio::SynthSpec& out) const
{
    return LoadSynthSpecFromPath(synthAsset.ResolvePath(), out);
}


bool ScriptAudioProxy::LoadSpec(std::string_view synthAssetPath, audio::SynthSpec& out) const
{
    return LoadSynthSpecFromPath(synthAssetPath, out);
}


SynthClip ScriptAudioProxy::Synthesize(const audio::SynthSpec& spec) const
{
    auto* manager = ActiveAudioManager();
    return manager ? SynthClip{ manager->AcquireGeneratedClip(spec) } : SynthClip{};
}


SynthClip ScriptAudioProxy::Synthesize(audio::SynthPreset preset, uint32_t seed) const
{
    return Synthesize(audio::MakePreset(preset, seed));
}


void ScriptAudioProxy::ReleaseClip(SynthClip clip) const
{
    if (auto* manager = ActiveAudioManager()) manager->ReleaseClip(clip.id);
}


void ScriptAudioProxy::PlayClip(SynthClip clip) const
{
    auto* manager = ActiveAudioManager();
    auto* audio   = SelfAudioSource(script, "PlayClip");
    if (!manager || !audio || !clip.IsValid()) return;
    /// @note 要求が参照を 1 つ握る。AudioSystem が再生後に手放す。
    manager->AddClipRef(clip.id);
    AudioSourceComponent::OneShotRequest request;
    request.clipId = clip.id;
    PushOneShot(*audio, std::move(request));
}


void ScriptAudioProxy::PlayClipAsSource(SynthClip clip) const
{
    auto* manager = ActiveAudioManager();
    auto* audio   = SelfAudioSource(script, "PlayClipAsSource");
    if (!manager || !audio || !clip.IsValid()) return;
    manager->AddClipRef(clip.id);
    HandOffPendingClip(*manager, audio->m_pendingClipId, clip.id);
    audio->enabled        = true;
    audio->m_pendingStop  = false;
    audio->m_pendingPause = false;
}


void ScriptAudioProxy::PlaySynth(const audio::SynthSpec& spec) const
{
    auto* manager = ActiveAudioManager();
    auto* audio   = SelfAudioSource(script, "PlaySynth");
    if (!manager || !audio) return;
    /// @note AcquireGeneratedClip が返す参照をそのまま要求へ譲る。
    const uint32_t clip = manager->AcquireGeneratedClip(spec);
    if (clip == 0) return;
    AudioSourceComponent::OneShotRequest request;
    request.clipId = clip;
    PushOneShot(*audio, std::move(request));
}


void ScriptAudioProxy::PlaySynth(audio::SynthPreset preset, uint32_t seed) const
{
    PlaySynth(audio::MakePreset(preset, seed));
}


void ScriptAudioProxy::PlaySynth2D(const audio::SynthSpec& spec, std::string_view bus) const
{
    auto* manager = ActiveAudioManager();
    if (!manager) return;
    const uint32_t clip = manager->AcquireGeneratedClip(spec);
    if (clip == 0) return;
    (void)manager->PlayClipVoice(clip, false, manager->FindBus(bus));
    /// @note 再生中は voice 側が実体を押さえる。ここでの参照はもう要らない。
    manager->ReleaseClip(clip);
}


void ScriptAudioProxy::PlayClip2D(SynthClip clip, std::string_view bus) const
{
    auto* manager = ActiveAudioManager();
    if (!manager || !clip.IsValid()) return;
    (void)manager->PlayClipVoice(clip.id, false, manager->FindBus(bus));
}


void ScriptAudioProxy::PlayAtPoint(std::string_view clipPath, const math::Vector3& position,
                                   float volume) const
{
    auto* manager = ActiveAudioManager();
    if (!manager || clipPath.empty()) return;

    audio::AudioManager::PositionalRequest request;
    request.path   = std::string(clipPath);
    request.volume = (std::max)(volume, 0.0f);
    request.x = position.x; request.y = position.y; request.z = position.z;
    /// @note 自 GameObject に AudioSource があれば、その 3D 設定を引き継ぐ (無ければ既定値)。
    /// @note «同じ銃の弾着音» を発生源の減衰カーブと揃えたい場合が普通なので設定を流用する。
    if (const auto* source = SelfComponent<AudioSourceComponent>(script)) {
        request.minDistance   = source->minDistance;
        request.maxDistance   = source->maxDistance;
        request.rolloff       = source->rolloffFactor;
        request.airAbsorption = source->airAbsorption;
        request.priority      = source->priority;
        request.bus           = manager->FindBus(source->busName);
    } else {
        request.bus = manager->FindBus("SE");
    }
    manager->QueuePositional(std::move(request));
}


void ScriptAudioProxy::PlayAtPoint(const AudioClipRef& clip, const math::Vector3& position,
                                   float volume) const
{
    PlayAtPoint(clip.ResolvePath(), position, volume);
}


void ScriptAudioProxy::PlayClipAtPoint(SynthClip clip, const math::Vector3& position,
                                       float volume) const
{
    auto* manager = ActiveAudioManager();
    if (!manager || !clip.IsValid()) return;

    manager->AddClipRef(clip.id);
    audio::AudioManager::PositionalRequest request;
    request.clip   = clip.id;
    request.volume = (std::max)(volume, 0.0f);
    request.x = position.x; request.y = position.y; request.z = position.z;
    if (const auto* source = SelfComponent<AudioSourceComponent>(script)) {
        request.minDistance   = source->minDistance;
        request.maxDistance   = source->maxDistance;
        request.rolloff       = source->rolloffFactor;
        request.airAbsorption = source->airAbsorption;
        request.priority      = source->priority;
        request.bus           = manager->FindBus(source->busName);
    } else {
        request.bus = manager->FindBus("SE");
    }
    manager->QueuePositional(std::move(request));
}


void ScriptAudioProxy::PlayBGM(std::string_view clipPath, bool loop, float fadeSeconds) const
{
    if (auto* manager = ActiveAudioManager())
        manager->PlayBGM(std::string(clipPath), loop, (std::max)(fadeSeconds, 0.0f));
}


void ScriptAudioProxy::PlayBGM(const AudioClipRef& clip, bool loop, float fadeSeconds) const
{
    PlayBGM(clip.ResolvePath(), loop, fadeSeconds);
}


void ScriptAudioProxy::StopBGM(float fadeSeconds) const
{
    if (auto* manager = ActiveAudioManager()) manager->StopBGM((std::max)(fadeSeconds, 0.0f));
}


void ScriptAudioProxy::FadeTo(float gain, float seconds) const
{
    auto* manager = ActiveAudioManager();
    auto* audio   = SelfAudioSource(script, "FadeTo");
    if (!manager || !audio || audio->m_voiceId == 0) return;
    manager->FadeVoice(audio->m_voiceId, gain, (std::max)(seconds, 0.0f));
}


void ScriptAudioProxy::FadeOutAndStop(float seconds) const
{
    auto* manager = ActiveAudioManager();
    auto* audio   = SelfAudioSource(script, "FadeOutAndStop");
    if (!manager || !audio || audio->m_voiceId == 0) return;
    manager->FadeOutAndStop(audio->m_voiceId, (std::max)(seconds, 0.0f));
    /// @note ハンドルはこの時点で AudioManager 側の持ち物になる。
    audio->m_voiceId   = 0;
    audio->m_isPlaying = false;
    audio->m_isPaused  = false;
}


bool ScriptAudioProxy::IsBGMPlaying() const
{
    const auto* manager = ActiveAudioManager();
    return manager && manager->IsBGMPlaying();
}


void ScriptAudioProxy::SetBus(std::string_view bus) const
{
    if (auto* audio = SelfAudioSource(script, "SetBus"))
        audio->busName = std::string(bus);
}


std::string_view ScriptAudioProxy::GetBus() const
{
    /// @note 参照先は AudioSourceComponent が所有する文字列。呼び出し側が SetBus を
    /// @note 挟まない限り有効で、コピーを返さずに済む。
    const auto* audio = SelfComponent<AudioSourceComponent>(script);
    return audio ? std::string_view(audio->busName) : std::string_view{};
}


void ScriptAudioProxy::SetBusVolume(std::string_view bus, float volume) const
{
    if (auto* manager = ActiveAudioManager()) manager->SetBusVolume(bus, volume);
}


float ScriptAudioProxy::GetBusVolume(std::string_view bus) const
{
    auto* manager = ActiveAudioManager();
    return manager ? manager->GetBusVolume(bus) : 0.0f;
}


void ScriptAudioProxy::SetBusLowPass(std::string_view bus, float normalizedCutoff) const
{
    if (auto* manager = ActiveAudioManager()) manager->SetBusLowPass(bus, normalizedCutoff);
}


} /// @note namespace fbzz::scene
