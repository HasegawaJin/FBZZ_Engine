/// @file    ShaderCapabilitiesTests.cpp
/// @brief   シェーダーの明示能力と GUID の実体を CPU だけで検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-02
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/TempDir.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/ShaderCapabilities.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Core/ILogSink.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>

namespace fbzz::tests {
namespace {

constexpr std::string_view kStandardGuid = "0123456789abcdef0123456789abcdef";
constexpr std::string_view kCustomGuid = "1123456789abcdef0123456789abcdef";
constexpr std::string_view kSurfaceDeclaration =
    "version = 1\nvertex = 'standard_surface_v1'\nsurface = 'metallic_roughness_v1'\n"
    "opacity = 'alpha_clip_v1'\nvariants = 'standard_surface_v1'\n";
constexpr std::string_view kCustomDeclaration =
    "version=1\nvertex='custom'\nsurface='custom'\nopacity='custom'\nvariants='none'\n";

void ExpectUnknown(const renderer::ShaderCapabilities& capabilities)
{
    EXPECT_EQ(capabilities.vertex, renderer::ShaderVertexContract::UNKNOWN);
    EXPECT_EQ(capabilities.surface, renderer::ShaderSurfaceContract::UNKNOWN);
    EXPECT_EQ(capabilities.opacity, renderer::ShaderOpacityContract::UNKNOWN);
    EXPECT_EQ(capabilities.variants, renderer::ShaderVariantSet::NONE);
    EXPECT_EQ(capabilities.preview, renderer::ShaderPreviewKind::UNKNOWN);
    EXPECT_FALSE(capabilities.SupportsGBuffer());
    EXPECT_FALSE(capabilities.SupportsSkinning());
    EXPECT_FALSE(capabilities.HasStandardGeometry());
    EXPECT_FALSE(capabilities.IsStandardPbr());
    EXPECT_FALSE(capabilities.HasOpaqueOutput());
}

void ExpectStandardSurface(const renderer::ShaderCapabilities& capabilities)
{
    EXPECT_EQ(capabilities.vertex, renderer::ShaderVertexContract::STANDARD_SURFACE);
    EXPECT_EQ(capabilities.surface, renderer::ShaderSurfaceContract::STANDARD_PBR);
    EXPECT_EQ(capabilities.opacity, renderer::ShaderOpacityContract::ALPHA_CLIP);
    EXPECT_EQ(capabilities.variants, renderer::ShaderVariantSet::STANDARD_SURFACE);
    EXPECT_EQ(capabilities.preview, renderer::ShaderPreviewKind::SURFACE);
    EXPECT_TRUE(capabilities.SupportsGBuffer());
    EXPECT_FALSE(capabilities.SupportsSkinning());
    EXPECT_TRUE(capabilities.HasStandardGeometry());
    EXPECT_TRUE(capabilities.IsStandardPbr());
    EXPECT_FALSE(capabilities.HasOpaqueOutput());
}

} /// @note namespace

class ShaderCapabilitiesTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        ASSERT_TRUE(m_temp.IsValid());
        std::error_code error;
        std::filesystem::create_directories(AssetsRoot(), error);
        ASSERT_FALSE(error);
    }

    void TearDown() override
    {
        asset::AssetDatabase::Shutdown();
        EngineFixture::TearDown();
    }

    std::filesystem::path AssetsRoot() const { return m_temp.File("Assets"); }
    std::string ShaderPath(const std::string& name) const { return (AssetsRoot() / name).generic_string(); }

    bool WriteShader(const std::string& name, std::string_view declaration,
                     std::string_view guid = {})
    {
        const std::string path = ShaderPath(name);
        if (!util::FileSystem::WriteText(path, "float4 PSMain() : SV_Target { return 1; }\n"))
            return false;
        std::string identifier(guid);
        if (identifier.empty()) {
            uint64_t hash = 14695981039346656037ull;
            for (const unsigned char character : name) { hash ^= character; hash *= 1099511628211ull; }
            const std::string suffix = std::to_string(hash);
            identifier = std::string(32 - suffix.size(), '0') + suffix;
        }
        const std::string meta = "[meta]\nguid = '" + identifier + "'\n[shader]\n" + std::string(declaration);
        return util::FileSystem::WriteText(path + ".meta", meta);
    }

    void InitDatabase() { asset::AssetDatabase::Init(AssetsRoot().generic_string()); }
    testkit::TempDir m_temp{"shader-capabilities"};
};

