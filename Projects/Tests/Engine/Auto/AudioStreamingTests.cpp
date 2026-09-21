/// @file    AudioStreamingTests.cpp
/// @brief   補助バスと長尺音声の分割読み込み・再生ライフサイクルを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/TempDir.hpp>
#include <Engine/Audio/AudioManager.hpp>
#include <Engine/Audio/AudioStreamReader.hpp>
#include <Engine/Audio/XAudio2Device.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/Systems/AudioSystem.hpp>
#include <Engine/Scene/Components/AudioSourceComponent.hpp>
#include <Engine/Scene/Components/AudioSpatialComponents.hpp>
#include <Physics/World.hpp>
#include <fstream>
#include <unordered_map>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

namespace fbzz::tests {
namespace {
class RecordingAudioDevice final : public audio::IAudioDevice {
public:
    struct Voice {
        bool playing = true;
        bool paused = false;
        bool loop = false;
        bool streaming = false;
        float volume = 1;
        float sendLevel = 0;
        audio::BusIndex bus = 0;
        audio::BusIndex send = audio::kInvalidBus;
        std::string path;
    };
    std::unordered_map<uint32_t, Voice> voices;
    uint32_t next = 1;
    bool Init() override { return true; }
    void Shutdown() override { voices.clear(); }
    bool RebuildBuses(const audio::BusDesc*, size_t) override { voices.clear(); return true; }
    void SetBusVolume(audio::BusIndex, float) override {}
    void SetBusLowPass(audio::BusIndex, float) override {}
    void SetBusReverb(audio::BusIndex, float, float, float) override {}
    uint32_t PlayBuffer(const void*, size_t, const audio::WaveFormat&, bool loop, audio::BusIndex bus) override {
        const auto id = next++;
        voices[id].loop = loop;
        voices[id].bus = bus;
        return id;
    }
    uint32_t PlayStream(const std::string& path, bool loop, audio::BusIndex bus) override {
        const auto id = PlayBuffer(nullptr, 0, {}, loop, bus);
        voices[id].streaming = true;
        voices[id].path = path;
        return id;
    }
    void StopBuffer(uint32_t id) override { voices.erase(id); }
    void PauseBuffer(uint32_t id) override { voices.at(id).paused = true; }
    void ResumeBuffer(uint32_t id) override { voices.at(id).paused = false; }
    void SetVolume(uint32_t id, float value) override { voices.at(id).volume = value; }
    void SetPitch(uint32_t, float) override {}
    void SetPan(uint32_t, float) override {}
    void SetLowPass(uint32_t, float) override {}
    void SetSend(uint32_t id, audio::BusIndex bus, float level) override {
        voices.at(id).send = bus;
        voices.at(id).sendLevel = level;
    }
    bool IsPlaying(uint32_t id) override {
        const auto it = voices.find(id);
        if (it == voices.end()) return false;
        if (it->second.playing) return true;
        voices.erase(it);
        return false;
    }
};

class AudioStreamingTest : public testkit::EngineFixture {
protected:
    testkit::TempDir m_temp{"AudioStreaming"};
    RecordingAudioDevice m_device;
    audio::AudioManager m_audio{m_device};
    HRESULT m_com = E_FAIL;
    void SetUp() override {
        testkit::EngineFixture::SetUp();
        m_com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ASSERT_TRUE(SUCCEEDED(m_com) || m_com == RPC_E_CHANGED_MODE);
        ASSERT_TRUE(m_audio.Init());
    }
    void TearDown() override {
        m_audio.Shutdown();
        if (SUCCEEDED(m_com)) CoUninitialize();
        testkit::EngineFixture::TearDown();
    }
    std::string WriteWave(const std::vector<uint8_t>& pcm) {
        const auto path = m_temp.File("blocks.wav");
        std::ofstream file(path, std::ios::binary);
        const auto number = [&](uint32_t value, unsigned bytes) {
            for (unsigned i = 0; i < bytes; ++i) file.put(static_cast<char>((value >> (8 * i)) & 255));
        };
        file.write("RIFF", 4); number(36 + static_cast<uint32_t>(pcm.size()), 4);
        file.write("WAVEfmt ", 8); number(16, 4); number(1, 2); number(1, 2);
        number(48000, 4); number(96000, 4); number(2, 2); number(16, 2);
        file.write("data", 4); number(static_cast<uint32_t>(pcm.size()), 4);
        file.write(static_cast<const char*>(static_cast<const void*>(pcm.data())), static_cast<std::streamsize>(pcm.size()));
        return path.generic_string();
    }
};

TEST_F(AudioStreamingTest, DecodesBoundedBlocksWithoutLosingSamplesAndRewinds)
{
    std::vector<uint8_t> expected(audio::AudioStreamReader::BLOCK_BYTES * 5 + 246);
    for (size_t i = 0; i < expected.size(); ++i) expected[i] = static_cast<uint8_t>((i * 17 + 9) % 256);
    audio::AudioStreamReader reader;
    ASSERT_TRUE(reader.Open(WriteWave(expected)));
    EXPECT_EQ(reader.Format().sampleRate, 48000u);
    EXPECT_EQ(reader.Format().channels, 1u);
    std::vector<uint8_t> actual, block;
    for (size_t i = 0; i < 8; ++i) {
        ASSERT_TRUE(reader.Read(block));
        ASSERT_LE(block.size(), audio::AudioStreamReader::BLOCK_BYTES);
        if (block.empty()) break;
        actual.insert(actual.end(), block.begin(), block.end());
    }
    EXPECT_EQ(actual, expected);
    ASSERT_TRUE(reader.Read(block));
    EXPECT_TRUE(block.empty());
    ASSERT_TRUE(reader.Rewind());
    ASSERT_TRUE(reader.Read(block));
    EXPECT_EQ(block, (std::vector<uint8_t>(expected.begin(), expected.begin() + block.size())));
}

TEST_F(AudioStreamingTest, RejectsMissingAndNonAudioFiles)
{
    audio::AudioStreamReader reader;
    EXPECT_FALSE(reader.Open(m_temp.File("missing.wav").generic_string()));
    std::ofstream(m_temp.File("broken.wav"), std::ios::binary) << "not a wave";
    EXPECT_FALSE(reader.Open(m_temp.File("broken.wav").generic_string()));
}

TEST_F(AudioStreamingTest, NativeStreamStopsWhilePausedAndRebuildsBusesWithoutKeepingBuffers)
{
    /// @note 出力デバイスのないランナーだけを除外し、デバイスがある環境での初期化失敗は検出する。
    /// @see https://learn.microsoft.com/en-us/windows/win32/api/mmdeviceapi/nf-mmdeviceapi-immdeviceenumerator-enumaudioendpoints 有効な再生エンドポイントの列挙。
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    ASSERT_HRESULT_SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                              IID_PPV_ARGS(enumerator.GetAddressOf())));
    Microsoft::WRL::ComPtr<IMMDeviceCollection> endpoints;
    ASSERT_HRESULT_SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, endpoints.GetAddressOf()));
    UINT endpointCount = 0;
    ASSERT_HRESULT_SUCCEEDED(endpoints->GetCount(&endpointCount));
    if (endpointCount == 0) GTEST_SKIP() << "No active audio output endpoint is available";

    audio::XAudio2Device device;
    ASSERT_TRUE(device.Init());
    const auto path = WriteWave(std::vector<uint8_t>(48000 * 2 * 4, 0));
    const auto voice = device.PlayStream(path, true, 1);
    ASSERT_NE(voice, 0u);
    device.PauseBuffer(voice);
    EXPECT_TRUE(device.IsPlaying(voice));
    device.SetSend(voice, 2, 0.25f);
    device.SetPan(voice, -0.5f);
    device.SetSend(voice, 1, 0.5f);
    device.SetSend(voice, audio::kInvalidBus, 0.0f);
    device.ResumeBuffer(voice);
    device.StopBuffer(voice);
    EXPECT_FALSE(device.IsPlaying(voice));
    const auto second = device.PlayStream(path, true, 1);
    ASSERT_NE(second, 0u);
    device.PauseBuffer(second);
    const auto layout = audio::DefaultBusLayout();
    ASSERT_TRUE(device.RebuildBuses(layout.data(), layout.size()));
    EXPECT_FALSE(device.IsPlaying(second));
    EXPECT_EQ(device.PlayStream(m_temp.File("missing.wav").generic_string(), false, 0), 0u);
}

