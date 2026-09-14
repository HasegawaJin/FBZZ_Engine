/// @file    SerpentHitboxTests.cpp
/// @brief   蛇の生成ヒットボックスが斬撃対象になり、リグ再構築でも耐久を失わないことを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-12
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Scripts/Combat/SerpentHitboxRigComponent.hpp>

namespace fbzz::tests {

class SerpentHitboxTest : public testkit::EngineFixture {
protected:
    scene::Scene m_scene;

    sandbox::SerpentHitboxRigComponent& BuildRig()
    {
        const auto rootId = m_scene.CreateGameObject("Boss02").GetID();
        auto parentId = rootId;
        for (int i = sandbox::serpent::kSegmentCount; i >= 0; --i) {
            auto& bone = m_scene.CreateGameObject(sandbox::serpent::BoneName(i));
            bone.SetParent(*m_scene.GetGameObject(parentId));
            bone.transform.position = {0.0f, 0.8f, 0.0f};
            parentId = bone.GetID();
        }
        auto* root = m_scene.GetGameObject(rootId);
        auto& rig = root->AddScript<sandbox::SerpentHitboxRigComponent>();
        rig.SetContext(&m_scene, root);
        rig.segmentHealth = 45;
        rig.OnStart();
        return rig;
    }
};

TEST_F(SerpentHitboxTest, EveryBodySegmentAcceptsSlashDamage)
{
    auto& rig = BuildRig();
    ASSERT_TRUE(rig.IsBuilt());

    for (int i = 1; i <= sandbox::serpent::kSegmentCount; ++i) {
        auto* hitbox = rig.SegmentHitbox(i);
        ASSERT_NE(hitbox, nullptr);
        auto* part = hitbox->GetScript<sandbox::BossPartComponent>();
        ASSERT_NE(part, nullptr) << "segment " << i;
        EXPECT_EQ(part->legSuffix, sandbox::serpent::PartSuffix(i));
        EXPECT_EQ(rig.SegmentOf(hitbox), i);
        EXPECT_NE(sandbox::SerpentHitboxRigComponent::SerpentRootOf(hitbox), nullptr);
        EXPECT_FALSE(part->Damage(25));
        EXPECT_EQ(part->Health(), 20);
        EXPECT_TRUE(part->Damage(25));
        EXPECT_TRUE(part->IsDepleted());
    }
}

TEST_F(SerpentHitboxTest, RebuildingPreservesDamagedAndBrokenParts)
{
    auto& rig = BuildRig();
    ASSERT_TRUE(rig.IsBuilt());
    ASSERT_NE(rig.SegmentHitbox(1), nullptr);
    ASSERT_NE(rig.SegmentHitbox(2), nullptr);
    auto* damaged = rig.SegmentHitbox(1)->GetScript<sandbox::BossPartComponent>();
    auto* broken = rig.SegmentHitbox(2)->GetScript<sandbox::BossPartComponent>();
    ASSERT_NE(damaged, nullptr);
    ASSERT_NE(broken, nullptr);
    const auto hitboxId = rig.SegmentHitbox(1)->GetID();
    damaged->Damage(10);
    broken->Break();

    rig.OnStart();

    EXPECT_EQ(rig.SegmentHitbox(1)->GetID(), hitboxId);
    EXPECT_EQ(rig.SegmentHitbox(1)->GetScript<sandbox::BossPartComponent>(), damaged);
    EXPECT_EQ(damaged->Health(), 35);
    EXPECT_TRUE(broken->IsBroken());
    ASSERT_NE(rig.SegmentHitbox(0), nullptr);
    EXPECT_EQ(rig.SegmentHitbox(0)->GetScript<sandbox::BossPartComponent>(), nullptr);
}

} // namespace fbzz::tests
