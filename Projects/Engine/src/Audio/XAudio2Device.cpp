/// @file    XAudio2Device.cpp
/// @brief   XAudio2 バックエンド実装。
/// @author  Hasegawa Jin
/// @date    2025-01-01
#include "Engine/Audio/XAudio2Device.hpp"
#include "XAudio2Stream.hpp"
#include "Engine/Core/Logger.hpp"
#include <xaudio2fx.h>
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cmath>
#include <string>

#pragma comment(lib, "xaudio2.lib")

namespace fbzz::audio
{
namespace {

/// @note バスはすべてステレオ。SetPan は左右 2ch にしか書かず、XAudio2 の残響も入力 1-2ch しか
/// @note 受けないので、マスターに合わせると 5.1 環境でだけ ReverbZone が死ぬ。展開は XAudio2 任せ。
constexpr uint32_t kBusChannels = 2;

bool EqualsIgnoreCase(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

} /// @note namespace

XAudio2Device::XAudio2Device() = default;
XAudio2Device::~XAudio2Device() { Shutdown(); }

bool XAudio2Device::Init()
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (hr == RPC_E_CHANGED_MODE) {
        /// @note Window が OLE D&D のためこのスレッドを STA で先に初期化していると MTA 要求が弾かれる。
        /// @note XAudio2 は STA でも動くので、COM を「所有しない」形で続行する (CoUninitialize しない)。
        m_comInitialized = false;
    } else if (FAILED(hr) && hr != S_FALSE) {
        /// @note S_FALSE はすでに初期化済みなので問題なし。それ以外の失敗のみエラー扱いにする。
        FBZZ_LOG_ERROR("XAudio2Device: CoInitializeEx failed");
        return false;
    } else {
        /// @note S_OK / S_FALSE: このスレッドの COM 参照を保持し、Shutdown で対に CoUninitialize する。
        m_comInitialized = true;
    }

    UINT32 flags = 0;
#if defined(_DEBUG)
    flags |= XAUDIO2_DEBUG_ENGINE;
#endif
    hr = XAudio2Create(m_xaudio2.GetAddressOf(), flags);
    if (FAILED(hr)) {
        FBZZ_LOG_ERROR("XAudio2Device: XAudio2Create failed");
        Shutdown();
        return false;
    }

    hr = m_xaudio2->CreateMasteringVoice(&m_masterVoice);
    if (FAILED(hr)) {
        FBZZ_LOG_ERROR("XAudio2Device: CreateMasteringVoice failed");
        Shutdown();
        return false;
    }

    XAUDIO2_VOICE_DETAILS details{};
    m_masterVoice->GetVoiceDetails(&details);
    m_masterChannels   = details.InputChannels;
    m_masterSampleRate = details.InputSampleRate;

