/// @file    ManagedServiceAudio.cpp
/// @brief   音声操作、合成パケット、クリップとループの解放。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#include "ManagedServiceAudio.hpp"
#include "ManagedAudioSourceServices.hpp"

#include <Engine/Scene/Components/AudioSourceComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <span>

namespace fbzz::scene::managed_detail {

namespace {

/// @note 下位 32 ビットはシード、上位 32 ビットはサンプルレート [Hz]。
bool DecodeSpec(std::span<const float> values, int64_t integer, audio::SynthSpec& spec)
{
    if (values.size() < 19 || values[0] < 0 || values[0] > 4 || std::floor(values[0]) != values[0]) return false;
    spec.wave = static_cast<audio::SynthWave>(static_cast<uint32_t>(values[0]));
    spec.attack = values[1];
    spec.sustain = values[2];
    spec.decay = values[3];
    spec.punch = values[4];
    spec.startFrequency = values[5];
    spec.minFrequency = values[6];
    spec.slide = values[7];
    spec.vibratoDepth = values[8];
    spec.vibratoRate = values[9];
    spec.dutyCycle = values[10];
    spec.dutySweep = values[11];
    spec.lowPassCutoff = values[12];
    spec.lowPassSweep = values[13];
    spec.highPassCutoff = values[14];
    spec.bitCrush = values[15];
    spec.repeatRate = values[16];
    spec.arpeggioMod = values[17];
    spec.amplitude = values[18];
    const uint64_t packed = std::bit_cast<uint64_t>(integer); spec.seed = static_cast<uint32_t>(packed); spec.sampleRate = static_cast<uint32_t>(packed >> 32);
    return true;
}
void EncodeSpec(const audio::SynthSpec& spec, ManagedServiceResult& result)
{
    result.values = {static_cast<float>(spec.wave), spec.attack, spec.sustain, spec.decay, spec.punch, spec.startFrequency, spec.minFrequency, spec.slide, spec.vibratoDepth, spec.vibratoRate, spec.dutyCycle, spec.dutySweep, spec.lowPassCutoff, spec.lowPassSweep, spec.highPassCutoff, spec.bitCrush, spec.repeatRate, spec.arpeggioMod, spec.amplitude};
    result.integer = std::bit_cast<int64_t>((static_cast<uint64_t>(spec.sampleRate) << 32) | spec.seed);
}

}

ManagedBridgeStatus ManagedServiceAudio::RunAdvanced(Scene& scene, Script& caller, Script& view, GameObject& target,
    const ManagedBridgeRequest& request, const ManagedServiceArguments& arguments, ManagedServiceResult& output)
{
    const auto& p = arguments.values; const auto& t = arguments.text; const int64_t integer = arguments.integer;
    if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
    const bool readOnly = request.index == 29 || request.index == 30 || request.index == 31 || request.index == 45;
    if (request.flags != (readOnly ? 1u : 0u)) return ManagedBridgeStatus::INVALID_ARGUMENT;
    const uint64_t sceneId = scene.GetRenderSceneGeneration();
    const auto clipRecord = m_clips.find(integer);
    const auto loopRecord = m_loops.find(integer);
    const auto owned = [&](const auto& record) { return record.generation == request.generation && record.owner == request.ownerId && record.scene == sceneId; };
    const auto requireClip = [&]() { return integer > 0 && clipRecord != m_clips.end() && owned(clipRecord->second); };
    const auto requireLoop = [&]() { return integer > 0 && loopRecord != m_loops.end() && owned(loopRecord->second); };
    audio::SynthSpec spec;
    const uint64_t packed = std::bit_cast<uint64_t>(integer);
    const uint32_t seed = static_cast<uint32_t>(packed);
    const uint32_t preset = static_cast<uint32_t>(packed >> 32);
    switch (request.index) {
    case 29:
        if (!p.empty() || !t.empty() || preset > 6) return ManagedBridgeStatus::INVALID_ARGUMENT;
        EncodeSpec(view.audio.MakeSpec(static_cast<audio::SynthPreset>(preset), seed), output); return ManagedBridgeStatus::OK;
    case 30: {
        if (p.size() != 20 || t.size() != 1 || p[19] < 0 || p[19] > 1 || !DecodeSpec(p, integer, spec)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        uint32_t mutationSeed = 0; const auto parsed = std::from_chars(t[0].data(), t[0].data() + t[0].size(), mutationSeed);
        if (parsed.ec != std::errc{} || parsed.ptr != t[0].data() + t[0].size()) return ManagedBridgeStatus::INVALID_ARGUMENT;
        EncodeSpec(view.audio.MutateSpec(spec, p[19], mutationSeed), output); return ManagedBridgeStatus::OK;
    }
    case 31:
        if (!p.empty() || t.size() != 1 || integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (view.audio.LoadSpec(t[0], spec)) EncodeSpec(spec, output); return ManagedBridgeStatus::OK;
    case 32: case 33: {
        if (!t.empty() || (request.index == 32 ? p.size() != 19 || !DecodeSpec(p, integer, spec) : !p.empty() || preset > 6))
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (m_nextHandle == INT64_MAX) return ManagedBridgeStatus::PENDING_LIMIT;
        const SynthClip clip = request.index == 32 ? view.audio.Synthesize(spec) : view.audio.Synthesize(static_cast<audio::SynthPreset>(preset), seed);
        if (!clip.IsValid()) return ManagedBridgeStatus::OK;
        output.integer = m_nextHandle++; m_clips.emplace(output.integer, ClipRecord{request.generation, request.ownerId, sceneId, clip}); return ManagedBridgeStatus::OK;
    }
    case 34:
        if (!p.empty() || !t.empty()) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (!integer) return ManagedBridgeStatus::OK;
        if (!requireClip()) return ManagedBridgeStatus::NOT_FOUND;
        view.audio.ReleaseClip(clipRecord->second.clip); m_clips.erase(clipRecord); return ManagedBridgeStatus::OK;
    case 35: case 36:
        if (!p.empty() || !t.empty()) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (!requireClip()) return ManagedBridgeStatus::NOT_FOUND;
        if (!target.GetComponent<AudioSourceComponent>()) return ManagedBridgeStatus::NOT_FOUND;
        if (request.index == 35) view.audio.PlayClip(clipRecord->second.clip); else view.audio.PlayClipAsSource(clipRecord->second.clip);
        return ManagedBridgeStatus::OK;
    case 37: case 38: case 39:
        if ((request.index == 39 ? t.size() != 1 : !t.empty())
            || (request.index == 38 ? !p.empty() || preset > 6 : p.size() != 19 || !DecodeSpec(p, integer, spec))) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (request.index != 39 && !target.GetComponent<AudioSourceComponent>()) return ManagedBridgeStatus::NOT_FOUND;
        if (request.index == 37) view.audio.PlaySynth(spec);
        else if (request.index == 38) view.audio.PlaySynth(static_cast<audio::SynthPreset>(preset), seed);
        else view.audio.PlaySynth2D(spec, t[0]);
        return ManagedBridgeStatus::OK;
    case 40: case 41:
        if (request.index == 40 ? !p.empty() || t.size() != 1 : p.size() != 4 || !t.empty()) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (!requireClip()) return ManagedBridgeStatus::NOT_FOUND;
        if (request.index == 40) view.audio.PlayClip2D(clipRecord->second.clip, t[0]);
        else view.audio.PlayClipAtPoint(clipRecord->second.clip, {p[0], p[1], p[2]}, p[3]);
        return ManagedBridgeStatus::OK;
    case 42: {
        if (p.size() != 5 || t.size() != 3 || (integer && !requireLoop())) return integer ? ManagedBridgeStatus::NOT_FOUND : ManagedBridgeStatus::INVALID_ARGUMENT;
        if (caller.scene.Self() != &target) return ManagedBridgeStatus::INVALID_SCOPE;
        if (m_nextHandle == INT64_MAX || !std::isfinite(std::max(p[3], 0.0f) + 0.001f)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        AudioLoopSettings settings; settings.label = t[1]; settings.bus = t[2]; settings.spatialBlend = p[2]; settings.minDistance = p[3]; settings.maxDistance = p[4];
        AudioLoop loop = integer ? loopRecord->second.loop : AudioLoop{};
        const bool succeeded = caller.audio.UpdateLoop(loop, t[0], p[0], p[1], settings);
        output.integer = succeeded ? 1 : 0;
        if (succeeded) {
            const int64_t handle = integer ? integer : m_nextHandle++;
            if (integer) loopRecord->second.loop = loop; else m_loops.emplace(handle, LoopRecord{request.generation, request.ownerId, sceneId, loop});
            output.text = {std::to_string(handle)};
        }
        return ManagedBridgeStatus::OK;
    }
    case 43: case 44: case 45:
        if (!p.empty() || !t.empty()) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (caller.scene.Self() != &target) return ManagedBridgeStatus::INVALID_SCOPE;
        if (!integer) { output.integer = 0; return ManagedBridgeStatus::OK; }
        if (!requireLoop()) return ManagedBridgeStatus::NOT_FOUND;
        if (request.index == 43) output.integer = caller.audio.StopLoop(loopRecord->second.loop) ? 1 : 0;
        else if (request.index == 44) { caller.audio.ReleaseLoop(loopRecord->second.loop); m_loops.erase(loopRecord); }
        else output.integer = loopRecord->second.loop.IsValid() ? 1 : 0;
        return ManagedBridgeStatus::OK;
    default: return ManagedBridgeStatus::UNSUPPORTED;
    }
}

ManagedBridgeStatus ManagedServiceAudio::Run(Scene& scene, Script& caller, Script& view, GameObject& target,
    const ManagedBridgeRequest& request, const ManagedServiceArguments& arguments, ManagedServiceResult& output)
{
    const auto& p = arguments.values;
    const auto& t = arguments.text;
    const int64_t integer = arguments.integer;
    if (request.index >= 46)
        return RunAudioSourceSettings(target, request, integer, p, t, arguments.relatedId, output);
    if (request.index >= 29)
        return RunAdvanced(scene, caller, view, target, request, arguments, output);
    if ((request.index <= 10 || request.index == 12 || (request.index >= 14 && request.index <= 19)
        || request.index == 23 || request.index == 24 || request.index == 25) && !target.GetComponent<AudioSourceComponent>())
        return ManagedBridgeStatus::NOT_FOUND;
    switch (request.index) {
    case 0: {
        if (!MatchesServiceArguments(request, arguments, 0u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.audio.Play();
        break;
    }
    case 1: {
        if (!MatchesServiceArguments(request, arguments, 0u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.audio.Stop();
        break;
    }
    case 2: {
        if (!MatchesServiceArguments(request, arguments, 0u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.audio.Pause();
        break;
    }
    case 3: {
        if (!MatchesServiceArguments(request, arguments, 0u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.audio.Resume();
        break;
    }
    case 4: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.integer = static_cast<int64_t>(view.audio.IsPlaying());
        break;
    }
    case 5: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.values = {view.audio.GetVolume()};
        break;
    }
    case 6: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.values = {view.audio.GetPitch()};
        break;
    }
    case 7: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.integer = static_cast<int64_t>(view.audio.GetLoop());
        break;
    }
    case 8: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.integer = static_cast<int64_t>(view.audio.IsPaused());
        break;
    }
    case 9: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.values = {view.audio.GetSpatialBlend()};
        break;
    }
    case 10: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.text.emplace_back(view.audio.GetBus());
        break;
    }
    case 11: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.integer = static_cast<int64_t>(view.audio.IsBGMPlaying());
        break;
    }
    case 12: {
        if (!MatchesServiceArguments(request, arguments, 0u, 0, 1)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.audio.Play(t[0]);
        break;
    }
    case 13: {
        if (!MatchesServiceArguments(request, arguments, 0u, 0, 1)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.integer = static_cast<int64_t>(view.audio.Preload(t[0]));
        break;
    }
    case 14: {
        if (!MatchesServiceArguments(request, arguments, 0u, 1, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.audio.SetVolume(p[0]);
        break;
    }
    case 15: {
        if (request.flags != 0u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (integer != 0 && integer != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.audio.SetLoop(integer != 0);
        break;
    }
    case 16: {
        if (!MatchesServiceArguments(request, arguments, 0u, 1, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.audio.SetPitch(p[0]);
        break;
    }
    case 17: {
        if (!MatchesServiceArguments(request, arguments, 0u, 1, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.audio.SetSpatialBlend(p[0]);
        break;
    }
    case 18: {
        if (!MatchesServiceArguments(request, arguments, 0u, 3, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.audio.Set3DDistances(p[0], p[1], p[2]);
        break;
    }
    case 19: {
        if (!MatchesServiceArguments(request, arguments, 0u, 1, 1)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.audio.PlayOneShot(t[0], p[0]);
        break;
    }
    case 20: {
        if (!MatchesServiceArguments(request, arguments, 0u, 4, 1)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.audio.PlayAtPoint(t[0], math::Vector3{p[0], p[1], p[2]}, p[3]);
        break;
    }
    case 21: {
        if (request.flags != 0u || p.size() != 1 || t.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (integer != 0 && integer != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.audio.PlayBGM(t[0], integer != 0, p[0]);
        break;
    }
    case 22: {
        if (!MatchesServiceArguments(request, arguments, 0u, 1, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.audio.StopBGM(p[0]);
        break;
    }
    case 23: {
        if (!MatchesServiceArguments(request, arguments, 0u, 2, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.audio.FadeTo(p[0], p[1]);
        break;
    }
    case 24: {
        if (!MatchesServiceArguments(request, arguments, 0u, 1, 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.audio.FadeOutAndStop(p[0]);
        break;
    }
    case 25: {
        if (!MatchesServiceArguments(request, arguments, 0u, 0, 1)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.audio.SetBus(t[0]);
        break;
    }
    case 26: {
        if (!MatchesServiceArguments(request, arguments, 0u, 1, 1)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.audio.SetBusVolume(t[0], p[0]);
        break;
    }
    case 27: {
        if (!MatchesServiceArguments(request, arguments, 1u, 0, 1)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.values = {view.audio.GetBusVolume(t[0])};
        break;
    }
    case 28: {
        if (!MatchesServiceArguments(request, arguments, 0u, 1, 1)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.audio.SetBusLowPass(t[0], p[0]);
        break;
    }
    default: return ManagedBridgeStatus::UNSUPPORTED;
    }
    return ManagedBridgeStatus::OK;
}

void ManagedServiceAudio::CancelOwner(uint64_t ownerId, Script* caller, Script& view)
{
    for (auto it = m_clips.begin(); it != m_clips.end();) {
        if (it->second.owner != ownerId) { ++it; continue; }
        view.audio.ReleaseClip(it->second.clip); it = m_clips.erase(it);
    }
    for (auto it = m_loops.begin(); it != m_loops.end();) {
        if (it->second.owner != ownerId) { ++it; continue; }
        if (caller) caller->audio.ReleaseLoop(it->second.loop);
        it = m_loops.erase(it);
    }
}

void ManagedServiceAudio::RetireGeneration(uint64_t generation, Script& view)
{
    for (auto it = m_clips.begin(); it != m_clips.end();) {
        if (it->second.generation != generation) { ++it; continue; }
        view.audio.ReleaseClip(it->second.clip); it = m_clips.erase(it);
    }
    std::erase_if(m_loops, [generation](const auto& entry) { return entry.second.generation == generation; });
}

}
