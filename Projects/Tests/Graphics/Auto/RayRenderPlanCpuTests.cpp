/// @file    RayRenderPlanCpuTests.cpp
/// @brief   GPU を作らずレイ反射パスの登録順と Engine 非依存の契約を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#include <TestKit/TestKit.hpp>
#include <Graphics/Pipeline/GeometryPipeline.hpp>
#include <Graphics/Pipeline/RenderPipeline.hpp>
#include <Graphics/Renderer/OpaqueRenderPlan.hpp>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <algorithm>

namespace fbzz::tests {
namespace {
class RayRenderPlanTest : public testkit::Fixture {
protected:
    void SetUp() override
    {
        testkit::Fixture::SetUp();
        ASSERT_EQ(GetModuleHandleW(L"FBZZEngine.dll"), nullptr);
        ASSERT_EQ(GetModuleHandleW(L"FBZZPhysics.dll"), nullptr);
        ASSERT_EQ(GetModuleHandleW(L"FBZZFluid.dll"), nullptr);
    }
};

TEST_F(RayRenderPlanTest, InsertsRayReflectionAfterSurfaceAndBeforeLightingOnlyForDeferred)
{
    for (const auto path : {renderer::OpaqueRenderPath::FORWARD, renderer::OpaqueRenderPath::DEFERRED}) {
        renderer::RenderPipeline pipeline;
        uint32_t callbackCount = 0;
        renderer::BuildGeometryPipeline(pipeline, {path}, [&]() {
            ++callbackCount;
            pipeline.AddRawPass("RayReflection", {}, {}, []() {});
        });
        const auto names = pipeline.RegisteredPassNames();
        const auto reflection = std::find(names.begin(), names.end(), "RayReflection");
        if (path == renderer::OpaqueRenderPath::DEFERRED) {
            EXPECT_EQ(callbackCount, 1u);
            EXPECT_LT(std::find(names.begin(), names.end(), "DeferredGBuffer"), reflection);
            EXPECT_LT(reflection, std::find(names.begin(), names.end(), "DeferredLighting"));
        } else {
            EXPECT_EQ(callbackCount, 0u);
            EXPECT_EQ(reflection, names.end());
        }
    }
}

}
}
