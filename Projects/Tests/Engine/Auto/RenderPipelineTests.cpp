/// @file    RenderPipelineTests.cpp
/// @brief   Plan キャッシュの鍵とパス上書きの畳み込みを固定する。
/// @author  Hasegawa Jin
/// @date    2026-09-16
/// @note デバイス不要の指紋計算・上書き・型付き資源登録を検証する。
/// @note 記述変更が指紋へ反映されないと古い実行順を再利用するため、項目ごとに差を確認する。
#include <TestKit/TestKit.hpp>

#include <Engine/Renderer/RenderSettings.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPipeline.hpp>
#include <Graphics/Pipeline/PassResources.hpp>

#include <string>
#include <vector>

namespace fbzz::tests {
namespace {

using Pipeline = scene::RenderPipeline;
using RG       = renderer::RenderGraph;

/// @brief 項目ごとの差分比較に使う記述を返す。
RG::ResourceDesc BaseDesc()
{
    return RG::ResourceDesc{ RG::ResourceKind::RenderTarget, 1920, 1080,
                             renderer::Format::RGBA16F, 1, true, false, true };
}

/// @brief 単一リソースの指紋を返す。
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

} /// @note namespace

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

    /// @note colorCount と withDepth は «実体の形» を決める。抜けていた 2 項目なので必ず押さえる。
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

    auto byteSize = BaseDesc();
    byteSize.byteSize = 4096;
    EXPECT_NE(ResourceKey(byteSize), base);

    auto stride = BaseDesc();
    stride.stride = 32;
    EXPECT_NE(ResourceKey(stride), base);

    auto allowAliasing = BaseDesc();
    allowAliasing.allowAliasing = false;
    EXPECT_NE(ResourceKey(allowAliasing), base);
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