TEST_F(ShaderCapabilitiesTest, SharedWaterAndFiberGeometryRequireExplicitVertexContracts)
{
    ASSERT_TRUE(WriteShader("ProjectOcean.hlsl",
        "version=1\nvertex='water_surface_v1'\nsurface='water_v1'\nopacity='custom'\nvariants='none'\n"));
    ASSERT_TRUE(WriteShader("CustomOcean.hlsl",
        "version=1\nvertex='custom'\nsurface='water_v1'\nopacity='custom'\nvariants='none'\n"));
    const auto water = asset::ResolveShaderCapabilities(ShaderPath("ProjectOcean.hlsl"));
    EXPECT_TRUE(water.SupportsWaterGeometry());
    EXPECT_FALSE(water.HasStandardGeometry());
    EXPECT_FALSE(water.SupportsGBuffer());
    EXPECT_EQ(water.preview, renderer::ShaderPreviewKind::WATER);
    EXPECT_FALSE(asset::ResolveShaderCapabilities(ShaderPath("CustomOcean.hlsl")).SupportsWaterGeometry());
    for (const char* vertex : {"fiber_shell_v1", "fiber_fin_v1", "fiber_blade_v1"}) {
        SCOPED_TRACE(vertex);
        ASSERT_TRUE(WriteShader(std::string(vertex) + ".hlsl", "version=1\nvertex='" + std::string(vertex)
            + "'\nsurface='custom'\nopacity='custom'\nvariants='none'\npreview='fiber_shell'\n"));
        const auto fiber = asset::ResolveShaderCapabilities(ShaderPath(std::string(vertex) + ".hlsl"));
        EXPECT_TRUE(fiber.SupportsFiberGeometry());
        EXPECT_FALSE(fiber.SupportsWaterGeometry());
        EXPECT_FALSE(fiber.HasStandardGeometry());
        EXPECT_FALSE(fiber.SupportsGBuffer());
    }
}
TEST_F(ShaderCapabilitiesTest, FilenameDoesNotGrantCapabilitiesWithoutADeclaration)
{
    ASSERT_TRUE(util::FileSystem::WriteText(ShaderPath("PBR.hlsl"), "float4 PSMain() : SV_Target { return 1; }\n"));
    ASSERT_TRUE(util::FileSystem::WriteText(ShaderPath("PBR_Skinned.hlsl"), "float4 PSMain() : SV_Target { return 1; }\n"));
    ASSERT_TRUE(util::FileSystem::WriteText(ShaderPath("PBR_Skinned.hlsl") + ".meta", "guid='1123456789abcdef0123456789abcdef'\n"));

    ExpectUnknown(asset::ResolveShaderCapabilities(ShaderPath("PBR.hlsl")));
    ExpectUnknown(asset::ResolveShaderCapabilities(ShaderPath("PBR_Skinned.hlsl")));
    ExpectUnknown(asset::ResolveShaderCapabilities(ShaderPath("Missing.hlsl")));
    ExpectUnknown(asset::ResolveShaderCapabilities(""));
}

TEST_F(ShaderCapabilitiesTest, ExplicitStandardContractsWorkWithAnyFilename)
{
    ASSERT_TRUE(WriteShader("CustomName.hlsl", kSurfaceDeclaration));

    ExpectStandardSurface(asset::ResolveShaderCapabilities(ShaderPath("CustomName.hlsl")));
}

