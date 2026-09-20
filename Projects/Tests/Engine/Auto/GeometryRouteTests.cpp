/// @file    GeometryRouteTests.cpp
/// @brief   GBuffer と Forward の振り分け規則を表明する。
/// @author  Hasegawa Jin
/// @date    2026-09-20
///
/// この規則が «どれにも当たらない» / «2 つに当たる» になると、物が消えるか二重に描かれる。
/// どちらも絵を見て原因に辿りつけない壊れ方なので、全組み合わせを機械で押さえる。
///
/// @see Docs/design/pipeline-boundary.md
#include <TestKit/TestKit.hpp>

#include <Engine/Scene/Systems/RenderPasses/GeometryRoute.hpp>

namespace fbzz::tests {
namespace {

using scene::GeometryRoute;
using scene::GeometryRouteInput;
using scene::ResolveGeometryRoute;

/// @brief GBuffer へ入る条件を満たした入力。各テストはここから 1 つだけ崩す。
constexpr GeometryRouteInput GBufferBound()
{
    GeometryRouteInput input;
    input.blend                   = renderer::BlendMode::OPAQUE_BLEND;
    input.gbufferPipeline         = true;
    input.gbufferEquivalentShader = true;
    input.advancedLobe            = false;
    return input;
}

class GeometryRouteTest : public testkit::Fixture {};

TEST_F(GeometryRouteTest, StandardOpaqueMaterialGoesToTheGBuffer)
{
    EXPECT_EQ(ResolveGeometryRoute(GBufferBound()), GeometryRoute::GBuffer);
}

/// 半透明は GBuffer が表せない。他の条件が何であっても必ず透明経路。
TEST_F(GeometryRouteTest, TransparencyAlwaysWins)
{
    for (const auto blend : { renderer::BlendMode::ALPHA_BLEND,
                              renderer::BlendMode::ADDITIVE,
                              renderer::BlendMode::PREMULTIPLIED }) {
        GeometryRouteInput input = GBufferBound();
        input.blend = blend;
        EXPECT_EQ(ResolveGeometryRoute(input), GeometryRoute::ForwardTransparent);

        /// @note シェーダーが GBuffer 相当でなくても、拡張ローブがあっても、判定は変わらない。
        input.gbufferEquivalentShader = false;
        input.advancedLobe            = true;
        EXPECT_EQ(ResolveGeometryRoute(input), GeometryRoute::ForwardTransparent);

        /// @note Forward パイプラインでも «半透明» であることは変わらない。深度順に描く必要がある。
        input.gbufferPipeline = false;
        EXPECT_EQ(ResolveGeometryRoute(input), GeometryRoute::ForwardTransparent);
    }
}

/// Forward パイプラインには GBuffer が無いので、不透明はすべて ForwardOpaque。
TEST_F(GeometryRouteTest, ForwardPipelineNeverProducesGBufferRoute)
{
    GeometryRouteInput input = GBufferBound();
    input.gbufferPipeline = false;
    EXPECT_EQ(ResolveGeometryRoute(input), GeometryRoute::ForwardOpaque);
}

/// GBuffer パスは材質のシェーダーを捨てて GBuffer.hlsl で描く。
/// 知らないシェーダーを通すと、そのシェーディングモデルが黙って PBR に化ける。
TEST_F(GeometryRouteTest, UnknownShaderStaysForward)
{
    GeometryRouteInput input = GBufferBound();
    input.gbufferEquivalentShader = false;
    EXPECT_EQ(ResolveGeometryRoute(input), GeometryRoute::ForwardOpaque);
}

/// 拡張ローブは GBuffer 2 枚に接線基底ごと入らない。
TEST_F(GeometryRouteTest, AdvancedLobeStaysForward)
{
    GeometryRouteInput input = GBufferBound();
    input.advancedLobe = true;
    EXPECT_EQ(ResolveGeometryRoute(input), GeometryRoute::ForwardOpaque);
}

/// 4 つのフラグの全組み合わせで、必ずどれか 1 つの経路に落ちること。
/// 規則に穴 (どれにも当たらない) も重なり (2 つに当たる) も無いことの表明。
TEST_F(GeometryRouteTest, EveryCombinationResolvesToExactlyOneRoute)
{
    int gbuffer = 0, forwardOpaque = 0, forwardTransparent = 0;

    for (const auto blend : { renderer::BlendMode::OPAQUE_BLEND,
                              renderer::BlendMode::ALPHA_BLEND }) {
        for (const bool pipeline : { false, true }) {
            for (const bool equivalent : { false, true }) {
                for (const bool advanced : { false, true }) {
                    GeometryRouteInput input;
                    input.blend                   = blend;
                    input.gbufferPipeline         = pipeline;
                    input.gbufferEquivalentShader = equivalent;
                    input.advancedLobe            = advanced;

                    switch (ResolveGeometryRoute(input)) {
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

/// 空欄は «既定の材質» で、実際には Fallback (標準 PBR 相当) が使われる。
/// ここを Forward に倒していたのが、Deferred なのに GBuffer がほとんど空だった原因。
TEST_F(GeometryRouteTest, EmptyShaderPathCountsAsGBufferEquivalent)
{
    EXPECT_TRUE(scene::IsGBufferEquivalentShader(""));
}

TEST_F(GeometryRouteTest, StandardPbrShadersAreGBufferEquivalent)
{
    EXPECT_TRUE(scene::IsGBufferEquivalentShader("Assets/Shaders/Material/Surface/PBR.hlsl"));
    EXPECT_TRUE(scene::IsGBufferEquivalentShader("Assets/Shaders/Material/Surface/Lit.hlsl"));
    EXPECT_TRUE(scene::IsGBufferEquivalentShader("Assets/Shaders/Material/Skinned/SkinnedPBR.hlsl"));
    /// @note `guid:...|パス` 形式でも同じ結論になること。参照の書き方で経路が変わってはいけない。
    EXPECT_TRUE(scene::IsGBufferEquivalentShader(
        "guid:b8a759120fac41289126bed4eabc9a3d|Assets/Shaders/Material/Surface/Lit.hlsl"));
    /// @note プロジェクト側の複製も同じ結論になること (ファイル名で比べている根拠)。
    EXPECT_TRUE(scene::IsGBufferEquivalentShader(
        "GreenWare/Assets/Shaders/Material/Surface/PBR.hlsl"));
}

/// シェーディングモデルが PBR でないものは、不透明でも GBuffer へ入れない。
TEST_F(GeometryRouteTest, NonPbrLightingModelsAreNotGBufferEquivalent)
{
    for (const char* path : { "Assets/Shaders/Material/Surface/Unlit.hlsl",
                              "Assets/Shaders/Material/Surface/Toon.hlsl",
                              "Assets/Shaders/Material/Surface/RimLight.hlsl",
                              "Assets/Shaders/Material/Surface/Dissolve.hlsl",
                              "Assets/Shaders/Material/Surface/Cloth.hlsl",
                              "Assets/Shaders/Material/Surface/Anisotropic.hlsl",
                              "Assets/Shaders/Material/Surface/Subsurface.hlsl",
                              "Assets/Shaders/Material/Skinned/SkinnedToon.hlsl",
                              "Assets/Shaders/Material/Skinned/SkinnedDissolve.hlsl" }) {
        EXPECT_FALSE(scene::IsGBufferEquivalentShader(path)) << path << " が GBuffer 相当になっている";
    }
}

/// プロジェクトが自分で書いたシェーダーは «知らないもの» なので Forward へ倒れること。
TEST_F(GeometryRouteTest, ProjectAuthoredShadersAreNotGBufferEquivalent)
{
    EXPECT_FALSE(scene::IsGBufferEquivalentShader(
        "GreenWare/Assets/Shaders/Material/Skinned/SkinnedEnemyDissolve.hlsl"));
    EXPECT_FALSE(scene::IsGBufferEquivalentShader(
        "GreenWare/Assets/Shaders/Material/Skinned/SkinnedBladeSteel.hlsl"));
}

} // namespace
} // namespace fbzz::tests
