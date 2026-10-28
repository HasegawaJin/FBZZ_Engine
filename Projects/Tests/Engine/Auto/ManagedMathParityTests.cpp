/// @file    ManagedMathParityTests.cpp
/// @brief   実 CoreCLR の純計算を自作 Math と比較し、閾値・外挿・成分配置を守る。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Scene/Managed/ManagedScriptAdapter.hpp>
#include <Engine/Scene/Managed/ManagedScriptRuntime.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Vector3.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace fbzz::tests {

class ManagedMathParityTest : public testkit::EngineFixture {
protected:
    scene::Scene m_scene;
    scene::ManagedScriptAdapter* m_adapter = nullptr;

    void SetUp() override
    {
        EngineFixture::SetUp();
        auto& runtime = scene::ManagedScriptRuntime::Instance();
        ASSERT_TRUE(runtime.Initialize(FBZZ_MANAGED_HOST_TEST_DIRECTORY)) << runtime.LastDiagnostic();
        ASSERT_TRUE(runtime.Load(FBZZ_MANAGED_FIXTURE_TEST_ASSEMBLY, true)) << runtime.LastDiagnostic();
        auto& object = m_scene.CreateGameObject("MathParity");
        auto& entry = object.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    entry.script = scene::ScriptFactory::Create("cs-internal:ManagedFixtures.MathParityComponent", entry.savedScriptId);
        ASSERT_NE(entry.script, nullptr);
        entry.script->SetContext(&m_scene, &object);
        m_adapter = runtime.FindAdapter(entry.script.get());
        ASSERT_NE(m_adapter, nullptr);
        ASSERT_TRUE(m_adapter->RefreshOwnerBinding()) << runtime.LastDiagnostic();
    }

    void TearDown() override
    {
        auto& runtime = scene::ManagedScriptRuntime::Instance();
        m_adapter = nullptr;
        runtime.Unload(&m_scene);
        m_scene.Clear();
        EXPECT_TRUE(runtime.Shutdown()) << runtime.LastDiagnostic();
        EngineFixture::TearDown();
    }

    void Evaluate(std::string_view settings = "{}")
    {
        ASSERT_TRUE(m_adapter->ApplySettingsJson(settings)) << scene::ManagedScriptRuntime::Instance().LastDiagnostic();
        ASSERT_TRUE(m_adapter->ExecuteProfiledCallback(&scene::Script::OnUpdate,
            scene::ScriptCallbackKind::UPDATE, "OnUpdate")) << scene::ManagedScriptRuntime::Instance().LastDiagnostic();
    }

    /// @note READ_FIELD は既存 ABI の float と x/y/z を使い、数学専用 native dispatch は追加しない。
    scene::ManagedBridgeReply Read(std::string_view field, scene::ManagedValueKind kind)
    {
        scene::ManagedBridgeRequest request;
        request.op = scene::ManagedBridgeOp::READ_FIELD;
        request.generation = m_adapter->Generation();
        request.instanceId = m_adapter->InstanceId();
        request.ownerId = m_adapter->OwnerId();
        request.input = field.data();
        request.inputBytes = static_cast<uint32_t>(field.size());
        scene::ManagedBridgeReply reply;
        auto& runtime = scene::ManagedScriptRuntime::Instance();
        EXPECT_TRUE(runtime.Dispatch(request, reply)) << field << ": " << runtime.LastDiagnostic();
        EXPECT_EQ(reply.valueKind, kind) << field;
        return reply;
    }

    float ReadFloat(std::string_view field) { return Read(field, scene::ManagedValueKind::FLOAT).x; }
    int64_t ReadInteger(std::string_view field) { return Read(field, scene::ManagedValueKind::INT).integer; }
    math::Vector3 ReadVector(std::string_view field)
    {
        const auto value = Read(field, scene::ManagedValueKind::VECTOR3);
        return {value.x, value.y, value.z};
    }
};

TEST_F(ManagedMathParityTest, MatchesSignedArithmeticAndUnclampedInterpolation)
{
    struct ScalarCase {
        const char* settings;
        float value;
        float factor;
    };
    const ScalarCase cases[] = {
        {R"({"scalarValue":-4,"factor":-0.5})", -4.0f, -0.5f},
        {R"({"scalarValue":-2,"factor":0})", -2.0f, 0.0f},
        {R"({"scalarValue":0,"factor":0.25})", 0.0f, 0.25f},
        {R"({"scalarValue":0.5,"factor":0.5})", 0.5f, 0.5f},
        {R"({"scalarValue":1,"factor":1})", 1.0f, 1.0f},
        {R"({"scalarValue":3,"factor":1.25})", 3.0f, 1.25f},
        {R"({"scalarValue":5,"factor":1.5})", 5.0f, 1.5f},
    };
    const math::Vector3 first{1.25f, -2.5f, 3.75f};
    const math::Vector3 second{-4.5f, 5.25f, -6.0f};

    for (const auto& value : cases) {
        SCOPED_TRACE(value.settings);
        Evaluate(value.settings);

        EXPECT_NEAR(ReadFloat("dot"), math::Vector3::Dot(first, second), testkit::kTolerance);
        EXPECT_VEC3_NEAR(ReadVector("cross"), math::Vector3::Cross(first, second), testkit::kTolerance);
        EXPECT_VEC3_NEAR(ReadVector("sum"), first + second, testkit::kTolerance);
        EXPECT_VEC3_NEAR(ReadVector("difference"), first - second, testkit::kTolerance);
        EXPECT_VEC3_NEAR(ReadVector("negated"), -first, testkit::kTolerance);
        EXPECT_VEC3_NEAR(ReadVector("scaled"), first * 2.0f, testkit::kTolerance);
        EXPECT_VEC3_NEAR(ReadVector("divided"), first / 2.0f, testkit::kTolerance);
        EXPECT_NEAR(ReadFloat("length"), first.Length(), testkit::kTolerance);
        EXPECT_NEAR(ReadFloat("lengthSquared"), first.LengthSq(), testkit::kTolerance);
        EXPECT_VEC3_NEAR(ReadVector("vectorLerp"), math::Vector3::Lerp(first, second, value.factor), testkit::kTolerance);
        EXPECT_NEAR(ReadFloat("scalarClamp"), math::Clamp(value.value, -2.0f, 3.0f), testkit::kTolerance);
        EXPECT_NEAR(ReadFloat("scalarClamp01"), math::Clamp01(value.value), testkit::kTolerance);
        EXPECT_NEAR(ReadFloat("scalarLerp"), math::Lerp(-2.0f, 6.0f, value.factor), testkit::kTolerance);
    }

    Evaluate(R"({"first":[1,0,0],"second":[0,1,0]})");
    EXPECT_VEC3_NEAR(ReadVector("cross"), math::Vector3::FORWARD, testkit::kTolerance);
    Evaluate(R"({"first":[0,1,0],"second":[1,0,0]})");
    EXPECT_VEC3_NEAR(ReadVector("cross"), -math::Vector3::FORWARD, testkit::kTolerance);
}

