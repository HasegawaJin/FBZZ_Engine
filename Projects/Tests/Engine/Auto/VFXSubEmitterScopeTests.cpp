/// @file    VFXSubEmitterScopeTests.cpp
/// @brief   SubEmitter の名前引きが自分の VFX の枝に閉じることを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-13
///
/// SubEmitter は GameObject の «名前» で相手を指す。範囲が空 (EntityID::INVALID) だと
/// ParticlePass::QueueSubEmitter がシーン全体から引くため、同じ .vfx を 2 つ鳴らすと
/// 片方の 1 段目がもう片方の 2 段目を吹かせる。範囲を配るのは VFXSystem の頭出し。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/VFXComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/VFXSystem.hpp>
#include <Physics/World.hpp>
#include <string>

namespace fbzz::tests {

class VFXSubEmitterScopeTest : public testkit::EngineFixture {
protected:
    physics::World m_world;
    scene::Scene   m_scene;

    /// «1 段目が死んだら 2 段目» の最小構成を 1 つ作り、ルートの EntityID を返す。
    scene::EntityID MakeEffect(const std::string& name)
    {
        scene::GameObject& root = m_scene.CreateGameObject(name);
        auto& vfx = root.AddComponent<scene::VFXComponent>();
        vfx.loop = true;          // 尺で畳まれて Despawn へ入らないようにする
        vfx.autoDestroy = false;

        scene::GameObject& shell = m_scene.CreateGameObject(name + " Shell");
        shell.SetParent(root);
        auto& shellEmitter = shell.AddComponent<scene::ParticleEmitter>();
        shellEmitter.settings.deathSubEmitter = name + " Burst";

        scene::GameObject& burst = m_scene.CreateGameObject(name + " Burst");
        burst.SetParent(root);
        burst.AddComponent<scene::ParticleEmitter>();

        return root.GetID();
    }

    void Tick(float dt = 1.0f / 60.0f)
    {
        SystemContext context{ m_scene, m_world, nullptr, nullptr, dt, 1.0f / 60.0f,
                               true, true };
        scene::VFXSystem system;
        system.Update(context);
    }

    [[nodiscard]] scene::EntityID ScopeOf(const std::string& objectName)
    {
        scene::GameObject* object = m_scene.Find(objectName);
        return object->GetComponent<scene::ParticleEmitter>()->settings.subEmitterScopeRoot;
    }
};

TEST_F(VFXSubEmitterScopeTest, EmittersUnderARootAreScopedToThatRoot)
{
    const scene::EntityID root = MakeEffect("Firework");
    Tick();

    EXPECT_EQ(ScopeOf("Firework Shell"), root);
    EXPECT_EQ(ScopeOf("Firework Burst"), root);
}

TEST_F(VFXSubEmitterScopeTest, TwoInstancesDoNotShareAScope)
{
    // 同じ .vfx を 2 つ置いた状態。名前は同じでも、参照先は自分の枝でなければならない。
    const scene::EntityID first  = MakeEffect("Firework");
    const scene::EntityID second = MakeEffect("Firework");
    Tick();

    ASSERT_NE(first, second);
    scene::GameObject* firstRoot  = m_scene.GetGameObject(first);
    scene::GameObject* secondRoot = m_scene.GetGameObject(second);
    ASSERT_NE(firstRoot, nullptr);
    ASSERT_NE(secondRoot, nullptr);

    EXPECT_EQ(firstRoot->GetChild(0)->GetComponent<scene::ParticleEmitter>()
                  ->settings.subEmitterScopeRoot, first);
    EXPECT_EQ(secondRoot->GetChild(0)->GetComponent<scene::ParticleEmitter>()
                  ->settings.subEmitterScopeRoot, second);
}

TEST_F(VFXSubEmitterScopeTest, ANestedRootOwnsItsOwnBranch)
{
    // 入れ子の VFX は自分が範囲になる。外側の範囲を配られると、内側の 2 段目を
    // 外側の別の枝から吹かせられてしまう。
    const scene::EntityID outer = MakeEffect("Outer");
    scene::GameObject& innerRoot = m_scene.CreateGameObject("Inner");
    innerRoot.SetParent(*m_scene.GetGameObject(outer));
    innerRoot.AddComponent<scene::VFXComponent>().autoDestroy = false;

    scene::GameObject& innerChild = m_scene.CreateGameObject("Inner Spark");
    innerChild.SetParent(innerRoot);
    innerChild.AddComponent<scene::ParticleEmitter>();

    // 外側と内側のどちらが先に回るかは登録順で決まる。順序に依存しないよう 2 回回す。
    Tick();
    Tick();

    EXPECT_EQ(ScopeOf("Inner Spark"), innerRoot.GetID());
    EXPECT_EQ(ScopeOf("Outer Shell"), outer);
}

TEST_F(VFXSubEmitterScopeTest, AnEmitterOutsideAnyVfxKeepsSceneWideLookup)
{
    // シーンへ手で置いた Emitter は従来どおりシーン全体から引く (範囲は INVALID のまま)。
    scene::GameObject& lone = m_scene.CreateGameObject("Lone Emitter");
    lone.AddComponent<scene::ParticleEmitter>();
    Tick();

    EXPECT_EQ(ScopeOf("Lone Emitter"), scene::EntityID::INVALID);
}

} // namespace fbzz::tests