TEST_F(ShaderCapabilitiesTest, StandardSkinnedVariantsRequireTheMatchingVertexContract)
{
    ASSERT_TRUE(WriteShader("Character.hlsl",
        "version=1\nvertex='standard_skinned_v1'\nsurface='metallic_roughness_v1'\n"
        "opacity='alpha_clip_v1'\nvariants='standard_skinned_v1'\n"));

    const auto capabilities = asset::ResolveShaderCapabilities(ShaderPath("Character.hlsl"));

    EXPECT_TRUE(capabilities.SupportsSkinning());
    EXPECT_TRUE(capabilities.SupportsGBuffer());
    EXPECT_TRUE(capabilities.HasStandardGeometry());
    EXPECT_TRUE(capabilities.IsStandardPbr());
    EXPECT_EQ(capabilities.preview, renderer::ShaderPreviewKind::SKINNED);
}

TEST_F(ShaderCapabilitiesTest, SurfaceOrGeometryAloneDoesNotGrantGBufferSupport)
{
    ASSERT_TRUE(WriteShader("ForwardPbr.hlsl",
        "version=1\nvertex='custom'\nsurface='metallic_roughness_v1'\n"
        "opacity='custom'\nvariants='none'\n"));
    ASSERT_TRUE(WriteShader("NoVariants.hlsl",
        "version=1\nvertex='standard_surface_v1'\nsurface='metallic_roughness_v1'\n"
        "opacity='alpha_clip_v1'\nvariants='none'\n"));
    ASSERT_TRUE(WriteShader("Lambert.hlsl",
        "version=1\nvertex='standard_surface_v1'\nsurface='lambert_v1'\n"
        "opacity='alpha_clip_v1'\nvariants='standard_surface_v1'\n"));
    ASSERT_TRUE(WriteShader("Opaque.hlsl",
        "version=1\nvertex='standard_surface_v1'\nsurface='metallic_roughness_v1'\n"
        "opacity='opaque_v1'\nvariants='standard_surface_v1'\n"));

    const auto customVertex = asset::ResolveShaderCapabilities(ShaderPath("ForwardPbr.hlsl"));
    EXPECT_TRUE(customVertex.IsStandardPbr());
    EXPECT_FALSE(customVertex.HasStandardGeometry());
    EXPECT_FALSE(customVertex.SupportsGBuffer());
    const auto noVariants = asset::ResolveShaderCapabilities(ShaderPath("NoVariants.hlsl"));
    EXPECT_TRUE(noVariants.HasStandardGeometry());
    EXPECT_FALSE(noVariants.SupportsGBuffer());
    const auto lambert = asset::ResolveShaderCapabilities(ShaderPath("Lambert.hlsl"));
    EXPECT_TRUE(lambert.HasStandardGeometry());
    EXPECT_FALSE(lambert.IsStandardPbr());
    EXPECT_FALSE(lambert.SupportsGBuffer());
    const auto opaque = asset::ResolveShaderCapabilities(ShaderPath("Opaque.hlsl"));
    EXPECT_TRUE(opaque.HasOpaqueOutput());
    EXPECT_FALSE(opaque.SupportsGBuffer());
}

