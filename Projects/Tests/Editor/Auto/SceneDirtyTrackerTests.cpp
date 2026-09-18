/// @file    SceneDirtyTrackerTests.cpp
/// @brief   シーンに未保存の変更があるかの判定。
/// @author  Hasegawa Jin
/// @date    2026-09-10
///
/// この判定が «変更なし» に倒れると、閉じるときに何も聞かれず作業が消える。
/// 逆に倒れると、何もしていないのに毎回保存を促されて «どうせ嘘» と学習してしまい、
/// 本当に消えるときにも無視される。どちらも実害が出るので両側を固定する。
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>

#include <Editor/Util/SceneDirtyTracker.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>

namespace fbzz::tests {

using editor::SceneDirtyTracker;

/// SceneDirtyTracker は SceneIO::Serialize を通してシーンをハッシュする。
/// その一時ファイルの置き場所を用意するため EditorFixture に載せる。
class SceneDirtyTrackerTest : public testkit::EditorFixture {};

TEST_F(SceneDirtyTrackerTest, StartsClean)
{
    SceneDirtyTracker tracker;
    EXPECT_FALSE(tracker.IsDirty());
}

TEST_F(SceneDirtyTrackerTest, StaysCleanWhenNothingChanges)
{
    scene::Scene scene;
    scene.CreateGameObject("Player");

    SceneDirtyTracker tracker;
    tracker.CaptureClean(scene);

    EXPECT_FALSE(tracker.Evaluate(scene));
    EXPECT_FALSE(tracker.IsDirty());
}

TEST_F(SceneDirtyTrackerTest, DetectsAnAddedObject)
{
    scene::Scene scene;
    SceneDirtyTracker tracker;
    tracker.CaptureClean(scene);

    scene.CreateGameObject("Player");

    EXPECT_TRUE(tracker.Evaluate(scene));
}

TEST_F(SceneDirtyTrackerTest, DetectsARename)
{
    scene::Scene scene;
    scene::GameObject& object = scene.CreateGameObject("Player");
    SceneDirtyTracker tracker;
    tracker.CaptureClean(scene);

    object.name = "Enemy";

    EXPECT_TRUE(tracker.Evaluate(scene));
}

TEST_F(SceneDirtyTrackerTest, DetectsAMove)
{
    scene::Scene scene;
    scene::GameObject& object = scene.CreateGameObject("Player");
    SceneDirtyTracker tracker;
    tracker.CaptureClean(scene);

    object.transform.position = { 1.0f, 2.0f, 3.0f };

    EXPECT_TRUE(tracker.Evaluate(scene));
}

TEST_F(SceneDirtyTrackerTest, DetectsAnActiveToggle)
{
    scene::Scene scene;
    scene::GameObject& object = scene.CreateGameObject("Player");
    SceneDirtyTracker tracker;
    tracker.CaptureClean(scene);

    object.SetActive(false);

    EXPECT_TRUE(tracker.Evaluate(scene));
}

TEST_F(SceneDirtyTrackerTest, GoesCleanAgainAfterCapturingTheNewState)
{
    /// @note 保存したら «今の姿» が新しい基準になる。ここが効かないと保存後も促され続ける。
    scene::Scene scene;
    SceneDirtyTracker tracker;
    tracker.CaptureClean(scene);
    scene.CreateGameObject("Player");
    ASSERT_TRUE(tracker.Evaluate(scene));

    tracker.CaptureClean(scene);

    EXPECT_FALSE(tracker.Evaluate(scene));
}

TEST_F(SceneDirtyTrackerTest, ReturnsToCleanWhenTheEditIsUndone)
{
    /// @note 追加して消せば元の姿。Undo で戻したのに «変更あり» のままだと嘘になる。
    scene::Scene scene;
    scene.CreateGameObject("Keep");
    SceneDirtyTracker tracker;
    tracker.CaptureClean(scene);

    scene::GameObject& added = scene.CreateGameObject("Temp");
    ASSERT_TRUE(tracker.Evaluate(scene));

    scene.DestroyGameObject(added.GetID());

    EXPECT_FALSE(tracker.Evaluate(scene));
}

TEST_F(SceneDirtyTrackerTest, MarkDirtyForcesTheFlagWithoutLookingAtTheScene)
{
    scene::Scene scene;
    SceneDirtyTracker tracker;
    tracker.CaptureClean(scene);

    tracker.MarkDirty();

    EXPECT_TRUE(tracker.IsDirty());
}

TEST_F(SceneDirtyTrackerTest, EvaluateOverridesAManualMarkWhenTheSceneIsUnchanged)
{
    /// @note MarkDirty はシリアライズに出ない変更を拾う逃げ道だが、次の Evaluate で
    ///       «中身は同じ» と判定されれば取り消される。この性質を知らずに MarkDirty だけに
    ///       頼ると、保存を促す機会を落とす。
    scene::Scene scene;
    SceneDirtyTracker tracker;
    tracker.CaptureClean(scene);
    tracker.MarkDirty();

    EXPECT_FALSE(tracker.Evaluate(scene));
}

TEST_F(SceneDirtyTrackerTest, ResetForgetsTheBaseline)
{
    scene::Scene scene;
    SceneDirtyTracker tracker;
    tracker.CaptureClean(scene);
    scene.CreateGameObject("Player");
    ASSERT_TRUE(tracker.Evaluate(scene));

    tracker.Reset();

    EXPECT_FALSE(tracker.IsDirty());
    /// @note 基準が無いので、次の Evaluate は «今の姿» を基準として取り直す。
    EXPECT_FALSE(tracker.Evaluate(scene));
}

TEST_F(SceneDirtyTrackerTest, EvaluateWithoutABaselineAdoptsTheCurrentScene)
{
    /// @note 基準を取らないまま呼ばれても «変更あり» と言わない。開いた直後がこれ。
    scene::Scene scene;
    scene.CreateGameObject("Player");

    SceneDirtyTracker tracker;
    EXPECT_FALSE(tracker.Evaluate(scene));

    scene.CreateGameObject("Enemy");
    EXPECT_TRUE(tracker.Evaluate(scene));
}

} // namespace fbzz::tests