    /// @note 設定トグルで reads が増減するフレームを、パス集合が同じでも別物として見分ける。
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
    /// @note 区切りが無いと "ab"+"c" と "a"+"bc" が同じ鍵になる。
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

TEST_F(RenderPipelineFingerprintTest, AccessPurposeChangesTheKeyWithoutChangingDependencies)
{
    Pipeline::GraphFingerprint build;
    build.MixPass("ReadVertices", true,
                  { { "Vertices", RG::ResourceUsage::Read, RG::ResourceAccessPurpose::BUILD_INPUT } });
    Pipeline::GraphFingerprint shader;
    shader.MixPass("ReadVertices", true,
                   { { "Vertices", RG::ResourceUsage::Read, RG::ResourceAccessPurpose::SHADER_READ } });
    EXPECT_NE(build.Value(), shader.Value());
}

class RenderPassResourcesTest : public testkit::Fixture {};

TEST_F(RenderPassResourcesTest, RegistryPreservesTypedHandlesAndClearsAllKinds)
{
    renderer::RenderResourceRegistry registry;
    const renderer::ResourceHandle<renderer::BufferTag> vertices{ 1, 2 };
    const renderer::ResourceHandle<renderer::StructuredBufferTag> table{ 3, 4 };
    const renderer::ResourceHandle<renderer::AccelerationStructureTag> tlas{ 5, 6 };
    registry.BindBuffer("Vertices", vertices);
    registry.BindStructuredBuffer("Table", table);
    registry.BindAccelerationStructure("TLAS", tlas);

    EXPECT_EQ(registry.Buffer("Vertices"), vertices);
    EXPECT_EQ(registry.StructuredBuffer("Table"), table);
    EXPECT_EQ(registry.AccelerationStructure("TLAS"), tlas);
    EXPECT_FALSE(registry.StructuredBuffer("Vertices").IsValid());
    EXPECT_FALSE(registry.Buffer("Table").IsValid());
    EXPECT_FALSE(registry.AccelerationStructure("Missing").IsValid());

    registry.Clear();
    EXPECT_FALSE(registry.Buffer("Vertices").IsValid());
    EXPECT_FALSE(registry.StructuredBuffer("Table").IsValid());
    EXPECT_FALSE(registry.AccelerationStructure("TLAS").IsValid());
}

TEST_F(RenderPassResourcesTest, SetupKeepsDependencyUsageSeparateFromGpuPurpose)
{
    renderer::PassBuilder builder;
    builder.Read("Vertices", RG::ResourceAccessPurpose::BUILD_INPUT)
           .Write("BLAS", RG::ResourceAccessPurpose::AS_WRITE)
           .Read("TLAS", RG::ResourceAccessPurpose::TRACE_READ)
           .Read("Table", RG::ResourceAccessPurpose::SHADER_READ)
           .ReadWrite("Scratch", RG::ResourceAccessPurpose::UAV);

    std::vector<RG::ResourceAccess> accesses;
    std::string autoTarget;
    builder.MoveOut(accesses, autoTarget);
    ASSERT_EQ(accesses.size(), 5u);
    EXPECT_EQ(accesses[0].usage, RG::ResourceUsage::Read);
    EXPECT_EQ(accesses[0].purpose, RG::ResourceAccessPurpose::BUILD_INPUT);
    EXPECT_EQ(accesses[1].usage, RG::ResourceUsage::Write);
    EXPECT_EQ(accesses[1].purpose, RG::ResourceAccessPurpose::AS_WRITE);
    EXPECT_EQ(accesses[2].purpose, RG::ResourceAccessPurpose::TRACE_READ);
    EXPECT_EQ(accesses[3].purpose, RG::ResourceAccessPurpose::SHADER_READ);
    EXPECT_EQ(accesses[4].usage, RG::ResourceUsage::ReadWrite);
    EXPECT_EQ(accesses[4].purpose, RG::ResourceAccessPurpose::UAV);
}

TEST_F(RenderPassResourcesTest, ExecuteResolvesDeclaredBuffersAndAccelerationStructures)
{
    renderer::RenderResourceRegistry registry;
    const renderer::ResourceHandle<renderer::BufferTag> vertices{ 1, 2 };
    const renderer::ResourceHandle<renderer::StructuredBufferTag> table{ 3, 4 };
    const renderer::ResourceHandle<renderer::AccelerationStructureTag> tlas{ 5, 6 };
    registry.BindBuffer("Vertices", vertices);
    registry.BindStructuredBuffer("Table", table);
    registry.BindAccelerationStructure("TLAS", tlas);
    const std::vector<RG::ResourceAccess> accesses{
        { "Vertices", RG::ResourceUsage::Read, RG::ResourceAccessPurpose::SHADER_READ },
        { "Table", RG::ResourceUsage::Read, RG::ResourceAccessPurpose::SHADER_READ },
        { "TLAS", RG::ResourceUsage::Read, RG::ResourceAccessPurpose::TRACE_READ }
    };
    renderer::PassResources resources(registry, accesses, "Trace");

    EXPECT_EQ(resources.Buffer("Vertices"), vertices);
    EXPECT_EQ(resources.StructuredBuffer("Table"), table);
    EXPECT_EQ(resources.AccelerationStructure("TLAS"), tlas);
    EXPECT_TRUE(resources.IsDeclared("TLAS"));
    EXPECT_FALSE(resources.IsDeclared("Missing"));
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
    /// @note 同じ名前を二重に申告しても辺は増えないが、鍵が変わって Plan を無駄にやり直す。
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
    /// @note 書いている名前を «追加で読む» と申告しても、自分自身への辺になるだけ。
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

    /// @note 刈ってよいパスを «残す» へ倒せる。
    renderer::RenderPassOverride keep;
    keep.allowCulling = false;
    bool cullable = true;
    Pipeline::ApplyOverride(keep, accesses, cullable);
    EXPECT_FALSE(cullable);

    /// @note パス自身のカリング禁止は UI の既定値から解除できない。
    renderer::RenderPassOverride release;
    release.allowCulling = true;
    bool mustRun = false;
    Pipeline::ApplyOverride(release, accesses, mustRun);
    EXPECT_FALSE(mustRun);
}

TEST_F(RenderPipelineOverrideTest, DefaultOverrideIsRecognisedAndChangesNothing)
{
    /// @note 既定に戻った行は保存も適用もしない。無害な行がプロジェクトに溜まるのを避ける。
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

} /// @note namespace fbzz::tests