TEST_F(ShaderCapabilitiesTest, InvalidVersionsTokensAndVariantPairsRejectTheWholeDeclaration)
{
    const std::string invalidDeclarations[] = {
        "version=2\nvertex='standard_surface_v1'\nsurface='metallic_roughness_v1'\nopacity='alpha_clip_v1'\nvariants='standard_surface_v1'\n",
        "version='1'\nvertex='standard_surface_v1'\nsurface='metallic_roughness_v1'\nopacity='alpha_clip_v1'\nvariants='standard_surface_v1'\n",
        "version=1.0\nvertex='standard_surface_v1'\nsurface='metallic_roughness_v1'\nopacity='alpha_clip_v1'\nvariants='standard_surface_v1'\n",
        "version=1\nvertex='unknown'\nsurface='metallic_roughness_v1'\nopacity='alpha_clip_v1'\nvariants='none'\n",
        "version=1\nvertex='standard_surface_v1'\nsurface='unknown'\nopacity='alpha_clip_v1'\nvariants='standard_surface_v1'\n",
        "version=1\nvertex='standard_surface_v1'\nsurface='metallic_roughness_v1'\nopacity='unknown'\nvariants='standard_surface_v1'\n",
        "version=1\nvertex='standard_surface_v1'\nsurface='metallic_roughness_v1'\nopacity='alpha_clip_v1'\nvariants='unknown'\n",
        "version=1\nvertex='standard_surface_v1'\nsurface='metallic_roughness_v1'\nopacity='alpha_clip_v1'\nvariants='standard_skinned_v1'\n",
        "version=1\nvertex='standard_skinned_v1'\nsurface='metallic_roughness_v1'\nopacity='alpha_clip_v1'\nvariants='standard_surface_v1'\n",
        "version=1\nvertex='custom'\nsurface='metallic_roughness_v1'\nopacity='alpha_clip_v1'\nvariants='standard_surface_v1'\n",
        "version=1\nvertex='standard_surface_v1'\nsurface='metallic_roughness_v1'\nopacity='alpha_clip_v1'\n",
        "version=1\nvertex='standard_surface_v1'\nsurface='metallic_roughness_v1'\nopacity='alpha_clip_v1'\nvariants='standard_surface_v1'\npreview='unknown'\n",
        "version=1\nvertex='standard_surface_v1'\nsurface='metallic_roughness_v1'\nopacity='alpha_clip_v1'\nvariants='standard_surface_v1'\npreview=1\n",
        "version=1\nvertex=[\n"
    };
    for (size_t i = 0; i < std::size(invalidDeclarations); ++i) {
        const std::string name = "Invalid" + std::to_string(i) + ".hlsl";
        ASSERT_TRUE(WriteShader(name, invalidDeclarations[i]));
        ExpectUnknown(asset::ResolveShaderCapabilities(ShaderPath(name)));
    }
}

TEST_F(ShaderCapabilitiesTest, GuidAuthorityIgnoresFalseAndUnresolvedPathHints)
{
    ASSERT_TRUE(WriteShader("Standard.hlsl", kSurfaceDeclaration, kStandardGuid));
    ASSERT_TRUE(WriteShader("PBR.hlsl",
        "version=1\nvertex='custom'\nsurface='custom'\nopacity='custom'\nvariants='none'\n", kCustomGuid));
    InitDatabase();
    const std::string standardWithFalseHint = "guid:" + std::string(kStandardGuid) + "|Assets/PBR.hlsl";
    const std::string customWithFalseHint = "guid:" + std::string(kCustomGuid) + "|Assets/Standard.hlsl";

    ExpectStandardSurface(asset::ResolveShaderCapabilities(standardWithFalseHint));
    ExpectUnknown(asset::ResolveShaderCapabilities(customWithFalseHint));
    ExpectUnknown(asset::ResolveShaderCapabilities("guid:ffffffffffffffffffffffffffffffff|Assets/Standard.hlsl"));
}

TEST_F(ShaderCapabilitiesTest, MovingShaderAndMetaKeepsTheGuidCapabilities)
{
    ASSERT_TRUE(WriteShader("Original.hlsl", kSurfaceDeclaration, kStandardGuid));
    InitDatabase();
    const std::string reference = "guid:" + std::string(kStandardGuid) + "|Assets/Original.hlsl";
    ExpectStandardSurface(asset::ResolveShaderCapabilities(reference));
    const std::string before = ShaderPath("Original.hlsl");
    const std::string after = ShaderPath("Renamed.hlsl");
    ASSERT_TRUE(util::FileSystem::Rename(before, after));
    ASSERT_TRUE(util::FileSystem::Rename(before + ".meta", after + ".meta"));
    asset::AssetDatabase::OnAssetMoved(before, after);

    EXPECT_EQ(asset::AssetDatabase::PathFromGuid(std::string(kStandardGuid)), after);
    ExpectStandardSurface(asset::ResolveShaderCapabilities(reference));
    ExpectStandardSurface(asset::ResolveShaderCapabilities(after));
    /// @note 改名前のパスは AssetManager が同じ GUID の現ファイルへ正規化する。
    ExpectStandardSurface(asset::ResolveShaderCapabilities(before));
}

