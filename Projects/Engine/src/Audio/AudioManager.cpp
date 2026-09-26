/// @file    AudioManager.cpp
/// @brief   クリップの読み込み・合成・寿命管理とバス操作の実装。
/// @author  Hasegawa Jin
/// @date    2025-01-01
#include "Engine/Audio/AudioManager.hpp"
#include "Engine/Audio/Synth.hpp"
#include "Engine/Core/Logger.hpp"
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/SynthAsset.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

namespace fbzz::audio
{
namespace {

/// @note 生成クリップの合計サイズ上限。超えても警告だけで生成は続ける。
/// @note 止めると「音が急に鳴らなくなる」という原因の追いにくい形で現れる。
constexpr size_t kGeneratedBudgetBytes = 32u * 1024u * 1024u;

/// @note StopVoice の立ち下がり。「プツッ」を消せる最小限で、遅れは耳に分からない。
constexpr float kStopFadeSeconds = 0.02f;

/// @note BGM はスティールしない。ループ音なので元から除外されるが、意図を数値でも残す。
constexpr int kBgmPriority = 100;

/// @name RIFF/WAV ヘッダ構造体 (リトルエンディアン前提)
#pragma pack(push, 1)
struct RiffHeader  { char id[4]; uint32_t size; char type[4]; };
struct ChunkHeader { char id[4]; uint32_t size; };
struct FmtChunk
{
    uint16_t audioFormat;
    uint16_t channels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
};
#pragma pack(pop)

std::string LowerExtension(const std::string& path)
{
    const auto dot = path.rfind('.');
    if (dot == std::string::npos) return {};
    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

/// @note 論理パス ("Assets/...") とリネーム後の実体を AssetManager の規則で解決する。
/// @note 生のまま ifstream へ渡すと、カレントディレクトリ次第で読めない。
std::string ResolveClipPath(const std::string& path)
{
    const std::string resolved = asset::AssetManager::ResolveAssetPath(path);
    if (!resolved.empty() && util::FileSystem::Exists(resolved)) return resolved;
    return path;
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

} /// @note namespace

AudioManager::AudioManager(IAudioDevice& device)
    : m_device(device)
{
}

bool AudioManager::Init()
{
    if (!m_device.Init()) return false;

    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (FAILED(hr)) {
        FBZZ_LOG_ERROR("AudioManager: MFStartup failed (hr=0x%08X)", hr);
        m_device.Shutdown();
        return false;
    }

    m_busLayout = DefaultBusLayout();
    return true;
}

void AudioManager::Shutdown()
{
    StopAllVoices();
    m_clips.clear();
    m_pathToClip.clear();
    m_specToClip.clear();
    m_generatedBytes = 0;
    MFShutdown();
    m_device.Shutdown();
}

void AudioManager::Update(float dt)
{
    dt = (std::max)(dt, 0.0f);

    /// @note 停止はクリップの解放まで連鎖して m_voices を書き換えるので、走査中には畳めない。
    std::vector<uint32_t> fadedOut;
    std::vector<uint32_t> finished;
    for (auto& [voiceId, state] : m_voices) {
        if (state.duration > 0.0f) {
            state.elapsed += dt;
            const float t = (std::min)(state.elapsed / state.duration, 1.0f);
            state.fadeGain = state.fadeFrom + (state.fadeTo - state.fadeFrom) * t;
            ApplyVoiceGain(voiceId, state);
            if (t >= 1.0f) {
                state.duration = 0.0f;
                if (state.stopAtEnd) { fadedOut.push_back(voiceId); continue; }
            }
        }
        if (!m_device.IsPlaying(voiceId)) finished.push_back(voiceId);
    }

    for (uint32_t voiceId : fadedOut) StopVoiceImmediate(voiceId);
    /// @note IsPlaying が false の時点でデバイス側は破棄済み。参照を戻すだけでよい。
    for (uint32_t voiceId : finished) ForgetVoice(voiceId);
}

void AudioManager::SetVoiceLimit(size_t limit)
{
    m_voiceLimit = limit < 4 ? 4 : limit;
}

bool AudioManager::MakeRoomForVoice(int priority)
{
    if (m_voices.size() < m_voiceLimit) return true;

    uint32_t victim         = 0;
    int      victimPriority = 0;
    uint64_t victimSequence = 0;
    for (const auto& [voiceId, state] : m_voices) {
        /// @note ループ音は「鳴り続けること」が役目なので奪わない。
        if (state.loop || voiceId == m_bgmVoiceId) continue;
        if (victim == 0 || state.priority < victimPriority
            || (state.priority == victimPriority && state.sequence < victimSequence)) {
            victim         = voiceId;
            victimPriority = state.priority;
            victimSequence = state.sequence;
        }
    }

    /// @note 奪える相手が居ない、または相手の方が大事なら、新しい音の方を捨てる。
    /// @note 上限に達するのは同種の音が湧いた場面で、そこで大事な音を消す方が痛い。
    if (victim == 0 || victimPriority > priority) return false;

    StopVoiceImmediate(victim);
    return true;
}

void AudioManager::ApplyVoiceGain(uint32_t voiceId, const VoiceState& state)
{
    m_device.SetVolume(voiceId, state.volume * state.fadeGain);
}

/// @name バス

void AudioManager::ApplyBusLayout(const std::vector<BusDesc>& buses)
{
    StopAllVoices();
    m_busLayout = NormalizeBusLayout(buses);
    if (!m_device.RebuildBuses(m_busLayout.data(), m_busLayout.size()))
        FBZZ_LOG_ERROR("AudioManager: bus layout rebuild failed (%zu buses)", m_busLayout.size());

    /// @note submix ごと作り直したので残響は初期状態。次の更新を素通りさせない。
    m_reverbWet = -1.0f;
}

void AudioManager::SetEnvironmentReverb(float wet, float decaySeconds, float highFrequencyRatio)
{
    wet = std::clamp(wet, 0.0f, 1.0f);
    if (std::fabs(wet - m_reverbWet) < 0.001f
        && std::fabs(decaySeconds - m_reverbDecay) < 0.001f
        && std::fabs(highFrequencyRatio - m_reverbHfRatio) < 0.001f) return;

    m_reverbWet     = wet;
    m_reverbDecay   = decaySeconds;
    m_reverbHfRatio = highFrequencyRatio;
    for (size_t i = 0; i < m_busLayout.size(); ++i) {
        if (!m_busLayout[i].reverb) continue;
        m_device.SetBusReverb(static_cast<BusIndex>(i), wet, decaySeconds, highFrequencyRatio);
    }
}

BusIndex AudioManager::FindBus(std::string_view name) const
{
    if (name.empty()) return kMasterBus;
    for (size_t i = 0; i < m_busLayout.size(); ++i)
        if (EqualsIgnoreCase(m_busLayout[i].name, name)) return static_cast<BusIndex>(i);
    return kMasterBus;
}

void AudioManager::SetBusVolume(std::string_view name, float volume)
{
    const BusIndex bus = FindBus(name);
    if (bus >= m_busLayout.size()) return;
    m_busLayout[bus].volume = (std::max)(volume, 0.0f);
    m_device.SetBusVolume(bus, m_busLayout[bus].volume);
}

float AudioManager::GetBusVolume(std::string_view name) const
{
    const BusIndex bus = FindBus(name);
    return bus < m_busLayout.size() ? m_busLayout[bus].volume : 1.0f;
}

void AudioManager::SetBusLowPass(std::string_view name, float normalizedCutoff)
{
    const BusIndex bus = FindBus(name);
    if (bus >= m_busLayout.size()) return;
    m_busLayout[bus].lowPassCutoff = std::clamp(normalizedCutoff, 0.0f, 1.0f);
    m_device.SetBusLowPass(bus, m_busLayout[bus].lowPassCutoff);
}

/// @name クリップ

AudioManager::ClipId AudioManager::InternClip(const WaveFormat& fmt,
                                              std::vector<uint8_t>&& pcm,
                                              bool persistent, uint64_t specHash)
{
    const ClipId id = m_nextClipId++;
    ClipEntry entry;
    entry.fmt        = fmt;
    entry.pcm        = std::move(pcm);
    entry.persistent = persistent;
    entry.specHash   = specHash;
    entry.refCount   = persistent ? 1 : 0;
    if (!persistent) m_generatedBytes += entry.pcm.size();
    m_clips.emplace(id, std::move(entry));
    return id;
}

AudioManager::ClipId AudioManager::AcquireClip(const std::string& path)
{
    if (path.empty()) return 0;
    if (const auto it = m_pathToClip.find(path); it != m_pathToClip.end()) return it->second;

    WaveFormat fmt{};
    std::vector<uint8_t> pcm;
    if (!LoadClipData(path, fmt, pcm)) {
        FBZZ_LOG_ERROR("AudioManager: load failed: %s", path.c_str());
        return 0;
    }

    const ClipId id = InternClip(fmt, std::move(pcm), true, 0);
    m_pathToClip[path] = id;
    return id;
}

AudioManager::ClipId AudioManager::AcquireGeneratedClip(const SynthSpec& spec)
{
    const uint64_t hash = HashSpec(spec);
    if (const auto it = m_specToClip.find(hash); it != m_specToClip.end()) {
        if (const auto entry = m_clips.find(it->second); entry != m_clips.end()) {
            ++entry->second.refCount;
            return it->second;
        }
        m_specToClip.erase(it);
    }

    WaveFormat fmt{};
    std::vector<uint8_t> pcm = Render(spec, fmt);
    if (pcm.empty()) return 0;

    const ClipId id = InternClip(fmt, std::move(pcm), false, hash);
    m_clips[id].refCount = 1;
    m_specToClip[hash] = id;

    if (m_generatedBytes > kGeneratedBudgetBytes) {
        FBZZ_LOG_WARN("AudioManager: generated clip budget exceeded (%zu MB / %zu clips)",
                      m_generatedBytes / (1024u * 1024u), m_specToClip.size());
    }
    return id;
}

void AudioManager::AddClipRef(ClipId clip)
{
    const auto it = m_clips.find(clip);
    if (it != m_clips.end() && !it->second.persistent) ++it->second.refCount;
}

void AudioManager::ReleaseClip(ClipId clip)
{
    const auto it = m_clips.find(clip);
    if (it == m_clips.end() || it->second.persistent) return;
    if (it->second.refCount > 0) --it->second.refCount;
    DropClipIfUnused(clip);
}

void AudioManager::DropClipIfUnused(ClipId clip)
{
    const auto it = m_clips.find(clip);
    if (it == m_clips.end()) return;
    const ClipEntry& entry = it->second;
    /// @note XAudio2 は PCM をポインタで参照し続ける。鳴っている最中に捨てると解放済みメモリを鳴らす。
    if (entry.persistent || entry.refCount > 0 || entry.voiceCount > 0) return;

    m_generatedBytes -= (std::min)(m_generatedBytes, entry.pcm.size());
    if (entry.specHash != 0) m_specToClip.erase(entry.specHash);
    m_clips.erase(it);
}

/// @name 再生

uint32_t AudioManager::PlayClipVoice(ClipId clip, bool loop, BusIndex bus,
                                     const PlayParams& params)
{
    /// @note スティールはクリップの解放まで連鎖するので、m_clips のイテレーターより先に済ませる。
    if (m_clips.find(clip) == m_clips.end()) return 0;
    if (!MakeRoomForVoice(params.priority)) return 0;

    const auto it = m_clips.find(clip);
    if (it == m_clips.end() || it->second.pcm.empty()) return 0;

    ClipEntry& entry = it->second;
    const uint32_t voiceId = m_device.PlayBuffer(
        entry.pcm.data(), entry.pcm.size(), entry.fmt, loop, bus);
    if (voiceId == 0) return 0;

    ++entry.voiceCount;

    VoiceState state;
    state.clip     = clip;
    state.loop     = loop;
    state.priority = params.priority;
    state.sequence = m_nextVoiceSequence++;
    if (params.fadeInSeconds > 0.0f) {
        state.fadeGain = 0.0f;
        state.fadeFrom = 0.0f;
        state.fadeTo   = 1.0f;
        state.duration = params.fadeInSeconds;
    }
    m_voices[voiceId] = state;
    ApplyVoiceGain(voiceId, state);
    return voiceId;
}

uint32_t AudioManager::PlayVoice(const std::string& path, bool loop, BusIndex bus,
                                 const PlayParams& params)
{
    const auto resolved = ResolveClipPath(path);
    if (params.streaming && LowerExtension(resolved) != ".synth") {
        if (path.empty() || !MakeRoomForVoice(params.priority)) return 0;
        const uint32_t voiceId = m_device.PlayStream(resolved, loop, bus);
        if (voiceId == 0) return 0;
        VoiceState state;
        state.loop = loop;
        state.priority = params.priority;
        state.sequence = m_nextVoiceSequence++;
        if (params.fadeInSeconds > 0.0f) {
            state.fadeGain = state.fadeFrom = 0.0f;
            state.duration = params.fadeInSeconds;
        }
        m_voices[voiceId] = state;
        ApplyVoiceGain(voiceId, state);
        return voiceId;
    }
    const ClipId clip = AcquireClip(path);
    return clip != 0 ? PlayClipVoice(clip, loop, bus, params) : 0;
}

void AudioManager::ForgetVoice(uint32_t voiceId)
{
    const auto it = m_voices.find(voiceId);
    if (it == m_voices.end()) return;
    const ClipId clip = it->second.clip;
    m_voices.erase(it);
    if (const auto entry = m_clips.find(clip); entry != m_clips.end()) {
        if (entry->second.voiceCount > 0) --entry->second.voiceCount;
    }
    DropClipIfUnused(clip);
    if (m_bgmVoiceId == voiceId) m_bgmVoiceId = 0;
}

void AudioManager::StopVoice(uint32_t voiceId)
{
    if (voiceId == 0) return;
    if (m_bgmVoiceId == voiceId) m_bgmVoiceId = 0;
    FadeOutAndStop(voiceId, kStopFadeSeconds);
}

void AudioManager::StopVoiceImmediate(uint32_t voiceId)
{
    if (voiceId == 0) return;
    m_device.StopBuffer(voiceId);
    ForgetVoice(voiceId);
}

void AudioManager::FadeVoice(uint32_t voiceId, float toGain, float seconds)
{
    const auto it = m_voices.find(voiceId);
    if (it == m_voices.end()) return;

    VoiceState& state = it->second;
    state.fadeFrom  = state.fadeGain;
    state.fadeTo    = std::clamp(toGain, 0.0f, 1.0f);
    state.elapsed   = 0.0f;
    state.duration  = (std::max)(seconds, 0.0f);
    state.stopAtEnd = false;
    if (state.duration <= 0.0f) {
        state.fadeGain = state.fadeTo;
        ApplyVoiceGain(voiceId, state);
    }
}

void AudioManager::FadeOutAndStop(uint32_t voiceId, float seconds)
{
    if (voiceId == 0) return;
    const auto it = m_voices.find(voiceId);
    /// @note 追跡していない voice はフェードを掛ける先が無いので、そのまま畳む。
    if (it == m_voices.end()) { m_device.StopBuffer(voiceId); return; }
    if (seconds <= 0.0f) { StopVoiceImmediate(voiceId); return; }

    VoiceState& state = it->second;
    state.fadeFrom  = state.fadeGain;
    state.fadeTo    = 0.0f;
    state.elapsed   = 0.0f;
    state.duration  = seconds;
    state.stopAtEnd = true;
}

void AudioManager::PauseVoice(uint32_t voiceId)
{
    if (voiceId != 0) m_device.PauseBuffer(voiceId);
}

void AudioManager::ResumeVoice(uint32_t voiceId)
{
    if (voiceId != 0) m_device.ResumeBuffer(voiceId);
}

void AudioManager::QueuePositional(PositionalRequest request)
{
    m_positional.push_back(std::move(request));
}

std::vector<AudioManager::PositionalRequest> AudioManager::TakePositional()
{
    std::vector<PositionalRequest> taken;
    taken.swap(m_positional);
    return taken;
}

bool AudioManager::DescribeClip(ClipId clip, ClipInfo& out) const
{
    const auto it = m_clips.find(clip);
    if (it == m_clips.end() || it->second.pcm.empty()) return false;

    const ClipEntry& entry = it->second;
    out.fmt   = entry.fmt;
    out.pcm   = entry.pcm.data();
    out.bytes = entry.pcm.size();

    const uint32_t bytesPerFrame =
        static_cast<uint32_t>(entry.fmt.channels) * (entry.fmt.bitsPerSample / 8u);
    out.durationSeconds = (bytesPerFrame > 0 && entry.fmt.sampleRate > 0)
        ? static_cast<float>(entry.pcm.size() / bytesPerFrame)
              / static_cast<float>(entry.fmt.sampleRate)
        : 0.0f;
    return true;
}

void AudioManager::StopAllVoices()
{
    /// @note 引き取られなかった位置指定要求も参照を握ったままなので、ここで手放す。
    for (const PositionalRequest& request : m_positional)
        if (request.clip != 0) ReleaseClip(request.clip);
    m_positional.clear();

    m_bgmVoiceId = 0;
    for (const auto& tracked : m_voices)
        m_device.StopBuffer(tracked.first);
    m_voices.clear();

    /// @note 生成クリップは参照数によらず全部捨てる。一括停止が起きる Play→Stop と Shutdown では
    /// @note スクリプト DLL ごと作り直され、ハンドルの持ち主が消えているため。
    /// @note (ScriptAudioProxy::SynthClip のコメントと対)
    for (auto it = m_clips.begin(); it != m_clips.end();) {
        if (it->second.persistent) { it->second.voiceCount = 0; ++it; continue; }
        if (it->second.specHash != 0) m_specToClip.erase(it->second.specHash);
        m_generatedBytes -= (std::min)(m_generatedBytes, it->second.pcm.size());
        it = m_clips.erase(it);
    }
}

void AudioManager::SetVoiceVolume(uint32_t voiceId, float volume)
{
    if (voiceId == 0) return;
    volume = (std::max)(volume, 0.0f);

    const auto it = m_voices.find(voiceId);
    if (it == m_voices.end()) { m_device.SetVolume(voiceId, volume); return; }
    it->second.volume = volume;
    ApplyVoiceGain(voiceId, it->second);
}

void AudioManager::SetVoicePitch(uint32_t voiceId, float pitch)
{
    if (voiceId != 0) m_device.SetPitch(voiceId, pitch);
}

void AudioManager::SetVoicePan(uint32_t voiceId, float pan)
{
    if (voiceId != 0) m_device.SetPan(voiceId, pan);
}

void AudioManager::SetVoiceLowPass(uint32_t voiceId, float normalizedCutoff)
{
    if (voiceId != 0) m_device.SetLowPass(voiceId, normalizedCutoff);
}

void AudioManager::SetVoiceSend(uint32_t voiceId, std::string_view busName, float level)
{
    if (voiceId == 0) return;
    BusIndex bus = kInvalidBus;
    for (size_t i = 0; i < m_busLayout.size(); ++i) {
        if (EqualsIgnoreCase(m_busLayout[i].name, busName)) {
            bus = static_cast<BusIndex>(i);
            break;
        }
    }
    m_device.SetSend(voiceId, bus, std::isfinite(level) ? std::clamp(level, 0.0f, 1.0f) : 0.0f);
}

bool AudioManager::IsVoicePlaying(uint32_t voiceId)
{
    if (voiceId == 0) return false;
    if (m_device.IsPlaying(voiceId)) return true;
    ForgetVoice(voiceId);
    return false;
}

/// @name BGM / SE

void AudioManager::PlayBGM(const std::string& path, bool loop, float fadeSeconds)
{
    /// @note クロスフェード中は新旧 2 本が鳴るが、BGM スロットが指すのは常に新しい方。
    const uint32_t previous = m_bgmVoiceId;
    m_bgmVoiceId = 0;
    if (previous != 0) {
        if (fadeSeconds > 0.0f) FadeOutAndStop(previous, fadeSeconds);
        else                    StopVoiceImmediate(previous);
    }

    PlayParams params;
    params.priority      = kBgmPriority;
    params.streaming     = true;
    params.fadeInSeconds = fadeSeconds;
    m_bgmVoiceId = PlayVoice(path, loop, FindBus("BGM"), params);
}

void AudioManager::StopBGM(float fadeSeconds)
{
    if (m_bgmVoiceId == 0) return;
    const uint32_t voiceId = m_bgmVoiceId;
    m_bgmVoiceId = 0;
    if (fadeSeconds > 0.0f) FadeOutAndStop(voiceId, fadeSeconds);
    else                    StopVoiceImmediate(voiceId);
}

void AudioManager::PlaySE(const std::string& path)
{
    (void)PlayVoice(path, false, FindBus("SE"));
}

void AudioManager::SetBGMVolume(float volume) { SetBusVolume("BGM", volume); }
void AudioManager::SetSEVolume(float volume)  { SetBusVolume("SE",  volume); }
float AudioManager::GetBGMVolume() const      { return GetBusVolume("BGM"); }
float AudioManager::GetSEVolume() const       { return GetBusVolume("SE"); }

/// @name 読み込み

bool AudioManager::LoadClipData(const std::string& path,
                                WaveFormat& outFmt, std::vector<uint8_t>& outPcm)
{
    const std::string ext      = LowerExtension(path);
    const std::string resolved = ResolveClipPath(path);

    if (ext == ".synth") return LoadSynth(resolved, outFmt, outPcm);
    if (ext == ".wav")   return LoadWav(resolved, outFmt, outPcm);
    return LoadWithMediaFoundation(resolved, outFmt, outPcm);
}

bool AudioManager::LoadSynth(const std::string& path,
                             WaveFormat& outFmt, std::vector<uint8_t>& outPcm)
{
    asset::SynthAsset synth;
    if (!asset::LoadSynthAssetFromFile(path, synth)) return false;
    outPcm = Render(synth.spec, outFmt);
    return !outPcm.empty();
}

bool AudioManager::LoadWav(const std::string& path,
                           WaveFormat& outFmt, std::vector<uint8_t>& outPcm)
{
    std::ifstream file(util::FileSystem::PathFromUtf8(path), std::ios::binary);
    if (!file) return false;

    RiffHeader riff{};
    file.read(static_cast<char*>(static_cast<void*>(&riff)), sizeof(riff));
    if (std::strncmp(riff.id,   "RIFF", 4) != 0 ||
        std::strncmp(riff.type, "WAVE", 4) != 0)
        return false;

    bool hasFmt  = false;
    bool hasData = false;
    while (file) {
        ChunkHeader chunk{};
        file.read(static_cast<char*>(static_cast<void*>(&chunk)), sizeof(chunk));
        if (!file) break;

        if (std::strncmp(chunk.id, "fmt ", 4) == 0) {
            FmtChunk fmt{};
            file.read(static_cast<char*>(static_cast<void*>(&fmt)), sizeof(fmt));
            if (chunk.size > sizeof(fmt))
                file.seekg(chunk.size - sizeof(fmt), std::ios::cur);
            /// @note PCM のみ対応
            if (fmt.audioFormat != 1) return false;
            outFmt.sampleRate    = fmt.sampleRate;
            outFmt.channels      = fmt.channels;
            outFmt.bitsPerSample = fmt.bitsPerSample;
            hasFmt = true;
        } else if (std::strncmp(chunk.id, "data", 4) == 0) {
            outPcm.resize(chunk.size);
            file.read(static_cast<char*>(static_cast<void*>(outPcm.data())), chunk.size);
            hasData = true;
        } else {
            file.seekg(chunk.size, std::ios::cur);
        }
    }
    return hasFmt && hasData;
}

bool AudioManager::LoadWithMediaFoundation(const std::string& path,
                                           WaveFormat& outFmt, std::vector<uint8_t>& outPcm)
{
    /// @note path は UTF-8。日本語フォルダへ移した配布版でも MF が正しく受け取れるようにする。
    const std::wstring wpath = fbzz::util::StringUtils::ToWide(path);

    Microsoft::WRL::ComPtr<IMFSourceReader> reader;
    HRESULT hr = MFCreateSourceReaderFromURL(wpath.c_str(), nullptr, &reader);
    if (FAILED(hr)) {
        FBZZ_LOG_ERROR("AudioManager: MFCreateSourceReaderFromURL failed: %s", path.c_str());
        return false;
    }

    /// @note デコード出力を PCM に固定
    Microsoft::WRL::ComPtr<IMFMediaType> pcmType;
    MFCreateMediaType(&pcmType);
    pcmType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    pcmType->SetGUID(MF_MT_SUBTYPE,    MFAudioFormat_PCM);
    hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, pcmType.Get());
    if (FAILED(hr)) return false;

    Microsoft::WRL::ComPtr<IMFMediaType> outputType;
    hr = reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &outputType);
    if (FAILED(hr)) return false;

    UINT32 sampleRate = 0, channels = 0, bitsPerSample = 0;
    outputType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sampleRate);
    outputType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS,       &channels);
    outputType->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,    &bitsPerSample);
    outFmt.sampleRate    = sampleRate;
    outFmt.channels      = static_cast<uint16_t>(channels);
    outFmt.bitsPerSample = static_cast<uint16_t>(bitsPerSample);

    while (true) {
        DWORD flags = 0;
        Microsoft::WRL::ComPtr<IMFSample> sample;
        hr = reader->ReadSample(
            MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0,
            nullptr, &flags, nullptr, &sample);
        if (FAILED(hr)) break;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (!sample) continue;

        Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
        sample->ConvertToContiguousBuffer(&buffer);
        BYTE*  data   = nullptr;
        DWORD  length = 0;
        buffer->Lock(&data, nullptr, &length);
        const size_t offset = outPcm.size();
        outPcm.resize(offset + length);
        std::memcpy(outPcm.data() + offset, data, length);
        buffer->Unlock();
    }
    return !outPcm.empty();
}

} /// @note namespace fbzz::audio
