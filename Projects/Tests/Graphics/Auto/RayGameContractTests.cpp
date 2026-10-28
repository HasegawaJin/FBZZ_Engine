/// @file    RayGameContractTests.cpp
/// @brief   GPU を作らず Game のカメラ切り替え・履歴再投影境界を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#include <TestKit/TestKit.hpp>
#include <Graphics/RayTracing/RayGameResources.hpp>
#include <cstdint>

namespace fbzz::tests {
namespace {
class RayGameContractTest : public testkit::Fixture {};

renderer::RayReconstructionSurface Surface(uint32_t pixel)
{
    renderer::RayReconstructionSurface value;
    value.positionDepth = {(static_cast<float>(pixel) + 0.5f) / 2 - 1, 0, 2, 0.5f};
    value.normalRoughness = {0, 0, -1, 0.045f};
    value.albedoMetallic = {0.5f, 0.25f, 0.125f, 0};
    value.previousPositionValid = {value.positionDepth.x, 0, 2, 1};
    value.objectMaterialValid = {7, 9, 1, 1};
    value.sceneFlags = {42, 1, 0, 0};
    return value;
}

TEST_F(RayGameContractTest, CameraCutSeparatesOrdinaryMotionFromProjectionAndLargePoseChanges)
{
    renderer::Camera previous;
    previous.m_position = {};
    auto current = previous;
    current.m_position.x = 0.1f;
    EXPECT_FALSE(renderer::IsRayGameCameraCut(previous, current));
    current.m_position.x = 2;
    EXPECT_TRUE(renderer::IsRayGameCameraCut(previous, current));
    current = previous; current.m_fovY = 61;
    EXPECT_TRUE(renderer::IsRayGameCameraCut(previous, current));
    current = previous; current.LookAt({1, 0, 0});
    EXPECT_TRUE(renderer::IsRayGameCameraCut(previous, current));
}

TEST_F(RayGameContractTest, UsesFullIdentityMaterialAndCameraRelativeDepthForHistoryValidation)
{
    renderer::Camera camera;
    camera.m_position = {};
    const auto original = Surface(0);
    auto current = original;
    EXPECT_TRUE(renderer::IsRayReconstructionHistoryCompatible(current, original, camera));
    current.objectMaterialValid[1]++;
    EXPECT_FALSE(renderer::IsRayReconstructionHistoryCompatible(current, original, camera));
    current = original; current.objectMaterialValid[2]++;
    EXPECT_FALSE(renderer::IsRayReconstructionHistoryCompatible(current, original, camera));
    current = original; current.sceneFlags[1]++;
    EXPECT_FALSE(renderer::IsRayReconstructionHistoryCompatible(current, original, camera));
    current = original; current.previousPositionValid.w = 0;
    EXPECT_FALSE(renderer::IsRayReconstructionHistoryCompatible(current, original, camera));
    current = original; current.previousPositionValid.x += 0.01f;
    EXPECT_FALSE(renderer::IsRayReconstructionHistoryCompatible(current, original, camera));
    auto translated = original;
    translated.positionDepth.z += 1000; translated.previousPositionValid.z += 1000;
    camera.m_position.z = 1000;
    current = translated; current.previousPositionValid.x += 0.01f;
    EXPECT_FALSE(renderer::IsRayReconstructionHistoryCompatible(current, translated, camera));
}

}
}