TEST_F(ShaderCapabilitiesTest, MetaTimestampChangesAndRemovedFilesInvalidateCachedCapabilities)
{
    ASSERT_TRUE(WriteShader("Changing.hlsl", kSurfaceDeclaration));
    const std::string path = ShaderPath("Changing.hlsl");
    const auto metaPath = std::filesystem::path(path + ".meta");
    std::error_code error;
    const auto originalTime = std::filesystem::last_write_time(metaPath, error);
    ASSERT_FALSE(error);
    ExpectStandardSurface(asset::ResolveShaderCapabilities(path));
    ASSERT_TRUE(WriteShader("Changing.hlsl",
        "version=1\nvertex='custom'\nsurface='custom'\nopacity='custom'\nvariants='none'\n"));
    /// @note sleep や現在時刻に依存せず、監視するファイル時刻の変更を確定させる。
    std::filesystem::last_write_time(metaPath, originalTime + std::chrono::seconds(2), error);
    ASSERT_FALSE(error);

    ExpectUnknown(asset::ResolveShaderCapabilities(path));
    ASSERT_TRUE(std::filesystem::remove(metaPath, error));
    ASSERT_FALSE(error);
    ExpectUnknown(asset::ResolveShaderCapabilities(path));
    ASSERT_TRUE(WriteShader("Changing.hlsl", kSurfaceDeclaration));
    std::filesystem::last_write_time(metaPath, originalTime + std::chrono::seconds(4), error);
    ASSERT_FALSE(error);
    ExpectStandardSurface(asset::ResolveShaderCapabilities(path));
    ASSERT_TRUE(std::filesystem::remove(std::filesystem::path(path), error));
    ASSERT_FALSE(error);
    ExpectUnknown(asset::ResolveShaderCapabilities(path));
    ASSERT_TRUE(WriteShader("Changing.hlsl", kSurfaceDeclaration));
    std::filesystem::last_write_time(metaPath, originalTime + std::chrono::seconds(6), error);
    ASSERT_FALSE(error);
    ExpectStandardSurface(asset::ResolveShaderCapabilities(path));
}

TEST_F(ShaderCapabilitiesTest, UnicodePreparedPathsKeepMetadataDeletionAndRestorationFresh)
{
    const auto nativePath = AssetsRoot() / L"\u6D77\u6D41\u30B7\u30A7\u30FC\u30C0\u30FC.hlsl";
    const std::string path = util::FileSystem::PathToUtf8(nativePath);
    const std::string metaPath = path + ".meta";
    ASSERT_TRUE(util::FileSystem::WriteText(path, "float4 PSMain() : SV_Target { return 1; }\n"));
    ASSERT_TRUE(util::FileSystem::WriteText(metaPath, "[shader]\n" + std::string(kSurfaceDeclaration)));
    ExpectStandardSurface(asset::ResolveShaderCapabilities(path));
    ExpectStandardSurface(asset::ResolveShaderCapabilities(path));
    std::error_code error;
    const auto nativeMetaPath = util::FileSystem::PathFromUtf8(metaPath);
    const auto originalTime = std::filesystem::last_write_time(nativeMetaPath, error);
    ASSERT_FALSE(error);
    ASSERT_TRUE(std::filesystem::remove(nativeMetaPath, error));
    ASSERT_FALSE(error);
    ExpectUnknown(asset::ResolveShaderCapabilities(path));
    ASSERT_TRUE(util::FileSystem::WriteText(metaPath, "[shader]\n" + std::string(kCustomDeclaration)));
    std::filesystem::last_write_time(nativeMetaPath, originalTime + std::chrono::seconds(2), error);
    ASSERT_FALSE(error);
    ExpectUnknown(asset::ResolveShaderCapabilities(path));
    ASSERT_TRUE(util::FileSystem::WriteText(metaPath, "[shader]\n" + std::string(kSurfaceDeclaration)));
    std::filesystem::last_write_time(nativeMetaPath, originalTime + std::chrono::seconds(4), error);
    ASSERT_FALSE(error);
    ExpectStandardSurface(asset::ResolveShaderCapabilities(path));
}

