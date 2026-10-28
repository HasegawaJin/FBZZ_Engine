/// @file    AudioSourceTests.cpp
/// @brief   移動 OneShot・所有権・voice 上限・局所ミックスの CPU 回帰。
/// @author  Hasegawa Jin
/// @date    2026-10-11
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/TempDir.hpp>
#include <Engine/Audio/AudioManager.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/Systems/AudioSystem.hpp>
#include <Engine/Scene/Components/AudioListenerComponent.hpp>
#include <Engine/Scene/Components/AudioSourceComponent.hpp>
#include "Scene/Managed/ManagedAudioSourceServices.hpp"
#include "Scene/Managed/ManagedServicesBridge.hpp"
#include <Physics/World.hpp>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <unordered_map>
namespace fbzz::tests {
namespace {
/// @note 既存 IAudioDevice 境界だけを記録する。減衰・voice 選択・fade は製品の処理を使う。
class SourceRecordingDevice final : public audio::IAudioDevice {
public:
    struct Voice { bool playing = true, paused = false, loop = false; float volume = 1, pitch = 1, pan = 0, cutoff = 1; };
    std::unordered_map<uint32_t, Voice> voices;
    uint32_t next = 1;
    size_t invalidOperations = 0, busVolumeWrites = 0, busFilterWrites = 0;
    bool Init() override { return true; }
    void Shutdown() override { voices.clear(); }
    bool RebuildBuses(const audio::BusDesc*, size_t) override { voices.clear(); return true; }
    void SetBusVolume(audio::BusIndex, float) override { ++busVolumeWrites; }
    void SetBusLowPass(audio::BusIndex, float) override { ++busFilterWrites; }
    void SetBusReverb(audio::BusIndex, float, float, float) override {}
    uint32_t PlayBuffer(const void*, size_t, const audio::WaveFormat&, bool loop, audio::BusIndex) override {
        const uint32_t id = next++; voices[id].loop = loop; return id;
    }
    uint32_t PlayStream(const std::string&, bool loop, audio::BusIndex bus) override { return PlayBuffer(nullptr, 0, {}, loop, bus); }
    void StopBuffer(uint32_t id) override { voices.erase(id); }
    template<typename F> void Edit(uint32_t id, F edit) {
        const auto it = voices.find(id); if (it != voices.end()) edit(it->second); else ++invalidOperations;
    }
    void PauseBuffer(uint32_t id) override { Edit(id, [](Voice& v) { v.paused = true; }); }
    void ResumeBuffer(uint32_t id) override { Edit(id, [](Voice& v) { v.paused = false; }); }
    void SetVolume(uint32_t id, float v) override { Edit(id, [v](Voice& x) { x.volume = v; }); }
    void SetPitch(uint32_t id, float v) override { Edit(id, [v](Voice& x) { x.pitch = v; }); }
    void SetPan(uint32_t id, float v) override { Edit(id, [v](Voice& x) { x.pan = v; }); }
    void SetLowPass(uint32_t id, float v) override { Edit(id, [v](Voice& x) { x.cutoff = v; }); }
    void SetSend(uint32_t id, audio::BusIndex, float) override { Edit(id, [](Voice&) {}); }
    bool IsPlaying(uint32_t id) override {
        const auto it = voices.find(id); if (it == voices.end()) return false;
        if (it->second.playing) return true; voices.erase(it); return false;
    }
};
} /// @note namespace
class AudioSourceTest : public testkit::EngineFixture {
protected:
    void SetUp() override {
        EngineFixture::SetUp(); m_previousPlay = scene::Script::IsInPlayMode(); scene::Script::SetInPlayMode(true);
        ASSERT_TRUE(m_audio.Init()); m_object = &m_scene.CreateGameObject("Source");
        m_object->AddComponent<scene::AudioSourceComponent>().streaming = true;
        m_view.SetContext(&m_scene, m_object);
    }
    void TearDown() override {
        m_view.SetContext(nullptr, nullptr); m_scene.Clear(); m_audio.Shutdown();
        scene::Script::SetInPlayMode(m_previousPlay); EngineFixture::TearDown();
    }
    void Tick(float dt = 0.01f) {
        SystemContext ctx{m_scene, m_world, nullptr, &m_audio, dt, dt, true, true}; m_system.Update(ctx);
    }
    scene::AudioSourceComponent& Source() { return *m_object->GetComponent<scene::AudioSourceComponent>(); }
    scene::GameObject& Listener() { auto& go = m_scene.CreateGameObject("Listener"); go.AddComponent<scene::AudioListenerComponent>(); return go; }
    scene::ManagedBridgeStatus Settings(uint32_t index, bool read, int64_t integer = 0,
        std::span<const float> values = {}, uint64_t related = 0) {
        scene::ManagedBridgeRequest request; request.op = scene::ManagedBridgeOp::SERVICE_AUDIO;
        request.index = index; request.flags = read ? 1 : 0;
        return scene::managed_detail::RunAudioSourceSettings(*m_object, request, integer, values, {}, related, m_result);
    }
    testkit::TempDir m_temp{"AudioSource"};
    SourceRecordingDevice m_device;
    audio::AudioManager m_audio{m_device};
    scene::Scene m_scene;
    physics::World m_world;
    scene::AudioSystem m_system;
    scene::Script m_view;
    scene::GameObject* m_object = nullptr;
    scene::managed_detail::ManagedServiceResult m_result;
    bool m_previousPlay = false;
};
TEST_F(AudioSourceTest, OneShotTracksMovingSourceAndListenerWithoutRestart)
{
    auto& listener = Listener(); Source().spatialBlend = 1; Source().minDistance = 0; Source().maxDistance = 10;
    Source().volume = 0.8f; Source().airAbsorption = 0.4f; Source().SetLowPassCutoff(0.6f);
    m_object->transform.worldPosition = {5, 0, 0};

    m_view.audio.PlayOneShot("effect.wav", 0.5f);
    Tick();

    ASSERT_EQ(m_device.voices.size(), 1U); const auto id = m_device.voices.begin()->first;
    EXPECT_NEAR(m_device.voices.at(id).volume, 0.2f, testkit::kTolerance);

    EXPECT_NEAR(m_device.voices.at(id).pan, 1, testkit::kTolerance);
    EXPECT_NEAR(m_device.voices.at(id).cutoff, 0.6f, testkit::kTolerance);
    m_object->transform.worldPosition = {-10, 0, 0};
    Tick();

    EXPECT_NEAR(m_device.voices.at(id).volume, 0, testkit::kTolerance);
    EXPECT_NEAR(m_device.voices.at(id).pan, -1, testkit::kTolerance);
    listener.transform.worldPosition = {-9, 0, 0};
    Tick();

    EXPECT_NEAR(m_device.voices.at(id).volume, 0.36f, testkit::kTolerance);
    EXPECT_EQ(m_device.next, 2U);

    EXPECT_EQ(m_device.invalidOperations, 0U);
}
TEST_F(AudioSourceTest, MissingListenerPreserves2DAndSuppresses3DContribution)
{
    Source().volume = 0.8f;

    m_view.audio.Play("loop.wav");
    Tick();

    ASSERT_EQ(m_device.voices.size(), 1U); const auto id = m_device.voices.begin()->first;

    EXPECT_NEAR(m_device.voices.at(id).volume, 0.8f, testkit::kTolerance);
    m_view.audio.SetSpatialBlend(0.25f);
    Tick();

    EXPECT_NEAR(m_device.voices.at(id).volume, 0.6f, testkit::kTolerance);
    m_view.audio.SetSpatialBlend(1);
    Tick();

    EXPECT_NEAR(m_device.voices.at(id).volume, 0, testkit::kTolerance);
    Listener();
    Tick();

    EXPECT_NEAR(m_device.voices.at(id).volume, 0.8f, testkit::kTolerance);
}
TEST_F(AudioSourceTest, PauseResumeAndStopControlOwnedOneShotsWithoutRestart)
{

    m_view.audio.PlayOneShot("effect.wav");
    Tick();

    ASSERT_EQ(m_device.voices.size(), 1U); const auto id = m_device.voices.begin()->first;

    m_view.audio.Pause();
    Tick();

    EXPECT_TRUE(m_device.voices.at(id).paused);

    m_view.audio.Resume();
    Tick();

    EXPECT_FALSE(m_device.voices.at(id).paused);

    EXPECT_EQ(m_device.next, 2U);

    m_view.audio.Stop();
    Tick(); m_audio.Update(0.02f);

    EXPECT_TRUE(m_device.voices.empty());
    Tick();

    EXPECT_EQ(m_device.next, 2U);
}
TEST_F(AudioSourceTest, BusRebuildClearsPausedSourceStateAndAllowsReplay)
{

    m_view.audio.Play("effect.wav");

    m_view.audio.PlayOneShot("effect.wav");
    Tick();

    m_view.audio.Pause();
    Tick();

    EXPECT_TRUE(m_view.audio.IsPaused());
    m_audio.ApplyBusLayout(audio::DefaultBusLayout());
    Tick();

    EXPECT_FALSE(m_view.audio.IsPaused());
    EXPECT_FALSE(m_view.audio.IsPlaying());

    m_view.audio.Play("effect.wav");
    Tick();

    EXPECT_TRUE(m_view.audio.IsPlaying());

    EXPECT_EQ(m_device.voices.size(), 1U);
}
TEST_F(AudioSourceTest, DisabledSourceDiscardsQueuedSoundsAndStopsExistingVoices)
{
    m_view.audio.SetLoop(true);

    m_view.audio.Play("loop.wav");

    m_view.audio.PlayOneShot("effect.wav");
    Tick();

    ASSERT_EQ(m_device.voices.size(), 2U);

    m_view.audio.PlayOneShot("queued.wav"); Source().enabled = false;
    Tick(); m_audio.Update(0.02f);

    EXPECT_TRUE(m_device.voices.empty()); Source().enabled = true;
    Tick();

    EXPECT_TRUE(m_device.voices.empty());
    EXPECT_EQ(m_device.next, 3U);
}
TEST_F(AudioSourceTest, ExplicitStopBeforeFirstUpdateCancelsPlayOnAwakeAndQueuedEffects)
{
    Source().playOnAwake = true; Source().clipPath = "effect.wav";

    m_view.audio.PlayOneShot("queued.wav");

    m_view.audio.Stop();
    Tick();
    Tick();

    EXPECT_TRUE(m_device.voices.empty());
    EXPECT_EQ(m_device.next, 1U);

    m_view.audio.Play();
    Tick();

    EXPECT_EQ(m_device.voices.size(), 1U);

    EXPECT_TRUE(m_view.audio.IsPlaying());
}
TEST_F(AudioSourceTest, RemovedSourceAndDestroyedOwnerRetireTheirLoopAndOneShots)
{
    m_view.audio.SetLoop(true);

    m_view.audio.Play("loop.wav");

    m_view.audio.PlayOneShot("effect.wav");
    Tick();
    m_object->RemoveComponent<scene::AudioSourceComponent>();
    Tick(); m_audio.Update(0.02f);

    EXPECT_TRUE(m_device.voices.empty());
    m_object->AddComponent<scene::AudioSourceComponent>().streaming = true;
    m_view.audio.SetLoop(true);

    m_view.audio.Play("loop.wav");

    m_view.audio.PlayOneShot("effect.wav");
    Tick();
    m_view.SetContext(nullptr, nullptr);

    ASSERT_TRUE(m_scene.DestroyGameObject(m_object->GetID()));
    m_scene.FlushDestroyQueue(0); m_object = nullptr;
    Tick(); m_audio.Update(0.02f);

    EXPECT_TRUE(m_device.voices.empty());
}
TEST_F(AudioSourceTest, OneShotBurstRemainsBoundedAndFinishedVoiceMakesRoom)
{
    for (int frame = 0; frame < 100; ++frame) {
        for (int i = 0; i < 1000; ++i) m_view.audio.PlayOneShot("effect.wav");
        Tick();

        ASSERT_LE(m_device.voices.size(), 16U);
    }
    EXPECT_EQ(m_device.next, 17U); const auto expired = m_device.voices.begin()->first;
    m_device.voices.at(expired).playing = false;

    m_view.audio.PlayOneShot("next.wav");
    Tick();

    EXPECT_EQ(m_device.next, 18U);
    EXPECT_EQ(m_device.voices.size(), 16U);

    EXPECT_EQ(m_device.invalidOperations, 0U);
}
TEST_F(AudioSourceTest, GlobalPriorityBudgetKeepsLoopsAndIgnoresStolenHandles)
{
    m_audio.SetVoiceLimit(4); audio::AudioManager::PlayParams params; params.streaming = true;
    params.priority = -100; const auto loop = m_audio.PlayVoice("loop.wav", true, 0, params);
    params.priority = 20; const auto important = m_audio.PlayVoice("important.wav", false, 0, params);
    params.priority = 0; const auto oldest = m_audio.PlayVoice("old.wav", false, 0, params);
    const auto other = m_audio.PlayVoice("other.wav", false, 0, params);

    ASSERT_NE(other, 0U); params.priority = -1;
    EXPECT_EQ(m_audio.PlayVoice("rejected.wav", false, 0, params), 0U);
    params.priority = 10;

    EXPECT_NE(m_audio.PlayVoice("new.wav", false, 0, params), 0U);

    EXPECT_EQ(m_device.voices.count(oldest), 0U);
    EXPECT_EQ(m_device.voices.count(loop), 1U);

    EXPECT_EQ(m_device.voices.count(important), 1U);
    m_audio.SetVoiceVolume(oldest, 1); m_audio.SetVoicePitch(oldest, 1); m_audio.SetVoicePan(oldest, 0);
    m_audio.SetVoiceLowPass(oldest, 1); m_audio.SetVoiceSend(oldest, "SE", 1); m_audio.PauseVoice(oldest); m_audio.ResumeVoice(oldest);

    EXPECT_EQ(m_device.voices.size(), 4U);
    EXPECT_EQ(m_device.invalidOperations, 0U);
}
TEST_F(AudioSourceTest, NonfiniteVoiceParametersAreIgnoredAndDopplerPitchKeepsBackendRange)
{

    m_view.audio.Play("effect.wav");
    Tick();

    ASSERT_EQ(m_device.voices.size(), 1U); const auto id = m_device.voices.begin()->first;
    const float nan = (std::numeric_limits<float>::quiet_NaN)();
    m_audio.SetVoiceVolume(id, nan); m_audio.SetVoicePitch(id, nan); m_audio.SetVoicePan(id, nan); m_audio.SetVoiceLowPass(id, nan);

    EXPECT_NEAR(m_device.voices.at(id).volume, 1, testkit::kTolerance);
    EXPECT_NEAR(m_device.voices.at(id).pitch, 1, testkit::kTolerance);

    EXPECT_NEAR(m_device.voices.at(id).pan, 0, testkit::kTolerance);
    EXPECT_NEAR(m_device.voices.at(id).cutoff, 1, testkit::kTolerance);
    m_audio.SetVoicePitch(id, 8);

    EXPECT_NEAR(m_device.voices.at(id).pitch, 8, testkit::kTolerance);

    EXPECT_EQ(m_device.invalidOperations, 0U);
}
TEST_F(AudioSourceTest, NewSceneGenerationRetiresOldVoicesWithoutReusingOwner)
{
    m_view.audio.SetLoop(true);

    m_view.audio.Play("loop.wav");

    m_view.audio.PlayOneShot("effect.wav");
    Tick();

    ASSERT_EQ(m_device.voices.size(), 2U); m_view.SetContext(nullptr, nullptr); m_scene.Clear();
    m_object = &m_scene.CreateGameObject("New source"); m_object->AddComponent<scene::AudioSourceComponent>().streaming = true;
    m_view.SetContext(&m_scene, m_object);
    Tick(); m_audio.Update(0.02f);

    EXPECT_TRUE(m_device.voices.empty());

    m_view.audio.PlayOneShot("new.wav");
    Tick();

    EXPECT_EQ(m_device.voices.size(), 1U);

    EXPECT_EQ(m_device.next, 4U);
}
TEST_F(AudioSourceTest, FilterFadeIsLocalAndVolumeFadeRemainsIndependent)
{
    Source().volume = 0.8f;

    m_view.audio.Play("loop.wav");
    Tick();

    ASSERT_EQ(m_device.voices.size(), 1U); const auto id = m_device.voices.begin()->first;
    const auto volumeWrites = m_device.busVolumeWrites, filterWrites = m_device.busFilterWrites;

    ASSERT_TRUE(Source().FadeLowPassTo(0.2f, 1)); m_audio.FadeVoice(id, 0, 1);
    Tick(0.5f); m_audio.Update(0.5f);

    EXPECT_NEAR(m_device.voices.at(id).cutoff, 0.6f, testkit::kTolerance);
    EXPECT_NEAR(m_device.voices.at(id).volume, 0.4f, testkit::kTolerance);
    Source().volume = 0.4f;
    Tick(0.5f); m_audio.Update(0.5f);

    EXPECT_NEAR(m_device.voices.at(id).cutoff, 0.2f, testkit::kTolerance);
    EXPECT_NEAR(m_device.voices.at(id).volume, 0, testkit::kTolerance);

    EXPECT_EQ(m_device.busVolumeWrites, volumeWrites);
    EXPECT_EQ(m_device.busFilterWrites, filterWrites);

    ASSERT_TRUE(Source().FadeLowPassTo(1, 1));
    Tick(0.25f);

    ASSERT_TRUE(Source().FadeLowPassTo(0, 0.5f));
    Tick(0.25f);

    EXPECT_NEAR(m_device.voices.at(id).cutoff, 0.2f, testkit::kTolerance);
}
TEST_F(AudioSourceTest, AudioSettingsValidatePacketAndScopeBeforeMutation)
{
    using Status = scene::ManagedBridgeStatus; const std::array<float, 1> valid{0.3f}, invalid{1.1f}, nan{(std::numeric_limits<float>::quiet_NaN)()};
    m_result.integer = 99;

    EXPECT_EQ(Settings(55, false, 0, invalid), Status::INVALID_ARGUMENT);

    EXPECT_EQ(Settings(55, false, 0, nan), Status::INVALID_ARGUMENT);
    EXPECT_EQ(Settings(55, true, 0, valid), Status::INVALID_ARGUMENT);

    EXPECT_EQ(Settings(55, false, 0, valid, 5), Status::INVALID_ARGUMENT);
    EXPECT_EQ(Settings(57, false), Status::INVALID_ARGUMENT);

    EXPECT_NEAR(Source().lowPassCutoff, 1, testkit::kTolerance);
    EXPECT_EQ(m_result.integer, 99);
    scene::Script::SetInPlayMode(false);

    EXPECT_EQ(Settings(55, false, 0, valid), Status::INVALID_SCOPE);

    EXPECT_EQ(Settings(54, true), Status::OK);
    EXPECT_NEAR(m_result.values[0], 1, testkit::kTolerance);
    scene::Script::SetInPlayMode(true);

    ASSERT_EQ(Settings(55, false, 0, valid), Status::OK);

    ASSERT_EQ(Settings(54, true), Status::OK);
    EXPECT_NEAR(m_result.values[0], 0.3f, testkit::kTolerance);

    EXPECT_EQ(Settings(47, false, INT64_MAX), Status::INVALID_ARGUMENT);
    ASSERT_EQ(Settings(47, false, -12), Status::OK);

    ASSERT_EQ(Settings(46, true), Status::OK);
    EXPECT_EQ(m_result.integer, -12);

    EXPECT_EQ(Settings(49, false, 2), Status::INVALID_ARGUMENT);
    ASSERT_EQ(Settings(49, false, 0), Status::OK);

    ASSERT_EQ(Settings(48, true), Status::OK);
    EXPECT_EQ(m_result.integer, 0);

    ASSERT_EQ(Settings(51, false, 0, valid), Status::OK);
    ASSERT_EQ(Settings(50, true), Status::OK);

    EXPECT_NEAR(m_result.values[0], 0.3f, testkit::kTolerance);
    ASSERT_EQ(Settings(53, false, 0, valid), Status::OK);

    ASSERT_EQ(Settings(52, true), Status::OK);
    EXPECT_NEAR(m_result.values[0], 0.3f, testkit::kTolerance);
    const std::array<float, 2> fade{0.8f, 1}, badFade{0.8f, -1};

    EXPECT_EQ(Settings(56, false, 0, badFade), Status::INVALID_ARGUMENT);
    ASSERT_EQ(Settings(56, false, 0, fade), Status::OK);

    ASSERT_EQ(Settings(49, false, 1), Status::OK);

    m_view.audio.Play("loop.wav");
    Tick(0.5f);

    ASSERT_EQ(m_device.voices.size(), 1U);

    EXPECT_NEAR(m_device.voices.begin()->second.cutoff, 0.55f, testkit::kTolerance);
}
TEST_F(AudioSourceTest, RejectedFilterChangesPreserveAnInProgressFade)
{

    m_view.audio.Play("effect.wav");
    Tick();

    ASSERT_EQ(m_device.voices.size(), 1U); const auto id = m_device.voices.begin()->first;

    ASSERT_TRUE(Source().FadeLowPassTo(0.2f, 1));
    Tick(0.25f);

    EXPECT_NEAR(m_device.voices.at(id).cutoff, 0.8f, testkit::kTolerance);
    EXPECT_FALSE(Source().SetLowPassCutoff(-0.1f));

    EXPECT_FALSE(Source().FadeLowPassTo(0.4f, (std::numeric_limits<float>::infinity)()));
    Tick(0.25f);

    EXPECT_NEAR(m_device.voices.at(id).cutoff, 0.6f, testkit::kTolerance);

    ASSERT_TRUE(Source().FadeLowPassTo(0.3f, 0));
    Tick(0);

    EXPECT_NEAR(m_device.voices.at(id).cutoff, 0.3f, testkit::kTolerance);
}
TEST_F(AudioSourceTest, FilterRoundTripsAndOldSceneDefaultsToUnfiltered)
{
    Source().priority = 30; Source().SetLowPassCutoff(0.3f); Source().FadeLowPassTo(0.8f, 2);
    const auto path = m_temp.File("audio.scene").generic_string();

    ASSERT_TRUE(scene::SceneSerializer::Save(m_scene, path));
    auto loaded = scene::SceneSerializer::LoadData(path);

    ASSERT_NE(loaded, nullptr);
    auto entities = loaded->GetEntities<scene::AudioSourceComponent>();

    ASSERT_EQ(entities.size(), 1U);
    const auto* source = loaded->GetComponent<scene::AudioSourceComponent>(entities[0]);

    EXPECT_NEAR(source->lowPassCutoff, 0.3f, testkit::kTolerance);
    EXPECT_EQ(source->priority, 30);
    std::ifstream input(path); std::string old, line;
    while (std::getline(input, line)) if (line.find("lowPassCutoff") == std::string::npos) old += line + "\n";
    std::ofstream(path, std::ios::trunc) << old; loaded = scene::SceneSerializer::LoadData(path);

    ASSERT_NE(loaded, nullptr);
    entities = loaded->GetEntities<scene::AudioSourceComponent>();

    ASSERT_EQ(entities.size(), 1U);
    ASSERT_EQ(entities.size(), 1U);

    EXPECT_NEAR(loaded->GetComponent<scene::AudioSourceComponent>(entities[0])->lowPassCutoff, 1, testkit::kTolerance);
}
TEST_F(AudioSourceTest, BridgeRoutesNewAudioSettingsWithoutChangingOldSelectors)
{
    scene::managed_detail::ManagedServicesBridge bridge;
    std::array<char, 28> packet{}; const uint32_t count = 1; const float cutoff = 0.4f;
    std::memcpy(packet.data() + 16, &count, sizeof(count)); std::memcpy(packet.data() + 24, &cutoff, sizeof(cutoff));
    scene::ManagedBridgeRequest request; request.op = scene::ManagedBridgeOp::SERVICE_AUDIO;
    request.index = 55; request.input = packet.data(); request.inputBytes = static_cast<uint32_t>(packet.size());

    ASSERT_EQ(bridge.Run(m_scene, m_view, *m_object, nullptr, request, m_result, false), scene::ManagedBridgeStatus::OK);
    EXPECT_NEAR(Source().lowPassCutoff, 0.4f, testkit::kTolerance);
    packet.fill(0); request.inputBytes = 24; request.index = 54; request.flags = 1;

    ASSERT_EQ(bridge.Run(m_scene, m_view, *m_object, nullptr, request, m_result, false), scene::ManagedBridgeStatus::OK);
    EXPECT_NEAR(m_result.values[0], 0.4f, testkit::kTolerance);
    Source().volume = 0.7f; request.index = 5;

    ASSERT_EQ(bridge.Run(m_scene, m_view, *m_object, nullptr, request, m_result, false), scene::ManagedBridgeStatus::OK);
    EXPECT_NEAR(m_result.values[0], 0.7f, testkit::kTolerance);
}
} /// @note namespace fbzz::tests
