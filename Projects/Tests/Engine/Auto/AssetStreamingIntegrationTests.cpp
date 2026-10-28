/// @file    AssetStreamingIntegrationTests.cpp
/// @brief   配布キャッシュの実ロードと描画利用権による常駐・解放を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#include <TestKit/TestKit.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/TempDir.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetStreaming.hpp>
#include <Engine/Asset/StreamedTextureResolver.hpp>
#include <Engine/Asset/TextureStreamCache.hpp>
#include <Engine/Asset/TextureStreamChannel.hpp>
#include <TestKit/Engine/LogSinks.hpp>
#include <Engine/Asset/TextureAsset.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Renderer/IShader.hpp>
#include <Engine/Renderer/IConstantBuffer.hpp>
#include <Engine/Asset/MaterialParamBinding.hpp>
#include "../../../Engine/src/Scene/Systems/RenderPasses/Geometry/GeometryPasses.hpp"
#include <functional>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/MatAssetImporter.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include "../../../Engine/src/Scene/Managed/ManagedServicesBridge.hpp"
#include "../../../Engine/src/Scene/Managed/ManagedMaterialTransfers.hpp"
#include <bit>
#include <array>
#include <cstring>
#include <limits>
#include <fstream>

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


class CpuTexture final : public renderer::ITexture {
public:
    CpuTexture(uint32_t width, uint32_t height) : m_width(width), m_height(height) {}
    uint32_t GetWidth() const override { return m_width; }
    uint32_t GetHeight() const override { return m_height; }
private:
    uint32_t m_width, m_height;
};
class CpuMaterialShader final : public renderer::IShader {
public:
    explicit CpuMaterialShader(std::string path) : m_path(std::move(path)) {
        renderer::ShaderVarDesc values; values.name = "values"; values.varClass = renderer::ShaderVarClass::Vector;
        values.varType = renderer::ShaderVarType::UInt; values.columns = 4; values.elements = 2251;
        values.arrayStride = 16; values.size = 9004 * 4;
        renderer::ShaderVarDesc matrix; matrix.name = "matrix"; matrix.varClass = renderer::ShaderVarClass::Matrix;
        matrix.rows = matrix.columns = 4; matrix.rowMajor = true; matrix.offset = values.size; matrix.size = 64;
        m_descriptor.vars = {values, matrix}; m_descriptor.cbufferSize = values.size + 64;
    }
    const std::string& GetPath() const override { return m_path; }
    const renderer::ShaderDescriptor& GetDescriptor() const override { return m_descriptor; }
private:
    std::string m_path;
    renderer::ShaderDescriptor m_descriptor;
};
/// @note A real SyncMaterial call needs a native descriptor and CB shadow; unrelated streaming fixtures keep their original renderer behavior.
class CpuPathMaterialShader final : public renderer::IShader {
public:
    CpuPathMaterialShader(std::string path, std::function<void()>& callback):m_path(std::move(path)),m_callback(callback)
    {
        renderer::ShaderVarDesc albedo;
        albedo.name="albedo";albedo.varClass=renderer::ShaderVarClass::Vector;albedo.columns=4;albedo.size=16;
        renderer::ShaderVarDesc tiling;
        tiling.name="uvTiling";tiling.varClass=renderer::ShaderVarClass::Vector;tiling.columns=2;tiling.offset=16;tiling.size=8;
        renderer::ShaderVarDesc offset=tiling;
        offset.name="uvOffset";offset.offset=24;
        m_descriptor.vars={albedo,tiling,offset};
        m_descriptor.textureMaskOffset=32;m_descriptor.cbufferSize=80;
        for(uint32_t index=0;index<renderer::kMaterialTextureSlotCount;++index)
            m_descriptor.textures.push_back({renderer::kMaterialTextureSlots[index].field,index,36+index*4});
    }
    const std::string& GetPath() const override {return m_path;}
    const renderer::ShaderDescriptor& GetDescriptor() const override
    {
        auto callback = std::move(m_callback);
        if (callback) callback();
        return m_descriptor;
    }
private:
    std::string m_path;
    renderer::ShaderDescriptor m_descriptor;
    std::function<void()>& m_callback;
};
class CpuPathMaterialBuffer final : public renderer::IConstantBuffer {
public:
    CpuPathMaterialBuffer(size_t size,std::vector<uint8_t>& uploaded):m_size(size),m_uploaded(uploaded){}
    size_t GetSize() const override {return m_size;}
    void Update(const void* data,size_t size) override
    {
        ASSERT_LE(size,m_size);
        m_uploaded.resize(size);
        if(size!=0)std::memcpy(m_uploaded.data(),data,size);
    }
private:
    size_t m_size;
    std::vector<uint8_t>& m_uploaded;
};