TEST_F(ShaderCapabilitiesTest, MissingSourceWithInvalidUtf8ReturnsUnknown)
{
    std::string reference = ShaderPath("MissingInvalidUtf8");
    reference.push_back(static_cast<char>(0xFF));
    reference += ".hlsl";

    ExpectUnknown(asset::ResolveShaderCapabilities(reference));
    ExpectUnknown(asset::ResolveShaderCapabilities(reference));
}

TEST_F(ShaderCapabilitiesTest, GuidPreparedPathsFollowRemovalRemappingAndDatabaseReset)
{
    ASSERT_TRUE(WriteShader("First.hlsl", kSurfaceDeclaration, kStandardGuid));
    ASSERT_TRUE(WriteShader("Second.hlsl", kCustomDeclaration, kCustomGuid));
    InitDatabase();
    const std::string reference = "guid:" + std::string(kStandardGuid) + "|Assets/First.hlsl";
    ExpectStandardSurface(asset::ResolveShaderCapabilities(reference));
    ExpectStandardSurface(asset::ResolveShaderCapabilities(reference));
    const auto firstRevision = asset::AssetDatabase::Revision();
    asset::AssetDatabase::OnAssetRemoved(ShaderPath("First.hlsl"));
    EXPECT_GT(asset::AssetDatabase::Revision(), firstRevision);
    /// @note The file and hint still exist; only the current GUID index may authorize this reference.
    ExpectUnknown(asset::ResolveShaderCapabilities(reference));
    asset::AssetDatabase::OnAssetRemoved(ShaderPath("Second.hlsl"));
    ASSERT_TRUE(WriteShader("Second.hlsl", kCustomDeclaration, kStandardGuid));
    EXPECT_EQ(asset::AssetDatabase::GuidFromPath(ShaderPath("Second.hlsl")), kStandardGuid);
    ExpectUnknown(asset::ResolveShaderCapabilities(reference));

    const auto replacementRoot = m_temp.File("ReplacementAssets");
    ASSERT_TRUE(util::FileSystem::EnsureDirectory(replacementRoot));
    const std::string replacementPath = util::FileSystem::PathToUtf8(replacementRoot / "Third.hlsl");
    ASSERT_TRUE(util::FileSystem::WriteText(replacementPath, "float4 PSMain() : SV_Target { return 1; }\n"));
    ASSERT_TRUE(util::FileSystem::WriteText(replacementPath + ".meta",
        "[meta]\nguid='" + std::string(kStandardGuid) + "'\n[shader]\n" + std::string(kSurfaceDeclaration)));
    asset::AssetDatabase::Shutdown();
    asset::AssetDatabase::Init(util::FileSystem::PathToUtf8(replacementRoot));
    ExpectStandardSurface(asset::ResolveShaderCapabilities(reference));
    asset::AssetDatabase::Shutdown();
    ExpectUnknown(asset::ResolveShaderCapabilities(reference));
    asset::AssetDatabase::Init(util::FileSystem::PathToUtf8(replacementRoot));
    ExpectStandardSurface(asset::ResolveShaderCapabilities(reference));
}

TEST_F(ShaderCapabilitiesTest, ManyDistinctGuidHintsKeepMetadataQueriesFreshAfterReferenceChurn)
{
    ASSERT_TRUE(WriteShader("ManyHints.hlsl", kSurfaceDeclaration, kStandardGuid));
    InitDatabase();
    const std::string prefix = "guid:" + std::string(kStandardGuid) + "|Assets/UntrustedHint";
    for (int i = 0; i < 96; ++i)
        ExpectStandardSurface(asset::ResolveShaderCapabilities(prefix + std::to_string(i) + ".hlsl"));
    const auto metaPath = std::filesystem::path(ShaderPath("ManyHints.hlsl") + ".meta");
    std::error_code error;
    const auto originalTime = std::filesystem::last_write_time(metaPath, error);
    ASSERT_FALSE(error);
    ASSERT_TRUE(WriteShader("ManyHints.hlsl", kCustomDeclaration, kStandardGuid));
    std::filesystem::last_write_time(metaPath, originalTime + std::chrono::seconds(2), error);
    ASSERT_FALSE(error);
    for (int i = 0; i < 96; ++i)
        ExpectUnknown(asset::ResolveShaderCapabilities(prefix + std::to_string(i) + ".hlsl"));
}

