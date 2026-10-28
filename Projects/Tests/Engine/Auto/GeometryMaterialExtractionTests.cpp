/// @file    GeometryMaterialExtractionTests.cpp
/// @brief   材質の抽出・上書きと複数ビュー間の値の独立性を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-20
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/TempDir.hpp>
#include <Engine/Renderer/GeometryRoute.hpp>
#include <Engine/Scene/Systems/RenderPasses/GeometryRoute.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <array>
#include <string>
#include <type_traits>

namespace fbzz::tests {
namespace {

/// @note Tests author through the same owned draft/publication path as production; no mutable stored alias is exposed.
template<typename Owner, typename Change>
void EditMaterialAuthoring(Owner& owner, Change&& change)
{
    auto edit = owner.BeginAuthoringEdit();
    change(edit.Draft());
    EXPECT_NE(edit.Commit(owner), asset::detail::MaterialAuthoringCommitResult::CONFLICT);
}



using renderer::GeometryRoute;
using renderer::ResolveGeometryRoute;
using scene::ExtractGeometryMaterial;

static_assert(std::is_trivially_copyable_v<renderer::GeometryMaterialInput>);

class GeometryMaterialExtractionTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        ASSERT_TRUE(m_temp.IsValid());
        ASSERT_TRUE(WriteShader("PBR.hlsl", "standard_surface_v1"));
        ASSERT_TRUE(WriteShader("SkinnedPBR.hlsl", "standard_skinned_v1"));
    }

    std::string ShaderPath(const std::string& name) const { return m_temp.File(name).generic_string(); }
    bool WriteShader(const std::string& name, const std::string& vertex)
    {
        const std::string path = ShaderPath(name);
        return util::FileSystem::WriteText(path, "float4 PSMain() : SV_Target { return 1; }\n")
            && util::FileSystem::WriteText(path + ".meta", "[shader]\nversion=1\nvertex='" + vertex
                + "'\nsurface='metallic_roughness_v1'\nopacity='alpha_clip_v1'\nvariants='" + vertex + "'\n");
    }

    testkit::TempDir m_temp{"geometry-material"};
};

TEST_F(GeometryMaterialExtractionTest, MissingAssetKeepsBlendOverrideAndStaysForward)
{
    scene::MaterialSlot slot;
    EXPECT_EQ(ResolveGeometryRoute(ExtractGeometryMaterial(slot), true), GeometryRoute::ForwardOpaque);
    EditMaterialAuthoring(slot, [&](auto& values) { values.hasBlendModeOverride = true; });
    EditMaterialAuthoring(slot, [&](auto& values) { values.blendModeOverride = renderer::BlendMode::ADDITIVE; });
    const auto input = ExtractGeometryMaterial(slot);
    EXPECT_FALSE(input.gbufferEquivalentShader);
    EXPECT_EQ(ResolveGeometryRoute(input, true), GeometryRoute::ForwardTransparent);
    EXPECT_EQ(ResolveGeometryRoute(input, false), GeometryRoute::ForwardTransparent);
    EXPECT_FALSE(slot.GetMaterialAsset().IsValid());
    EXPECT_EQ(slot.material, nullptr);
}

TEST_F(GeometryMaterialExtractionTest, BlendOverrideTakesPrecedenceOverSharedAsset)
{
    asset::MaterialAssetAuthoring material;
    material.shaderPath = ShaderPath("PBR.hlsl");
    material.blendMode = renderer::BlendMode::ALPHA_BLEND;
    scene::MaterialSlot slot;
    EXPECT_EQ(ResolveGeometryRoute(ExtractGeometryMaterial(slot.Authoring(), &material, scene::IsGBufferEquivalentShader(material.shaderPath)), true),
              GeometryRoute::ForwardTransparent);
    EditMaterialAuthoring(slot, [&](auto& values) { values.hasBlendModeOverride = true; });
    EditMaterialAuthoring(slot, [&](auto& values) { values.blendModeOverride = renderer::BlendMode::OPAQUE_BLEND; });
    EXPECT_EQ(ResolveGeometryRoute(ExtractGeometryMaterial(slot.Authoring(), &material, scene::IsGBufferEquivalentShader(material.shaderPath)), true), GeometryRoute::GBuffer);
    EXPECT_EQ(material.blendMode, renderer::BlendMode::ALPHA_BLEND);
}

TEST_F(GeometryMaterialExtractionTest, LobeOverridesCanEnableAndDisableSharedFeatures)
{
    for (const char* name : { "clearcoat", "sheen", "anisotropy" }) {
        asset::MaterialAssetAuthoring material;
        material.params[name] = { 0.5f };
        scene::MaterialSlot slot;
        EXPECT_TRUE(ExtractGeometryMaterial(slot.Authoring(), &material, scene::IsGBufferEquivalentShader(material.shaderPath)).advancedLobe) << name;
        EditMaterialAuthoring(slot, [&](auto& values) { values.paramOverrides[name] = { 0.0f }; });
        EXPECT_FALSE(ExtractGeometryMaterial(slot.Authoring(), &material, scene::IsGBufferEquivalentShader(material.shaderPath)).advancedLobe) << name;
        EditMaterialAuthoring(slot, [&](auto& values) { values.paramOverrides[name] = { -0.5f }; });
        EXPECT_TRUE(ExtractGeometryMaterial(slot.Authoring(), &material, scene::IsGBufferEquivalentShader(material.shaderPath)).advancedLobe) << name;
        EditMaterialAuthoring(slot, [&](auto& values) { values.paramOverrides[name] = {}; });
        EXPECT_FALSE(ExtractGeometryMaterial(slot.Authoring(), &material, scene::IsGBufferEquivalentShader(material.shaderPath)).advancedLobe) << name;
        EditMaterialAuthoring(slot, [&](auto& values) { values.paramOverrides[name] = { 1.0e-4f }; });
        EXPECT_FALSE(ExtractGeometryMaterial(slot.Authoring(), &material, scene::IsGBufferEquivalentShader(material.shaderPath)).advancedLobe) << name;
        EditMaterialAuthoring(slot, [&](auto& values) { values.paramOverrides[name] = { 2.0e-4f }; });
        EXPECT_TRUE(ExtractGeometryMaterial(slot.Authoring(), &material, scene::IsGBufferEquivalentShader(material.shaderPath)).advancedLobe) << name;
    }
}

