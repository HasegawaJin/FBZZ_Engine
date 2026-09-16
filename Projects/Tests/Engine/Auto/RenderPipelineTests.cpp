/// @file    RenderPipelineTests.cpp
/// @brief   Plan キャッシュの鍵とパス上書きの畳み込みを固定する。
/// @author  Hasegawa Jin
/// @date    2026-09-16
///
/// RenderPipeline::Execute は RenderPassContext (デバイス付き) を要るので直接は試せない。
/// 代わりに «鍵の計算» と «上書きの畳み込み» を ctx を要らない静的な形へ切り出してあり、
/// ここで契約を固定する。
///
/// 鍵が «変わったのに同じ値» になると、パイプラインは古い実行順を注入し続ける。
/// 絵が壊れるまで誰も気付かない種類の故障なので、項目ごとに «その 1 つだけ変えた 2 つが
/// 異なる» を並べて押さえる。
#include <TestKit/TestKit.hpp>

#include <Engine/Renderer/RenderSettings.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPipeline.hpp>

#include <string>
#include <vector>

namespace fbzz::tests {
namespace {

using Pipeline = scene::RenderPipeline;
using RG       = renderer::RenderGraph;

/// 既定の «全部埋まった» 記述。1 項目だけ変えて鍵の差を見るための土台。
RG::ResourceDesc BaseDesc()
{
    return RG::ResourceDesc{ RG::ResourceKind::RenderTarget, 1920, 1080,
                             renderer::Format::RGBA16F, 1, true, false, true };
}

/// リソース 1 件だけを混ぜた鍵。
uint64_t ResourceKey(const RG::ResourceDesc& desc)
{
    Pipeline::GraphFingerprint fingerprint;
    fingerprint.MixResource("HDR", desc);
    return fingerprint.Value();
}

std::vector<RG::ResourceAccess> Accesses(std::initializer_list<const char*> reads)
{
    std::vector<RG::ResourceAccess> result;
    for (const char* read : reads)
        result.push_back({ std::string(read), RG::ResourceUsage::Read });
    return result;
}

} // namespace

class RenderPipelineFingerprintTest : public testkit::Fixture {};

TEST_F(RenderPipelineFingerprintTest, IdenticalDeclarationsProduceTheSameKey)
{
    Pipeline::GraphFingerprint first;
    Pipeline::GraphFingerprint second;
    for (auto* fingerprint : { &first, &second }) {
        fingerprint->MixResource("HDR", BaseDesc());
        fingerprint->MixOutput("Output");
        fingerprint->MixPass("Sky", true, Accesses({ "HDR" }));
    }
    EXPECT_EQ(first.Value(), second.Value());
}

TEST_F(RenderPipelineFingerprintTest, EveryResourceDescFieldChangesTheKey)
{
    const uint64_t base = ResourceKey(BaseDesc());

    auto kind = BaseDesc();
    kind.kind = RG::ResourceKind::Texture;
    EXPECT_NE(ResourceKey(kind), base);

    auto width = BaseDesc();
    width.width = 1280;
    EXPECT_NE(ResourceKey(width), base);

    auto height = BaseDesc();
    height.height = 720;
    EXPECT_NE(ResourceKey(height), base);

    auto format = BaseDesc();
    format.format = renderer::Format::RGBA8;
    EXPECT_NE(ResourceKey(format), base);

    // colorCount と withDepth は «実体の形» を決める。抜けていた 2 項目なので必ず押さえる。
    auto colorCount = BaseDesc();
    colorCount.colorCount = 2;
    EXPECT_NE(ResourceKey(colorCount), base);

    auto withDepth = BaseDesc();
    withDepth.withDepth = false;
    EXPECT_NE(ResourceKey(withDepth), base);

    auto external = BaseDesc();
    external.external = true;
    EXPECT_NE(ResourceKey(external), base);

    auto transient = BaseDesc();
    transient.transient = false;
    EXPECT_NE(ResourceKey(transient), base);
}

TEST_F(RenderPipelineFingerprintTest, ResourceNameChangesTheKey)
{
    Pipeline::GraphFingerprint hdr;
    hdr.MixResource("HDR", BaseDesc());
    Pipeline::GraphFingerprint ldr;
    ldr.MixResource("LDR", BaseDesc());
    EXPECT_NE(hdr.Value(), ldr.Value());
}

TEST_F(RenderPipelineFingerprintTest, PassAccessesAndCullingChangeTheKey)
{
    Pipeline::GraphFingerprint base;
    base.MixPass("SSR", true, Accesses({ "GBuffer" }));

    // 設定トグルで reads が増減するフレームを、パス集合が同じでも別物として見分ける。
    Pipeline::GraphFingerprint extraRead;
    extraRead.MixPass("SSR", true, Accesses({ "GBuffer", "GTAOResult" }));
    EXPECT_NE(extraRead.Value(), base.Value());

    Pipeline::GraphFingerprint culling;
    culling.MixPass("SSR", false, Accesses({ "GBuffer" }));
    EXPECT_NE(culling.Value(), base.Value());

    Pipeline::GraphFingerprint usage;
    usage.MixPass("SSR", true, { { std::string("GBuffer"), RG::ResourceUsage::Write } });
    EXPECT_NE(usage.Value(), base.Value());
}

TEST_F(RenderPipelineFingerprintTest, ConcatenationCannotCollide)
{
    // 区切りが無いと "ab"+"c" と "a"+"bc" が同じ鍵になる。名前は連結して混ぜるので、
    // 隣り合う宣言が入れ替わっただけのときに «変わっていない» と誤判定しうる。
    Pipeline::GraphFingerprint split;
    split.MixOutput("ab");
    split.MixOutput("c");

    Pipeline::GraphFingerprint joined;
    joined.MixOutput("a");
    joined.MixOutput("bc");

    EXPECT_NE(split.Value(), joined.Value());
}

TEST_F(RenderPipelineFingerprintTest, PassOrderChangesTheKey)
{
    Pipeline::GraphFingerprint forward;
    forward.MixPass("Sky", true, {});
    forward.MixPass("Fog", true, {});

    Pipeline::GraphFingerprint reversed;
    reversed.MixPass("Fog", true, {});
    reversed.MixPass("Sky", true, {});

    EXPECT_NE(forward.Value(), reversed.Value());
}

class RenderPipelineOverrideTest : public testkit::Fixture {};

TEST_F(RenderPipelineOverrideTest, ExtraReadsAreAppendedAsReads)
{
    renderer::RenderPassOverride over;
    over.name = "Sky";
    over.extraReads = { "ShadowMap" };

    auto accesses = Accesses({ "HDR" });
    bool allowCulling = true;
    Pipeline::ApplyOverride(over, accesses, allowCulling);

    ASSERT_EQ(accesses.size(), 2u);
    EXPECT_EQ(accesses[1].name, "ShadowMap");
    EXPECT_EQ(accesses[1].usage, RG::ResourceUsage::Read);
}

TEST_F(RenderPipelineOverrideTest, AlreadyDeclaredNamesAreNotDuplicated)
{
    // 同じ名前を二重に申告しても辺は増えないが、鍵が変わって Plan を無駄にやり直す。
    renderer::RenderPassOverride over;
    over.extraReads = { "HDR", "ShadowMap" };

    auto accesses = Accesses({ "HDR" });
    bool allowCulling = true;
    Pipeline::ApplyOverride(over, accesses, allowCulling);

    ASSERT_EQ(accesses.size(), 2u);
    EXPECT_EQ(accesses[0].name, "HDR");
    EXPECT_EQ(accesses[1].name, "ShadowMap");
}

TEST_F(RenderPipelineOverrideTest, WriteDeclarationBlocksTheSameExtraRead)
{
    // 書いている名前を «追加で読む» と申告しても、自分自身への辺になるだけ。
    renderer::RenderPassOverride over;
    over.extraReads = { "HDR" };

    std::vector<RG::ResourceAccess> accesses{ { std::string("HDR"), RG::ResourceUsage::Write } };
    bool allowCulling = true;
    Pipeline::ApplyOverride(over, accesses, allowCulling);

    EXPECT_EQ(accesses.size(), 1u);
    EXPECT_EQ(accesses[0].usage, RG::ResourceUsage::Write);
}

TEST_F(RenderPipelineOverrideTest, CullingOnlyEverBecomesStricter)
{
    std::vector<RG::ResourceAccess> accesses;

    // 刈ってよいパスを «残す» へ倒せる。
    renderer::RenderPassOverride keep;
    keep.allowCulling = false;
    bool cullable = true;
    Pipeline::ApplyOverride(keep, accesses, cullable);
    EXPECT_FALSE(cullable);

    // 逆向きは効かない。パス自身が «刈られては困る» と言っているものを
    // UI の既定値 (allowCulling = true) が黙って刈れるようにはしない。
    renderer::RenderPassOverride release;
    release.allowCulling = true;
    bool mustRun = false;
    Pipeline::ApplyOverride(release, accesses, mustRun);
    EXPECT_FALSE(mustRun);
}

TEST_F(RenderPipelineOverrideTest, DefaultOverrideIsRecognisedAndChangesNothing)
{
    // 既定に戻った行は保存も適用もしない。無害な行がプロジェクトに溜まるのを避ける。
    renderer::RenderPassOverride over;
    over.name = "Sky";
    EXPECT_TRUE(over.IsDefault());

    over.extraReads = { "HDR" };
    EXPECT_FALSE(over.IsDefault());
    over.extraReads.clear();
    over.enabled = false;
    EXPECT_FALSE(over.IsDefault());
    over.enabled = true;
    over.allowCulling = false;
    EXPECT_FALSE(over.IsDefault());

    renderer::RenderPassOverride neutral;
    auto accesses = Accesses({ "HDR" });
    bool allowCulling = true;
    Pipeline::ApplyOverride(neutral, accesses, allowCulling);
    EXPECT_EQ(accesses.size(), 1u);
    EXPECT_TRUE(allowCulling);
}

} // namespace fbzz::tests
