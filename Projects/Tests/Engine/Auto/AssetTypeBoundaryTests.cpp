/// @file    AssetTypeBoundaryTests.cpp
/// @brief   EXE から組込みアセットを操作しても Engine DLL の単一ストアを使うことを検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/Engine/ResourceTrackingRenderer.hpp>
#include <TestKit/TempDir.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/ParticleCurveAsset.hpp>
#include <Engine/Asset/VectorFieldFile.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Fluid/VectorFieldAsset.hpp>

namespace fbzz::tests {
class AssetTypeBoundaryTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        ASSERT_TRUE(m_temp.IsValid());
        m_resources = std::make_unique<renderer::ResourceManager>(m_device);
        asset::AssetManager::Init(*m_resources, m_temp.Path().generic_string() + "/");
    }
    void TearDown() override
    {
        asset::AssetManager::UnloadAll();
        m_resources.reset();
        EngineFixture::TearDown();
    }
    testkit::TempDir m_temp{"asset-type-boundary"};
    testkit::ResourceTrackingRenderer m_device;
    std::unique_ptr<renderer::ResourceManager> m_resources;
};

TEST_F(AssetTypeBoundaryTest, ClientLoadsVectorFieldRegisteredByEngineAndReloadsTheSameHandle)
{
    const auto path = m_temp.File("Field.fga").generic_string();
    ASSERT_TRUE(util::FileSystem::WriteText(path, "1,1,1,-100,-100,-100,100,100,100,1,2,3,"));
    const auto handle = asset::AssetManager::Load<fluid::VectorFieldAsset>(path);
    ASSERT_TRUE(handle.IsValid());
    const auto* initial = asset::AssetManager::Get(handle);
    ASSERT_NE(initial, nullptr);
    EXPECT_EQ(initial->sizeX, fluid::kVelocityFieldTileResolution);
    const auto magnitude = initial->maxMagnitude;
    ASSERT_TRUE(util::FileSystem::WriteText(path, "1,1,1,-100,-100,-100,100,100,100,4,5,6,"));
    EXPECT_EQ(asset::AssetManager::ReloadPath(path), 1);
    EXPECT_EQ(asset::AssetManager::Load<fluid::VectorFieldAsset>(path), handle);
    ASSERT_NE(asset::AssetManager::Get(handle), nullptr);
    EXPECT_GT(asset::AssetManager::Get(handle)->maxMagnitude, magnitude);
    asset::AssetManager::Unload<fluid::VectorFieldAsset>(path);
    EXPECT_EQ(asset::AssetManager::Get(handle), nullptr);
    EXPECT_NE(asset::AssetManager::Load<fluid::VectorFieldAsset>(path), handle);
}

TEST_F(AssetTypeBoundaryTest, FailedCurveLoadCanRecoverThroughEngineFlushAndProjectSwitchInvalidatesTheHandle)
{
    const auto path = m_temp.File("Shape.curve").generic_string();
    EXPECT_FALSE(asset::AssetManager::Load<asset::ParticleCurveAsset>(path).IsValid());
    asset::ParticleCurveAsset curve;
    curve.hasCurve = true;
    ASSERT_TRUE(asset::SaveParticleCurveAsset(path, curve));
    EXPECT_FALSE(asset::AssetManager::Load<asset::ParticleCurveAsset>(path).IsValid());
    asset::AssetManager::FlushFailed();
    const auto handle = asset::AssetManager::Load<asset::ParticleCurveAsset>(path);
    ASSERT_TRUE(handle.IsValid());
    ASSERT_NE(asset::AssetManager::Get(handle), nullptr);
    EXPECT_TRUE(asset::AssetManager::Get(handle)->hasCurve);
    asset::AssetManager::UnloadAll();
    asset::AssetManager::Init(*m_resources, m_temp.Path().generic_string() + "/");
    EXPECT_EQ(asset::AssetManager::Get(handle), nullptr);
    const auto reloaded = asset::AssetManager::Load<asset::ParticleCurveAsset>(path);
    EXPECT_TRUE(reloaded.IsValid());
    EXPECT_NE(reloaded, handle);
}

TEST_F(AssetTypeBoundaryTest, ClientImporterRegistrationIsUsedByEngineReload)
{
    class Importer final : public asset::IAssetImporter<fluid::VectorFieldAsset> {
    public:
        explicit Importer(int& imports) : m_imports(imports) {}
        std::unique_ptr<fluid::VectorFieldAsset> Import(const std::string&, renderer::ResourceManager*) override
        {
            auto field = std::make_unique<fluid::VectorFieldAsset>();
            field->maxMagnitude = static_cast<float>(++m_imports);
            return field;
        }
        std::span<const std::string_view> SupportedExtensions() const override
        {
            static constexpr std::string_view EXTENSIONS[]{".fga"};
            return EXTENSIONS;
        }
    private:
        int& m_imports;
    };
    int imports = 0;
    const auto path = m_temp.File("Custom.fga").generic_string();
    ASSERT_TRUE(util::FileSystem::WriteText(path, "custom"));
    asset::AssetManager::RegisterImporter<fluid::VectorFieldAsset>(std::make_unique<Importer>(imports));
    const auto handle = asset::AssetManager::Load<fluid::VectorFieldAsset>(path);
    ASSERT_NE(asset::AssetManager::Get(handle), nullptr);
    EXPECT_NEAR(asset::AssetManager::Get(handle)->maxMagnitude, 1.0f, testkit::kTolerance);
    EXPECT_EQ(asset::AssetManager::ReloadPath(path), 1);
    EXPECT_EQ(imports, 2);
    EXPECT_NEAR(asset::AssetManager::Get(handle)->maxMagnitude, 2.0f, testkit::kTolerance);
}
} /// @note namespace fbzz::tests