TEST_F(AudioStreamingTest, BgmStreamsAndRetainsPauseFadeAndStopLifecycle)
{
    m_audio.PlayBGM("long.wav", true, 1.0f);
    ASSERT_EQ(m_device.voices.size(), 1u);
    const auto first = m_device.voices.begin()->first;
    EXPECT_TRUE(m_device.voices.at(first).streaming);
    EXPECT_TRUE(m_device.voices.at(first).loop);
    EXPECT_EQ(m_device.voices.at(first).bus, m_audio.FindBus("BGM"));
    EXPECT_FLOAT_EQ(m_device.voices.at(first).volume, 0.0f);
    m_audio.Update(0.5f);
    EXPECT_FLOAT_EQ(m_device.voices.at(first).volume, 0.5f);
    m_audio.PauseVoice(first);
    EXPECT_TRUE(m_device.voices.at(first).paused);
    EXPECT_TRUE(m_audio.IsVoicePlaying(first));
    m_audio.ResumeVoice(first);
    EXPECT_FALSE(m_device.voices.at(first).paused);
    m_audio.PlayBGM("next.wav", false, 1.0f);
    EXPECT_EQ(m_device.voices.size(), 2u);
    m_audio.Update(1.0f);
    EXPECT_EQ(m_device.voices.count(first), 0u);
    m_audio.ApplyBusLayout(audio::DefaultBusLayout());
    EXPECT_TRUE(m_device.voices.empty());
    EXPECT_EQ(m_audio.ActiveVoiceCount(), 0u);
}