TEST_F(GeometryMaterialExtractionTest, ClothRemainsForwardWhenSheenIsZero)
{
    asset::MaterialAssetAuthoring material;
    material.params["clothSheenColor"] = { 0.0f, 0.0f, 0.0f };
    scene::MaterialSlot slot;
    EditMaterialAuthoring(slot, [&](auto& values) { values.paramOverrides["sheen"] = { 0.0f }; });
    const auto input = ExtractGeometryMaterial(slot.Authoring(), &material, scene::IsGBufferEquivalentShader(material.shaderPath));
    EXPECT_TRUE(input.advancedLobe);
    EXPECT_EQ(ResolveGeometryRoute(input, true), GeometryRoute::ForwardOpaque);
}

TEST_F(GeometryMaterialExtractionTest, SnapshotSurvivesMaterialEditingAndDestruction)
{
    renderer::GeometryMaterialInput saved;
    {
        asset::MaterialAssetAuthoring material;
        material.shaderPath = ShaderPath("PBR.hlsl");
        scene::MaterialSlot slot;
        saved = ExtractGeometryMaterial(slot.Authoring(), &material, scene::IsGBufferEquivalentShader(material.shaderPath));
        material.shaderPath = "Custom.hlsl";
        EditMaterialAuthoring(slot, [&](auto& values) { values.hasBlendModeOverride = true; });
        EditMaterialAuthoring(slot, [&](auto& values) { values.blendModeOverride = renderer::BlendMode::ALPHA_BLEND; });
        EXPECT_EQ(ResolveGeometryRoute(ExtractGeometryMaterial(slot.Authoring(), &material, scene::IsGBufferEquivalentShader(material.shaderPath)), true),
                  GeometryRoute::ForwardTransparent);
    }
    EXPECT_EQ(ResolveGeometryRoute(saved, true), GeometryRoute::GBuffer);
    EXPECT_EQ(ResolveGeometryRoute(saved, false), GeometryRoute::ForwardOpaque);
}

TEST_F(GeometryMaterialExtractionTest, SubmeshSlotsKeepIndependentRoutesAcrossViews)
{
    asset::MaterialAssetAuthoring material;
    material.shaderPath = ShaderPath("SkinnedPBR.hlsl");
    std::array<scene::MaterialSlot, 3> slots;
    EditMaterialAuthoring(slots[1], [&](auto& values) { values.hasBlendModeOverride = true; });
    EditMaterialAuthoring(slots[1], [&](auto& values) { values.blendModeOverride = renderer::BlendMode::PREMULTIPLIED; });
    EditMaterialAuthoring(slots[2], [&](auto& values) { values.paramOverrides["clearcoat"] = { 1.0f }; });
    const std::array inputs{
        ExtractGeometryMaterial(slots[0].Authoring(), &material, scene::IsGBufferEquivalentShader(material.shaderPath)),
        ExtractGeometryMaterial(slots[1].Authoring(), &material, scene::IsGBufferEquivalentShader(material.shaderPath)),
        ExtractGeometryMaterial(slots[2].Authoring(), &material, scene::IsGBufferEquivalentShader(material.shaderPath)),
    };
    const std::array deferred{
        GeometryRoute::GBuffer, GeometryRoute::ForwardTransparent, GeometryRoute::ForwardOpaque,
    };
    const std::array forward{
        GeometryRoute::ForwardOpaque, GeometryRoute::ForwardTransparent, GeometryRoute::ForwardOpaque,
    };
    for (size_t i = 0; i < inputs.size(); ++i) {
        EXPECT_EQ(ResolveGeometryRoute(inputs[i], true), deferred[i]);
        EXPECT_EQ(ResolveGeometryRoute(inputs[i], false), forward[i]);
    }
    EXPECT_TRUE(material.params.empty());
}

TEST_F(GeometryMaterialExtractionTest, UnknownShaderDoesNotBecomeStandardPbr)
{
    asset::MaterialAssetAuthoring material;
    material.shaderPath = "GreenWare/Assets/Shaders/Custom.hlsl";
    scene::MaterialSlot slot;
    const auto input = ExtractGeometryMaterial(slot.Authoring(), &material, scene::IsGBufferEquivalentShader(material.shaderPath));
    EXPECT_FALSE(input.gbufferEquivalentShader);
    EXPECT_EQ(ResolveGeometryRoute(input, true), GeometryRoute::ForwardOpaque);
}

} /// @note namespace
} /// @note namespace fbzz::tests