TEST_F(ShaderCapabilitiesTest, PreparedSharedGuidBecomesUnknownWhenTheIndexMakesItAmbiguous)
{
    const auto sharedRoot = m_temp.File("Shared/Assets");
    ASSERT_TRUE(util::FileSystem::EnsureDirectory(sharedRoot));
    const auto writeSharedShader = [&](const std::string& name, std::string_view declaration) {
        const std::string path = util::FileSystem::PathToUtf8(sharedRoot / name);
        return util::FileSystem::WriteText(path, "float4 PSMain() : SV_Target { return 1; }\n")
            && util::FileSystem::WriteText(path + ".meta",
                "[meta]\nguid='" + std::string(kStandardGuid) + "'\n[shader]\n" + std::string(declaration));
    };
    ASSERT_TRUE(writeSharedShader("First.hlsl", kSurfaceDeclaration));
    asset::AssetDatabase::Init(AssetsRoot().generic_string(), util::FileSystem::PathToUtf8(sharedRoot));
    const std::string reference = "guid:" + std::string(kStandardGuid);
    ExpectStandardSurface(asset::ResolveShaderCapabilities(reference));
    const auto previousRevision = asset::AssetDatabase::Revision();
    ASSERT_TRUE(writeSharedShader("Second.hlsl", kCustomDeclaration));
    EXPECT_TRUE(asset::AssetDatabase::GuidFromPath(util::FileSystem::PathToUtf8(sharedRoot / "Second.hlsl")).empty());
    EXPECT_GT(asset::AssetDatabase::Revision(), previousRevision);
    ExpectUnknown(asset::ResolveShaderCapabilities(reference));
}

TEST_F(ShaderCapabilitiesTest, NonGuidInputEditsDuringLoggingCannotOverwriteAnotherPreparedGuidIdentity)
{
    ASSERT_TRUE(WriteShader("Original.hlsl", kSurfaceDeclaration, kStandardGuid));
    ASSERT_TRUE(WriteShader("Other.hlsl", kCustomDeclaration, kCustomGuid));
    InitDatabase();
    ASSERT_TRUE(WriteShader("DuplicateAfterIndexing.hlsl", kSurfaceDeclaration, kStandardGuid));
    std::string input = ShaderPath("DuplicateAfterIndexing.hlsl");
    const std::string prefix = "guid:" + std::string(kCustomGuid) + "|";
    ASSERT_GE(input.size(), prefix.size());
    const std::string otherReference = prefix + std::string(input.size() - prefix.size(), 'h');
    ExpectUnknown(asset::ResolveShaderCapabilities(otherReference));
    const auto preparedRevision = asset::AssetDatabase::Revision();

    class InputRewritingSink final : public core::ILogSink {
    public:
        InputRewritingSink(std::string& input, const std::string& replacement)
            : m_input(input), m_replacement(replacement) { core::Logger::AddSink(this); }
        ~InputRewritingSink() override { core::Logger::RemoveSink(this); }
        void OnLog(const core::LogEntry& entry) override {
            if (rewritten || entry.message.find("AssetDatabase: duplicate guid") == std::string::npos) return;
            /// @note Change bytes in place so the caller's view stays valid; the callback never reenters Logger or AssetDatabase.
            std::copy(m_replacement.begin(), m_replacement.end(), m_input.begin());
            rewritten = true;
        }
        bool rewritten = false;
    private:
        std::string& m_input;
        const std::string& m_replacement;
    } sink(input, otherReference);

    ExpectStandardSurface(asset::ResolveShaderCapabilities(input));

    ASSERT_TRUE(sink.rewritten);
    EXPECT_EQ(input, otherReference);
    EXPECT_EQ(asset::AssetDatabase::Revision(), preparedRevision);
    ExpectUnknown(asset::ResolveShaderCapabilities(otherReference));
}

} /// @note namespace fbzz::tests
