/// @file    GeometryRouteTests.cpp
/// @brief   GBuffer と Forward の振り分け規則を表明する。
/// @author  Hasegawa Jin
/// @date    2026-09-20
/// @note この規則が «どれにも当たらない» / «2 つに当たる» になると、物が消えるか二重に描かれる。
/// @note どちらも絵を見て原因に辿りつけない壊れ方なので、全組み合わせを機械で押さえる。
/// @see Docs/design/pipeline-boundary.md
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/TempDir.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <string>

#include <Engine/Scene/Systems/RenderPasses/GeometryRoute.hpp>

namespace fbzz::tests {
namespace {

using renderer::GeometryRoute;
using renderer::GeometryMaterialInput;
using renderer::ResolveGeometryRoute;

/// @brief GBuffer へ入る条件を満たした入力。各テストはここから 1 つだけ崩す。
constexpr GeometryMaterialInput GBufferBound()
{
    GeometryMaterialInput input;
    input.blend                   = renderer::BlendMode::OPAQUE_BLEND;
    input.gbufferEquivalentShader = true;
    input.advancedLobe            = false;
    return input;
}

class GeometryRouteTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        ASSERT_TRUE(m_temp.IsValid());
        ASSERT_TRUE(WriteShader("PBR.hlsl", "standard_surface_v1", "metallic_roughness_v1", "standard_surface_v1"));
        ASSERT_TRUE(WriteShader("SkinnedPBR.hlsl", "standard_skinned_v1", "metallic_roughness_v1", "standard_skinned_v1"));
        ASSERT_TRUE(WriteShader("Lit.hlsl", "standard_surface_v1", "lambert_v1", "standard_surface_v1"));
        ASSERT_TRUE(WriteShader("Fallback.hlsl", "standard_surface_v1", "unlit_v1", "standard_surface_v1"));
    }

    std::string ShaderPath(const std::string& name) const { return m_temp.File(name).generic_string(); }
    bool WriteShader(const std::string& name, const std::string& vertex,
                     const std::string& surface, const std::string& variants)
    {
        const std::string path = ShaderPath(name);
        return util::FileSystem::WriteText(path, "float4 PSMain() : SV_Target { return 1; }\n")
            && util::FileSystem::WriteText(path + ".meta", "[shader]\nversion=1\nvertex='" + vertex
                + "'\nsurface='" + surface + "'\nopacity='alpha_clip_v1'\nvariants='" + variants + "'\n");
    }

    testkit::TempDir m_temp{"geometry-route"};
};

TEST_F(GeometryRouteTest, StandardOpaqueMaterialGoesToTheGBuffer)
{
    EXPECT_EQ(ResolveGeometryRoute(GBufferBound(), true), GeometryRoute::GBuffer);
}

/// @note 半透明は GBuffer が表せない。他の条件が何であっても必ず透明経路。
TEST_F(GeometryRouteTest, TransparencyAlwaysWins)
{
    for (const auto blend : { renderer::BlendMode::ALPHA_BLEND,
                              renderer::BlendMode::ADDITIVE,
                              renderer::BlendMode::PREMULTIPLIED }) {
        GeometryMaterialInput input = GBufferBound();
        input.blend = blend;
        EXPECT_EQ(ResolveGeometryRoute(input, true), GeometryRoute::ForwardTransparent);

        /// @note シェーダーが GBuffer 相当でなくても、拡張ローブがあっても、判定は変わらない。
        input.gbufferEquivalentShader = false;
        input.advancedLobe            = true;
        EXPECT_EQ(ResolveGeometryRoute(input, true), GeometryRoute::ForwardTransparent);

        /// @note Forward パイプラインでも «半透明» であることは変わらない。深度順に描く必要がある。
        EXPECT_EQ(ResolveGeometryRoute(input, false), GeometryRoute::ForwardTransparent);
    }
}

/// @note Forward パイプラインには GBuffer が無いので、不透明はすべて ForwardOpaque。
TEST_F(GeometryRouteTest, ForwardPipelineNeverProducesGBufferRoute)
{
    GeometryMaterialInput input = GBufferBound();
    EXPECT_EQ(ResolveGeometryRoute(input, false), GeometryRoute::ForwardOpaque);
}

