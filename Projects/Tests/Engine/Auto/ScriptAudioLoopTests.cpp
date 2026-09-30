/// @file    ScriptAudioLoopTests.cpp
/// @brief   独立ループ音の再生要求・個体分離・所有者破棄時の回収を実音声処理経路で検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-29
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/TempDir.hpp>
#include <Engine/Asset/SynthAsset.hpp>
#include <Engine/Audio/AudioManager.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/AudioSourceComponent.hpp>
#include <Engine/Scene/Systems/AudioSystem.hpp>
#include <Physics/World.hpp>
#include <limits>
#include <unordered_map>

namespace fbzz::tests {
namespace {
/// @note OS 出力だけ記録器へ置換する。Proxy、AudioSystem、AudioManager、Synth の実装は実物を通す。
class LoopRecordingDevice final : public audio::IAudioDevice {
public:
    struct Voice { float volume = 1; float pitch = 1; bool loop = false; audio::BusIndex bus = 0; };
    std::unordered_map<uint32_t, Voice> voices;
    uint32_t starts = 0;
    bool Init() override { return true; }
    void Shutdown() override { voices.clear(); }
    bool RebuildBuses(const audio::BusDesc*, size_t) override { voices.clear(); return true; }
    void SetBusVolume(audio::BusIndex, float) override {}
    void SetBusLowPass(audio::BusIndex, float) override {}
    void SetBusReverb(audio::BusIndex, float, float, float) override {}
    uint32_t PlayBuffer(const void*, size_t, const audio::WaveFormat&, bool loop, audio::BusIndex bus) override
    { const auto id = ++starts; voices.emplace(id, Voice{1, 1, loop, bus}); return id; }
    uint32_t PlayStream(const std::string&, bool loop, audio::BusIndex bus) override
    { return PlayBuffer(nullptr, 0, {}, loop, bus); }
    void StopBuffer(uint32_t id) override { voices.erase(id); }
    void PauseBuffer(uint32_t) override {}
    void ResumeBuffer(uint32_t) override {}
    void SetVolume(uint32_t id, float value) override { if (voices.contains(id)) voices[id].volume = value; }
    void SetPitch(uint32_t id, float value) override { if (voices.contains(id)) voices[id].pitch = value; }
    void SetPan(uint32_t, float) override {}
    void SetSend(uint32_t, audio::BusIndex, float) override {}
    void SetLowPass(uint32_t, float) override {}
    bool IsPlaying(uint32_t id) override { return voices.contains(id); }
};
} /// @note namespace

class ScriptAudioLoopTest : public testkit::EngineFixture {
protected:
    testkit::TempDir m_temp{"ScriptAudioLoops"};
    LoopRecordingDevice m_device;
    audio::AudioManager m_audio{m_device};
    scene::Scene m_scene;
    physics::World m_world;
    std::string m_clip;
    scene::AudioLoopSettings m_settings;
    void SetUp() override
    {
        EngineFixture::SetUp();
        ASSERT_TRUE(m_audio.Init());
        m_clip = m_temp.File("loop.synth").generic_string();
        ASSERT_TRUE(asset::SaveSynthAssetToFile(m_clip, asset::SynthAsset{}));
        m_settings.label = "SameKey";
        m_settings.spatialBlend = 0;
    }
    void TearDown() override
    {
        m_scene.Clear();
        m_audio.Shutdown();
        EngineFixture::TearDown();
    }
    scene::Script& Owner(const char* name = "Owner")
    {
        auto& object = m_scene.CreateGameObject(name);
        auto& script = object.AddScript<scene::Script>();
        script.SetContext(&m_scene, &object);
        return script;
    }
    void Tick()
    {
        SystemContext context{m_scene, m_world, nullptr, &m_audio, 0.05f, 0.02f, true, true};
        scene::AudioSystem{}.Update(context);
        m_audio.Update(0.05f);
        m_scene.FlushDestroyQueue(0.05f);
    }
};

TEST_F(ScriptAudioLoopTest, SameLabelOwnersRemainIndependentAndDoNotChangePrimaryAudio)
{
    auto& first = Owner("A");
    auto& second = Owner("B");
    auto& main = first.scene.Self()->AddComponent<scene::AudioSourceComponent>();
    main.volume = 0.8f;
    main.pitch = 1.0f;
    first.audio.Play(m_clip);
    scene::AudioLoop a, b;
    ASSERT_TRUE(first.audio.UpdateLoop(a, m_clip, 0.2f, 0.5f, m_settings));
    ASSERT_TRUE(second.audio.UpdateLoop(b, m_clip, 0.7f, 1.5f, m_settings));
    EXPECT_NE(first.scene.Self()->GetChild(0), second.scene.Self()->GetChild(0));
    Tick();
    ASSERT_EQ(m_device.voices.size(), 3u);
    auto* primary = first.scene.Self()->GetComponent<scene::AudioSourceComponent>();
    ASSERT_NE(primary, nullptr);
    EXPECT_FLOAT_EQ(primary->volume, 0.8f);
    EXPECT_FLOAT_EQ(primary->pitch, 1.0f);
    auto* aSource = first.scene.Self()->GetChild(0)->GetComponent<scene::AudioSourceComponent>();
    auto* bSource = second.scene.Self()->GetChild(0)->GetComponent<scene::AudioSourceComponent>();
    EXPECT_FLOAT_EQ(m_device.voices.at(aSource->m_voiceId).volume, 0.2f);
    EXPECT_FLOAT_EQ(m_device.voices.at(bSource->m_voiceId).pitch, 1.5f);
    EXPECT_FALSE(second.audio.StopLoop(a));
    EXPECT_FALSE(second.audio.UpdateLoop(a, m_clip, 1.0f, 1.0f, m_settings));
    EXPECT_TRUE(a.IsValid());
}

TEST_F(ScriptAudioLoopTest, RepeatedUpdatesKeepPlaybackPositionAndStopCanRestart)
{
    auto& owner = Owner();
    scene::AudioLoop loop;
    ASSERT_TRUE(owner.audio.UpdateLoop(loop, m_clip, 0.2f, 0.5f, m_settings));
    Tick();
    ASSERT_EQ(m_device.starts, 1u);
    ASSERT_TRUE(owner.audio.UpdateLoop(loop, m_clip, 0.6f, 1.5f, m_settings));
    Tick();
    EXPECT_EQ(m_device.starts, 1u);
    EXPECT_FLOAT_EQ(m_device.voices.begin()->second.volume, 0.6f);
    EXPECT_FLOAT_EQ(m_device.voices.begin()->second.pitch, 1.5f);
    ASSERT_TRUE(owner.audio.StopLoop(loop));
    Tick();
    EXPECT_TRUE(m_device.voices.empty());
    EXPECT_TRUE(loop.IsValid());
    ASSERT_TRUE(owner.audio.UpdateLoop(loop, m_clip, 0.3f, 1.0f, m_settings));
    Tick();
    EXPECT_EQ(m_device.starts, 2u);
    EXPECT_EQ(owner.scene.Self()->GetChildCount(), 1);
}

TEST_F(ScriptAudioLoopTest, StopBeforeAudioTickCancelsPendingPlaybackAndSilenceAllocatesNothing)
{
    auto& owner = Owner();
    scene::AudioLoop loop;
    ASSERT_TRUE(owner.audio.UpdateLoop(loop, m_clip, 0.0f, 1.0f, m_settings));
    EXPECT_EQ(owner.scene.Self()->GetChildCount(), 0);
    ASSERT_TRUE(owner.audio.UpdateLoop(loop, m_clip, 0.2f, 1.0f, m_settings));
    EXPECT_TRUE(owner.audio.StopLoop(loop));
    Tick();
    EXPECT_EQ(m_device.starts, 0u);
}

TEST_F(ScriptAudioLoopTest, DestroyingOwnerReclaimsVoiceWithoutAudioSourceStillExisting)
{
    auto& owner = Owner();
    const auto id = owner.scene.Self()->GetID();
    scene::AudioLoop loop;
    ASSERT_TRUE(owner.audio.UpdateLoop(loop, m_clip, 0.2f, 1.0f, m_settings));
    Tick();
    ASSERT_EQ(m_device.voices.size(), 1u);
    ASSERT_TRUE(m_scene.DestroyGameObject(id));
    EXPECT_FALSE(loop.IsValid());
    m_audio.Update(0.0f);
    m_audio.Update(0.05f);
    EXPECT_TRUE(m_device.voices.empty());
}

TEST_F(ScriptAudioLoopTest, ScriptReloadInvalidatesOldHandleAndRemovesOrphanSource)
{
    auto& owner = Owner();
    auto* object = owner.scene.Self();
    scene::AudioLoop old;
    ASSERT_TRUE(owner.audio.UpdateLoop(old, m_clip, 0.2f, 1.0f, m_settings));
    Tick();
    object->GetComponent<scene::ScriptComponent>()->scripts.clear();
    EXPECT_FALSE(old.IsValid());
    auto& replacement = object->AddScript<scene::Script>();
    replacement.SetContext(&m_scene, object);
    scene::AudioLoop fresh;
    ASSERT_TRUE(replacement.audio.UpdateLoop(fresh, m_clip, 0.7f, 1.0f, m_settings));
    Tick();
    EXPECT_EQ(object->GetChildCount(), 1);
    EXPECT_EQ(m_device.voices.size(), 1u);
    EXPECT_TRUE(fresh.IsValid());
    EXPECT_FALSE(old.IsValid());
}

TEST_F(ScriptAudioLoopTest, DeletedSourceAndReusedEntityDoNotAcceptStaleHandle)
{
    auto& owner = Owner();
    scene::AudioLoop loop;
    ASSERT_TRUE(owner.audio.UpdateLoop(loop, m_clip, 0.2f, 1.0f, m_settings));
    Tick();
    const auto child = owner.scene.Self()->GetChild(0)->GetID();
    ASSERT_TRUE(m_scene.DestroyGameObject(child));
    m_scene.CreateGameObject("Unrelated").AddComponent<scene::AudioSourceComponent>();
    EXPECT_FALSE(loop.IsValid());
    EXPECT_FALSE(owner.audio.StopLoop(loop));
    m_audio.Update(0.0f);
    m_audio.Update(0.05f);
    EXPECT_TRUE(m_device.voices.empty());
}

TEST_F(ScriptAudioLoopTest, DisableAndLifecycleResetInvalidateHandles)
{
    auto& owner = Owner();
    owner.SynchronizeEnabledState(true);
    scene::AudioLoop loop;
    ASSERT_TRUE(owner.audio.UpdateLoop(loop, m_clip, 0.2f, 1.0f, m_settings));
    Tick();
    owner.enabled = false;
    owner.SynchronizeEnabledState(true);
    EXPECT_FALSE(loop.IsValid());
    Tick();
    EXPECT_TRUE(m_device.voices.empty());
    owner.enabled = true;
    owner.SynchronizeEnabledState(true);
    ASSERT_TRUE(owner.audio.UpdateLoop(loop, m_clip, 0.2f, 1.0f, m_settings));
    owner.ResetLifecycleState();
    EXPECT_FALSE(loop.IsValid());
    Tick();
    EXPECT_TRUE(m_device.voices.empty());
}

TEST_F(ScriptAudioLoopTest, ExplicitReleaseInvalidatesCopiesAndNonFiniteInputDoesNotAllocate)
{
    auto& owner = Owner();
    scene::AudioLoop loop;
    EXPECT_FALSE(owner.audio.UpdateLoop(loop, m_clip, std::numeric_limits<float>::quiet_NaN(), 1.0f, m_settings));
    EXPECT_EQ(owner.scene.Self()->GetChildCount(), 0);
    ASSERT_TRUE(owner.audio.UpdateLoop(loop, m_clip, 0.2f, 1.0f, m_settings));
    const auto copy = loop;
    owner.audio.ReleaseLoop(loop);
    EXPECT_FALSE(loop.IsValid());
    EXPECT_FALSE(copy.IsValid());
    Tick();
    EXPECT_EQ(owner.scene.Self()->GetChildCount(), 0);
    EXPECT_EQ(m_device.starts, 0u);
}
} /// @note namespace fbzz::tests