    /// @note AudioManager が ProjectSettings の構成を流し込むまでの間も再生できるようにしておく。
    const std::vector<BusDesc> defaults = DefaultBusLayout();
    return RebuildBuses(defaults.data(), defaults.size());
}

void XAudio2Device::Shutdown()
{
    DestroyAllSourceVoices();
    DestroyBuses();

    if (m_masterVoice) {
        m_masterVoice->DestroyVoice();
        m_masterVoice = nullptr;
    }
    m_xaudio2.Reset();

    if (m_comInitialized) {
        CoUninitialize();
        m_comInitialized = false;
    }
}

void XAudio2Device::DestroyAllSourceVoices()
{
    for (auto& [id, entry] : m_voices) {
        if (entry.stream) { entry.stream.reset(); continue; }
        if (entry.voice) {
            entry.voice->Stop();
            entry.voice->DestroyVoice();
        }
    }
    m_voices.clear();

    /// @note プールの voice は «作ったときの送り先» を握る。ここが呼ばれるのはバスを作り直す
    /// @note ときと終了時で、どちらも直後にその送り先が消えるため、取り置きも一緒に畳む。
    for (auto& [key, pool] : m_voicePool) {
        for (IXAudio2SourceVoice* voice : pool)
            if (voice) voice->DestroyVoice();
    }
    m_voicePool.clear();
}

void XAudio2Device::DestroyBuses()
{
    /// @note 子が親へ送っているため、末尾 (葉) から壊す。
    for (auto it = m_buses.rbegin(); it != m_buses.rend(); ++it) {
        if (it->voice) it->voice->DestroyVoice();
    }
    m_buses.clear();
}

IXAudio2Voice* XAudio2Device::BusVoice(BusIndex bus) const
{
    if (bus < m_buses.size() && m_buses[bus].voice) return m_buses[bus].voice;
    if (!m_buses.empty() && m_buses[kMasterBus].voice) return m_buses[kMasterBus].voice;
    return nullptr;
}

bool XAudio2Device::RebuildBuses(const BusDesc* descs, size_t count)
{
    if (!m_xaudio2 || !m_masterVoice || !descs || count == 0) return false;

    /// @note 送り先が消えるため、既存 voice はすべて畳んでから作り直す。
    DestroyAllSourceVoices();
    DestroyBuses();

    /// @note descs は親が子より前に並んでいる前提 (NormalizeBusLayout が保証)。
    std::vector<size_t>   parentIndex(count, 0);
    std::vector<uint32_t> depth(count, 0);
    for (size_t i = 1; i < count; ++i) {
        parentIndex[i] = 0;
        for (size_t p = 0; p < i; ++p) {
            if (EqualsIgnoreCase(descs[i].parent, descs[p].name)) {
                parentIndex[i] = p;
                break;
            }
        }
        depth[i] = depth[parentIndex[i]] + 1;
    }
    const uint32_t maxDepth = *std::max_element(depth.begin(), depth.end());

    m_buses.assign(count, BusEntry{});
    for (size_t i = 0; i < count; ++i) {
        XAUDIO2_SEND_DESCRIPTOR send{};
        XAUDIO2_VOICE_SENDS    sends{};
        const XAUDIO2_VOICE_SENDS* sendList = nullptr;
        if (i != 0) {
            send  = XAUDIO2_SEND_DESCRIPTOR{ 0, m_buses[parentIndex[i]].voice };
            sends = XAUDIO2_VOICE_SENDS{ 1, &send };
            sendList = &sends;
        }

        /// @note 残響 XAPO は submix の生成時にしか差し込めない。止めた状態で載せ、ゾーンで回し始める。
        IUnknown* reverbApo = nullptr;
        XAUDIO2_EFFECT_DESCRIPTOR effect{};
        XAUDIO2_EFFECT_CHAIN      chain{};
        const XAUDIO2_EFFECT_CHAIN* chainPtr = nullptr;
        if (descs[i].reverb && SUCCEEDED(XAudio2CreateReverb(&reverbApo))) {
            effect.pEffect        = reverbApo;
            effect.InitialState   = FALSE;
            effect.OutputChannels = kBusChannels;
            chain    = XAUDIO2_EFFECT_CHAIN{ 1, &effect };
            chainPtr = &chain;
        }

        /// @note XAudio2 は ProcessingStage の小さい submix から処理する。子は親より
        /// @note 先に処理されなければならないので、深いバスほど小さい stage を与える。
        const UINT32 stage = maxDepth - depth[i];
        HRESULT hr = m_xaudio2->CreateSubmixVoice(
            &m_buses[i].voice, kBusChannels, m_masterSampleRate,
            XAUDIO2_VOICE_USEFILTER, stage, sendList, chainPtr);
        if (FAILED(hr) && chainPtr) {
            /// @note 残響を諦めれば作れる。無音のバスにするよりは素通しで残す。
            FBZZ_LOG_WARN("XAudio2Device: reverb unavailable on bus [%s], falling back to dry",
                          descs[i].name.c_str());
            chainPtr = nullptr;
            hr = m_xaudio2->CreateSubmixVoice(
                &m_buses[i].voice, kBusChannels, m_masterSampleRate,
                XAUDIO2_VOICE_USEFILTER, stage, sendList, nullptr);
        }
        /// @note submix が AddRef 済みなので、こちらの参照は返す。
        if (reverbApo) reverbApo->Release();

        if (FAILED(hr)) {
            FBZZ_LOG_ERROR("XAudio2Device: CreateSubmixVoice failed for bus [%s]",
                           descs[i].name.c_str());
            DestroyBuses();
            return false;
        }
        m_buses[i].hasReverb = chainPtr != nullptr;

        SetBusVolume(static_cast<BusIndex>(i), descs[i].volume);
        SetBusLowPass(static_cast<BusIndex>(i), descs[i].lowPassCutoff);
    }
    return true;
}

void XAudio2Device::SetBusVolume(BusIndex bus, float volume)
{
    if (bus >= m_buses.size() || !m_buses[bus].voice) return;
    m_buses[bus].voice->SetVolume((std::max)(volume, 0.0f));
}

void XAudio2Device::SetBusLowPass(BusIndex bus, float normalizedCutoff)
{
    if (bus >= m_buses.size() || !m_buses[bus].voice) return;
    normalizedCutoff = (std::max)(0.001f, (std::min)(normalizedCutoff, 1.0f));
    XAUDIO2_FILTER_PARAMETERS filter{};
    filter.Type      = LowPassFilter;
    filter.Frequency = 2.0f * std::sin(3.14159265358979323846f * normalizedCutoff / 6.0f);
    filter.OneOverQ  = 1.0f;
    m_buses[bus].voice->SetFilterParameters(&filter);
}

void XAudio2Device::SetBusReverb(BusIndex bus, float wet,
                                 float decaySeconds, float highFrequencyRatio)
{
    if (bus >= m_buses.size()) return;
    BusEntry& entry = m_buses[bus];
    if (!entry.voice || !entry.hasReverb) return;

    wet = (std::max)(0.0f, (std::min)(wet, 1.0f));
    const bool wantEnabled = wet > 0.0f;
    if (wantEnabled != entry.reverbEnabled) {
        if (wantEnabled) entry.voice->EnableEffect(0);
        else             entry.voice->DisableEffect(0);
        entry.reverbEnabled = wantEnabled;
    }
    if (!wantEnabled) return;

    /// @note I3DL2 の一般的な部屋を土台に、ゾーンが持つ 3 つだけ差し替える。
    /// @note ネイティブ値を直に組むと反射遅延や密度まで決めることになり、対応が付かない。
    XAUDIO2FX_REVERB_I3DL2_PARAMETERS i3dl2 = XAUDIO2FX_I3DL2_PRESET_GENERIC;
    i3dl2.WetDryMix    = wet * 100.0f;
    i3dl2.DecayTime    = (std::max)(decaySeconds, 0.1f);
    i3dl2.DecayHFRatio = (std::max)(0.1f, (std::min)(highFrequencyRatio, 2.0f));

    XAUDIO2FX_REVERB_PARAMETERS native{};
    ReverbConvertI3DL2ToNative(&i3dl2, &native, FALSE);
    entry.voice->SetEffectParameters(0, &native, sizeof(native));
}

uint32_t XAudio2Device::PlayBuffer(
    const void* pcmData, size_t bytes,
    const WaveFormat& fmt, bool loop, BusIndex bus)
{
    assert(pcmData && bytes > 0);
    PurgeFinishedVoices();

    WAVEFORMATEX wfx{};
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = fmt.channels;
    wfx.nSamplesPerSec  = fmt.sampleRate;
    wfx.wBitsPerSample  = fmt.bitsPerSample;
    wfx.nBlockAlign     = (wfx.nChannels * wfx.wBitsPerSample) / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    IXAudio2Voice* destination = BusVoice(bus);
    XAUDIO2_SEND_DESCRIPTOR send{ 0, destination };
    XAUDIO2_VOICE_SENDS    sends{ 1, &send };

    /// @note 使い終わった voice を取っておいて回す。CreateSourceVoice/DestroyVoice は
    /// @note オーディオ処理の区切りを待つ呼び出しでゲームスレッドが止まるため、毎回作ると
    /// @note 音が鳴るたびに前の音の後始末がまとまって数ミリ秒のスパイクになる。
    const VoiceKey key{ fmt.channels, fmt.sampleRate, fmt.bitsPerSample, destination };
    IXAudio2SourceVoice* voice = TakePooledVoice(key);
    if (!voice) {
        HRESULT created = m_xaudio2->CreateSourceVoice(
            &voice, &wfx, XAUDIO2_VOICE_USEFILTER, XAUDIO2_MAX_FREQ_RATIO,
            nullptr, destination ? &sends : nullptr);
        if (FAILED(created)) return 0;
    }

    HRESULT hr = S_OK;
    XAUDIO2_BUFFER buf{};
    buf.AudioBytes = static_cast<UINT32>(bytes);
    buf.pAudioData = static_cast<const BYTE*>(pcmData);
    buf.Flags      = XAUDIO2_END_OF_STREAM;
    if (loop) buf.LoopCount = XAUDIO2_LOOP_INFINITE;

    hr = voice->SubmitSourceBuffer(&buf);
    if (FAILED(hr)) {
        voice->DestroyVoice();
        return 0;
    }
    voice->Start();

    uint32_t destinationChannels = m_masterChannels;
    if (destination) {
        XAUDIO2_VOICE_DETAILS details{};
        destination->GetVoiceDetails(&details);
        destinationChannels = details.InputChannels;
    }

    const uint32_t id = m_nextId++;
    auto& entry = m_voices[id];
    entry.voice = voice;
    entry.loop = loop;
    entry.channels = fmt.channels;
    entry.destination = destination;
    entry.destinationChannels = destinationChannels;
    entry.key = key;
    entry.mainMatrix.resize(fmt.channels * destinationChannels);
    voice->GetOutputMatrix(destination, fmt.channels, destinationChannels, entry.mainMatrix.data());
    return id;
}

uint32_t XAudio2Device::PlayStream(const std::string& path, bool loop, BusIndex bus)
{
    if (!m_xaudio2) return 0;
    auto stream = std::make_unique<XAudio2Stream>();
    IXAudio2Voice* destination = BusVoice(bus);
    if (!stream->Start(*m_xaudio2.Get(), destination, path, loop)) return 0;
    const uint32_t id = m_nextId++;
    auto& entry = m_voices[id];
    entry.voice = stream->Voice();
    entry.loop = loop;
    entry.channels = stream->Format().channels;
    entry.destination = destination;
    XAUDIO2_VOICE_DETAILS details{};
    destination->GetVoiceDetails(&details);
    entry.destinationChannels = details.InputChannels;
    entry.mainMatrix.resize(entry.channels * entry.destinationChannels);
    entry.voice->GetOutputMatrix(destination, entry.channels, entry.destinationChannels, entry.mainMatrix.data());
    entry.stream = std::move(stream);
    return id;
}

void XAudio2Device::StopBuffer(uint32_t voiceId)
{
    auto it = m_voices.find(voiceId);
    if (it == m_voices.end()) return;
    if (!it->second.stream) RecycleVoice(it->second);
    m_voices.erase(it);
}

void XAudio2Device::PauseBuffer(uint32_t voiceId)
{
    auto it = m_voices.find(voiceId);
    if (it == m_voices.end()) return;
    /// @note FlushSourceBuffers を呼ぶとキューが空になり再生位置も失われる。
    /// @note Stop() だけならバッファを保持したままなので Start() で続きから鳴る。
    it->second.voice->Stop();
}

void XAudio2Device::ResumeBuffer(uint32_t voiceId)
{
    auto it = m_voices.find(voiceId);
    if (it == m_voices.end()) return;
    it->second.voice->Start();
}

void XAudio2Device::SetVolume(uint32_t voiceId, float volume)
{
    auto it = m_voices.find(voiceId);
    if (it == m_voices.end()) return;
    it->second.voice->SetVolume(volume);
}

void XAudio2Device::SetPitch(uint32_t voiceId, float pitch)
{
    auto it = m_voices.find(voiceId);
    if (it == m_voices.end()) return;
    /// @note 許容範囲外は HRESULT 失敗になるので、公開 API 境界で丸める。
    pitch = (std::max)(XAUDIO2_MIN_FREQ_RATIO, (std::min)(pitch, XAUDIO2_MAX_FREQ_RATIO));
    it->second.voice->SetFrequencyRatio(pitch);
}

void XAudio2Device::SetPan(uint32_t voiceId, float pan)
{
    const auto it = m_voices.find(voiceId);
    if (it == m_voices.end()) return;
    it->second.pan = std::isfinite(pan) ? std::clamp(pan, -1.0f, 1.0f) : 0.0f;
    ApplyOutputMatrices(it->second);
}

void XAudio2Device::SetSend(uint32_t voiceId, BusIndex bus, float level)
{
    const auto it = m_voices.find(voiceId);
    if (it == m_voices.end()) return;
    auto& entry = it->second;
    level = std::isfinite(level) ? std::clamp(level, 0.0f, 1.0f) : 0.0f;
    IXAudio2Voice* destination = bus < m_buses.size() && level > 0.0f ? BusVoice(bus) : nullptr;
    if (entry.sendDestination == destination && entry.sendLevel == level) return;
    if (entry.sendDestination != destination) {
        XAUDIO2_SEND_DESCRIPTOR descriptors[2]{{0, entry.destination}, {0, destination}};
        const bool separate = destination && destination != entry.destination;
        XAUDIO2_VOICE_SENDS sends{separate ? 2u : 1u, descriptors};
        /// @see https://learn.microsoft.com/en-us/windows/win32/api/xaudio2/nf-xaudio2-ixaudio2voice-setoutputvoices XAudio2 の複数出力。
        if (FAILED(entry.voice->SetOutputVoices(&sends))) {
            FBZZ_LOG_ERROR("XAudio2Device: auxiliary send routing failed");
            return;
        }
        entry.sendMatrix.clear();
        if (separate) {
            XAUDIO2_VOICE_DETAILS details{};
            destination->GetVoiceDetails(&details);
            entry.sendMatrix.resize(entry.channels * details.InputChannels);
            entry.voice->GetOutputMatrix(destination, entry.channels, details.InputChannels, entry.sendMatrix.data());
        }
        entry.sendDestination = destination;
    }
    entry.sendLevel = level;
    ApplyOutputMatrices(entry);
}

void XAudio2Device::ApplyOutputMatrices(VoiceEntry& entry)
{
    const auto apply = [&](IXAudio2Voice* destination, const std::vector<float>& base, float gain) {
        if (!destination || base.empty()) return;
        const auto channels = static_cast<uint32_t>(base.size() / entry.channels);
        auto matrix = base;
        /// @note モノラルは主出力と補助出力へ同じ等電力パンを掛ける。多チャンネルは既定の変換行列を保つ。
        if (entry.channels == 1 && channels >= 2) {
            std::fill(matrix.begin(), matrix.end(), 0.0f);
            matrix[0] = std::sqrt(0.5f * (1.0f - entry.pan));
            matrix[1] = std::sqrt(0.5f * (1.0f + entry.pan));
        }
        for (auto& coefficient : matrix) coefficient *= gain;
        /// @see https://learn.microsoft.com/en-us/windows/win32/api/xaudio2/nf-xaudio2-ixaudio2voice-setoutputmatrix 送り先ごとのゲイン行列。
        if (FAILED(entry.voice->SetOutputMatrix(destination, entry.channels, channels, matrix.data())))
            FBZZ_LOG_ERROR("XAudio2Device: output matrix update failed");
    };
    apply(entry.destination, entry.mainMatrix,
          entry.sendDestination == entry.destination ? 1.0f + entry.sendLevel : 1.0f);
    if (entry.sendDestination != entry.destination)
        apply(entry.sendDestination, entry.sendMatrix, entry.sendLevel);
}

void XAudio2Device::SetLowPass(uint32_t voiceId, float normalizedCutoff)
{
    auto it = m_voices.find(voiceId);
    if (it == m_voices.end()) return;

    normalizedCutoff = (std::max)(0.001f, (std::min)(normalizedCutoff, 1.0f));
    XAUDIO2_FILTER_PARAMETERS filter{};
    filter.Type = LowPassFilter;
    /// @note XAudio2 の周波数係数は 2*sin(pi*cutoff/6)。1.0 が無加工相当。
    filter.Frequency = 2.0f * std::sin(3.14159265358979323846f * normalizedCutoff / 6.0f);
    filter.OneOverQ  = 1.0f;
    it->second.voice->SetFilterParameters(&filter);
}

bool XAudio2Device::IsPlaying(uint32_t voiceId)
{
    auto it = m_voices.find(voiceId);
    if (it == m_voices.end()) return false;
    if (it->second.stream && it->second.stream->Failed()) {
        FBZZ_LOG_ERROR("XAudio2Device: streaming voice failed");
        m_voices.erase(it);
        return false;
    }

    XAUDIO2_VOICE_STATE state{};
    it->second.voice->GetState(&state);
    if (state.BuffersQueued != 0) return true;
    if (it->second.stream && !it->second.stream->Finished()) return true;

    if (!it->second.stream) RecycleVoice(it->second);
    m_voices.erase(it);
    return false;
}

void XAudio2Device::RecycleVoice(const VoiceEntry& entry)
{
    IXAudio2SourceVoice* voice = entry.voice;
    if (!voice) return;

    std::vector<IXAudio2SourceVoice*>& pool = m_voicePool[entry.key];
    if (pool.size() >= kMaxPooledPerKey) {
        voice->DestroyVoice();
        return;
    }

    /// @note 積む前に «次の音がそのまま鳴らせる» 状態へ戻す。使い回す以上、前の音の設定は
    /// @note すべて «次の音の初期値» になるため、1 つでも戻し忘れると再生側からは原因不明の
    /// @note 音量・音質のばらつきとして出る。
    voice->Stop();
    voice->FlushSourceBuffers();
    if (entry.sendDestination) {
        XAUDIO2_SEND_DESCRIPTOR descriptor{0, entry.destination};
        XAUDIO2_VOICE_SENDS sends{1, &descriptor};
        if (FAILED(voice->SetOutputVoices(&sends))) {
            voice->DestroyVoice();
            return;
        }
        voice->SetOutputMatrix(entry.destination, entry.channels, entry.destinationChannels, entry.mainMatrix.data());
    }
    voice->SetVolume(1.0f);
    voice->SetFrequencyRatio(1.0f);
    XAUDIO2_FILTER_PARAMETERS bypass{ LowPassFilter, 1.0f, 1.0f };
    voice->SetFilterParameters(&bypass);

    /// @note パンはモノラル voice の出力行列として書かれている (SetPan)。中央へ戻す。
    if (entry.channels == 1 && entry.destinationChannels >= 2) {
        IXAudio2Voice* destination = entry.destination
            ? entry.destination : static_cast<IXAudio2Voice*>(m_masterVoice);
        if (destination) {
            std::vector<float> matrix(entry.destinationChannels, 0.0f);
            matrix[0] = std::sqrt(0.5f);
            matrix[1] = std::sqrt(0.5f);
            voice->SetOutputMatrix(destination, 1, entry.destinationChannels, matrix.data());
        }
    }

    pool.push_back(voice);
}

IXAudio2SourceVoice* XAudio2Device::TakePooledVoice(const VoiceKey& key)
{
    const auto it = m_voicePool.find(key);
    if (it == m_voicePool.end() || it->second.empty()) return nullptr;

    IXAudio2SourceVoice* voice = it->second.back();
    it->second.pop_back();
    return voice;
}

void XAudio2Device::PurgeFinishedVoices()
{
    for (auto it = m_voices.begin(); it != m_voices.end();) {
        if (it->second.loop || it->second.stream) { ++it; continue; }
        XAUDIO2_VOICE_STATE state{};
        it->second.voice->GetState(&state);
        if (state.BuffersQueued == 0) {
            RecycleVoice(it->second);
            it = m_voices.erase(it);
        } else {
            ++it;
        }
    }
}

} /// @note namespace fbzz::audio