TEST_F(AudioStreamingTest, MixerSendUsesNamedBusWithoutReducingDryVolumeAndCanBeRemoved)
{
    scene::Scene scene;
    physics::World world;
    auto& go = scene.CreateGameObject("Audio");
    scene::AudioSourceComponent source;
    source.clipPath = "stream.wav";
    source.streaming = true;
    source.playOnAwake = true;
    source.volume = 0.8f;
    go.AddComponent<scene::AudioSourceComponent>(source);
    scene::AudioMixerSendComponent send;
    send.busName = "UI";
    send.sendLevel = 0.25f;
    go.AddComponent<scene::AudioMixerSendComponent>(send);
    SystemContext context{scene, world, nullptr, &m_audio, 0.01f, 0.01f, true, true};
    scene::AudioSystem system;
    system.Update(context);
    ASSERT_EQ(m_device.voices.size(), 1u);
    const auto id = m_device.voices.begin()->first;
    EXPECT_FLOAT_EQ(m_device.voices.at(id).volume, 0.8f);
    EXPECT_EQ(m_device.voices.at(id).bus, m_audio.FindBus("SE"));
    EXPECT_EQ(m_device.voices.at(id).send, m_audio.FindBus("UI"));
    EXPECT_FLOAT_EQ(m_device.voices.at(id).sendLevel, 0.25f);
    go.GetComponent<scene::AudioMixerSendComponent>()->enabled = false;
    system.Update(context);
    EXPECT_EQ(m_device.voices.at(id).send, audio::kInvalidBus);
    EXPECT_FLOAT_EQ(m_device.voices.at(id).volume, 0.8f);
    m_audio.SetVoiceSend(id, "missing", 1.0f);
    EXPECT_EQ(m_device.voices.at(id).send, audio::kInvalidBus);
    m_audio.SetVoiceSend(id, "bgm", 4.0f);
    EXPECT_EQ(m_device.voices.at(id).send, m_audio.FindBus("BGM"));
    EXPECT_FLOAT_EQ(m_device.voices.at(id).sendLevel, 1.0f);
}

TEST_F(AudioStreamingTest, StreamingAndSendSettingsSurviveSceneSerialization)
{
    scene::Scene source;
    auto& go = source.CreateGameObject("Audio");
    scene::AudioSourceComponent audioSource;
    audioSource.streaming = true;
    go.AddComponent<scene::AudioSourceComponent>(audioSource);
    scene::AudioMixerSendComponent send;
    send.busName = "Voice";
    send.sendLevel = 0.3f;
    go.AddComponent<scene::AudioMixerSendComponent>(send);
    const auto path = m_temp.File("audio.scene").generic_string();
    ASSERT_TRUE(scene::SceneSerializer::Save(source, path));
    auto loaded = scene::SceneSerializer::LoadData(path);
    ASSERT_NE(loaded, nullptr);
    const auto entities = loaded->GetEntities<scene::AudioSourceComponent>();
    ASSERT_EQ(entities.size(), 1u);
    EXPECT_TRUE(loaded->GetComponent<scene::AudioSourceComponent>(entities[0])->streaming);
    const auto* restored = loaded->GetComponent<scene::AudioMixerSendComponent>(entities[0]);
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->busName, "Voice");
    EXPECT_FLOAT_EQ(restored->sendLevel, 0.3f);
}
}
}
