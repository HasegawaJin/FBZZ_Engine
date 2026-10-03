/// @file    SceneCapacityScriptsTests.cpp
/// @brief   実ゲームの HUD とタイトル演出が容量不足を部分生成せず扱う契約。
/// @author  Hasegawa Jin
/// @date    2026-10-02
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Scripts/Combat/Boss03HitboxRigComponent.hpp>
#include <Scripts/Combat/BossBeamComponent.hpp>
#include <Scripts/Combat/BossCollapsePostureComponent.hpp>
#include <Scripts/Combat/BossHitboxRigComponent.hpp>
#include <Scripts/Combat/BossLegHealthBarComponent.hpp>
#include <Scripts/Combat/BossRigComponent.hpp>
#include <Scripts/Combat/BossShockwaveComponent.hpp>
#include <Scripts/Combat/BossTelegraphComponent.hpp>
#include <Scripts/Combat/LaserVolleyComponent.hpp>
#include <Scripts/Combat/SerpentAiComponent.hpp>
#include <Scripts/Combat/SerpentBodyComponent.hpp>
#include <Scripts/Combat/SerpentHitboxRigComponent.hpp>
#include <Scripts/Player/AimMarkerComponent.hpp>
#include <Scripts/Player/PlayerClimbComponent.hpp>
#include <Scripts/Player/PlayerHeadLookComponent.hpp>
#include <Scripts/SceneManagerScript.hpp>
#include <Scripts/Utils/BeatVoice.hpp>
#include <Scripts/Utils/ElectricArc.hpp>
#include <Scripts/Vfx/BladeTrailComponent.hpp>
#include <Scripts/Vfx/Boss03VfxComponent.hpp>
#include <Scripts/Vfx/PartDamageHudComponent.hpp>
#include <Scripts/Vfx/SlashCutFxComponent.hpp>
#include <Scripts/Vfx/SlashScarComponent.hpp>
#include <Scripts/Vfx/SpinSlashFxComponent.hpp>

namespace fbzz::tests {

class GreenWareSceneCapacityTest : public testkit::EngineFixture {
protected:
    bool FillLeaving(std::size_t available)
    {
        while (m_scene.RemainingEntityCapacity() > available) {
            auto* filler = m_scene.TryCreateGameObject("Existing");
            if (!filler) return false;
            m_lastFiller = filler->GetID();
        }
        return true;
    }

    scene::Scene m_scene;
    scene::EntityID m_lastFiller;
};

TEST_F(GreenWareSceneCapacityTest, AimMarkerRejectsPartialHudAndRetriesAtTheExactBoundary)
{
    auto& owner = m_scene.CreateGameObject("Player");
    sandbox::AimMarkerComponent marker;
    marker.SetContext(&m_scene, &owner);
    ASSERT_TRUE(FillLeaving(8));
    const auto count = m_scene.GameObjectCount();
    const auto canvasName = "AimMarker_" + owner.instanceId;

    marker.OnStart();

    EXPECT_EQ(m_scene.GameObjectCount(), count);
    EXPECT_EQ(m_scene.Find(canvasName), nullptr);
    ASSERT_TRUE(m_scene.DestroyGameObject(m_lastFiller));
    marker.OnStart();
    auto* canvas = m_scene.Find(canvasName);
    ASSERT_NE(canvas, nullptr);
    EXPECT_EQ(canvas->GetChildCount(), 8);
    EXPECT_EQ(m_scene.RemainingEntityCapacity(), 0u);
    marker.OnStart();
    EXPECT_EQ(m_scene.Find(canvasName), canvas);
    EXPECT_EQ(m_scene.GameObjectCount(), scene::Scene::MAX_ENTITIES);
}

TEST_F(GreenWareSceneCapacityTest, BossLegBarsDoNotPublishAnIncompleteSet)
{
    auto& owner = m_scene.CreateGameObject("Boss");
    owner.AddScript<sandbox::BossRigComponent>();
    sandbox::BossLegHealthBarComponent bars;
    bars.SetContext(&m_scene, &owner);
    ASSERT_TRUE(FillLeaving(11));
    const auto count = m_scene.GameObjectCount();
    const auto firstName = "BossLegBar_" + owner.instanceId + "_0";

    bars.OnStart();

    EXPECT_TRUE(bars.enabled);
    EXPECT_EQ(m_scene.GameObjectCount(), count);
    EXPECT_EQ(m_scene.Find(firstName), nullptr);
    ASSERT_TRUE(m_scene.DestroyGameObject(m_lastFiller));
    bars.OnStart();
    ASSERT_NE(m_scene.Find(firstName), nullptr);
    for (int leg = 0; leg < 4; ++leg) {
        auto* canvas = m_scene.Find("BossLegBar_" + owner.instanceId + "_" + std::to_string(leg));
        ASSERT_NE(canvas, nullptr);
        EXPECT_EQ(canvas->GetChildCount(), 2);
    }
    EXPECT_EQ(m_scene.RemainingEntityCapacity(), 0u);
}

TEST_F(GreenWareSceneCapacityTest, TitleGlyphRetriesWithoutLeavingPartialParts)
{
    auto& object = m_scene.CreateGameObject("Electrode");
    scene::Script owner;
    owner.SetContext(&m_scene, &object);
    sandbox::ElectrodeCoreGlyph glyph;
    sandbox::ElectrodeCoreStyle style;
    style.size = 1.0f;
    ASSERT_TRUE(FillLeaving(2));
    const auto count = m_scene.GameObjectCount();

    glyph.Update(owner, sandbox::Pole::Plus, math::Vector3::ZERO, style, 0.0f);

    EXPECT_EQ(m_scene.GameObjectCount(), count);
    EXPECT_EQ(m_scene.Find("ElectrodeCore_Part"), nullptr);
    ASSERT_TRUE(m_scene.DestroyGameObject(m_lastFiller));
    glyph.Update(owner, sandbox::Pole::Plus, math::Vector3::ZERO, style, 0.0f);
    EXPECT_EQ(m_scene.RemainingEntityCapacity(), 0u);
    EXPECT_EQ(m_scene.GetEntities<scene::LineRendererComponent>().size(), 3u);
    glyph.Update(owner, sandbox::Pole::Plus, math::Vector3::ZERO, style, 0.0f);
    EXPECT_EQ(m_scene.GetEntities<scene::LineRendererComponent>().size(), 3u);
}

} /// @note namespace fbzz::tests
