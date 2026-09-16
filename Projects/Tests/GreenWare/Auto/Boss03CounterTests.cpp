/// @file    Boss03CounterTests.cpp
/// @brief   反射後の落下・反撃時間と本体HPへの斬撃転送を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-15
#include <TestKit/TestKit.hpp>
#include <TestKit/Deterministic.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Scripts/Combat/Boss03BossComponent.hpp>

namespace fbzz::tests {
class Boss03CounterTest : public testkit::EngineFixture {
protected:
    scene::Scene m_scene;
    scene::GameObject* m_root = nullptr;
    scene::GameObject* m_bodyObject = nullptr;
    sandbox::Boss03AiComponent* m_ai = nullptr;
    sandbox::Boss03BossComponent* m_boss = nullptr;
    sandbox::EnemyHealthComponent* m_health = nullptr;
    sandbox::BossPartComponent* m_body = nullptr;
    float m_savedDelta = 0.0f;

    void SetUp() override
    {
        EngineFixture::SetUp();
        m_savedDelta = Time::deltaTime;
        m_root = &m_scene.CreateGameObject("Boss03");
        m_root->transform.position = m_root->transform.worldPosition = {0, 4, 0};
        m_health = &m_root->AddScript<sandbox::EnemyHealthComponent>();
        m_health->SetContext(&m_scene, m_root);
        m_health->maxHealth = 1400;
        m_health->destroyDelay = 12.0f;
        m_health->ResetHealth();
        auto& animator = m_root->AddScript<sandbox::Boss03AnimatorComponent>();
        animator.SetContext(&m_scene, m_root);
        m_ai = &m_root->AddScript<sandbox::Boss03AiComponent>();
        m_ai->SetContext(&m_scene, m_root);
        m_boss = &m_root->AddScript<sandbox::Boss03BossComponent>();
        m_boss->SetContext(&m_scene, m_root);
        sandbox::IBoss::Bind(m_root, m_boss);
        m_bodyObject = &m_scene.CreateGameObject("Body");
        m_bodyObject->SetParent(*m_root);
        m_bodyObject->transform.worldPosition = m_root->transform.worldPosition;
        m_body = &m_bodyObject->AddScript<sandbox::BossPartComponent>();
        m_body->SetContext(&m_scene, m_bodyObject);
        m_body->bodyTarget = true;
        m_body->bossOwner = scene::EntityRef{m_root->GetID()};
        m_ai->OnStart();
        Step(m_ai->appearSeconds + 0.01f);
    }

    void TearDown() override
    {
        sandbox::IBoss::Unbind(m_root, m_boss);
        Time::deltaTime = m_savedDelta;
        EngineFixture::TearDown();
    }

    void Step(float dt)
    {
        Time::deltaTime = dt;
        m_ai->OnUpdate();
        m_root->transform.worldPosition = m_root->transform.position;
        m_bodyObject->transform.worldPosition = m_root->transform.worldPosition;
    }

    void LandCounter()
    {
        m_ai->OnWingParriedHit(0);
        testkit::StepFixed([this](float dt) { Step(dt); }, 15, 0.05f);
    }
};

TEST_F(Boss03CounterTest, ReflectedWingFallsBeforeStartingTheFullOpening)
{
    m_ai->OnWingParriedHit(0);
    EXPECT_EQ(m_health->Current(), 1320);
    EXPECT_FALSE(m_ai->IsToppled());
    Step(0.3f);
    EXPECT_FALSE(m_ai->IsToppled());
    EXPECT_LT(m_root->transform.position.y, 4.0f);
    Step(0.4f);
    EXPECT_TRUE(m_ai->IsToppled());
    EXPECT_NEAR(m_root->transform.position.y, 3.05f, 0.001f);
    Step(5.8f);
    EXPECT_TRUE(m_ai->IsToppled());
    Step(0.3f);
    EXPECT_FALSE(m_ai->IsToppled());
}

TEST_F(Boss03CounterTest, BodyDamageUsesSharedHealthAndRewardsGroundedAttacks)
{
    EXPECT_FALSE(m_body->IsExecutable());
    EXPECT_FALSE(m_body->Damage(100));
    EXPECT_EQ(m_health->Current(), 1365);
    LandCounter();
    const int before = m_health->Current();
    EXPECT_FALSE(m_body->Damage(100));
    EXPECT_EQ(m_health->Current(), before - 125);
    EXPECT_EQ(m_body->Health(), m_health->Current());
    EXPECT_FALSE(m_body->IsDepleted());
    EXPECT_FALSE(m_ai->Execute(m_bodyObject, {}));
}

TEST_F(Boss03CounterTest, OneWingExecutionKeepsTheRemainingBodyPunishWindow)
{
    LandCounter();
    auto& wing = m_scene.CreateGameObject("HB_Wing_L_Upper");
    wing.SetParent(*m_root);
    auto& part = wing.AddScript<sandbox::BossPartComponent>();
    part.SetContext(&m_scene, &wing);
    EXPECT_TRUE(m_boss->CanExecute());
    const int before = m_health->Current();
    EXPECT_TRUE(m_ai->Execute(&wing, {}));
    EXPECT_EQ(m_health->Current(), before - 140);
    EXPECT_TRUE(m_ai->IsToppled());
    EXPECT_FALSE(m_boss->CanExecute());
    EXPECT_FALSE(m_ai->Execute(&wing, {}));
    EXPECT_TRUE(m_ai->ApplyBodyDamage(100));
}

TEST_F(Boss03CounterTest, BodyHealthAlsoAdvancesPhaseAndCocoonRejectsDamage)
{
    m_health->ApplyDamage(701);
    EXPECT_EQ(m_ai->CurrentPhase(), 2);
    // 繭へ入るのは待機を撃ち切った次の手を選ぶ瞬間。待機より短く進めても開いたまま。
    Step(m_ai->idleSeconds + 0.01f);
    const int before = m_health->Current();
    EXPECT_FALSE(m_body->CanBeHit());
    EXPECT_FALSE(m_ai->ApplyBodyDamage(100));
    EXPECT_EQ(m_health->Current(), before);
}
} // namespace fbzz::tests
