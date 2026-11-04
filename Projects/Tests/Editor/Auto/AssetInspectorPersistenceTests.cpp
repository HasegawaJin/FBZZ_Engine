/// @file    AssetInspectorPersistenceTests.cpp
/// @brief   アセット選択変更・保存再試行と、各形式の原本保護を実ファイルで検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-06
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Panels/InspectorPanel.hpp>
#include <Editor/Import/FbxMetaSerializer.hpp>
#include <Editor/Import/FbxImportTool.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/UndoStack.hpp>
#include "Util/MaterialAuthoringEditing.hpp"
#include "Ai/Bus/BusInternal.hpp"
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/ModelSource.hpp>
#include <Engine/Asset/ModelAsset.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Asset/AnimatorControllerAsset.hpp>
#include <Engine/Asset/AvatarMaskAsset.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/PhysicsMaterialAsset.hpp>
#include <Engine/Asset/TexDescSerializer.hpp>
#include <Engine/Asset/TextureAsset.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/Material.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/TerrainAssetSerializer.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <fstream>
#include <functional>
#include <sstream>
#include <utility>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace fbzz::tests {
namespace {
/// @note UI とファイル処理だけを検証し、GPU 資源を作成しない。
class PersistenceCpuRenderer final : public renderer::IRenderer {
public:
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
    std::function<void()> beforeShaderCreate;
    std::unique_ptr<renderer::IShader> CreateNativeShader(const std::string&) override
    {
        if (auto callback = std::exchange(beforeShaderCreate, {})) callback();
        return {};
    }
    std::unique_ptr<renderer::IConstantBuffer> CreateNativeConstantBuffer(size_t) override { return {}; }
    std::unique_ptr<renderer::IBuffer> CreateNativeVertexBuffer(const void*, size_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::IBuffer> CreateNativeIndexBuffer(const void*, uint32_t) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTexture(const std::string&) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromData(const uint8_t*, uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTexture3DFromData(const uint8_t*, uint32_t, uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromRenderTarget(renderer::IRenderTarget&, uint32_t, renderer::RenderTargetTextureKind) override { return {}; }
    std::unique_ptr<renderer::IPipelineState> CreateNativePipelineState(const renderer::PipelineStateDesc&) override { return {}; }
    std::unique_ptr<renderer::IRenderTarget> CreateNativeRenderTarget(uint32_t, uint32_t, const renderer::RenderTargetDesc&) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeComputeTexture(uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeStructuredBuffer(const void*, uint32_t, uint32_t) override { return {}; }
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeRWStructuredBuffer(const void*, uint32_t, uint32_t) override { return {}; }
};
class PersistenceInspector final : public editor::InspectorPanel {
public:
    void Draw(editor::EditorContext& context) { OnRenderContent(context); }
};
} /// @note namespace

class AssetInspectorPersistenceTest : public testkit::EditorFixture {
protected:
    PersistenceCpuRenderer m_renderer;
    renderer::ResourceManager m_resources{m_renderer};
    PersistenceInspector m_panel;
    ImGuiContext* m_previous = nullptr;
    ImGuiContext* m_imgui = nullptr;
    void SetUp() override
    {
        EditorFixture::SetUp();
        Context().projectRoot = util::FileSystem::PathToUtf8(ProjectRoot());
        editor::AssetDirtyRegistry::DiscardAll();
        editor::InspectorPanel::ResetPendingDataAssetSaves();
        asset::AssetManager::Init(m_resources, util::FileSystem::PathToUtf8(ProjectRoot() / "Assets"));
        Context().resources = &m_resources;
        m_previous = ImGui::GetCurrentContext();
        m_imgui = ImGui::CreateContext();
        ImGui::SetCurrentContext(m_imgui);
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = {1600, 1600};
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }
    void TearDown() override
    {
        m_renderer.beforeShaderCreate = {};
        m_panel.OnShutdown();
        editor::AssetDirtyRegistry::DiscardAll();
        asset::AssetManager::UnloadAll();
        ImGui::DestroyContext(m_imgui);
        ImGui::SetCurrentContext(m_previous);
        EditorFixture::TearDown();
    }
    std::string Path(const char* name) const
    { return util::FileSystem::PathToUtf8(ProjectRoot() / "Assets" / name); }
    std::string Bytes(const std::string& path) const
    {
        std::ifstream file(util::FileSystem::PathFromUtf8(path), std::ios::binary);
        std::ostringstream output;
        output << file.rdbuf();
        return output.str();
    }
    std::string Draw(const std::string& path, const char* widget = nullptr,
                     const char* scope = nullptr, bool textInput = false,
                     const char* nestedScope = nullptr)
    {
        Context().selectedAssetPath = path;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({1400, 1400});
        ImGui::Begin("Asset Persistence Test");
        ImGuiID activatedId = 0;
        if (widget) {
            if (scope) ImGui::PushID(scope);
            if (nestedScope) ImGui::PushID(nestedScope);
            const auto id = ImGui::GetID(widget);
            activatedId = id;
            GImGui->NavActivateId = id;
            GImGui->NavActivateDownId = id;
            GImGui->NavActivatePressedId = id;
            GImGui->NavActivateFlags = textInput ? ImGuiActivateFlags_PreferInput : ImGuiActivateFlags_None;
            if (nestedScope) ImGui::PopID();
            if (scope) ImGui::PopID();
        }
        ImGui::LogToBuffer();
        m_panel.Draw(Context());
        const std::string text = GImGui->LogBuffer.c_str();
        if (textInput) EXPECT_EQ(GImGui->ActiveId, activatedId) << text;
        ImGui::LogFinish();
        ImGui::End();
        ImGui::EndFrame();
        return text;
    }
    void Click(const std::string& path, const char* widget)
    {
        (void)Draw(path);
        (void)Draw(path, widget);
    }
    void EditNumber(const std::string& path, const char* widget, const char* value,
                    const char* scope = nullptr, const char* nestedScope = nullptr)
    {
        (void)Draw(path);
        (void)Draw(path, widget, scope, true, nestedScope);
        ImGui::GetIO().AddInputCharactersUTF8(value);
        (void)Draw(path);
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
        (void)Draw(path);
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
        (void)Draw(path);
    }
    void MakeTerrain(const std::string& path, int columns)
    {
        scene::TerrainComponent terrain;
        terrain.columns = columns;
        terrain.rows = 2;
        terrain.heightData.assign(terrain.VertexCount(), 0.25f);
        ASSERT_TRUE(scene::TerrainAssetSerializer::Save(terrain, path));
    }
};

TEST_F(AssetInspectorPersistenceTest, ImportsGltfGlbAndObjWithoutFbxUnitConversion)
{
    std::string binary(36, '\0');
    binary[14] = static_cast<char>(0x80); binary[15] = static_cast<char>(0x3f);
    binary[30] = static_cast<char>(0x80); binary[31] = static_cast<char>(0x3f);
    for (const std::string extension : { ".gltf", ".glb", ".obj" }) {
        const std::string stem = "Triangle_" + extension.substr(1);
        const std::string source = Path((stem + extension).c_str());
        std::string bytes;
        if (extension == ".obj") {
            bytes = "o Triangle\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
        } else {
            std::string json = R"({"asset":{"version":"2.0","generator":"Blender"},"buffers":[{"byteLength":36)";
            if (extension == ".gltf")
                json += R"(,"uri":"data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA")";
            json += R"(}],"bufferViews":[{"buffer":0,"byteLength":36}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]}],"meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})";
            if (extension == ".gltf") bytes = json;
            else {
                while (json.size() % 4 != 0) json += ' ';
                const auto appendUint = [&bytes](uint32_t value) {
                    for (unsigned shift = 0; shift < 32; shift += 8)
                        bytes.push_back(static_cast<char>((value >> shift) & 0xffu));
                };
                appendUint(0x46546c67u); appendUint(2u);
                appendUint(static_cast<uint32_t>(28 + json.size() + binary.size()));
                appendUint(static_cast<uint32_t>(json.size())); appendUint(0x4e4f534au);
                bytes += json;
                appendUint(static_cast<uint32_t>(binary.size())); appendUint(0x004e4942u);
                bytes += binary;
            }
        }
        { std::ofstream out(util::FileSystem::PathFromUtf8(source), std::ios::binary); out << bytes; }
        ASSERT_TRUE(editor::FbxImportTool::Import(source, Path(stem.c_str()), source, {})) << extension;
        const auto* model = asset::AssetManager::LoadAndGet<asset::ModelAsset>(source);
        ASSERT_NE(model, nullptr) << extension;
        ASSERT_FALSE(model->lods.empty());
        ASSERT_FALSE(model->lods[0].submeshes.empty());
        const auto* mesh = model->lods[0].submeshes[0].mesh.get();
        ASSERT_NE(mesh, nullptr);
        float maxX = 0.0f, maxY = 0.0f;
        for (const auto& vertex : mesh->cpuVertices) {
            maxX = (std::max)(maxX, vertex.position.x);
            maxY = (std::max)(maxY, vertex.position.y);
        }
        EXPECT_NEAR(maxX, 1.0f, 0.0001f) << extension;
        EXPECT_NEAR(maxY, 1.0f, 0.0001f) << extension;
    }
}

TEST_F(AssetInspectorPersistenceTest, AllModelSourcesResolveTheirOwnBakedContainersAndMaterials)
{
    int sourceIndex = 0;
    for (const std::string extension : { ".fbx", ".gltf", ".glb", ".obj", ".GLTF", ".GLB", ".OBJ" }) {
        const std::string stem = "Model_" + std::to_string(sourceIndex++);
        const std::string source = Path((stem + extension).c_str());
        ASSERT_TRUE(util::FileSystem::WriteText(source, "source"));
        const std::string guid = asset::AssetDatabase::GuidFromPath(source);
        ASSERT_FALSE(guid.empty());
        const std::string baked = asset::AssetManager::BakedDirForSource(source);
        ASSERT_FALSE(baked.empty()) << source;
        ASSERT_TRUE(util::FileSystem::EnsureDirectory(baked + "/materials"));
        const std::string model = baked + "/" + stem + ".fzasset";
        const std::string material = baked + "/materials/Surface.mat";
        ASSERT_TRUE(util::FileSystem::WriteText(model, "cached container"));
        ASSERT_TRUE(util::FileSystem::WriteText(material, "cached material"));
        EXPECT_TRUE(util::FileSystem::SamePathText(
            asset::AssetManager::ResolveImportPath(source, true), model)) << extension;
        EXPECT_TRUE(util::FileSystem::SamePathText(
            asset::AssetManager::ResolveImportPath("Assets/" + stem + extension, true), model)) << extension;
        EXPECT_TRUE(util::FileSystem::SamePathText(
            asset::AssetManager::ResolveAssetPath("Assets/" + stem + "/materials/Surface.mat"), material)) << extension;
        const std::string extracted = Path((stem + "/materials/Surface.mat").c_str());
        ASSERT_TRUE(util::FileSystem::EnsureDirectory(
            util::FileSystem::PathFromUtf8(extracted).parent_path()));
        ASSERT_TRUE(util::FileSystem::WriteText(extracted, "extracted material"));
        EXPECT_TRUE(util::FileSystem::SamePathText(
            asset::AssetManager::ResolveAssetPath("Assets/" + stem + "/materials/Surface.mat"), extracted)) << extension;
    }
}

TEST_F(AssetInspectorPersistenceTest, SaveAllKeepsDistinctTerrainValuesAfterSelectionChanges)
{
    const auto first = Path("First.terrain"), second = Path("Second.terrain");
    MakeTerrain(first, 2);
    MakeTerrain(second, 3);
    EditNumber(first, "Height Blend Depth", "0.65");
    ASSERT_TRUE(editor::AssetDirtyRegistry::IsDirty(first));
    const auto firstDraft = editor::AssetDirtyRegistry::FindDraft<scene::TerrainComponent>(first);
    ASSERT_NE(firstDraft, nullptr);
    EditNumber(second, "Height Blend Depth", "0.35");
    (void)Draw(first);
    EXPECT_EQ(editor::AssetDirtyRegistry::FindDraft<scene::TerrainComponent>(first), firstDraft);
    EXPECT_FLOAT_EQ(firstDraft->heightBlendDepth, 0.65f);
    (void)Draw(second);
    ASSERT_EQ(editor::AssetDirtyRegistry::SaveAll(), 0);
    scene::TerrainComponent savedFirst, savedSecond;
    ASSERT_TRUE(scene::TerrainAssetSerializer::Load(first, savedFirst));
    ASSERT_TRUE(scene::TerrainAssetSerializer::Load(second, savedSecond));
    EXPECT_EQ(savedFirst.columns, 2);
    EXPECT_EQ(savedSecond.columns, 3);
    EXPECT_FLOAT_EQ(savedFirst.heightBlendDepth, 0.65f);
    EXPECT_FLOAT_EQ(savedSecond.heightBlendDepth, 0.35f);
}

TEST_F(AssetInspectorPersistenceTest, TextureDraftSurvivesRevisitAndApply)
{
    const auto first = Path("First.png"), second = Path("Second.png");
    ASSERT_TRUE(util::FileSystem::WriteTextAtomic(first, "source"));
    ASSERT_TRUE(util::FileSystem::WriteTextAtomic(second, "source"));
    (void)asset::AssetDatabase::GuidFromPath(first);
    (void)asset::AssetDatabase::GuidFromPath(second);
    asset::TextureAsset texture;
    asset::TexDescSerializer serializer;
    texture.sourcePath = first;
    ASSERT_TRUE(serializer.Save(texture, first + ".meta"));
    texture.sourcePath = second;
    ASSERT_TRUE(serializer.Save(texture, second + ".meta"));
    Click(first, "sRGB");
    const auto draft = editor::AssetDirtyRegistry::FindDraft<asset::TextureAsset>(first + ".meta");
    ASSERT_NE(draft, nullptr);
    EXPECT_FALSE(draft->settings.srgb);
    (void)Draw(second);
    Click(first, "Apply");
    asset::TextureAsset saved;
    ASSERT_TRUE(serializer.Load(first + ".meta", saved));
    EXPECT_FALSE(saved.settings.srgb);
    EXPECT_FALSE(editor::AssetDirtyRegistry::IsDirty(first + ".meta"));
}

TEST_F(AssetInspectorPersistenceTest, ModelDraftSurvivesRevisitAndApply)
{
    const auto first = Path("First.obj"), second = Path("Second.obj");
    ASSERT_TRUE(util::FileSystem::WriteTextAtomic(first, "# source\n"));
    ASSERT_TRUE(util::FileSystem::WriteTextAtomic(second, "# source\n"));
    (void)asset::AssetDatabase::GuidFromPath(first);
    (void)asset::AssetDatabase::GuidFromPath(second);
    editor::FbxImportOptions options;
    ASSERT_TRUE(editor::FbxMetaSerializer::SaveOptions(first, options));
    ASSERT_TRUE(editor::FbxMetaSerializer::SaveOptions(second, options));
    Click(first, "Generate Normals");
    const auto draft = editor::AssetDirtyRegistry::FindDraft<editor::FbxImportOptions>(first + ".meta");
    ASSERT_NE(draft, nullptr);
    EXPECT_FALSE(draft->generateNormals);
    (void)Draw(second);
    Click(first, "Apply");
    editor::FbxImportOptions saved;
    ASSERT_TRUE(editor::FbxMetaSerializer::LoadOptions(first, saved));
    EXPECT_FALSE(saved.generateNormals);
}

TEST_F(AssetInspectorPersistenceTest, MaskDraftSurvivesRevisitAndApply)
{
    const auto first = Path("First.mask"), second = Path("Second.mask");
    asset::AvatarMaskAsset mask;
    mask.defaultInclude = true;
    ASSERT_TRUE(asset::SaveAvatarMaskAsset(first, mask));
    ASSERT_TRUE(asset::SaveAvatarMaskAsset(second, mask));
    Click(first, "Default Include (未列挙のボーンも有効にする)");
    const auto draft = editor::AssetDirtyRegistry::FindDraft<asset::AvatarMaskAsset>(first);
    ASSERT_NE(draft, nullptr);
    EXPECT_FALSE(draft->defaultInclude);
    (void)Draw(second);
    Click(first, "Apply");
    asset::AvatarMaskAsset saved;
    ASSERT_TRUE(asset::LoadAvatarMaskAsset(first, saved));
    EXPECT_FALSE(saved.defaultInclude);
}

TEST_F(AssetInspectorPersistenceTest, DiscardReloadsVisibleDraftWithoutSavingIt)
{
    const auto path = Path("Discard.terrain");
    MakeTerrain(path, 2);
    const auto original = Bytes(path);
    EditNumber(path, "Height Blend Depth", "0.65");
    ASSERT_TRUE(editor::AssetDirtyRegistry::IsDirty(path));
    editor::AssetDirtyRegistry::DiscardAll();
    EditNumber(path, "Height Blend Depth", "0.35");
    ASSERT_TRUE(editor::AssetDirtyRegistry::IsDirty(path));
    EXPECT_EQ(Bytes(path), original);
    ASSERT_TRUE(editor::AssetDirtyRegistry::Save(path));
    scene::TerrainComponent saved;
    ASSERT_TRUE(scene::TerrainAssetSerializer::Load(path, saved));
    EXPECT_FLOAT_EQ(saved.heightBlendDepth, 0.35f);
}

TEST_F(AssetInspectorPersistenceTest, PhysicsFailureKeepsDirtyValueAndSaveAllRetriesAfterSelectionChange)
{
    const auto first = Path("First.physmat"), second = Path("Second.physmat");
    asset::PhysicsMaterialAsset material;
    ASSERT_TRUE(asset::SavePhysicsMaterialAssetToFile(first, material));
    ASSERT_TRUE(asset::SavePhysicsMaterialAssetToFile(second, material));
    const auto original = Bytes(first);
    HANDLE lock = CreateFileW(util::FileSystem::PathFromUtf8(first).c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(lock, INVALID_HANDLE_VALUE);
    /// @note FloatRange の ID は field key → RangeField label → slider の順に入れ子になる。
    EditNumber(first, "##slider", "1234", "density", "##v");
    const auto* edited = asset::AssetManager::Get<asset::PhysicsMaterialAsset>(
        asset::AssetManager::Load<asset::PhysicsMaterialAsset>(first));
    const bool densityEdited = edited && edited->material.density == 1234.0f;
    editor::InspectorPanel::FlushPendingDataAssetSaves(Context());
    const auto status = Draw(first);
    CloseHandle(lock);
    ASSERT_TRUE(densityEdited) << status;
    ASSERT_TRUE(editor::AssetDirtyRegistry::IsDirty(first));
    EXPECT_NE(status.find("Could not save .physmat"), std::string::npos);
    EXPECT_EQ(Bytes(first), original);
    (void)Draw(second);
    ASSERT_EQ(editor::AssetDirtyRegistry::SaveAll(), 0);
    asset::PhysicsMaterialAsset saved;
    ASSERT_TRUE(asset::LoadPhysicsMaterialAssetFromFile(first, saved));
    EXPECT_FLOAT_EQ(saved.material.density, 1234.0f);
    EXPECT_FALSE(editor::AssetDirtyRegistry::HasAny());
}

TEST_F(AssetInspectorPersistenceTest, HiddenPhysicsInspectorStillFlushesItsQueuedSave)
{
    const auto path = Path("Hidden.physmat");
    asset::PhysicsMaterialAsset material;
    ASSERT_TRUE(asset::SavePhysicsMaterialAssetToFile(path, material));
    EditNumber(path, "##slider", "1234", "density", "##v");
    const auto* edited = asset::AssetManager::Get<asset::PhysicsMaterialAsset>(
        asset::AssetManager::Load<asset::PhysicsMaterialAsset>(path));
    ASSERT_NE(edited, nullptr);
    ASSERT_FLOAT_EQ(edited->material.density, 1234.0f);
    ASSERT_TRUE(editor::AssetDirtyRegistry::IsDirty(path));
    (void)Draw(Path("Other.txt"));
    editor::InspectorPanel::FlushPendingDataAssetSaves(Context());
    asset::PhysicsMaterialAsset saved;
    ASSERT_TRUE(asset::LoadPhysicsMaterialAssetFromFile(path, saved));
    EXPECT_FLOAT_EQ(saved.material.density, 1234.0f);
    EXPECT_FALSE(editor::AssetDirtyRegistry::HasAny());
}

TEST_F(AssetInspectorPersistenceTest, EveryAssetSerializerPreservesLockedOriginalAndCanRetry)
{
    asset::MaterialAssetAuthoring material;
    material.shaderPath.clear();
    asset::PhysicsMaterialAsset physics;
    asset::AvatarMaskAsset mask;
    asset::AnimatorControllerAsset controller;
    asset::TextureAsset texture;
    scene::TerrainComponent terrain;
    terrain.columns = terrain.rows = 2;
    terrain.heightData.assign(4, 0.25f);
    editor::FbxImportOptions model;
    const std::vector<std::pair<std::string, std::function<bool(const std::string&)>>> serializers = {
        {"Locked.mat", [&](const auto& path) { return asset::SaveMaterialAssetToFile(path, material); }},
        {"Locked.physmat", [&](const auto& path) { return asset::SavePhysicsMaterialAssetToFile(path, physics); }},
        {"Locked.mask", [&](const auto& path) { return asset::SaveAvatarMaskAsset(path, mask); }},
        {"Locked.animcontroller", [&](const auto& path) { return asset::SaveAnimatorControllerAsset(path, controller); }},
        {"Locked.terrain", [&](const auto& path) { return scene::TerrainAssetSerializer::Save(terrain, path); }},
        {"Locked.png.meta", [&](const auto& path) { return asset::TexDescSerializer{}.Save(texture, path); }},
        {"Locked.obj.meta", [&](const auto& path) { return editor::FbxMetaSerializer::SaveOptions(path.substr(0, path.size() - 5), model); }}
    };
    for (const auto& [name, save] : serializers) {
        SCOPED_TRACE(name);
        const auto path = Path(name.c_str());
        const std::string original = "# original\r\n";
        ASSERT_TRUE(util::FileSystem::WriteTextAtomic(path, original));
        HANDLE lock = CreateFileW(util::FileSystem::PathFromUtf8(path).c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        ASSERT_NE(lock, INVALID_HANDLE_VALUE);
        const bool saved = save(path);
        CloseHandle(lock);
        EXPECT_FALSE(saved);
        EXPECT_EQ(Bytes(path), original);
        EXPECT_TRUE(save(path));
        EXPECT_NE(Bytes(path), original);
    }
}

TEST_F(AssetInspectorPersistenceTest, MaterialUiPublishesAndPersistsExactUndoRedoValues)
{
    const auto path = Path("MaterialUndo.mat");
    asset::MaterialAssetAuthoring values;
    values.shaderPath.clear();
    values.doubleSided = false;
    ASSERT_TRUE(asset::SaveMaterialAssetToFile(path, values));
    editor::UndoStack undo;
    struct RestoreUndo {
        editor::EditorContext& context;
        editor::UndoStack* previous;
        ~RestoreUndo() { context.undoStack = previous; }
    } restore{Context(), std::exchange(Context().undoStack, &undo)};
    Click(path, "Double Sided");
    (void)Draw(path);
    const auto handle = asset::AssetManager::Load<asset::MaterialAsset>(path);
    const auto published = asset::ReadMaterialAuthoring(handle);
    ASSERT_NE(published, nullptr);
    EXPECT_TRUE(published->Data().doubleSided);
    ASSERT_EQ(undo.GetHistorySize(), 1u);
    asset::MaterialAssetAuthoring saved;
    ASSERT_TRUE(asset::LoadMaterialAssetFromFile(path, saved));
    EXPECT_TRUE(saved.doubleSided);
    ASSERT_TRUE(undo.Undo());
    ASSERT_TRUE(asset::LoadMaterialAssetFromFile(path, saved));
    EXPECT_FALSE(saved.doubleSided);
    ASSERT_TRUE(undo.Redo());
    ASSERT_TRUE(asset::LoadMaterialAssetFromFile(path, saved));
    EXPECT_TRUE(saved.doubleSided);
    EXPECT_TRUE(published->Data().doubleSided);
}

TEST_F(AssetInspectorPersistenceTest, UnchangedMaterialUiDoesNotPublishOrMarkDirty)
{
    const auto path = Path("UnchangedMaterial.mat");
    asset::MaterialAssetAuthoring values;
    values.shaderPath.clear();
    ASSERT_TRUE(asset::SaveMaterialAssetToFile(path, values));
    const auto handle = asset::AssetManager::Load<asset::MaterialAsset>(path);
    (void)Draw(path);
    editor::AssetDirtyRegistry::DiscardAll();
    const auto normalized = asset::ReadMaterialAuthoring(handle);
    ASSERT_NE(normalized, nullptr);
    const auto revision = Context().MaterialPreviewRevision(editor::NormalizeAssetPath(path));
    (void)Draw(path);
    (void)Draw(path);
    EXPECT_EQ(asset::ReadMaterialAuthoring(handle), normalized);
    EXPECT_EQ(Context().MaterialPreviewRevision(editor::NormalizeAssetPath(path)), revision);
    EXPECT_FALSE(editor::AssetDirtyRegistry::IsDirty(path));
}

TEST_F(AssetInspectorPersistenceTest, ReloadConflictsWithOwnedEditorDraftWithoutOverwritingNewAsset)
{
    const auto path = Path("ReloadConflict.mat");
    asset::MaterialAssetAuthoring original;
    original.shaderPath.clear();
    original.params["albedo"] = {0.2f, 0.4f, 0.6f, 1.0f};
    ASSERT_TRUE(asset::SaveMaterialAssetToFile(path, original));
    const auto handle = asset::AssetManager::Load<asset::MaterialAsset>(path);
    auto edit = editor::material_edit_detail::BeginAssetEdit(handle);
    ASSERT_TRUE(edit.has_value());
    edit->Draft().params["albedo"][0] = 0.9f;
    const auto oldSnapshot = asset::ReadMaterialAuthoring(handle);
    auto replacement = original;
    replacement.params["albedo"][0] = 0.7f;
    ASSERT_TRUE(asset::SaveMaterialAssetToFile(path, replacement));
    asset::AssetManager::ReloadPath(path);
    ASSERT_NE(asset::ReadMaterialAuthoring(handle), oldSnapshot);
    EXPECT_EQ(editor::material_edit_detail::FinishAssetEdit(handle, *edit),
        asset::detail::MaterialAuthoringCommitResult::CONFLICT);
    ASSERT_TRUE(editor::material_edit_detail::SaveAssetSnapshot(path, handle));
    asset::MaterialAssetAuthoring saved;
    ASSERT_TRUE(asset::LoadMaterialAssetFromFile(path, saved));
    EXPECT_FLOAT_EQ(saved.params.at("albedo")[0], 0.7f);
    EXPECT_FLOAT_EQ(oldSnapshot->Data().params.at("albedo")[0], 0.2f);
    EXPECT_FALSE(editor::AssetDirtyRegistry::IsDirty(path));
}

TEST_F(AssetInspectorPersistenceTest, DraftAliasesCannotChangePublishedValuesUsedByDirtySave)
{
    const auto path = Path("DraftAlias.mat");
    asset::MaterialAssetAuthoring original;
    original.shaderPath.clear();
    ASSERT_TRUE(asset::SaveMaterialAssetToFile(path, original));
    const auto handle = asset::AssetManager::Load<asset::MaterialAsset>(path);
    auto edit = editor::material_edit_detail::BeginAssetEdit(handle);
    ASSERT_TRUE(edit.has_value());
    auto& alias = edit->Draft();
    alias.params["albedo"] = {0.25f, 0.5f, 0.75f, 1.0f};
    alias.textures["normal"] = "Assets/CommittedNormal.png";
    ASSERT_EQ(editor::material_edit_detail::FinishAssetEdit(handle, *edit),
        asset::detail::MaterialAuthoringCommitResult::COMMITTED);
    alias.params["albedo"][0] = 0.95f;
    alias.textures["normal"] = "Assets/StaleDraft.png";
    editor::AssetDirtyRegistry::Register(path, path, "MAT", [path, handle] {
        return editor::material_edit_detail::SaveAssetSnapshot(path, handle);
    });
    ASSERT_TRUE(editor::AssetDirtyRegistry::Save(path));
    asset::MaterialAssetAuthoring saved;
    ASSERT_TRUE(asset::LoadMaterialAssetFromFile(path, saved));
    EXPECT_FLOAT_EQ(saved.params.at("albedo")[0], 0.25f);
    EXPECT_EQ(saved.textures.at("normal"), "Assets/CommittedNormal.png");
}


TEST_F(AssetInspectorPersistenceTest, MaterialUiRejectsAStaleDraftAfterNativeShaderCallback)
{
    const auto path = Path("UiNativeConflict.mat");
    const auto shader = Path("UiNativeConflict.hlsl");
    ASSERT_TRUE(util::FileSystem::WriteText(shader, "float4 PSMain() : SV_Target { return 1; }"));
    asset::MaterialAssetAuthoring values;
    values.shaderPath = shader;
    values.doubleSided = false;
    ASSERT_TRUE(asset::SaveMaterialAssetToFile(path, values));
    const auto handle = asset::AssetManager::Load<asset::MaterialAsset>(path);
    bool called = false;
    m_renderer.beforeShaderCreate = [handle, &called] {
        called = true;
        if (auto* live = asset::AssetManager::Get<asset::MaterialAsset>(handle)) {
            auto edit = live->BeginAuthoringEdit();
            edit.Draft().doubleSided = true;
            EXPECT_EQ(edit.Commit(*live), asset::detail::MaterialAuthoringCommitResult::COMMITTED);
        }
    };
    (void)Draw(path);
    ASSERT_TRUE(called);
    const auto published = asset::ReadMaterialAuthoring(handle);
    ASSERT_NE(published, nullptr);
    EXPECT_TRUE(published->Data().doubleSided);
    EXPECT_FALSE(editor::AssetDirtyRegistry::IsDirty(path));
    asset::MaterialAssetAuthoring saved;
    ASSERT_TRUE(asset::LoadMaterialAssetFromFile(path, saved));
    EXPECT_FALSE(saved.doubleSided);
}


TEST_F(AssetInspectorPersistenceTest, AcceptedSamePathElementRetiresRuntimeAndPropertyCaches)
{
    AttachScene();
    const auto path = Path("SamePathElement.mat");
    asset::MaterialAssetAuthoring values;
    values.shaderPath.clear();
    ASSERT_TRUE(asset::SaveMaterialAssetToFile(path, values));
    const auto handle = asset::AssetManager::Load<asset::MaterialAsset>(path);
    ASSERT_TRUE(handle.IsValid());
    auto& object = AttachedScene()->CreateGameObject("SamePathElement");
    auto& slot = object.AddComponent<scene::MaterialComponent>();
    slot.SetMaterialPath(path);
    ASSERT_TRUE(slot.EnsureMaterialAsset());
    slot.material = std::make_unique<renderer::Material>();
    renderer::ShaderDescriptor descriptor;
    slot.propertyValidationDescriptor = &descriptor;
    slot.propertyValidationCache[7] = 1;
    editor::material_edit_detail::SlotTarget target(Context(), object, 0);
    auto passive = slot.BeginAuthoringEdit();
    ASSERT_FALSE(passive.Draft().materialAsset.IsValid());
    EXPECT_EQ(editor::material_edit_detail::FinishSlotPathEdit(target, passive, false),
        asset::detail::MaterialAuthoringCommitResult::UNCHANGED);
    ASSERT_NE(slot.material, nullptr);
    ASSERT_EQ(slot.propertyValidationDescriptor, &descriptor);
    auto edit = slot.BeginAuthoringEdit();
    ASSERT_EQ(edit.Draft().materialPath, path);
    /// @note Simulate AssetPathField's accepted=true event with identical text; execute the actual Element finalization helper once.
    EXPECT_EQ(editor::material_edit_detail::FinishSlotPathEdit(target, edit, true),
        asset::detail::MaterialAuthoringCommitResult::COMMITTED);
    EXPECT_EQ(slot.GetMaterialPath(), path);
    EXPECT_EQ(slot.GetMaterialAsset(), handle);
    EXPECT_EQ(slot.material, nullptr);
    EXPECT_EQ(slot.propertyValidationDescriptor, nullptr);
    EXPECT_TRUE(slot.propertyValidationCache.empty());
}


TEST_F(AssetInspectorPersistenceTest, MaterialOverrideRejectsSceneSwitchDuringNativeShaderCreation)
{
    AttachScene();
    auto& original = AttachedScene()->CreateGameObject("OriginalMaterial");
    const std::string id = original.instanceId;
    const auto path = Path("AiSceneSwitch.mat");
    const auto shader = Path("AiSceneSwitch.hlsl");
    ASSERT_TRUE(util::FileSystem::WriteText(shader, "float4 PSMain() : SV_Target { return 1; }"));
    asset::MaterialAssetAuthoring values;
    values.shaderPath = shader;
    ASSERT_TRUE(asset::SaveMaterialAssetToFile(path, values));
    const auto handle = asset::AssetManager::Load<asset::MaterialAsset>(path);
    original.AddComponent<scene::MaterialComponent>().SetMaterialReference(path, handle);
    scene::Scene replacement;
    auto& other = replacement.CreateGameObject("SameGuidOtherScene");
    other.instanceId = id;
    auto& otherMaterial = other.AddComponent<scene::MaterialComponent>();
    otherMaterial.SetMaterialReference(path, handle);
    const auto otherBefore = otherMaterial.ReadAuthoring();
    struct RestoreContext {
        editor::EditorContext& context;
        scene::Scene* scene;
        renderer::ResourceManager* resources;
        ~RestoreContext() { context.activeScene = scene; context.resources = resources; }
    } restore{Context(), Context().activeScene, Context().resources};
    Context().resources = &m_resources;
    bool called = false;
    m_renderer.beforeShaderCreate = [&] { called = true; Context().activeScene = &replacement; };
    editor::ai::bus::BusHandlerTable table;
    editor::ai::bus::RegisterSceneHandlers(table);
    const auto* entry = table.Find("material.override");
    ASSERT_NE(entry, nullptr);
    ASSERT_NE(entry->builder, nullptr);
    auto payload = editor::ai::JsonValue::MakeObject();
    payload.Set("id", editor::ai::JsonValue(id));
    payload.Set("parameter", editor::ai::JsonValue(std::string("roughness")));
    auto parameter = editor::ai::JsonValue::MakeArray();
    parameter.Push(editor::ai::JsonValue(0.4));
    payload.Set("value", std::move(parameter));
    editor::ai::bus::Outcome error;
    const auto command = entry->builder(Context(), "material.override", payload, error, {}, nullptr);
    EXPECT_TRUE(called);
    EXPECT_EQ(command, nullptr);
    EXPECT_EQ(error.code, "CONFLICT");
    EXPECT_EQ(otherMaterial.ReadAuthoring(), otherBefore);
}

TEST_F(AssetInspectorPersistenceTest, MaterialOverrideRejectsReplacementOwnerDuringNativeShaderCreation)
{
    AttachScene();
    auto& object = AttachedScene()->CreateGameObject("ReplaceMaterial");
    const std::string id = object.instanceId;
    const auto path = Path("AiOwnerReplace.mat");
    const auto shader = Path("AiOwnerReplace.hlsl");
    ASSERT_TRUE(util::FileSystem::WriteText(shader, "float4 PSMain() : SV_Target { return 1; }"));
    asset::MaterialAssetAuthoring values;
    values.shaderPath = shader;
    ASSERT_TRUE(asset::SaveMaterialAssetToFile(path, values));
    const auto handle = asset::AssetManager::Load<asset::MaterialAsset>(path);
    object.AddComponent<scene::MaterialComponent>().SetMaterialReference(path, handle);
    struct RestoreResources {
        editor::EditorContext& context;
        renderer::ResourceManager* resources;
        ~RestoreResources() { context.resources = resources; }
    } restore{Context(), std::exchange(Context().resources, &m_resources)};
    scene::MaterialSlot::AuthoringSnapshotPtr replacementSnapshot;
    m_renderer.beforeShaderCreate = [&] {
        object.RemoveComponent<scene::MaterialComponent>();
        auto& replacement = object.AddComponent<scene::MaterialComponent>();
        replacement.SetMaterialReference(path, handle);
        replacementSnapshot = replacement.ReadAuthoring();
    };
    editor::ai::bus::BusHandlerTable table;
    editor::ai::bus::RegisterSceneHandlers(table);
    const auto* entry = table.Find("material.override");
    ASSERT_NE(entry, nullptr);
    ASSERT_NE(entry->builder, nullptr);
    auto payload = editor::ai::JsonValue::MakeObject();
    payload.Set("id", editor::ai::JsonValue(id));
    payload.Set("parameter", editor::ai::JsonValue(std::string("roughness")));
    auto parameter = editor::ai::JsonValue::MakeArray();
    parameter.Push(editor::ai::JsonValue(0.4));
    payload.Set("value", std::move(parameter));
    editor::ai::bus::Outcome error;
    const auto command = entry->builder(Context(), "material.override", payload, error, {}, nullptr);
    ASSERT_NE(replacementSnapshot, nullptr);
    EXPECT_EQ(command, nullptr);
    EXPECT_EQ(error.code, "CONFLICT");
    auto* current = object.GetComponent<scene::MaterialComponent>();
    ASSERT_NE(current, nullptr);
    EXPECT_EQ(current->ReadAuthoring(), replacementSnapshot);
}

} /// @note namespace fbzz::tests
