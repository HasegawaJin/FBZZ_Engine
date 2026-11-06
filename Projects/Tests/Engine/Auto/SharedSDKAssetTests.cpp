/// @file    SharedSDKAssetTests.cpp
/// @brief   SDK 原本のモデルロードと共有 shader include の依存追跡を検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-06
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/Engine/ResourceTrackingRenderer.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/ModelAsset.hpp>
#include <Engine/Scene/MeshResolver.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Graphics/Renderer/ShaderDependencyTracker.hpp>
#include <Graphics/Renderer/ShaderPathResolver.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>

namespace fbzz::tests {
namespace {
std::filesystem::path g_sharedShaderRoot;
std::filesystem::path ResolveSharedShader(const std::filesystem::path& requested)
{
    const auto relative = requested.lexically_relative("Assets/Shaders");
    return !relative.empty() && *relative.begin() != ".." ? g_sharedShaderRoot / relative : requested;
}
} /// @note namespace

class SharedSDKAssetTest : public testkit::EngineFixture {
protected:
    testkit::TempDir m_temp{"shared-sdk-assets"};
    testkit::ResourceTrackingRenderer m_renderer;
    renderer::ResourceManager m_resources{m_renderer};
    void SetUp() override
    {
        EngineFixture::SetUp();
        ASSERT_TRUE(m_temp.IsValid());
        std::filesystem::create_directories(m_temp.File("Project/Assets"));
        asset::AssetManager::Init(m_resources, m_temp.File("Project/Assets").generic_string());
    }
    void TearDown() override
    {
        renderer::SetShaderPathResolver(nullptr);
        g_sharedShaderRoot.clear();
        asset::AssetManager::UnloadAll();
        EngineFixture::TearDown();
    }
};

TEST_F(SharedSDKAssetTest, LoadsSharedObjWithoutProjectCopyOrBakedContainer)
{
    const auto shared = m_temp.File("SDK/Assets/Models/Triangle.obj");
    const std::string guid = "531c0c8123456789abcdef0123456789";
    const std::string meta = "[meta]\nguid = '" + guid + "'\n";
    const std::string obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    std::filesystem::create_directories(shared.parent_path());
    ASSERT_TRUE(util::FileSystem::WriteText(shared, obj));
    ASSERT_TRUE(util::FileSystem::WriteText(shared.string() + ".meta", meta));
    asset::AssetDatabase::Init(m_temp.File("Project/Assets").generic_string(),
        m_temp.File("SDK/Assets").generic_string());
    const auto handle = asset::AssetManager::Load<asset::ModelAsset>("guid:" + guid);
    const auto* model = asset::AssetManager::Get(handle);
    ASSERT_NE(model, nullptr);
    ASSERT_EQ(model->lods.size(), 1u);
    ASSERT_EQ(model->lods[0].submeshes.size(), 1u);
    ASSERT_NE(model->lods[0].submeshes[0].mesh, nullptr);
    EXPECT_EQ(model->lods[0].submeshes[0].mesh->cpuIndices.size(), 3u);
    const auto& mesh = *model->lods[0].submeshes[0].mesh;
    EXPECT_EQ(mesh.vertexCount, 3u);
    EXPECT_EQ(mesh.indexCount, 3u);
    EXPECT_VEC3_NEAR(mesh.boundsCenter, (math::Vector3{0.5f, 0.5f, 0.0f}), 0.00001f);
    auto* instanceMesh = scene::ResolveMeshPath("guid:" + guid + "|Assets/Models/Triangle.obj:0", m_resources);
    ASSERT_NE(instanceMesh, nullptr);
    EXPECT_EQ(instanceMesh->indexCount, 3u);
    EXPECT_VEC3_NEAR(instanceMesh->boundsCenter, (math::Vector3{0.5f, 0.5f, 0.0f}), 0.00001f);
    EXPECT_NE(m_resources.Get(instanceMesh->vertexBuffer), nullptr);
    EXPECT_NE(m_resources.Get(instanceMesh->indexBuffer), nullptr);
    EXPECT_FALSE(std::filesystem::exists(m_temp.File("Project/Assets/Models/Triangle.obj")));
    EXPECT_FALSE(std::filesystem::exists(m_temp.File("Project/Library/Baked") / guid));
    std::string saved;
    ASSERT_TRUE(util::FileSystem::ReadText(shared.string() + ".meta", saved));
    EXPECT_EQ(saved, meta);
}

TEST_F(SharedSDKAssetTest, SourceGltfPreservesMeterGeometryAndBakedNodeTranslation)
{
    const auto source = m_temp.File("Project/Assets/Triangle.GLTF");
    const std::string text = R"({
        "asset":{"version":"2.0"},
        "buffers":[{"byteLength":36,"uri":"data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA"}],
        "bufferViews":[{"buffer":0,"byteLength":36,"target":34962}],
        "accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]}],
        "meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],
        "nodes":[{"mesh":0,"translation":[0,2,0]}],
        "scenes":[{"nodes":[0]}],"scene":0
    })";
    ASSERT_TRUE(util::FileSystem::WriteText(source, text));
    const auto handle = asset::AssetManager::Load<asset::ModelAsset>(source.generic_string());
    const auto* model = asset::AssetManager::Get(handle);
    ASSERT_NE(model, nullptr);
    ASSERT_EQ(model->lods.size(), 1u);
    ASSERT_EQ(model->lods[0].submeshes.size(), 1u);
    ASSERT_NE(model->lods[0].submeshes[0].mesh, nullptr);
    const auto& mesh = *model->lods[0].submeshes[0].mesh;
    EXPECT_EQ(mesh.indexCount, 3u);
    EXPECT_VEC3_NEAR(mesh.boundsCenter, (math::Vector3{0.5f, 2.5f, 0.0f}), 0.00001f);
    EXPECT_VEC3_NEAR(mesh.boundsExtents, (math::Vector3{0.5f, 0.5f, 0.0f}), 0.00001f);
}

TEST_F(SharedSDKAssetTest, SharedIncludesRemainCacheableAndTrackSdkChanges)
{
    const auto source = m_temp.File("Project/Assets/Shaders/Custom/Surface.hlsl");
    g_sharedShaderRoot = m_temp.File("SDK/Assets/Shaders");
    const auto include = g_sharedShaderRoot / "Common/Shared.hlsli";
    const auto binary = m_temp.File("Surface.cso");
    std::filesystem::create_directories(source.parent_path());
    std::filesystem::create_directories(include.parent_path());
    ASSERT_TRUE(util::FileSystem::WriteText(source, "#include \"Common/Shared.hlsli\"\n"));
    ASSERT_TRUE(util::FileSystem::WriteText(include, "float Value = 1;\n"));
    ASSERT_TRUE(util::FileSystem::WriteText(binary, "compiled"));
    renderer::SetShaderPathResolver(&ResolveSharedShader);
    const auto timestamp = std::filesystem::file_time_type{} + std::chrono::hours(24);
    std::filesystem::last_write_time(source, timestamp);
    std::filesystem::last_write_time(include, timestamp);
    std::filesystem::last_write_time(binary, timestamp + std::chrono::seconds(2));
    EXPECT_FALSE(renderer::shader_dependency::IsBinaryStale(source, binary));
    std::filesystem::last_write_time(include, timestamp + std::chrono::seconds(3));
    EXPECT_TRUE(renderer::shader_dependency::IsBinaryStale(source, binary));
}
} /// @note namespace fbzz::tests
