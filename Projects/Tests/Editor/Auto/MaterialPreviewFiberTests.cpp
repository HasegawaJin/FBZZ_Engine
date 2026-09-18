/// @file    MaterialPreviewFiberTests.cpp
/// @brief   Fiber の .mat をプレビューで専用経路へ振り分ける判定を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Panels/MaterialPreviewCore.hpp>
#include <Engine/Asset/MaterialAsset.hpp>

namespace fbzz::tests {
namespace mp = editor::matpreview;

class MaterialPreviewFiberTest : public testkit::EditorFixture {
protected:
    [[nodiscard]] static asset::MaterialAsset FiberMaterial(const char* shader, float grassShading)
    {
        asset::MaterialAsset material;
        material.shaderPath = shader;
        material.params["grassShading"] = { grassShading };
        return material;
    }
};

TEST_F(MaterialPreviewFiberTest, FiberShadersUseTheFiberFlavorInsteadOfTheSurfaceSphere)
{
    /// @note Surface へ流すと b5 が 0 のまま全画素 clip され、サムネイルが空になる。
    EXPECT_EQ(mp::DetectFlavor(FiberMaterial("Assets/Shaders/Fiber/FiberShell.hlsl", 0.0f)), mp::Flavor::Fiber);
    EXPECT_EQ(mp::DetectFlavor(FiberMaterial("guid:aef89576fbea4916a2a3905f4ef1ead5|Assets/Shaders/Fiber/FiberShell.hlsl", 0.0f)),
              mp::Flavor::Fiber);
    EXPECT_EQ(mp::DetectFlavor(FiberMaterial("Assets/Shaders/Fiber/FiberBlade.hlsl", 1.0f)), mp::Flavor::Fiber);
    asset::MaterialAsset base;
    base.shaderPath = "Assets/Shaders/Material/Surface/Lit.hlsl";
    EXPECT_EQ(mp::DetectFlavor(base), mp::Flavor::Surface);
    EXPECT_FALSE(mp::UsesOwnGeometry(mp::Flavor::Fiber));
}

TEST_F(MaterialPreviewFiberTest, ModeShapeAndChannelsFollowTheMaterial)
{
    EXPECT_EQ(mp::DefaultFiberMode(FiberMaterial("Assets/Shaders/Fiber/FiberShell.hlsl", 0.0f)), mp::FiberMode::Shell);
    EXPECT_EQ(mp::DefaultFiberMode(FiberMaterial("Assets/Shaders/Fiber/FiberFinSkinned.hlsl", 0.0f)), mp::FiberMode::Fin);
    EXPECT_EQ(mp::DefaultFiberMode(FiberMaterial("Assets/Shaders/Fiber/FiberBladeShadow.hlsl", 0.0f)), mp::FiberMode::Blade);

    const auto fur = FiberMaterial("Assets/Shaders/Fiber/FiberShell.hlsl", 0.0f);
    const auto grass = FiberMaterial("Assets/Shaders/Fiber/FiberShell.hlsl", 1.0f);
    EXPECT_EQ(mp::ThumbnailShape(fur, mp::Flavor::Fiber), mp::Shape::Sphere);
    EXPECT_EQ(mp::ThumbnailShape(grass, mp::Flavor::Fiber), mp::Shape::Plane);
    EXPECT_EQ(mp::ThumbnailShape(grass, mp::Flavor::Surface), mp::Shape::Sphere);

    EXPECT_TRUE(mp::ChannelSupported(mp::Flavor::Fiber, mp::Channel::Shaded));
    EXPECT_FALSE(mp::ChannelSupported(mp::Flavor::Fiber, mp::Channel::Albedo));
    EXPECT_FALSE(mp::ChannelSupported(mp::Flavor::Fiber, mp::Channel::Wireframe));
    EXPECT_STREQ(mp::FiberModeLabel(mp::FiberMode::Hybrid), "Hybrid");
}

} // namespace fbzz::tests
