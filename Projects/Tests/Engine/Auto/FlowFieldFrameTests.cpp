/// @file    FlowFieldFrameTests.cpp
/// @brief   Scene の «流れ» フレームキャッシュが、フレームごとに 1 回だけ集め直すこと。
/// @author  Hasegawa Jin
/// @date    2026-09-16
///
/// @brief 収集を 1 か所へ畳んだ目的は «同じフレーム内では誰が引いても同じ値» にすること。
/// @brief 同じフレームで集め直すと、CPU 粒子と GPU 粒子と描画パスが別々の場を見る余地が戻る。
/// @brief 逆にフレームが進んでも集め直さないと、動く力場が 1 フレーム古いまま凍る。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Environment/SceneEnvironment.hpp>
#include <Engine/Scene/Fields/FlowFieldEval.hpp>
#include <Engine/Scene/Fields/FlowFieldFrame.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>

namespace fbzz::tests {
namespace {

scene::GameObject& AddUniformField(scene::Scene& scene, const char* name, float speed)
{
    scene::GameObject& go = scene.CreateGameObject(name);
    scene::FlowFieldSettings uniform;
    uniform.fieldType = scene::FlowFieldType::Uniform;
    uniform.direction = { 1.0f, 0.0f, 0.0f };
    uniform.strength  = speed;
    uniform.radius    = 0.0f;
    scene::FlowField field;
    field.forces = { uniform };
    go.AddComponent<scene::FlowField>(std::move(field));
    scene::FlushWorldTransforms(scene);
    return go;
}

} // namespace

class FlowFieldFrameTest : public testkit::EngineFixture {};

TEST_F(FlowFieldFrameTest, ConsumersShareAnImmutableSnapshotAcrossRegather)
{
    scene::Scene scene;
    AddUniformField(scene, "First", 2.0f);
    const auto old = scene.FlowFrame().fields;
    EXPECT_EQ(old.get(), scene.FlowFrame().fields.get());
    AddUniformField(scene, "Second", 3.0f);
    scene.InvalidateFlowFrame();
    const auto current = scene.FlowFrame().fields;
    EXPECT_NE(old.get(), current.get());
    EXPECT_EQ(old->size(), 1u);
    EXPECT_EQ(current->size(), 2u);
}

TEST_F(FlowFieldFrameTest, TheSameFrameReusesTheGatheredFlow)
{
    scene::Scene scene;
    AddUniformField(scene, "Breeze", 3.0f);

    const std::uint64_t frame = Time::frameCount;
    ASSERT_EQ(scene.FlowFrame().fields->size(), 1u);

    /// @note 同じフレームで場を 1 本足しても、キャッシュは集め直さない。
    AddUniformField(scene, "Breeze2", 4.0f);
    EXPECT_EQ(scene.FlowFrame().fields->size(), 1u)
        << "同じフレームで集め直すと、CPU / GPU / 描画パスが別々の場を見る余地が戻る";
    EXPECT_EQ(Time::frameCount, frame);
}

TEST_F(FlowFieldFrameTest, ANewFrameGathersAgain)
{
    scene::Scene scene;
    AddUniformField(scene, "Breeze", 3.0f);
    ASSERT_EQ(scene.FlowFrame().fields->size(), 1u);

    AddUniformField(scene, "Breeze2", 4.0f);
    ++Time::frameCount;
    EXPECT_EQ(scene.FlowFrame().fields->size(), 2u)
        << "フレームが進んだら集め直さないと、動く場が 1 フレーム古いまま凍る";
}

TEST_F(FlowFieldFrameTest, InvalidateForcesAGatherInTheSameFrame)
{
    scene::Scene scene;
    AddUniformField(scene, "Breeze", 3.0f);
    ASSERT_EQ(scene.FlowFrame().fields->size(), 1u);

    AddUniformField(scene, "Breeze2", 4.0f);
    scene.InvalidateFlowFrame();
    EXPECT_EQ(scene.FlowFrame().fields->size(), 2u);
}

TEST_F(FlowFieldFrameTest, TheAmbientFlowIsAlsoAFieldParticlesCanSample)
{
    /// @note 環境流を «要約 (雲・水面)» にしか出さないと、粒子だけが風を受けなくなる。
    scene::Scene scene;
    scene::SceneEnvironment& environment = scene.Environment();
    environment.enabled    = true;
    environment.direction  = { 1.0f, 0.0f, 0.0f };
    environment.speed      = 6.0f;
    environment.turbulence = 0.0f;
    scene.InvalidateFlowFrame();

    const scene::FlowFieldFrame& frame = scene.FlowFrame();
    EXPECT_TRUE(frame.ambient.active);
    EXPECT_NEAR(frame.ambient.speed, 6.0f, testkit::kTolerance);
    ASSERT_EQ(frame.fields->size(), 1u);

    const math::Vector3 flow =
        scene::SampleFlow({ 100.0f, 0.0f, 0.0f }, *frame.fields, 0xFFFFFFFFu, 0.0f);
    EXPECT_VEC3_NEAR(flow, math::Vector3(6.0f, 0.0f, 0.0f), testkit::kTolerance);
}

TEST_F(FlowFieldFrameTest, SceneFieldsPrecedeTheAmbientFlow)
{
    /// @note 環境風を AmbientWind で受ける消費者 (繊維) は、先頭 sceneFieldCount 本だけを場として読む。
    ///       境界がずれると環境風を二重に受けるか、置いた場を取りこぼす。
    scene::Scene scene;
    AddUniformField(scene, "Local", 2.0f);
    scene::SceneEnvironment& environment = scene.Environment();
    environment.enabled    = true;
    environment.direction  = { 0.0f, 0.0f, 1.0f };
    environment.speed      = 5.0f;
    environment.turbulence = 1.0f;
    scene.InvalidateFlowFrame();

    const scene::FlowFieldFrame& frame = scene.FlowFrame();
    ASSERT_EQ(frame.fields->size(), 3u);
    ASSERT_EQ(frame.sceneFieldCount, 1u);
    EXPECT_NEAR((*frame.fields)[0].strength, 2.0f, testkit::kTolerance);
    EXPECT_EQ((*frame.fields)[1].type, scene::FlowFieldType::Uniform);
    EXPECT_EQ((*frame.fields)[2].type, scene::FlowFieldType::Curl);

    environment.enabled = false;
    scene.InvalidateFlowFrame();
    EXPECT_EQ(scene.FlowFrame().sceneFieldCount, scene.FlowFrame().fields->size());
}

TEST_F(FlowFieldFrameTest, APointOutsideEveryRadiusIsNotCovered)
{
    /// @note «場が無い» は «流速 0 の静止した空気» ではない。ここを覆いとして数えると、
    ///       場を 1 つ置いただけで半径の外の粒子まで一斉に減速する。
    scene::Scene scene;
    scene::GameObject& go = scene.CreateGameObject("Local Swirl");
    scene::FlowFieldSettings local;
    local.fieldType = scene::FlowFieldType::Uniform;
    local.direction = { 0.0f, 1.0f, 0.0f };
    local.strength  = 2.0f;
    local.radius    = 1.0f;
    scene::FlowField field;
    field.forces = { local };
    go.AddComponent<scene::FlowField>(std::move(field));
    scene::FlushWorldTransforms(scene);
    scene.InvalidateFlowFrame();

    const std::vector<scene::ActiveFlowField>& fields = *scene.FlowFrame().fields;
    bool covered = true;
    (void)scene::SampleFlow({ 50.0f, 0.0f, 0.0f }, fields, 0xFFFFFFFFu, 0.0f, &covered);
    EXPECT_FALSE(covered);

    math::Vector3 velocity = { 10.0f, 0.0f, 0.0f };
    scene::ApplyFlowFields(fields, 0xFFFFFFFFu, { 50.0f, 0.0f, 0.0f }, velocity,
                           1.0f / 60.0f, 0.0f, scene::kDefaultFlowCoupling);
    EXPECT_VEC3_NEAR(velocity, math::Vector3(10.0f, 0.0f, 0.0f), testkit::kTolerance);
}

} // namespace fbzz::tests
