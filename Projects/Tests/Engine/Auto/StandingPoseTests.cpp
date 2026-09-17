/// @file    StandingPoseTests.cpp
/// @brief   立位射影とブレンドで根位置・骨長・許容変位が保たれることを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-13
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Scene/Ragdoll/StandingPose.hpp>
#include <Engine/Scene/Ragdoll/RagdollPose.hpp>
#include <Math/MathUtils.hpp>
#include <limits>

namespace fbzz::tests {
class StandingPoseTest : public testkit::EngineFixture {
protected:
    std::vector<scene::RagdollBonePose> m_targets;
    std::vector<math::Vector3> m_positions;
    std::vector<math::Quaternion> m_rotations;
    std::vector<math::Quaternion> m_scratch;

    void SetUp() override
    {
        testkit::EngineFixture::SetUp();
        for (int i = 0; i < 5; ++i) {
            scene::RagdollBonePose bone;
            bone.parent = i - 1;
            bone.position = {0.0f, static_cast<float>(i), 0.0f};
            m_targets.push_back(bone);
            m_positions.push_back(bone.position);
            m_rotations.push_back(bone.rotation);
        }
    }

    void ExpectStanding()
    {
        EXPECT_VEC3_NEAR(m_positions[0], m_targets[0].position, 1.0e-5f);
        for (std::size_t i = 0; i < m_targets.size(); ++i) {
            EXPECT_LE((m_positions[i] - m_targets[i].position).Length(), 0.12001f);
            EXPECT_GE(std::abs(math::Quaternion::Dot(m_rotations[i], m_targets[i].rotation)),
                      std::cos(0.125f) - 1.0e-5f);
            if (i > 0) EXPECT_NEAR((m_positions[i] - m_positions[i - 1]).Length(), 1.0f, 1.0e-5f);
        }
    }
};

TEST_F(StandingPoseTest, TranslatingTheWholeChainDoesNotStretchBones)
{
    for (auto& position : m_positions) position.y += 0.3f;
    EXPECT_TRUE(scene::LimitStandingPose(m_targets, 0.12f, 0.25f,
                                        m_positions, m_rotations, m_scratch));
    ExpectStanding();
    EXPECT_NEAR(m_positions[1].y, 1.0f, 1.0e-5f);
}

TEST_F(StandingPoseTest, RepeatedLargeRotationsStayWithinStandingEnvelope)
{
    for (int frame = 0; frame < 100; ++frame) {
        for (std::size_t i = 0; i < m_targets.size(); ++i) {
            m_positions[i] += math::Vector3{10.0f, -20.0f, 4.0f};
            m_rotations[i] = math::Quaternion::FromAxisAngle(math::Vector3::RIGHT,
                0.8f + static_cast<float>(i) * 0.2f);
        }
        EXPECT_TRUE(scene::LimitStandingPose(m_targets, 0.12f, 0.25f,
                                            m_positions, m_rotations, m_scratch));
        ExpectStanding();
    }
}

TEST_F(StandingPoseTest, CrossingTheLimitDoesNotHalveTheResponse)
{
    m_targets.resize(1);
    m_positions.resize(1);
    m_rotations.resize(1);
    m_rotations[0] = math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, 0.26f);
    ASSERT_TRUE(scene::LimitStandingPose(m_targets, 0.12f, 0.25f,
                                        m_positions, m_rotations, m_scratch));
    const float angle = 2.0f * std::acos(std::clamp(std::abs(
        math::Quaternion::Dot(m_rotations[0], m_targets[0].rotation)), 0.0f, 1.0f));
    EXPECT_NEAR(angle, 0.25f, 0.0001f);
}

TEST_F(StandingPoseTest, UsesTheRequestedAngleWithoutAGameSpecificCap)
{
    m_targets.resize(1);
    m_positions.resize(1);
    m_rotations.resize(1);
    const auto candidate = math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, 0.8f);
    m_rotations[0] = candidate;
    ASSERT_TRUE(scene::LimitStandingPose(m_targets, 1.0f, 1.2f,
                                        m_positions, m_rotations, m_scratch));
    EXPECT_QUAT_NEAR(m_rotations[0], candidate, 0.0001f);
}

TEST_F(StandingPoseTest, InvalidBoneRestoresTheEntireChain)
{
    m_positions[2].x = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(scene::LimitStandingPose(m_targets, 0.12f, 0.25f,
                                         m_positions, m_rotations, m_scratch));
    for (std::size_t i = 0; i < m_targets.size(); ++i) {
        EXPECT_VEC3_NEAR(m_positions[i], m_targets[i].position, 1.0e-5f);
        EXPECT_QUAT_NEAR(m_rotations[i], m_targets[i].rotation, 1.0e-5f);
    }
}

TEST_F(StandingPoseTest, PartialBlendingPreservesBoneLengths)
{
    const auto rotation = math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, 0.25f);
    for (std::size_t i = 0; i < m_targets.size(); ++i) {
        m_positions[i] = rotation * m_targets[i].position;
        m_rotations[i] = rotation;
    }
    scene::BlendRagdollPose(m_targets, 0.5f, true, m_positions, m_rotations);
    for (std::size_t i = 1; i < m_targets.size(); ++i)
        EXPECT_NEAR((m_positions[i] - m_positions[i - 1]).Length(), 1.0f, 1.0e-5f);
}

TEST_F(StandingPoseTest, ExcludedDescendantsInheritTheFinalParentTransform)
{
    asset::Skeleton skeleton;
    skeleton.rootNodeIndex = 2;
    skeleton.nodes.resize(3);
    /// @note インデックス順に依存せず、深さ制限・除外の先の孫まで伝播すること。
    skeleton.nodes[2].children = {0};
    skeleton.nodes[0].parentIndex = 2;
    skeleton.nodes[0].children = {1};
    skeleton.nodes[1].parentIndex = 0;
    std::vector<math::Matrix4> original{
        math::Matrix4::TRS({0, 1, 0}, math::Quaternion::Identity(), math::Vector3::ONE),
        math::Matrix4::TRS({0, 2, 0}, math::Quaternion::Identity(), math::Vector3::ONE),
        math::Matrix4::Identity()
    };
    auto finalPose = original;
    const auto rotation = math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, math::PI * 0.5f);
    finalPose[2] = math::Matrix4::TRS({3, 0, 0}, rotation, math::Vector3::ONE);
    scene::PropagateRagdollDescendants(skeleton, original, {false, false, true}, finalPose);
    const math::Vector3 finger{finalPose[1].m[0][3], finalPose[1].m[1][3], finalPose[1].m[2][3]};
    EXPECT_VEC3_NEAR(finger, math::Vector3(3, 0, 0) + rotation * math::Vector3(0, 2, 0), 1.0e-5f);
}
} // namespace fbzz::tests