TEST_F(ManagedMathParityTest, MatchesNormalizationAtAndAroundTheSharedEpsilon)
{
    struct NormalizationCase {
        const char* settings;
        math::Vector3 vector;
    };
    const NormalizationCase cases[] = {
        {R"({"first":[0,0,0]})", math::Vector3::ZERO},
        {R"({"first":[5e-7,0,0]})", {math::EPSILON * 0.5f, 0, 0}},
        {R"({"first":[1e-6,0,0]})", {math::EPSILON, 0, 0}},
        {R"({"first":[2e-6,0,0]})", {math::EPSILON * 2.0f, 0, 0}},
        {R"({"first":[-1e-6,0,0]})", {-math::EPSILON, 0, 0}},
        {R"({"first":[-3,4,-12]})", {-3, 4, -12}},
    };
    const math::Vector3 fallback{2, -3, 4};

    for (const auto& value : cases) {
        SCOPED_TRACE(value.settings);
        Evaluate(value.settings);

        EXPECT_VEC3_NEAR(ReadVector("normalized"), value.vector.NormalizedOr(fallback), testkit::kTolerance);
        if (math::NearlyZero(value.vector.Length())) {
            EXPECT_VEC3_NEAR(ReadVector("normalized"), fallback, testkit::kTolerance);
        } else {
            EXPECT_UNIT_LENGTH(ReadVector("normalized"), testkit::kTolerance);
        }
    }
}

TEST_F(ManagedMathParityTest, PreservesConstantsAndVectorLayoutBeforeAnyCallback)
{
    const math::Vector3 first{2, -3, 4};
    const math::Vector3 second{-5, 6, -7};
    std::string helperKey;

    EXPECT_NEAR(ReadFloat("constructorDot"), math::Vector3::Dot(first, second), testkit::kTolerance);
    EXPECT_FLOAT_EQ(ReadFloat("pi"), math::PI);
    EXPECT_FLOAT_EQ(ReadFloat("twoPi"), math::TWO_PI);
    EXPECT_FLOAT_EQ(ReadFloat("halfPi"), math::HALF_PI);
    EXPECT_FLOAT_EQ(ReadFloat("epsilon"), math::EPSILON);
    EXPECT_FLOAT_EQ(ReadFloat("degreesToRadians"), math::DEG2RAD);
    EXPECT_FLOAT_EQ(ReadFloat("radiansToDegrees"), math::RAD2DEG);
    EXPECT_EQ(ReadInteger("vectorSize"), static_cast<int64_t>(sizeof(math::Vector3)));
    EXPECT_EQ(ReadInteger("vectorXOffset"), static_cast<int64_t>(offsetof(math::Vector3, x)));
    EXPECT_EQ(ReadInteger("vectorYOffset"), static_cast<int64_t>(offsetof(math::Vector3, y)));
    EXPECT_EQ(ReadInteger("vectorZOffset"), static_cast<int64_t>(offsetof(math::Vector3, z)));
    EXPECT_FALSE(scene::ScriptFactory::ResolveTypeName("ManagedFixtures.MathParity", helperKey));

    Evaluate();
    EXPECT_NEAR(ReadFloat("toRadians"), math::ToRad(-450.0f), testkit::kTolerance);
    EXPECT_NEAR(ReadFloat("toDegrees"), math::ToDeg(-math::PI * 2.5f), testkit::kTolerance);
    Evaluate(R"({"degrees":720,"radians":1.5707963267948966})");
    EXPECT_NEAR(ReadFloat("toRadians"), math::ToRad(720.0f), testkit::kTolerance);
    EXPECT_NEAR(ReadFloat("toDegrees"), math::ToDeg(math::HALF_PI), testkit::kTolerance);
    Evaluate(R"({"degrees":0,"radians":0})");
    EXPECT_FLOAT_EQ(ReadFloat("toRadians"), math::ToRad(0.0f));
    EXPECT_FLOAT_EQ(ReadFloat("toDegrees"), math::ToDeg(0.0f));
}

} /// @note namespace fbzz::tests