class StreamingCpuRenderer final : public renderer::IRenderer {
public:
    int fileLoads = 0;
    int memoryLoads = 0;
    bool materialPathFixture=false;
    std::function<void()> beforeTextureCreate;
    std::function<void()> beforeMaterialDescriptor;
    std::vector<uint8_t> materialUpload;
    void RunTextureCallback()
    {
        auto callback=std::move(beforeTextureCreate);
        if(callback)callback();
    }
    void Shutdown() override {}
    void BeginFrame() override {}
    void EndFrame() override {}
    void Clear(const math::Vector4&) override {}
    void Submit(const renderer::DrawCall&, renderer::ResourceManager&) override {}
    void Dispatch(const renderer::ComputeCall&, renderer::ResourceManager&) override {}
    void Resize(uint32_t, uint32_t) override {}
    uint32_t GetWidth() const override { return 1; }
    uint32_t GetHeight() const override { return 1; }
    void SetRenderTarget(renderer::ResourceHandle<renderer::RenderTargetTag>, renderer::ResourceManager&) override {}
    void ClearDepth() override {}
    void SetViewport(uint32_t, uint32_t, uint32_t, uint32_t) override {}
    std::unique_ptr<renderer::IShader> CreateNativeShader(const std::string& path) override {
        if (path == "ManagedMaterialFixture.hlsl") return std::make_unique<CpuMaterialShader>(path);
        if(materialPathFixture && path.ends_with("MaterialPathScratchFixture.hlsl"))return std::make_unique<CpuPathMaterialShader>(path, beforeMaterialDescriptor);
        return {};
    }
    std::unique_ptr<renderer::IConstantBuffer> CreateNativeConstantBuffer(size_t size) override {
        return materialPathFixture?std::make_unique<CpuPathMaterialBuffer>(size,materialUpload):nullptr;
    }
    std::unique_ptr<renderer::IBuffer> CreateNativeVertexBuffer(const void*, size_t size, uint32_t stride) override
    { return {}; }
    std::unique_ptr<renderer::IBuffer> CreateNativeIndexBuffer(const void*, uint32_t count) override
    { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTexture(const std::string&) override { RunTextureCallback(); ++fileLoads; return std::make_unique<CpuTexture>(16, 16); }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromData(const uint8_t*, uint32_t width, uint32_t height) override { RunTextureCallback(); ++memoryLoads; return std::make_unique<CpuTexture>(width, height); }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromDataMips(const renderer::TextureMipData* mips, uint32_t count) override {
        if (!mips || count == 0) return {};
        RunTextureCallback();
        ++memoryLoads;
        return std::make_unique<CpuTexture>(mips[0].width, mips[0].height);
    }
    std::unique_ptr<renderer::ITexture> CreateNativeTexture3DFromData(const uint8_t*, uint32_t, uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromRenderTarget(renderer::IRenderTarget&, uint32_t, renderer::RenderTargetTextureKind) override { return {}; }
    std::unique_ptr<renderer::IPipelineState> CreateNativePipelineState(const renderer::PipelineStateDesc&) override { return {}; }
    std::unique_ptr<renderer::IRenderTarget> CreateNativeRenderTarget(uint32_t, uint32_t, const renderer::RenderTargetDesc&) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeComputeTexture(uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeStructuredBuffer(const void*, uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeRWStructuredBuffer(const void*, uint32_t, uint32_t) override { return {}; }
};
class AssetStreamingIntegrationTest : public testkit::EngineFixture {
protected:
    testkit::TempDir m_temp{"StreamIntegration"};
    StreamingCpuRenderer m_renderer;
    renderer::ResourceManager m_resources{m_renderer};
    void SetUp() override {
        testkit::EngineFixture::SetUp();
        asset::AssetManager::Init(m_resources, m_temp.File("Assets/").generic_string());
        asset::AssetStreamer::Config config;
        config.evictionGracePumps = 1;
        asset::AssetStreamer::Engine().Configure(config);
        asset::StreamedTextureResolver::Engine().SetReleaseAfterFrames(1);
    }
    void TearDown() override {
        asset::AssetManager::UnloadAll();
        asset::AssetStreamer::Engine().Configure({});
        asset::StreamedTextureResolver::Engine().SetReleaseAfterFrames(300);
        testkit::EngineFixture::TearDown();
    }
    std::string TexturePath() {
        const auto path = m_temp.File("sample.tga");
        std::ofstream file(path, std::ios::binary);
        const uint8_t header[18] = {0,0,2,0,0,0,0,0,0,0,0,0,16,0,16,0,32,0x28};
        file.write(static_cast<const char*>(static_cast<const void*>(header)), sizeof(header));
        for (unsigned i = 0; i < 1024; ++i) file.put(static_cast<char>(i % 256));
        return path.generic_string();
    }
};

class MaterialPathScratchTest : public AssetStreamingIntegrationTest {
protected:
    void SetUp() override
    {
        AssetStreamingIntegrationTest::SetUp();
        m_renderer.materialPathFixture=true;
        auto& resolver=asset::StreamedTextureResolver::Engine();
        m_savedPolicy=resolver.GetMissPolicy();
        resolver.SetMissPolicy(asset::StreamedTextureResolver::MissPolicy::Synchronous);
    }
    void TearDown() override
    {
        m_renderer.beforeTextureCreate={};
        m_renderer.beforeMaterialDescriptor={};
        asset::StreamedTextureResolver::Engine().SetMissPolicy(m_savedPolicy);
        AssetStreamingIntegrationTest::TearDown();
    }
    std::string MakeTexture(const std::string& name)
    {
        const auto path=m_temp.File(name);
        std::ofstream file(path,std::ios::binary);
        const uint8_t header[18]={0,0,2,0,0,0,0,0,0,0,0,0,16,0,16,0,32,0x28};
        file.write(static_cast<const char*>(static_cast<const void*>(header)),sizeof(header));
        for(unsigned index=0;index<1024;++index)file.put(static_cast<char>(index%256));
        file.close();
        std::string error;
        EXPECT_TRUE(asset::texturecache::BakeDistributionTexture(path.generic_string(),error))<<error;
        return path.generic_string();
    }
    asset::AssetHandle<asset::MaterialAsset> MakeMaterial(const std::string& name)
    {
        asset::MaterialAssetAuthoring material;
        material.shaderPath="MaterialPathScratchFixture.hlsl";
        material.params["albedo"]={0.2f,0.3f,0.4f,1.0f};
        const auto path=m_temp.File(name).generic_string();
        EXPECT_TRUE(asset::SaveMaterialAssetToFile(path,material));
        return asset::AssetManager::Load<asset::MaterialAsset>(path);
    }
    /// @note The native synchronization path requires the same explicit surface metadata as the existing nested-sync fixture.
    std::string MakeMaterialShader()
    {
        const auto shaderPath = m_temp.File("MaterialPathScratchFixture.hlsl").generic_string();
        EXPECT_TRUE(util::FileSystem::WriteText(shaderPath, "float4 PSMain() : SV_Target { return 1; }\n"));
        EXPECT_TRUE(util::FileSystem::WriteText(shaderPath + ".meta",
            "[meta]\nguid='00000000000000000000000000000011'\n[shader]\nversion=1\n"
            "vertex='standard_surface_v1'\nsurface='lambert_v1'\nopacity='opaque_v1'\nvariants='standard_surface_v1'\n"));
        return shaderPath;
    }
    uint32_t TextureMask(const renderer::Material& material)
    {
        uint32_t result=0;
        if(material.paramData.size()>=36)std::memcpy(&result,material.paramData.data()+32,4);
        return result;
    }
private:
    asset::StreamedTextureResolver::MissPolicy m_savedPolicy=asset::StreamedTextureResolver::MissPolicy::Placeholder;
};

/// @note Exercises the actual AssetManager::Load importer boundary without introducing production test access.
class RelocatingMaterialImporter final : public asset::IAssetImporter<asset::MaterialAsset> {
public:
    explicit RelocatingMaterialImporter(std::string shaderPath) : m_shaderPath(std::move(shaderPath)) {}
    std::function<void()> beforeImport;
    std::unique_ptr<asset::MaterialAsset> Import(const std::string&, renderer::ResourceManager*) override
    {
        auto callback = std::move(beforeImport);
        if (callback) callback();
        asset::MaterialAssetAuthoring values;
        values.shaderPath = m_shaderPath;
        values.params["albedo"] = {0.2f, 0.3f, 0.4f, 1.0f};
        return std::make_unique<asset::MaterialAsset>(values);
    }
    std::span<const std::string_view> SupportedExtensions() const override
    {
        static constexpr std::string_view extensions[] = {".mat"};
        return extensions;
    }
private:
    std::string m_shaderPath;
};

TEST_F(MaterialPathScratchTest, MaterialLoadReacquiresRelocatedAndReplacedSelectedSlots)
{
    /// @note This fixture's Init installs MatAssetImporter in the DLL; restore that known dependency on every exit.
    struct RestoreImporter final {
        ~RestoreImporter() { asset::AssetManager::RegisterImporter<asset::MaterialAsset>(std::make_unique<asset::MatAssetImporter>()); }
    } restore;
    auto importer = std::make_unique<RelocatingMaterialImporter>(MakeMaterialShader());
    auto* callbacks = importer.get();
    asset::AssetManager::RegisterImporter<asset::MaterialAsset>(std::move(importer));
    const auto firstPath = m_temp.File("MoveOnLoad.mat").generic_string();
    const auto stalePath = m_temp.File("ReplaceOnLoad.mat").generic_string();
    const auto replacementPath = m_temp.File("CurrentAfterLoad.mat").generic_string();
    scene::MaterialComponent component;
    component.ResizeSlots(2);
    component.RawSlotAt(1).SetMaterialPath(firstPath);
    const auto movedSnapshot = component.RawSlotAt(1).ReadAuthoring();
    const size_t previousCapacity = component.extraSlots.capacity();
    bool moved = false;
    callbacks->beforeImport = [&] {
        component.extraSlots.reserve(previousCapacity + 16u);
        moved = true;
    };
    const auto* first = scene::SyncMaterialSlot(component, 1, m_resources, false, 200);
    ASSERT_NE(first, nullptr); ASSERT_TRUE(moved);
    EXPECT_GT(component.extraSlots.capacity(), previousCapacity);
    EXPECT_EQ(component.RawSlotAt(1).ReadAuthoring(), movedSnapshot);
    EXPECT_TRUE(component.RawSlotAt(1).GetMaterialAsset().IsValid());
    float color = 0;
    std::memcpy(&color, first->paramData.data(), sizeof(color));
    EXPECT_FLOAT_EQ(color, 0.2f);

    component.RawSlotAt(1).SetMaterialPath(stalePath);
    const auto replacedSnapshot = component.RawSlotAt(1).ReadAuthoring();
    bool replaced = false;
    callbacks->beforeImport = [&] {
        component.RawSlotAt(1) = scene::MaterialSlot{};
        component.RawSlotAt(1).SetMaterialPath(replacementPath);
        EditMaterialAuthoring(component.RawSlotAt(1), [&](auto& values) {
            values.paramOverrides["albedo"] = {0.75f, 0.6f, 0.5f, 1.0f};
        });
        replaced = true;
    };
    const auto* current = scene::SyncMaterialSlot(component, 1, m_resources, false, 75);
    ASSERT_NE(current, nullptr); ASSERT_TRUE(replaced);
    EXPECT_NE(component.RawSlotAt(1).ReadAuthoring()->OwnerToken(), replacedSnapshot->OwnerToken());
    EXPECT_EQ(component.RawSlotAt(1).GetMaterialPath(), replacementPath);
    EXPECT_EQ(component.RawSlotAt(1).GetMaterialAsset(), asset::AssetManager::Load<asset::MaterialAsset>(replacementPath));
    EXPECT_EQ(replacedSnapshot->Data().materialPath, stalePath);
    EXPECT_TRUE(replacedSnapshot->Data().paramOverrides.empty());
    std::memcpy(&color, current->paramData.data(), sizeof(color));
    EXPECT_FLOAT_EQ(color, 0.75f);
    EXPECT_EQ(m_renderer.materialUpload, current->paramData);
}

TEST_F(MaterialPathScratchTest, ManagedTransferReacquiresLoadOwnersAndHonorsReflectionCallbackCancellation)
{
    /// @note This fixture's Init installs MatAssetImporter in the DLL; restore that known dependency on every exit.
    struct RestoreImporter final {
        ~RestoreImporter() { asset::AssetManager::RegisterImporter<asset::MaterialAsset>(std::make_unique<asset::MatAssetImporter>()); }
    } restore;
    auto importer = std::make_unique<RelocatingMaterialImporter>(MakeMaterialShader());
    auto* callbacks = importer.get();
    asset::AssetManager::RegisterImporter<asset::MaterialAsset>(std::move(importer));
    scene::Scene testScene;
    auto& target = testScene.CreateGameObject("MaterialTransferOwner");
    auto& component = target.AddComponent<scene::MaterialComponent>();
    component.ResizeSlots(2);
    component.RawSlotAt(1).SetMaterialPath(m_temp.File("TransferMove.mat").generic_string());
    scene::Script caller; caller.SetContext(&testScene, &target);
    scene::managed_detail::ManagedMaterialTransfers transfers;
    scene::ManagedBridgeRequest request;
    request.index = 30; request.flags = 1; request.callId = 13;
    request.ownerId = 9; request.instanceId = 11; request.generation = 7;
    scene::managed_detail::ManagedServiceResult result;
    const std::array<std::string, 1> property = {"albedo"};
    const auto initial = component.RawSlotAt(1).ReadAuthoring();
    const size_t capacity = component.extraSlots.capacity();
    bool moved = false;
    callbacks->beforeImport = [&] { component.extraSlots.reserve(capacity + 16u); moved = true; };
    ASSERT_EQ(transfers.Open(testScene, target, caller, request, 1, {}, property, result), scene::ManagedBridgeStatus::OK);
    ASSERT_TRUE(moved);
    EXPECT_GT(component.extraSlots.capacity(), capacity);
    EXPECT_GT(result.integer, 0); EXPECT_EQ(result.text, std::vector<std::string>{"4"});
    EXPECT_EQ(component.RawSlotAt(1).ReadAuthoring(), initial);

    const auto replacementPath = m_temp.File("TransferReplacement.mat").generic_string();
    component.RawSlotAt(1).SetMaterialPath(replacementPath);
    const auto replaced = component.RawSlotAt(1).ReadAuthoring();
    callbacks->beforeImport = [&] {
        component.RawSlotAt(1) = scene::MaterialSlot{};
        component.RawSlotAt(1).SetMaterialPath(replacementPath);
    };
    EXPECT_EQ(transfers.Open(testScene, target, caller, request, 1, {}, property, result), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    EXPECT_NE(component.RawSlotAt(1).ReadAuthoring()->OwnerToken(), replaced->OwnerToken());
    ASSERT_EQ(transfers.Open(testScene, target, caller, request, 1, {}, property, result), scene::ManagedBridgeStatus::OK);
    const uint64_t token = static_cast<uint64_t>(result.integer);
    ASSERT_NE(token, 0u);
    const auto append = []<typename T>(std::string& bytes, const T& value) {
        bytes.append(std::bit_cast<const char*>(&value), sizeof(T));
    };
    std::string packet;
    append(packet, token); append(packet, uint64_t{0}); append(packet, uint32_t{4}); append(packet, uint32_t{0});
    std::array<double, 4> output = {-7.0, -7.0, -7.0, -7.0};
    request.index = 31; request.input = packet.data(); request.inputBytes = static_cast<uint32_t>(packet.size());
    request.output = static_cast<char*>(static_cast<void*>(output.data())); request.outputCapacity = static_cast<uint32_t>(sizeof(output));
    scene::ManagedBridgeReply reply;
    bool cancelled = false;
    m_renderer.beforeMaterialDescriptor = [&] { cancelled = true; transfers.EndCall(request.callId); };
    EXPECT_EQ(transfers.Run(testScene, target, caller, request, result, &reply), scene::ManagedBridgeStatus::NOT_FOUND);
    EXPECT_TRUE(cancelled);
    EXPECT_EQ(output, (std::array<double, 4>{-7.0, -7.0, -7.0, -7.0}));
}

/// @note Mutating defaults/overrides and reentering from native texture creation must not replace the outer call's already-owned path snapshot.
TEST_F(MaterialPathScratchTest, TextureBackendCallbackPreservesOuterSnapshotAcrossMutationAndNestedSync)
{
    const auto first=MakeTexture("FirstLongOwnedTextureReference.tga");
    const auto previous=MakeTexture("PreviousLongOwnedTextureReference.tga");
    const auto changed=MakeTexture("ChangedLongOwnedTextureReference.tga");
    const auto handle=MakeMaterial("Snapshot.mat");
    ASSERT_TRUE(handle.IsValid());
    auto* source=asset::AssetManager::Get<asset::MaterialAsset>(handle);
    ASSERT_NE(source,nullptr);
    EditMaterialAuthoring(*source, [&](auto& values) { values.textures["albedo"]=first; });
    EditMaterialAuthoring(*source, [&](auto& values) { values.textures["normal"]=previous; });
    scene::MaterialComponent outer,inner;
    outer.SetMaterialAsset(handle); inner.SetMaterialAsset(handle);
    EditMaterialAuthoring(outer, [&](auto& values) { values.presentationTextureDefaults["albedo"]=previous; });
    EditMaterialAuthoring(outer, [&](auto& values) { values.textureOverrides["albedo"]=first; });
    EditMaterialAuthoring(outer, [&](auto& values) { values.textureOverrides["metallic"]=previous; });
    bool callbackRan=false;
    m_renderer.beforeTextureCreate=[&]{
        callbackRan=true;
        EditMaterialAuthoring(*source, [&](auto& values) { values.textures["normal"]=changed; });
        EditMaterialAuthoring(outer, [&](auto& values) { values.textureOverrides["metallic"]=changed; });
        EditMaterialAuthoring(outer, [&](auto& values) { values.paramOverrides["albedo"]={0.9f,0.8f,0.7f,1.0f}; });
        for(const auto* name:asset::kMaterialTextureSlotNames)EditMaterialAuthoring(inner, [&](auto& values) { values.textureOverrides[name]=""; });
        auto* nested=scene::SyncMaterial(inner,m_resources,false,50);
        ASSERT_NE(nested,nullptr);
        EXPECT_EQ(TextureMask(*nested),0u);
        for(const auto texture:nested->textures)EXPECT_FALSE(texture.IsValid());
    };
    auto* material=scene::SyncMaterial(outer,m_resources,false,200);
    ASSERT_NE(material,nullptr);
    ASSERT_TRUE(callbackRan);
    ASSERT_EQ(material->textures.size(),8u);
    auto& resolver=asset::StreamedTextureResolver::Engine();
    const auto firstHandle=resolver.ResolveGpu(m_resources,first);
    const auto previousHandle=resolver.ResolveGpu(m_resources,previous);
    EXPECT_EQ(material->textures[0],firstHandle);
    EXPECT_EQ(material->textures[1],previousHandle);
    EXPECT_EQ(material->textures[2],previousHandle);
    EXPECT_EQ(TextureMask(*material),7u);
    EXPECT_EQ(m_renderer.materialUpload,material->paramData);
    float originalAlbedo=0;
    std::memcpy(&originalAlbedo,material->paramData.data(),sizeof(float));
    EXPECT_FLOAT_EQ(originalAlbedo,0.2f);

    material=scene::SyncMaterial(outer,m_resources,false,75);
    ASSERT_NE(material,nullptr);
    const auto changedHandle=resolver.ResolveGpu(m_resources,changed);
    EXPECT_EQ(material->textures[1],changedHandle);
    EXPECT_EQ(material->textures[2],changedHandle);
    float updatedAlbedo=0;
    std::memcpy(&updatedAlbedo,material->paramData.data(),sizeof(float));
    EXPECT_FLOAT_EQ(updatedAlbedo,0.9f);
}

TEST_F(MaterialPathScratchTest, LateTextureCallbackAfterEmptySlotsReacquiresTheReplacedSlot)
{
    const auto texture = MakeTexture("LastSlotAfterEmptyTextures.tga");
    const auto handle = MakeMaterial("LateTextureCallback.mat");
    const auto materialPath = m_temp.File("LateTextureCallback.mat").generic_string();
    ASSERT_TRUE(handle.IsValid());
    auto* source = asset::AssetManager::Get<asset::MaterialAsset>(handle);
    ASSERT_NE(source, nullptr);
    EditMaterialAuthoring(*source, [&](auto& values) {
        values.shaderPath = MakeMaterialShader();
        values.textures["tex7"] = texture;
    });
    scene::MaterialComponent component;
    component.ResizeSlots(2);
    component.RawSlotAt(1).SetMaterialReference(materialPath, handle);
    ASSERT_EQ(&component.SlotAt(1), &component.RawSlotAt(1));
    const auto oldOwner = component.RawSlotAt(1).ReadAuthoring()->OwnerToken();
    const auto oldCapacity = component.extraSlots.capacity();
    int callbacks = 0;
    m_renderer.beforeTextureCreate = [&] {
        ++callbacks;
        component.extraSlots.reserve(oldCapacity + 16u);
        component.RawSlotAt(1) = scene::MaterialSlot{};
        component.RawSlotAt(1).SetMaterialReference(materialPath, handle);
        EditMaterialAuthoring(component.RawSlotAt(1), [&](auto& values) {
            values.paramOverrides["albedo"] = {0.75f, 0.5f, 0.25f, 1.0f};
        });
    };
    const auto* material = scene::SyncMaterialSlot(component, 1, m_resources, false, 200);
    ASSERT_NE(material, nullptr);
    EXPECT_EQ(callbacks, 1);
    EXPECT_GT(component.extraSlots.capacity(), oldCapacity);
    EXPECT_NE(component.RawSlotAt(1).ReadAuthoring()->OwnerToken(), oldOwner);
    EXPECT_EQ(material, component.RawSlotAt(1).material.get());
    ASSERT_EQ(material->textures.size(), 8u);
    for (size_t index = 0; index < 7; ++index) EXPECT_FALSE(material->textures[index].IsValid());
    EXPECT_TRUE(material->textures[7].IsValid());
    EXPECT_EQ(TextureMask(*material), 1u << 7);
    float color = 0;
    std::memcpy(&color, material->paramData.data(), sizeof(color));
    EXPECT_FLOAT_EQ(color, 0.75f);
    EXPECT_EQ(m_renderer.materialUpload, material->paramData);

    EditMaterialAuthoring(component.RawSlotAt(1), [&](auto& values) { values.textureOverrides["tex7"] = ""; });
    material = scene::SyncMaterialSlot(component, 1, m_resources);
    ASSERT_NE(material, nullptr);
    EXPECT_EQ(TextureMask(*material), 0u);
    for (const auto current : material->textures) EXPECT_FALSE(current.IsValid());
}

/// @note Shorter/removed paths start from an empty snapshot, and distinct oversized references remain untruncated local values.
TEST_F(MaterialPathScratchTest, ShortEmptyAndOversizedReferencesKeepIndependentBindings)
{
    const auto longPath=MakeTexture(std::string(96,'l')+".tga");
    const auto shortPath=MakeTexture("S.tga");
    const auto handle=MakeMaterial("Lengths.mat");
    ASSERT_TRUE(handle.IsValid());
    auto* source=asset::AssetManager::Get<asset::MaterialAsset>(handle);
    ASSERT_NE(source,nullptr);
    EditMaterialAuthoring(*source, [&](auto& values) { values.textures["albedo"]=longPath; });EditMaterialAuthoring(*source, [&](auto& values) { values.textures["normal"]=shortPath; });
    scene::MaterialComponent component;
    component.SetMaterialAsset(handle);
    auto* material=scene::SyncMaterial(component,m_resources);
    ASSERT_NE(material,nullptr);
    auto& resolver=asset::StreamedTextureResolver::Engine();
    EXPECT_EQ(material->textures[0],resolver.ResolveGpu(m_resources,longPath));
    EXPECT_EQ(material->textures[1],resolver.ResolveGpu(m_resources,shortPath));
    EditMaterialAuthoring(*source, [&](auto& values) { values.textures.erase("normal"); });EditMaterialAuthoring(component, [&](auto& values) { values.textureOverrides["albedo"]=shortPath; });
    material=scene::SyncMaterial(component,m_resources);
    ASSERT_NE(material,nullptr);
    EXPECT_EQ(material->textures[0],resolver.ResolveGpu(m_resources,shortPath));
    EXPECT_FALSE(material->textures[1].IsValid());
    EXPECT_EQ(TextureMask(*material),1u);
    EditMaterialAuthoring(component, [&](auto& values) { values.textureOverrides["albedo"]=""; });
    material=scene::SyncMaterial(component,m_resources);
    ASSERT_NE(material,nullptr);
    EXPECT_EQ(TextureMask(*material),0u);
    for(const auto texture:material->textures)EXPECT_FALSE(texture.IsValid());

    resolver.SetMissPolicy(asset::StreamedTextureResolver::MissPolicy::Placeholder);
    const size_t heldBefore=resolver.HeldCount();
    const std::string oversized="Textures/"+std::string(5000,'x');
    EditMaterialAuthoring(component, [&](auto& values) { values.textureOverrides["albedo"]=oversized+"A.tga"; });
    material=scene::SyncMaterial(component,m_resources);
    ASSERT_NE(material,nullptr);
    EXPECT_EQ(material->textures[0],resolver.ResolveGpu(m_resources,oversized+"A.tga"));
    EXPECT_EQ(resolver.HeldCount(),heldBefore+1);
    EditMaterialAuthoring(component, [&](auto& values) { values.textureOverrides["albedo"]=oversized+"B.tga"; });
    material=scene::SyncMaterial(component,m_resources);
    ASSERT_NE(material,nullptr);
    EXPECT_EQ(material->textures[0],resolver.ResolveGpu(m_resources,oversized+"B.tga"));
    EXPECT_EQ(resolver.HeldCount(),heldBefore+2);
    EditMaterialAuthoring(component, [&](auto& values) { values.textureOverrides.clear(); });EditMaterialAuthoring(*source, [&](auto& values) { values.textures.clear(); });
    material=scene::SyncMaterial(component,m_resources);
    ASSERT_NE(material,nullptr);
    EXPECT_EQ(TextureMask(*material),0u);
}

/// @note The string scratch contains no resource owner; shader release and manager reset still create current shader/CB generations.
TEST_F(MaterialPathScratchTest, ShaderReleaseAndResourceResetDoNotReuseRetiredBindings)
{
    const auto handle=MakeMaterial("Lifecycle.mat");
    ASSERT_TRUE(handle.IsValid());
    scene::MaterialComponent component;
    component.SetMaterialAsset(handle);
    auto* material=scene::SyncMaterial(component,m_resources);
    ASSERT_NE(material,nullptr);
    const auto shader=material->shader;
    const auto buffer=material->paramsBuffer;
    ASSERT_TRUE(shader.IsValid());ASSERT_TRUE(buffer.IsValid());
    m_resources.Release(shader);
    material=scene::SyncMaterial(component,m_resources);
    ASSERT_NE(material,nullptr);
    EXPECT_NE(material->shader,shader);
    EXPECT_EQ(material->paramsBuffer,buffer);
    EXPECT_TRUE(m_resources.ReloadShader("MaterialPathScratchFixture.hlsl").IsValid());
    material=scene::SyncMaterial(component,m_resources);
    ASSERT_NE(material,nullptr);
    EXPECT_EQ(TextureMask(*material),0u);
    m_resources.Reset();
    material=scene::SyncMaterial(component,m_resources);
    ASSERT_NE(material,nullptr);
    EXPECT_NE(material->paramsBuffer,buffer);
    EXPECT_NE(m_resources.Get(material->shader),nullptr);
    EXPECT_NE(m_resources.Get(material->paramsBuffer),nullptr);
    EXPECT_EQ(m_renderer.materialUpload,material->paramData);
    EXPECT_EQ(TextureMask(*material),0u);
}

TEST_F(AssetStreamingIntegrationTest, CachePublicationFailureStillUploadsAndPublishesDecodedSourcePixels)
{
    SetLogLevel(core::LogLevel::WARNING);
    testkit::RecordingLogSink sink;
    testkit::ScopedLogSink recording(&sink);
    const std::string path = TexturePath();
    auto channel = asset::CreateTextureStreamChannel(m_resources);
    const std::string key = channel->MakeKey(path);
    asset::AssetJobInput input;
    std::string error;
    ASSERT_TRUE(channel->Snapshot(key, input, error)) << error;
    const auto cachePath = util::FileSystem::PathFromUtf8(asset::AssetManager::StreamCacheDir())
        / asset::texturecache::CacheFileName(input.resolvedPath);
    ASSERT_TRUE(std::filesystem::create_directories(cachePath));
    const auto sentinel = cachePath / "untouched.txt";
    ASSERT_TRUE(util::FileSystem::WriteText(sentinel.generic_string(), "keep"));

    auto decoded = channel->Decode(input);

    EXPECT_EQ(decoded.error, asset::AssetLoadError::None);
    ASSERT_NE(decoded.decoded, nullptr);
    EXPECT_EQ(decoded.decoded->CpuBytes(), 16u * 16u * 4u);
    ASSERT_NE(sink.Find("stage=rename"), nullptr);
    ASSERT_NE(sink.Find("using decoded pixels"), nullptr);
    const auto handle = channel->Reserve(key);
    uint64_t token = 0;
    auto candidate = channel->BeginUpload(*decoded.decoded, token, error);
    ASSERT_NE(candidate, nullptr) << error;
    ASSERT_TRUE(channel->IsUploadComplete(token));
    ASSERT_TRUE(channel->Publish(handle, *candidate));
    const auto* texture = static_cast<const asset::TextureAsset*>(channel->Get(handle));
    ASSERT_NE(texture, nullptr);
    EXPECT_EQ(texture->sourceWidth, 16u);
    EXPECT_EQ(texture->sourceHeight, 16u);
    ASSERT_NE(m_resources.Get(texture->gpuHandle), nullptr);
    EXPECT_EQ(m_resources.Get(texture->gpuHandle)->GetWidth(), 16u);
    EXPECT_EQ(m_resources.Get(texture->gpuHandle)->GetHeight(), 16u);
    EXPECT_EQ(m_renderer.memoryLoads, 1);
    EXPECT_EQ(m_renderer.fileLoads, 0);
    std::string preserved;
    ASSERT_TRUE(util::FileSystem::ReadText(sentinel.generic_string(), preserved));
    EXPECT_EQ(preserved, "keep");
    std::size_t temporaries = 0;
    for (const auto& entry : std::filesystem::directory_iterator(cachePath.parent_path()))
        if (entry.path().wstring().find(L".tmp") != std::wstring::npos) ++temporaries;
    EXPECT_EQ(temporaries, 0u);
}

TEST_F(AssetStreamingIntegrationTest, RelativeAbsoluteGuidAndDotPathsShareMaterialAndUnload)
{
    const auto absolute = m_temp.File("Assets/Aliases.mat").generic_string();
    asset::MaterialAssetAuthoring material;
    material.shaderPath.clear();
    material.params["roughness"] = {0.25f};
    ASSERT_TRUE(asset::SaveMaterialAssetToFile(absolute, material));
    const auto guid = asset::AssetDatabase::GuidFromPath(absolute);
    ASSERT_FALSE(guid.empty());
    const auto relative = asset::AssetManager::Load<asset::MaterialAsset>("Assets/Aliases.mat");
    ASSERT_TRUE(relative.IsValid());
    EXPECT_EQ(asset::AssetManager::Load<asset::MaterialAsset>(absolute), relative);
    EXPECT_EQ(asset::AssetManager::Load<asset::MaterialAsset>("guid:" + guid), relative);
    EXPECT_EQ(asset::AssetManager::Load<asset::MaterialAsset>("Assets/Sub/../aliases.mat"), relative);
    auto* shared = asset::AssetManager::Get<asset::MaterialAsset>(relative);
    ASSERT_NE(shared, nullptr);
    EditMaterialAuthoring(*shared, [&](auto& values) { values.params["roughness"] = {0.75f}; });
    const auto* viaAbsolute = asset::AssetManager::Get<asset::MaterialAsset>(
        asset::AssetManager::Load<asset::MaterialAsset>(absolute));
    ASSERT_NE(viaAbsolute, nullptr);
    EXPECT_FLOAT_EQ(viaAbsolute->Authoring().params.at("roughness")[0], 0.75f);
    asset::AssetManager::Unload<asset::MaterialAsset>("guid:" + guid);
    EXPECT_EQ(asset::AssetManager::Get<asset::MaterialAsset>(relative), nullptr);
}

TEST_F(AssetStreamingIntegrationTest, ConsumerLeasePreventsEvictionAndReloadsAfterUnusedFrames)
{
    const auto path = TexturePath();
    std::string error;
    ASSERT_TRUE(asset::texturecache::BakeDistributionTexture(path, error)) << error;
    auto& resolver = asset::StreamedTextureResolver::Engine();
    auto& streamer = asset::AssetStreamer::Engine();
    struct MissPolicyRestore {
        asset::StreamedTextureResolver& resolver;
        asset::StreamedTextureResolver::MissPolicy saved;
        ~MissPolicyRestore() { resolver.SetMissPolicy(saved); }
    } restore{resolver, resolver.GetMissPolicy()};
    resolver.SetMissPolicy(asset::StreamedTextureResolver::MissPolicy::Placeholder);
    const auto white = m_resources.GetWhiteTexture();
    ASSERT_TRUE(white.IsValid());
    uint32_t width = 0, height = 0;
    const auto pending = resolver.ResolveGpuWithSourceSize(m_resources, path, width, height);
    EXPECT_EQ(pending, white);
    EXPECT_EQ(width, 1u); EXPECT_EQ(height, 1u);
    EXPECT_EQ(m_renderer.fileLoads, 0);
    EXPECT_EQ(m_renderer.memoryLoads, 1);
    /// @note Complete a temporary typed request explicitly; its lease ends before retention is tested, leaving only the Resolver consumer.
    const auto completeReady = [&] {
        auto request = streamer.Request<asset::TextureAsset>(path);
        if (!request.IsHeld()) return false;
        return streamer.CompleteNow(request.Handle());
    };
    ASSERT_TRUE(completeReady());
    const auto handle = resolver.ResolveGpuWithSourceSize(m_resources, path, width, height);
    ASSERT_TRUE(handle.IsValid());
    EXPECT_NE(handle, white);
    EXPECT_EQ(width, 16u); EXPECT_EQ(height, 16u);
    EXPECT_EQ(m_renderer.fileLoads, 0);
    EXPECT_EQ(m_renderer.memoryLoads, 2);
    for (unsigned i = 0; i < 5; ++i) {
        EXPECT_EQ(resolver.ResolveGpu(m_resources, path), handle);
        resolver.EndFrame();
        streamer.Pump();
        EXPECT_NE(m_resources.Get(handle), nullptr);
    }
    for (unsigned i = 0; i < 5; ++i) {
        resolver.EndFrame();
        streamer.Pump();
    }
    EXPECT_EQ(resolver.HeldCount(), 0u);
    EXPECT_EQ(m_resources.Get(handle), nullptr);
    EXPECT_NE(m_resources.Get(white), nullptr);
    const auto reloadPending = resolver.ResolveGpuWithSourceSize(m_resources, path, width, height);
    EXPECT_EQ(reloadPending, white);
    EXPECT_EQ(width, 1u); EXPECT_EQ(height, 1u);
    ASSERT_TRUE(completeReady());
    const auto reloaded = resolver.ResolveGpuWithSourceSize(m_resources, path, width, height);
    EXPECT_TRUE(reloaded.IsValid());
    EXPECT_NE(reloaded, white);
    EXPECT_NE(reloaded, handle);
    EXPECT_EQ(width, 16u); EXPECT_EQ(height, 16u);
    EXPECT_NE(m_resources.Get(reloaded), nullptr);
    EXPECT_EQ(m_resources.Get(handle), nullptr);
    EXPECT_EQ(m_renderer.fileLoads, 0);
    EXPECT_EQ(m_renderer.memoryLoads, 3);
}

TEST_F(AssetStreamingIntegrationTest, PendingTextureUsesPlaceholderWithoutSynchronousFileLoad)
{
    auto& resolver = asset::StreamedTextureResolver::Engine();
    const auto savedPolicy = resolver.GetMissPolicy();
    resolver.SetMissPolicy(asset::StreamedTextureResolver::MissPolicy::Placeholder);
    const auto white = m_resources.GetWhiteTexture();
    EXPECT_TRUE(white.IsValid());
    if (!white.IsValid()) {
        resolver.SetMissPolicy(savedPolicy);
        return;
    }

    const uint64_t syncAttemptsBefore = m_resources.GetSynchronousTextureLoadStats().attempts;
    const auto missingPath = m_temp.File("missing-texture.tga").generic_string();
    const auto resolved = resolver.ResolveGpu(m_resources, missingPath);
    EXPECT_EQ(resolved, white);
    EXPECT_EQ(m_renderer.fileLoads, 0);
    EXPECT_EQ(m_resources.GetSynchronousTextureLoadStats().attempts, syncAttemptsBefore);
    resolver.SetMissPolicy(savedPolicy);
}

TEST_F(AssetStreamingIntegrationTest, ManagedMaterialNumericPagesValidateRealPropertyShapeAndPublishOneCandidate)
{
    asset::MaterialAssetAuthoring shared; shared.shaderPath = "ManagedMaterialFixture.hlsl";
    shared.blendMode = renderer::BlendMode::ALPHA_BLEND; shared.doubleSided = true; shared.renderQueue = 2450;
    shared.integerParams["values"].resize(9004, 17); shared.integerParams["values"].back() = UINT32_MAX;
    const std::string materialPath = m_temp.File("fixture.mat").generic_string();
    ASSERT_TRUE(asset::SaveMaterialAssetToFile(materialPath, shared));
    scene::Scene scene; auto& target = scene.CreateGameObject("Material"); auto& component = target.AddComponent<scene::MaterialComponent>();
    component.SetMaterialPath(materialPath); scene::Script caller; caller.SetContext(&scene, &target);
    scene::managed_detail::ManagedServicesBridge services; scene::managed_detail::ManagedServiceResult result;
    const auto append = []<typename T>(std::string& bytes, const T& value) { bytes.append(std::bit_cast<const char*>(&value), sizeof(T)); };
    const auto request = [&](uint32_t index, bool read, const std::string& input) {
        scene::ManagedBridgeRequest value; value.op = scene::ManagedBridgeOp::SERVICE_MATERIAL; value.index = index; value.flags = read ? 1u : 0u;
        value.ownerId = 9; value.instanceId = 11; value.generation = 7; value.callId = 13; value.input = input.data(); value.inputBytes = static_cast<uint32_t>(input.size()); return value;
    };
    const auto run = [&](uint32_t index, bool read, std::initializer_list<std::string> texts) {
        std::string input; append(input, uint64_t{0}); append(input, int64_t{0}); append(input, uint32_t{0}); append(input, static_cast<uint32_t>(texts.size()));
        for (const auto& text : texts) { append(input, static_cast<uint32_t>(text.size())); input += text; }
        return services.Run(scene, caller, target, nullptr, request(index, read, input), result, false);
    };
    ASSERT_EQ(run(37, true, {}), scene::ManagedBridgeStatus::OK); EXPECT_EQ(result.integer, 1);
    ASSERT_EQ(run(39, true, {}), scene::ManagedBridgeStatus::OK); EXPECT_EQ(result.integer, 1);
    ASSERT_EQ(run(41, true, {}), scene::ManagedBridgeStatus::OK); EXPECT_EQ(result.integer, 2450);
    size_t count = 73; ASSERT_TRUE(caller.material.Instance().TryGetValueCount(scene::MaterialPropertyId("values"), count)); EXPECT_EQ(count, 9004u);
    ASSERT_EQ(run(30, true, {"values"}), scene::ManagedBridgeStatus::OK); const auto token = static_cast<uint64_t>(result.integer);
    ASSERT_GT(token, 0u); ASSERT_EQ(result.text.size(), 1u); EXPECT_EQ(result.text[0], "9004");
    std::string packet; append(packet, token); append(packet, uint64_t{0}); append(packet, uint32_t{8189}); append(packet, uint32_t{0});
    std::array<double, 8189> page; page.fill(-77); auto read = request(31, true, packet); read.output = std::bit_cast<char*>(page.data()); read.outputCapacity = 8;
    scene::ManagedBridgeReply reply;
    EXPECT_EQ(services.ReadMaterialPacket(scene, caller, target, read, reply), scene::ManagedBridgeStatus::BUFFER_TOO_SMALL);
    EXPECT_EQ(reply.outputBytes, 8189u * 8u); EXPECT_EQ(page.front(), -77); EXPECT_EQ(page.back(), -77);
    read.outputCapacity = static_cast<uint32_t>(sizeof(page)); ASSERT_EQ(services.ReadMaterialPacket(scene, caller, target, read, reply), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(page.front(), 17); EXPECT_EQ(page.back(), 17);
    packet.clear(); append(packet, token); append(packet, uint64_t{8189}); append(packet, uint32_t{815}); append(packet, uint32_t{0});
    read = request(31, true, packet); read.output = std::bit_cast<char*>(page.data()); read.outputCapacity = static_cast<uint32_t>(sizeof(page));
    ASSERT_EQ(services.ReadMaterialPacket(scene, caller, target, read, reply), scene::ManagedBridgeStatus::OK); EXPECT_EQ(page[814], UINT32_MAX);
    packet.clear(); append(packet, token); ASSERT_EQ(services.Run(scene, caller, target, nullptr, request(35, true, packet), result, false), scene::ManagedBridgeStatus::OK);
    ASSERT_EQ(run(32, false, {"values", "9004"}), scene::ManagedBridgeStatus::OK); const auto writeToken = static_cast<uint64_t>(result.integer);
    const auto stage = [&](uint64_t offset, uint32_t size, double last) {
        std::string bytes; append(bytes, writeToken); append(bytes, offset); append(bytes, size); append(bytes, uint32_t{0});
        for (uint32_t index = 0; index < size; ++index) append(bytes, index + 1 == size ? last : 42.0);
        return services.Run(scene, caller, target, nullptr, request(33, false, bytes), result, false);
    };
    ASSERT_EQ(stage(0, 8189, 42), scene::ManagedBridgeStatus::OK); EXPECT_TRUE(component.Authoring().integerParamOverrides.empty());
    ASSERT_EQ(stage(8189, 815, UINT32_MAX), scene::ManagedBridgeStatus::OK); EXPECT_TRUE(component.Authoring().integerParamOverrides.empty());
    packet.clear(); append(packet, writeToken); ASSERT_EQ(services.Run(scene, caller, target, nullptr, request(34, false, packet), result, false), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(result.integer, 1); ASSERT_EQ(component.Authoring().integerParamOverrides.at("values").size(), 9004u); EXPECT_EQ(component.Authoring().integerParamOverrides.at("values").back(), UINT32_MAX);
    ASSERT_EQ(run(32, false, {"values", "9004"}), scene::ManagedBridgeStatus::OK); const auto invalidToken = static_cast<uint64_t>(result.integer);
    std::string invalid; append(invalid, invalidToken); append(invalid, uint64_t{0}); append(invalid, uint32_t{2}); append(invalid, uint32_t{0}); append(invalid, 1.0); append(invalid, std::numeric_limits<double>::quiet_NaN());
    EXPECT_EQ(services.Run(scene, caller, target, nullptr, request(33, false, invalid), result, false), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    EXPECT_EQ(component.Authoring().integerParamOverrides.at("values").front(), 42); EXPECT_EQ(component.Authoring().integerParamOverrides.at("values").back(), UINT32_MAX);
    math::Matrix4 matrix; for (unsigned row = 0; row < 4; ++row) for (unsigned column = 0; column < 4; ++column) matrix.m[row][column] = static_cast<float>(row * 4 + column);
    ASSERT_TRUE(caller.material.Instance().SetMatrix(scene::MaterialPropertyId("matrix"), matrix)); math::Matrix4 copied;
    ASSERT_TRUE(caller.material.Instance().TryGetMatrix(scene::MaterialPropertyId("matrix"), copied)); EXPECT_EQ(copied.m[2][3], 11); EXPECT_EQ(copied.m[3][3], 15);
}

TEST_F(AssetStreamingIntegrationTest, ManagedAssetLeasesBindToCallerAndCancelBeforeOwnerRetirement)
{
    const auto path = TexturePath();
    std::string error;
    ASSERT_TRUE(asset::texturecache::BakeDistributionTexture(path, error)) << error;
    const std::string guid = asset::AssetDatabase::GuidFromPath(path);
    ASSERT_FALSE(guid.empty());
    scene::Scene testScene;
    auto& target = testScene.CreateGameObject("AssetOwner");
    scene::Script caller; caller.SetContext(&testScene, &target);
    scene::Script foreign; foreign.SetContext(&testScene, &target);
    struct RestorePlayMode final { bool previous; ~RestorePlayMode() { scene::Script::SetInPlayMode(previous); } } restore{scene::Script::IsInPlayMode()};
    scene::Script::SetInPlayMode(true);
    scene::managed_detail::ManagedServicesBridge services;
    scene::managed_detail::ManagedServiceResult result;
    const auto append = []<typename T>(std::string& bytes, const T& value) { bytes.append(std::bit_cast<const char*>(&value), sizeof(T)); };
    const auto invoke = [&](scene::Script& script, uint32_t index, bool readOnly, int64_t integer,
        std::initializer_list<float> values = {}, std::initializer_list<std::string> text = {}) {
        std::string input; append(input, uint64_t{0}); append(input, integer);
        append(input, static_cast<uint32_t>(values.size())); append(input, static_cast<uint32_t>(text.size()));
        for (float value : values) append(input, value);
        for (const std::string& value : text) { append(input, static_cast<uint32_t>(value.size())); input += value; }
        scene::ManagedBridgeRequest request; request.op = scene::ManagedBridgeOp::SERVICE_ASSETS;
        request.index = index; request.flags = readOnly ? 1u : 0u; request.generation = 7;
        request.ownerId = 9; request.instanceId = 11; request.callId = 13;
        request.input = input.data(); request.inputBytes = static_cast<uint32_t>(input.size());
        return services.Run(testScene, script, target, nullptr, request, result, false);
    };
    ASSERT_EQ(invoke(caller, 0, false, 0, {0}, {"guid:" + guid}), scene::ManagedBridgeStatus::OK);
    const uint64_t lease = std::bit_cast<uint64_t>(result.integer);
    ASSERT_NE(lease, 0u);
    EXPECT_EQ(invoke(foreign, 1, true, std::bit_cast<int64_t>(lease)), scene::ManagedBridgeStatus::INVALID_SCOPE);
    ASSERT_EQ(invoke(caller, 1, true, std::bit_cast<int64_t>(lease)), scene::ManagedBridgeStatus::OK);
    ASSERT_EQ(result.values.size(), 5u);
    services.CancelOwner(9, &caller);
    EXPECT_EQ(invoke(caller, 1, true, std::bit_cast<int64_t>(lease)), scene::ManagedBridgeStatus::NOT_FOUND);

    ASSERT_EQ(invoke(caller, 0, false, 0, {0}, {"guid:" + guid}), scene::ManagedBridgeStatus::OK);
    const uint64_t retiredLease = std::bit_cast<uint64_t>(result.integer);
    ASSERT_NE(retiredLease, 0u);
    services.RetireGeneration(7);
    EXPECT_EQ(invoke(caller, 1, true, std::bit_cast<int64_t>(retiredLease)), scene::ManagedBridgeStatus::NOT_FOUND);
    services.CancelOwner(9, &caller);
}
} /// @note namespace
TEST_F(MaterialPathScratchTest, NestedSameSlotSyncKeepsCurrentPublicationAndOuterImmutableCallSnapshot)
{
    const auto first = MakeTexture("TemplateFirst.tga"), previous = MakeTexture("TemplatePrevious.tga");
    const auto handle = MakeMaterial("TemplateReentry.mat");
    const auto shaderPath = m_temp.File("MaterialPathScratchFixture.hlsl").generic_string();
    ASSERT_TRUE(util::FileSystem::WriteText(shaderPath, "float4 PSMain() : SV_Target { return 1; }\n"));
    ASSERT_TRUE(util::FileSystem::WriteText(shaderPath + ".meta",
        "[meta]\nguid='00000000000000000000000000000011'\n[shader]\nversion=1\n"
        "vertex='standard_surface_v1'\nsurface='lambert_v1'\nopacity='opaque_v1'\nvariants='standard_surface_v1'\n"));
    auto* source = asset::AssetManager::Get<asset::MaterialAsset>(handle);
    ASSERT_NE(source, nullptr);
    EditMaterialAuthoring(*source, [&](auto& values) { values.shaderPath = shaderPath; });
    scene::MaterialComponent component; component.SetMaterialAsset(handle);
    EditMaterialAuthoring(component, [&](auto& values) {
        values.textureOverrides["albedo"] = first; values.textureOverrides["metallic"] = previous;
    });
    const auto outerSnapshot = component.ReadAuthoring();
    bool nestedRan = false;
    m_renderer.beforeTextureCreate = [&] {
        nestedRan = true;
        EditMaterialAuthoring(component, [&](auto& values) {
            for (const auto* name : asset::kMaterialTextureSlotNames) values.textureOverrides[name] = "";
            values.paramOverrides["albedo"] = {0.9f, 0.8f, 0.7f, 1.0f};
        });
        const auto* nested = scene::SyncMaterial(component, m_resources, false, 50);
        ASSERT_NE(nested, nullptr); EXPECT_EQ(TextureMask(*nested), 0u);
    };
    const auto* outer = scene::SyncMaterial(component, m_resources, false, 200);
    ASSERT_NE(outer, nullptr); ASSERT_TRUE(nestedRan);
    EXPECT_EQ(m_resources.GetShaderCapabilities(outer->shader).vertex, renderer::ShaderVertexContract::STANDARD_SURFACE);
    EXPECT_EQ(TextureMask(*outer), 5u);
    float color = 0; std::memcpy(&color, outer->paramData.data(), sizeof(color)); EXPECT_FLOAT_EQ(color, 0.2f);
    EXPECT_EQ(outerSnapshot->Data().textureOverrides.at("albedo"), first);
    ASSERT_NE(component.ReadAuthoring(), outerSnapshot);
    std::fill(component.material->paramData.begin(), component.material->paramData.end(), uint8_t{0xCD});
    const auto* current = scene::SyncMaterial(component, m_resources, false, 75);
    ASSERT_NE(current, nullptr); EXPECT_EQ(TextureMask(*current), 0u);
    std::memcpy(&color, current->paramData.data(), sizeof(color)); EXPECT_FLOAT_EQ(color, 0.9f);
    EXPECT_EQ(m_renderer.materialUpload, current->paramData);
}


} /// @note namespace fbzz::tests
