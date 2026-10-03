/// @file    MaterialPreviewFiberTests.cpp
/// @brief   Fiber の .mat をプレビューで専用経路へ振り分ける判定を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <TestKit/TempDir.hpp>
#include <Editor/Panels/MaterialPreviewCore.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <filesystem>
#include <string>
#include <system_error>

namespace fbzz::tests {
namespace mp = editor::matpreview;

class MaterialPreviewFiberTest : public testkit::EditorFixture {
protected:
    void SetUp() override
    {
        EditorFixture::SetUp();
        ASSERT_TRUE(m_temp.IsValid());
        std::error_code error;
        std::filesystem::create_directories(m_temp.File("Shaders"), error);
        ASSERT_FALSE(error);
        ASSERT_TRUE(WritePreviewShader("Shell.hlsl", "fiber_shell"));
        ASSERT_TRUE(WritePreviewShader("Fin.hlsl", "fiber_fin"));
        ASSERT_TRUE(WritePreviewShader("Blade.hlsl", "fiber_blade"));
        ASSERT_TRUE(WritePreviewShader("Mesh.hlsl", "mesh_trail"));
        ASSERT_TRUE(WritePreviewShader("Gpu.hlsl", "gpu_particle"));
        ASSERT_TRUE(WritePreviewShader("Lit.hlsl", "surface"));
    }

    std::string ShaderPath(const std::string& name) const
    {
        return m_temp.File("Shaders/" + name).generic_string();
    }

    bool WritePreviewShader(const std::string& name, const std::string& preview)
    {
        const std::string path = ShaderPath(name);
        return util::FileSystem::WriteText(path, "float4 PSMain() : SV_Target { return 1; }\n")
            && util::FileSystem::WriteText(path + ".meta",
                "[shader]\nversion=1\nvertex='custom'\nsurface='custom'\nopacity='custom'\nvariants='none'\npreview='" + preview + "'\n");
    }

    [[nodiscard]] static asset::MaterialAsset FiberMaterial(const std::string& shader, float grassShading)
    {
        asset::MaterialAsset material;
        material.shaderPath = shader;
        material.params["grassShading"] = { grassShading };
        return material;
    }

    testkit::TempDir m_temp{"material-preview-fiber"};
};

TEST_F(MaterialPreviewFiberTest, FiberShadersUseTheFiberFlavorInsteadOfTheSurfaceSphere)
{
    /// @note Surface へ流すと b5 が 0 のまま全画素 clip され、サムネイルが空になる。
    EXPECT_EQ(mp::DetectFlavor(FiberMaterial(ShaderPath("Shell.hlsl"), 0.0f)), mp::Flavor::Fiber);
    EXPECT_EQ(mp::DetectFlavor(FiberMaterial("guid:ffffffffffffffffffffffffffffffff|Assets/Shaders/Fiber/FiberShell.hlsl", 0.0f)),
              mp::Flavor::Unsupported);
    EXPECT_EQ(mp::DetectFlavor(FiberMaterial(ShaderPath("Blade.hlsl"), 1.0f)), mp::Flavor::Fiber);
    asset::MaterialAsset base;
    base.shaderPath = ShaderPath("Lit.hlsl");
    EXPECT_EQ(mp::DetectFlavor(base), mp::Flavor::Surface);
    EXPECT_FALSE(mp::UsesOwnGeometry(mp::Flavor::Fiber));
}

TEST_F(MaterialPreviewFiberTest, ModeShapeAndChannelsFollowTheMaterial)
{
    EXPECT_EQ(mp::DefaultFiberMode(FiberMaterial(ShaderPath("Shell.hlsl"), 0.0f)), mp::FiberMode::Shell);
    EXPECT_EQ(mp::DefaultFiberMode(FiberMaterial(ShaderPath("Fin.hlsl"), 0.0f)), mp::FiberMode::Fin);
    EXPECT_EQ(mp::DefaultFiberMode(FiberMaterial(ShaderPath("Blade.hlsl"), 0.0f)), mp::FiberMode::Blade);

    const auto fur = FiberMaterial(ShaderPath("Shell.hlsl"), 0.0f);
    const auto grass = FiberMaterial(ShaderPath("Shell.hlsl"), 1.0f);
    EXPECT_EQ(mp::ThumbnailShape(fur, mp::Flavor::Fiber), mp::Shape::Sphere);
    EXPECT_EQ(mp::ThumbnailShape(grass, mp::Flavor::Fiber), mp::Shape::Plane);
    EXPECT_EQ(mp::ThumbnailShape(grass, mp::Flavor::Surface), mp::Shape::Sphere);

    EXPECT_TRUE(mp::ChannelSupported(mp::Flavor::Fiber, mp::Channel::Shaded));
    EXPECT_FALSE(mp::ChannelSupported(mp::Flavor::Fiber, mp::Channel::Albedo));
    EXPECT_FALSE(mp::ChannelSupported(mp::Flavor::Fiber, mp::Channel::Wireframe));
    EXPECT_STREQ(mp::FiberModeLabel(mp::FiberMode::Hybrid), "Hybrid");
}

TEST_F(MaterialPreviewFiberTest, UnsupportedVertexInputsTakePrecedenceOverTheRenderPath)
{
    asset::MaterialAsset material;
    material.shaderPath = ShaderPath("Mesh.hlsl");
    material.renderPath = asset::RenderPath::Trail;
    EXPECT_EQ(mp::DetectFlavor(material), mp::Flavor::Unsupported);
    EXPECT_STREQ(mp::UnsupportedBadge(material), "TRAIL");
    material.shaderPath = ShaderPath("Gpu.hlsl");
    material.renderPath = asset::RenderPath::Particle;
    EXPECT_EQ(mp::DetectFlavor(material), mp::Flavor::Unsupported);
    EXPECT_STREQ(mp::UnsupportedBadge(material), "PARTICLE");
}

} /// @note namespace fbzz::tests