/// @note GBuffer パスは材質のシェーダーを捨てて GBuffer.hlsl で描く。
/// @note 知らないシェーダーを通すと、そのシェーディングモデルが黙って PBR に化ける。
TEST_F(GeometryRouteTest, UnknownShaderStaysForward)
{
    GeometryMaterialInput input = GBufferBound();
    input.gbufferEquivalentShader = false;
    EXPECT_EQ(ResolveGeometryRoute(input, true), GeometryRoute::ForwardOpaque);
}

/// @note 拡張ローブは GBuffer 2 枚に接線基底ごと入らない。
TEST_F(GeometryRouteTest, AdvancedLobeStaysForward)
{
    GeometryMaterialInput input = GBufferBound();
    input.advancedLobe = true;
    EXPECT_EQ(ResolveGeometryRoute(input, true), GeometryRoute::ForwardOpaque);
}

/// @note 4 つのフラグの全組み合わせで、必ずどれか 1 つの経路に落ちること。
/// @note 規則に穴 (どれにも当たらない) も重なり (2 つに当たる) も無いことの表明。
TEST_F(GeometryRouteTest, EveryCombinationResolvesToExactlyOneRoute)
{
    int gbuffer = 0, forwardOpaque = 0, forwardTransparent = 0;

    for (const auto blend : { renderer::BlendMode::OPAQUE_BLEND,
                              renderer::BlendMode::ALPHA_BLEND }) {
        for (const bool pipeline : { false, true }) {
            for (const bool equivalent : { false, true }) {
                for (const bool advanced : { false, true }) {
                    GeometryMaterialInput input;
                    input.blend                   = blend;
                    input.gbufferEquivalentShader = equivalent;
                    input.advancedLobe            = advanced;

                    switch (ResolveGeometryRoute(input, pipeline)) {
                    case GeometryRoute::GBuffer:            ++gbuffer;            break;
                    case GeometryRoute::ForwardOpaque:      ++forwardOpaque;      break;
                    case GeometryRoute::ForwardTransparent: ++forwardTransparent; break;
                    }
                }
            }
        }
    }

    EXPECT_EQ(gbuffer + forwardOpaque + forwardTransparent, 16);
    /// @note GBuffer へ入るのは «不透明 × GBuffer 経路 × 相当シェーダー × ローブ無し» の 1 通りだけ。
    EXPECT_EQ(gbuffer, 1);
    /// @note 半透明は blend の 1 通り × 残り 8 通り。
    EXPECT_EQ(forwardTransparent, 8);
    EXPECT_EQ(forwardOpaque, 7);
}

TEST_F(GeometryRouteTest, EmptyShaderPathDoesNotDeclareGBufferEquivalence)
{
    EXPECT_FALSE(scene::IsGBufferEquivalentShader(""));
}

TEST_F(GeometryRouteTest, StandardPbrShadersAreGBufferEquivalent)
{
    EXPECT_TRUE(scene::IsGBufferEquivalentShader(ShaderPath("PBR.hlsl")));
    EXPECT_TRUE(scene::IsGBufferEquivalentShader(ShaderPath("SkinnedPBR.hlsl")));
    EXPECT_FALSE(scene::IsGBufferEquivalentShader(
        "guid:ffffffffffffffffffffffffffffffff|Assets/Shaders/Material/Surface/PBR.hlsl"));
}

/// @note シェーディングモデルが PBR でないものは、不透明でも GBuffer へ入れない。
TEST_F(GeometryRouteTest, NonPbrLightingModelsAreNotGBufferEquivalent)
{
    for (const char* name : { "Lit.hlsl", "Fallback.hlsl" }) {
        EXPECT_FALSE(scene::IsGBufferEquivalentShader(ShaderPath(name))) << name;
    }
}

/// @note プロジェクトが自分で書いたシェーダーは «知らないもの» なので Forward へ倒れること。
TEST_F(GeometryRouteTest, ProjectAuthoredShadersAreNotGBufferEquivalent)
{
    EXPECT_FALSE(scene::IsGBufferEquivalentShader(
        "GreenWare/Assets/Shaders/Material/Skinned/SkinnedEnemyDissolve.hlsl"));
    EXPECT_FALSE(scene::IsGBufferEquivalentShader(
        "GreenWare/Assets/Shaders/Material/Skinned/SkinnedBladeSteel.hlsl"));
}

} /// @note namespace
} /// @note namespace fbzz::tests
