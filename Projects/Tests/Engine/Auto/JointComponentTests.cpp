/// @file    JointComponentTests.cpp
/// @brief   JointComponent がシーンを往復し、physics::World へ張られて距離を保つことを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// Physics 側の制約 (ロープ・鎖・ヒンジ) は前から実装済みだったが、Scene からの
/// 入口が無かった。橋渡しで壊れるのは 2 か所しかない:
///
///   1. Reflect() のキーが片側だけ抜ける → 保存はできるのに開くと既定値へ戻る。
///      しかも «たまたま既定値でも動く» ので、エラーも警告も出ないまま消える。
///   2. 申告の寿命がずれる → 無効にした関節が残って «外したのに垂れない»、
///      または毎フレーム作り直して基準姿勢が更新され続け «溶接が効かない»。
///
/// どちらも «少しおかしい» の形で表面化して原因が読めないので、通しで押さえる。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Scene/Components/JointComponent.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/Systems/JointSync.hpp>

#include <Math/Vector3.hpp>
#include <Physics/World.hpp>

#include <memory>
#include <string>

namespace fbzz::tests {
namespace {

scene::GameObject* FindObject(scene::Scene& scene, const std::string& name)
{
    for (auto& candidate : scene.GameObjects())
        if (candidate.name == name) return &candidate;
    return nullptr;
}

/// 剛体を持つ GameObject を作る。位置は物理側を正として直接入れる。
scene::GameObject& MakeBody(scene::Scene& scene,
                            const std::string& name,
                            const math::Vector3& position,
                            bool isStatic)
{
    scene::GameObject& go = scene.CreateGameObject(name);
    go.AddComponent<scene::RigidBodyComponent>();
    // AddComponent の戻り参照は他の追加で無効になりうるので引き直す。
    auto* rb = go.GetComponent<scene::RigidBodyComponent>();
    rb->rigidBody = std::make_unique<physics::RigidBody>();
    rb->rigidBody->m_isStatic = isStatic;
    rb->rigidBody->SetMass(1.0f);
    rb->rigidBody->SetPosition(position);
    go.transform.position = position;
    go.transform.worldPosition = position;
    return go;
}

physics::RigidBody* BodyOf(scene::GameObject& go)
{
    auto* rb = go.GetComponent<scene::RigidBodyComponent>();
    return rb ? rb->rigidBody.get() : nullptr;
}

} // namespace

class JointComponentTest : public testkit::EngineFixture {
protected:
    /// PhysicsSystem::Update と同じ順で 1 フレーム進める。
    /// (BeginSceneSync → 剛体 → 関節 → EndSceneSync → Step)
    void StepPhysics(scene::Scene& scene, physics::World& world, int frames = 1)
    {
        for (int i = 0; i < frames; ++i) {
            world.BeginSceneSync();
            for (scene::EntityID id : scene.GetEntities<scene::RigidBodyComponent>()) {
                auto* rb = scene.GetComponent<scene::RigidBodyComponent>(id);
                if (!rb || !rb->enabled || !rb->rigidBody) continue;
                rb->bodyHandle = world.SyncBody(rb->bodyHandle, rb->rigidBody.get());
            }
            scene::SyncJointComponents(scene, world);
            world.EndSceneSync();
            world.Step(testkit::kFixedDeltaTime);
        }
    }
};

// --- シーン往復 -------------------------------------------------------------

TEST_F(JointComponentTest, KeepsEveryAuthoredFieldAcrossASave)
{
    scene::Scene source;
    scene::GameObject& anchor = source.CreateGameObject("Anchor");
    scene::GameObject& hanging = source.CreateGameObject("Hanging");

    scene::JointComponent joint{};
    joint.enabled          = false;
    joint.type             = scene::JointType::Hinge;
    joint.connectedBody.id = anchor.GetID();
    joint.connectToParent  = false;
    joint.solverIterations = 7;
    joint.autoDistance     = false;
    joint.distance         = 2.5f;
    joint.spring           = 33.0f;
    joint.damping          = 0.125f;
    joint.axis             = { 1.0f, 0.0f, 0.0f };
    joint.anchor           = { 0.0f, 0.5f, 0.0f };
    joint.connectedAnchor  = { 0.0f, -0.5f, 0.0f };
    joint.useLimits        = true;
    joint.lowerLimit       = -45.0f;
    joint.upperLimit       = 90.0f;
    joint.useMotor         = true;
    joint.motorSpeed       = 120.0f;
    joint.motorMaxTorque   = 6.5f;
    hanging.AddComponent<scene::JointComponent>(joint);

    const std::string text = scene::SceneSerializer::SaveToText(source);
    ASSERT_FALSE(text.empty());

    const std::unique_ptr<scene::Scene> loaded =
        scene::SceneSerializer::LoadDataFromText(text, "");
    ASSERT_NE(loaded, nullptr);

    scene::GameObject* restoredObject = FindObject(*loaded, "Hanging");
    ASSERT_NE(restoredObject, nullptr);
    const auto* restored = restoredObject->GetComponent<scene::JointComponent>();
    ASSERT_NE(restored, nullptr);

    EXPECT_FALSE(restored->enabled);
    EXPECT_EQ(restored->type, scene::JointType::Hinge);
    EXPECT_FALSE(restored->connectToParent);
    EXPECT_EQ(restored->solverIterations, 7);
    EXPECT_FALSE(restored->autoDistance);
    EXPECT_NEAR(restored->distance, 2.5f, testkit::kTolerance);
    EXPECT_NEAR(restored->spring, 33.0f, testkit::kTolerance);
    EXPECT_NEAR(restored->damping, 0.125f, testkit::kTolerance);
    EXPECT_VEC3_NEAR(restored->axis, math::Vector3(1.0f, 0.0f, 0.0f), testkit::kTolerance);
    EXPECT_VEC3_NEAR(restored->anchor, math::Vector3(0.0f, 0.5f, 0.0f), testkit::kTolerance);
    EXPECT_VEC3_NEAR(restored->connectedAnchor, math::Vector3(0.0f, -0.5f, 0.0f),
                     testkit::kTolerance);
    EXPECT_TRUE(restored->useLimits);
    EXPECT_NEAR(restored->lowerLimit, -45.0f, testkit::kTolerance);
    EXPECT_NEAR(restored->upperLimit, 90.0f, testkit::kTolerance);
    EXPECT_TRUE(restored->useMotor);
    EXPECT_NEAR(restored->motorSpeed, 120.0f, testkit::kTolerance);
    EXPECT_NEAR(restored->motorMaxTorque, 6.5f, testkit::kTolerance);
}

TEST_F(JointComponentTest, KeepsTheConnectedBodyReference)
{
    // 相手は EntityID (実行時の添字) ではなく instanceId で保存される。
    // ここが崩れると、開き直した瞬間に全部の関節が «相手なし» になる。
    scene::Scene source;
    scene::GameObject& anchor  = source.CreateGameObject("Anchor");
    scene::GameObject& hanging = source.CreateGameObject("Hanging");

    scene::JointComponent joint{};
    joint.type             = scene::JointType::Rope;
    joint.connectedBody.id = anchor.GetID();
    hanging.AddComponent<scene::JointComponent>(joint);

    const std::string text = scene::SceneSerializer::SaveToText(source);
    const std::unique_ptr<scene::Scene> loaded =
        scene::SceneSerializer::LoadDataFromText(text, "");
    ASSERT_NE(loaded, nullptr);

    scene::GameObject* restoredObject = FindObject(*loaded, "Hanging");
    ASSERT_NE(restoredObject, nullptr);
    const auto* restored = restoredObject->GetComponent<scene::JointComponent>();
    ASSERT_NE(restored, nullptr);
    ASSERT_TRUE(restored->connectedBody.IsValid());

    scene::GameObject* resolved = restored->connectedBody.Resolve(*loaded);
    ASSERT_NE(resolved, nullptr);
    EXPECT_EQ(resolved->name, "Anchor");
}

TEST_F(JointComponentTest, KeepsTheChainBodyList)
{
    scene::Scene source;
    scene::GameObject& head = source.CreateGameObject("Head");
    scene::GameObject& mid  = source.CreateGameObject("Mid");
    scene::GameObject& tail = source.CreateGameObject("Tail");

    scene::JointComponent joint{};
    joint.type        = scene::JointType::Chain;
    joint.chainBodies = { scene::EntityRef{ mid.GetID() }, scene::EntityRef{ tail.GetID() } };
    head.AddComponent<scene::JointComponent>(joint);

    const std::string text = scene::SceneSerializer::SaveToText(source);
    const std::unique_ptr<scene::Scene> loaded =
        scene::SceneSerializer::LoadDataFromText(text, "");
    ASSERT_NE(loaded, nullptr);

    scene::GameObject* restoredObject = FindObject(*loaded, "Head");
    ASSERT_NE(restoredObject, nullptr);
    const auto* restored = restoredObject->GetComponent<scene::JointComponent>();
    ASSERT_NE(restored, nullptr);
    ASSERT_EQ(restored->chainBodies.size(), 2u);

    scene::GameObject* first  = restored->chainBodies[0].Resolve(*loaded);
    scene::GameObject* second = restored->chainBodies[1].Resolve(*loaded);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(first->name, "Mid");
    EXPECT_EQ(second->name, "Tail");
}

TEST_F(JointComponentTest, DoesNotKeepTheRuntimeHandleWhenCopied)
{
    // 複製で同じスロットを 2 つのコンポーネントが指すと、片方を消したときに
    // もう片方の関節が黙って消える。
    scene::JointComponent joint{};
    joint.constraintHandle = { 3u, 1u };
    joint.connected        = true;

    const scene::JointComponent copy = joint;

    EXPECT_FALSE(copy.constraintHandle.IsValid());
    EXPECT_FALSE(copy.connected);
}

// --- 物理への橋渡し ---------------------------------------------------------

TEST_F(JointComponentTest, RegistersOneConstraintInTheWorld)
{
    scene::Scene scene;
    physics::World world;
    scene::GameObject& anchor  = MakeBody(scene, "Anchor", math::Vector3::ZERO, true);
    scene::GameObject& hanging = MakeBody(scene, "Hanging", { 0.0f, -0.5f, 0.0f }, false);

    scene::JointComponent joint{};
    joint.type             = scene::JointType::Rope;
    joint.connectedBody.id = anchor.GetID();
    joint.autoDistance     = false;
    joint.distance         = 1.0f;
    hanging.AddComponent<scene::JointComponent>(joint);

    StepPhysics(scene, world);

    ASSERT_EQ(world.GetConstraints().size(), 1u);
    EXPECT_EQ(world.GetConstraints()[0]->GetType(), physics::ConstraintType::ROPE);
    const auto* live = hanging.GetComponent<scene::JointComponent>();
    ASSERT_NE(live, nullptr);
    EXPECT_TRUE(live->connected);
}

TEST_F(JointComponentTest, DoesNotGrowTheConstraintCountAcrossFrames)
{
    // 毎フレーム «作り直す» と、基準姿勢を抱える Fixed / Hinge が効かなくなる。
    // 数が増えないことは «同じ制約を生かし続けている» ことの外から見える証拠。
    scene::Scene scene;
    physics::World world;
    scene::GameObject& anchor  = MakeBody(scene, "Anchor", math::Vector3::ZERO, true);
    scene::GameObject& hanging = MakeBody(scene, "Hanging", { 0.0f, -1.0f, 0.0f }, false);

    scene::JointComponent joint{};
    joint.type             = scene::JointType::Fixed;
    joint.connectedBody.id = anchor.GetID();
    hanging.AddComponent<scene::JointComponent>(joint);

    StepPhysics(scene, world, 30);

    EXPECT_EQ(world.GetConstraints().size(), 1u);
}

TEST_F(JointComponentTest, ReleasesTheConstraintWhenTheJointIsDisabled)
{
    scene::Scene scene;
    physics::World world;
    scene::GameObject& anchor  = MakeBody(scene, "Anchor", math::Vector3::ZERO, true);
    scene::GameObject& hanging = MakeBody(scene, "Hanging", { 0.0f, -1.0f, 0.0f }, false);

    scene::JointComponent joint{};
    joint.type             = scene::JointType::Distance;
    joint.connectedBody.id = anchor.GetID();
    hanging.AddComponent<scene::JointComponent>(joint);

    StepPhysics(scene, world);
    ASSERT_EQ(world.GetConstraints().size(), 1u);

    hanging.GetComponent<scene::JointComponent>()->enabled = false;
    StepPhysics(scene, world);

    EXPECT_TRUE(world.GetConstraints().empty());
    EXPECT_FALSE(hanging.GetComponent<scene::JointComponent>()->connected);
}

TEST_F(JointComponentTest, DoesNotConnectWhenThePartnerHasNoRigidBody)
{
    scene::Scene scene;
    physics::World world;
    scene::GameObject& target  = scene.CreateGameObject("NoBody");
    scene::GameObject& hanging = MakeBody(scene, "Hanging", { 0.0f, -1.0f, 0.0f }, false);

    scene::JointComponent joint{};
    joint.type             = scene::JointType::Distance;
    joint.connectedBody.id = target.GetID();
    hanging.AddComponent<scene::JointComponent>(joint);

    StepPhysics(scene, world);

    EXPECT_TRUE(world.GetConstraints().empty());
    EXPECT_FALSE(hanging.GetComponent<scene::JointComponent>()->connected);
}

// --- 距離が保たれる ---------------------------------------------------------

TEST_F(JointComponentTest, ADistanceJointHoldsTheGapWhileFalling)
{
    scene::Scene scene;
    physics::World world;
    scene::GameObject& anchor  = MakeBody(scene, "Anchor", math::Vector3::ZERO, true);
    scene::GameObject& hanging = MakeBody(scene, "Hanging", { 0.0f, -1.0f, 0.0f }, false);

    scene::JointComponent joint{};
    joint.type             = scene::JointType::Distance;
    joint.connectedBody.id = anchor.GetID();
    joint.autoDistance     = false;
    joint.distance         = 1.0f;
    hanging.AddComponent<scene::JointComponent>(joint);

    StepPhysics(scene, world, 120);

    const physics::RigidBody* body = BodyOf(hanging);
    ASSERT_NE(body, nullptr);
    // 静的な吊り元は動かない。距離だけが保たれる。
    EXPECT_VEC3_NEAR(BodyOf(anchor)->GetPosition(), math::Vector3::ZERO, testkit::kTolerance);
    EXPECT_NEAR(body->GetPosition().Length(), 1.0f, testkit::kLooseTolerance);
}

TEST_F(JointComponentTest, ARopeJointClampsOnlyTheMaximumLength)
{
    scene::Scene scene;
    physics::World world;
    scene::GameObject& anchor  = MakeBody(scene, "Anchor", math::Vector3::ZERO, true);
    // 最大長より内側から落とす。たるんでいる間は何もせず、伸び切ったところで止まる。
    scene::GameObject& hanging = MakeBody(scene, "Hanging", { 0.0f, -0.25f, 0.0f }, false);

    scene::JointComponent joint{};
    joint.type             = scene::JointType::Rope;
    joint.connectedBody.id = anchor.GetID();
    joint.autoDistance     = false;
    joint.distance         = 1.0f;
    hanging.AddComponent<scene::JointComponent>(joint);

    // 1 フレームではまだ最大長に届かない (= 拘束が «押し戻して» いない)。
    StepPhysics(scene, world);
    EXPECT_LT(BodyOf(hanging)->GetPosition().Length(), 1.0f);

    StepPhysics(scene, world, 180);
    EXPECT_NEAR(BodyOf(hanging)->GetPosition().Length(), 1.0f, testkit::kLooseTolerance);
}

TEST_F(JointComponentTest, AutoDistanceTakesTheGapAtTheMomentItIsTied)
{
    scene::Scene scene;
    physics::World world;
    scene::GameObject& anchor  = MakeBody(scene, "Anchor", math::Vector3::ZERO, true);
    scene::GameObject& hanging = MakeBody(scene, "Hanging", { 0.0f, -2.75f, 0.0f }, false);

    scene::JointComponent joint{};
    joint.type             = scene::JointType::Distance;
    joint.connectedBody.id = anchor.GetID();
    joint.autoDistance     = true;
    hanging.AddComponent<scene::JointComponent>(joint);

    StepPhysics(scene, world, 60);

    const auto* live = hanging.GetComponent<scene::JointComponent>();
    ASSERT_NE(live, nullptr);
    // 置いた位置の間隔をそのまま保つ。毎フレーム測り直していると «じわじわ伸びる»。
    EXPECT_NEAR(live->resolvedDistance, 2.75f, testkit::kTolerance);
    EXPECT_NEAR(BodyOf(hanging)->GetPosition().Length(), 2.75f, testkit::kLooseTolerance);
}

TEST_F(JointComponentTest, AChainHoldsEverySegmentLength)
{
    scene::Scene scene;
    physics::World world;
    scene::GameObject& head = MakeBody(scene, "Head", math::Vector3::ZERO, true);
    scene::GameObject& mid  = MakeBody(scene, "Mid", { 0.0f, -1.0f, 0.0f }, false);
    scene::GameObject& tail = MakeBody(scene, "Tail", { 0.0f, -2.0f, 0.0f }, false);

    scene::JointComponent joint{};
    joint.type         = scene::JointType::Chain;
    joint.chainBodies  = { scene::EntityRef{ mid.GetID() }, scene::EntityRef{ tail.GetID() } };
    joint.autoDistance = false;
    joint.distance     = 1.0f;
    // WHY 反復を増やすか: 鎖は隣の組を順に射影する Gauss-Seidel なので、1 組を直すと
    //     前の組がわずかに崩れる。残差は反復ごとに幾何級数で小さくなるだけで 0 にはならない。
    joint.solverIterations = 32;
    head.AddComponent<scene::JointComponent>(joint);

    StepPhysics(scene, world, 30);

    const math::Vector3 headPos = BodyOf(head)->GetPosition();
    const math::Vector3 midPos  = BodyOf(mid)->GetPosition();
    const math::Vector3 tailPos = BodyOf(tail)->GetPosition();
    // 許容は «1 フレームの落下量» の桁。位置ベースの拘束は速度を消さないので、
    // 落ち続けている鎖はこの幅の中で振れる。ここが 10cm 単位で崩れるなら
    // 節が繋がっていない (並びが壊れている)。
    constexpr float kChainTolerance = 0.01f;
    EXPECT_NEAR((midPos - headPos).Length(), 1.0f, kChainTolerance);
    EXPECT_NEAR((tailPos - midPos).Length(), 1.0f, kChainTolerance);
}

TEST_F(JointComponentTest, FindsThePartnerThroughTheParentWhenNoneIsNamed)
{
    // EntityRef は Prefab に保存できないので、«吊り元は親» の宣言だけで組める必要がある。
    scene::Scene scene;
    physics::World world;
    scene::GameObject& anchor  = MakeBody(scene, "Anchor", math::Vector3::ZERO, true);
    scene::GameObject& hanging = MakeBody(scene, "Hanging", { 0.0f, -1.0f, 0.0f }, false);
    hanging.SetParent(anchor);

    scene::JointComponent joint{};
    joint.type            = scene::JointType::Distance;
    joint.connectToParent = true;
    joint.autoDistance    = false;
    joint.distance        = 1.0f;
    hanging.AddComponent<scene::JointComponent>(joint);

    StepPhysics(scene, world);

    ASSERT_EQ(world.GetConstraints().size(), 1u);
    // 並びは [自分, 相手]。Hinge の anchor / connectedAnchor がこの順に対応する。
    EXPECT_EQ(world.GetConstraints()[0]->GetBodyA(), BodyOf(hanging));
    EXPECT_EQ(world.GetConstraints()[0]->GetBodyB(), BodyOf(anchor));
}

} // namespace fbzz::tests
