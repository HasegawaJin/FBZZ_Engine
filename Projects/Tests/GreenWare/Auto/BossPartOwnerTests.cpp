/// @file    BossPartOwnerTests.cpp
/// @brief   親階層と分離部位の所有者参照からボス本体を解決する経路を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-15
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Scripts/Combat/BossPartComponent.hpp>

namespace fbzz::tests {

class BossPartOwnerTest : public testkit::EngineFixture {
protected:
    struct Boss : sandbox::IBoss {
        const char* BossName() const override { return "Test"; }
        int CurrentPhase() const override { return 1; }
        int PhaseCount() const override { return 1; }
        bool IsStaggered() const override { return false; }
    };
    scene::Scene m_scene;
    Boss m_boss;
    scene::GameObject* m_root = nullptr;

    void SetUp() override
    {
        testkit::EngineFixture::SetUp();
        m_root = &m_scene.CreateGameObject("Boss");
        sandbox::IBoss::Bind(m_root, &m_boss);
    }

    void TearDown() override
    {
        sandbox::IBoss::Unbind(m_root, &m_boss);
        testkit::EngineFixture::TearDown();
    }
};

TEST_F(BossPartOwnerTest, NestedHitboxResolvesRegisteredBoss)
{
    auto& bone = m_scene.CreateGameObject("Bone");
    bone.SetParent(*m_root);
    auto& hitbox = m_scene.CreateGameObject("Hitbox");
    hitbox.SetParent(bone);
    auto& part = hitbox.AddScript<sandbox::BossPartComponent>();
    part.SetContext(&m_scene, &hitbox);
    EXPECT_EQ(part.BossRoot(), m_root);
}

TEST_F(BossPartOwnerTest, DetachedWingKeepsExplicitOwner)
{
    auto& wing = m_scene.CreateGameObject("Wing");
    wing.SetParent(*m_root);
    auto& part = wing.AddScript<sandbox::BossPartComponent>();
    part.SetContext(&m_scene, &wing);
    part.bossOwner = scene::EntityRef{m_root->GetID()};
    wing.ClearParent();
    EXPECT_EQ(part.BossRoot(), m_root);
    part.bossOwner = {};
    EXPECT_EQ(part.BossRoot(), nullptr);
}

} // namespace fbzz::tests
