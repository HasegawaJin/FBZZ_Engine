/// @file    MaterialPreviewShaderTests.cpp
/// @brief   材質サムネイルの本番描画を実DX12で読み返し、水と粒子の設定反映を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-09
#include <TestKit/TestKit.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Components/FiberComponent.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <initializer_list>
#include <Editor/Panels/MaterialPreviewCore.hpp>
#include <Tools/TerrainPaintLibrary.hpp>
#include <Tools/TerrainTool.hpp>
#include <Engine/Asset/MeshInstancesAsset.hpp>
#include <Engine/Scene/Components/InstancedMeshComponent.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Renderer/Camera.hpp>
#include <ImGuizmo.h>
#include <Editor/EditorContext.hpp>
#include <Engine/Asset/TerrainPaintAsset.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/GuidRefCodec.hpp>
#include <Engine/Asset/ShaderCapabilities.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Graphics/Renderer/RendererFactory.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Graphics/Renderer/IRenderer.hpp>
#include <Graphics/Renderer/DrawCall.hpp>
#include <Math/MathUtils.hpp>
#include <Graphics/Renderer/IImGuiRenderer.hpp>
#include <Graphics/Renderer/IShader.hpp>
#include <Graphics/Renderer/ShaderPathResolver.hpp>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <objbase.h>
#pragma comment(lib, "ole32.lib")
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iterator>
#include <memory>
#include <optional>
#include <vector>
#include <array>
#include <imgui.h>

namespace fbzz::tests {
namespace {
std::filesystem::path ResolvePreviewShader(const std::filesystem::path& requested)
{
    const auto path = requested.generic_string();
    constexpr std::string_view SHADER_PREFIX = "Assets/Shaders/";
    if (path.starts_with(SHADER_PREFIX)) return std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT) / path.substr(SHADER_PREFIX.size());
    if (path.starts_with("guid:")) return std::filesystem::u8path(asset::AssetManager::ResolveAssetPath(path));
    return requested;
}
float RgbEnergy(const std::vector<float>& pixels)
{
    float result = 0.0f;
    for (size_t offset = 0; offset + 3 < pixels.size(); offset += 4)
        result += std::abs(pixels[offset]) + std::abs(pixels[offset + 1]) + std::abs(pixels[offset + 2]);
    return result;
}
}

class MaterialPreviewShaderTest : public testkit::Fixture {
protected:
    void SetUp() override
    {
        Fixture::SetUp();
        renderer::SetShaderPathResolver(ResolvePreviewShader);
        m_com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ASSERT_TRUE(SUCCEEDED(m_com));
        m_window = CreateWindowExW(0, L"STATIC", L"Material preview GPU test", WS_POPUP,
            0, 0, 256, 256, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(m_window, nullptr);
        m_bundle = renderer::CreateRenderer(renderer::RendererBackend::DX12, m_window, 256, 256);
        ASSERT_NE(m_bundle.renderer, nullptr);
        m_bundle.renderer->SetRenderWhenOccluded(true);
        m_resources = std::make_unique<renderer::ResourceManager>(*m_bundle.renderer);
        m_root = std::filesystem::path(FBZZ_GRAPHICS_SHADER_ROOT).parent_path().parent_path();
        ASSERT_TRUE(m_temp.IsValid());
        m_project = m_temp.Path();
        std::error_code error;
        /// @note Copy actual material/texture bytes and their GUIDs into a unique project; Engine shader metadata remains read-only.
        for (const char* relative : {"Assets/Materials/Water/WaterBubbles.mat", "Assets/Materials/Water/WaterBubbles.mat.meta",
                                    "Assets/Materials/Water/Lake.mat", "Assets/Materials/Water/Lake.mat.meta",
                                    "Assets/Textures/Particles/light_02.png", "Assets/Textures/Particles/light_02.png.meta",
                                    "Assets/Textures/Terrain/Grass/Grass_Color.png", "Assets/Textures/Terrain/Grass/Grass_Color.png.meta"}) {
            const auto destination = m_project / relative;
            std::filesystem::create_directories(destination.parent_path(), error);
            ASSERT_FALSE(error);
            ASSERT_TRUE(std::filesystem::copy_file(m_root / relative, destination, error));
            ASSERT_FALSE(error);
        }
        const auto projectAssets = (m_project / "Assets").generic_string() + "/";
        asset::AssetManager::Init(*m_resources, projectAssets);
        /// @note Explicitly index the source Engine shaders because standalone test runtime discovery need not find the source Assets root.
        asset::AssetDatabase::Init(projectAssets, (m_root / "Assets").generic_string());
        const auto texture = asset::AssetManager::ResolveAssetPath("guid:5a3f2a1eacdc4fd8add33d31f66254cb");
        ASSERT_TRUE(std::filesystem::equivalent(m_project / "Assets/Textures/Particles/light_02.png",
            util::FileSystem::PathFromUtf8(texture), error));
        ASSERT_FALSE(error);
        ASSERT_TRUE(m_resources->LoadTexture(texture).IsValid());
        const auto particleShader = asset::AssetManager::ResolveAssetPath("guid:3d413c28f00c4f6e95937aabdbf71ee5");
        ASSERT_TRUE(std::filesystem::equivalent(m_root / "Assets/Shaders/Material/Effects/Particle.hlsl",
            util::FileSystem::PathFromUtf8(particleShader), error));
        ASSERT_FALSE(error);
        const auto waterShader = asset::AssetManager::ResolveAssetPath("guid:c496f8e37c024b17aac1fce28730e300");
        ASSERT_TRUE(std::filesystem::equivalent(m_root / "Assets/Shaders/Water/Water.hlsl",
            util::FileSystem::PathFromUtf8(waterShader), error));
        ASSERT_FALSE(error);
        m_target = m_resources->CreateRenderTarget(256, 256, renderer::CameraDepthTargetDesc(1));
        ASSERT_TRUE(m_target.IsValid());
    }
    void TearDown() override
    {
        if (m_frameOpen) m_bundle.renderer->EndFrame();
        asset::AssetManager::UnloadAll();
        if (m_resources) m_resources->Reset();
        m_resources.reset();
        m_bundle.imguiRenderer.reset();
        if (m_bundle.renderer) m_bundle.renderer->Shutdown();
        m_bundle.renderer.reset();
        if (m_window) DestroyWindow(m_window);
        if (SUCCEEDED(m_com)) CoUninitialize();
        Fixture::TearDown();
    }
    std::vector<float> Capture(const asset::MaterialAssetAuthoring& material, const char* name,
        std::optional<editor::matpreview::FiberMode> fiberMode = {})
    {
        namespace preview = editor::matpreview;
        preview::GpuData gpu;
        if (!preview::BuildGpuData(gpu, material, *m_resources, m_project.generic_string())) {
            ADD_FAILURE() << "Cannot build preview for " << name;
            return {};
        }
        m_resources->AdvanceFrame();
        m_bundle.renderer->BeginFrame();
        m_frameOpen = true;
        preview::RenderDesc desc;
        desc.target = m_target;
        desc.flavor = preview::DetectFlavor(material);
        if (desc.flavor == preview::Flavor::Fiber) desc.fiberMode = fiberMode.value_or(preview::DefaultFiberMode(material));
        desc.mesh = preview::ShapeMesh(*m_resources, preview::ThumbnailShape(material, desc.flavor), desc.flavor);
        desc.material = std::make_shared<const asset::MaterialAssetAuthoring>(material);
        desc.gpu = &gpu;
        desc.time = 2.0f;
        EXPECT_TRUE(preview::Render(*m_bundle.renderer, *m_resources, desc));
        m_bundle.renderer->EndFrame();
        m_frameOpen = false;
        std::vector<float> pixels;
        uint32_t width = 0, height = 0;
        EXPECT_TRUE(m_bundle.renderer->CaptureRenderTargetToLinearRGBA(m_target, *m_resources, pixels, width, height));
        EXPECT_EQ(width, 256u);
        EXPECT_EQ(height, 256u);
        std::vector<uint8_t> png;
        if (m_bundle.renderer->CaptureRenderTargetToPng(m_target, *m_resources, png, width, height)) {
            const auto path = m_root / "Scratch/MaterialPreviewGpu" / (std::string(name) + ".png");
            std::error_code error;
            std::filesystem::create_directories(path.parent_path(), error);
            EXPECT_FALSE(error);
            EXPECT_TRUE(util::FileSystem::WriteBinary(path, png.data(), png.size()));
        }
        preview::ResetGpuData(gpu, m_resources.get());
        return pixels;
    }
    renderer::RendererBundle m_bundle;
    std::unique_ptr<renderer::ResourceManager> m_resources;
    renderer::ResourceHandle<renderer::RenderTargetTag> m_target;
    testkit::TempDir m_temp{"material_preview_shader"};
    std::filesystem::path m_root, m_project;
    HWND m_window = nullptr;
    HRESULT m_com = E_FAIL;
    bool m_frameOpen = false;
};

TEST_F(MaterialPreviewShaderTest, TerrainPaintLibraryRendersItsMeshAndRefreshesAfterMaterialReload)
{
    struct ImGuiScope {
        ImGuiContext* previous = ImGui::GetCurrentContext();
        ImGuiContext* current = ImGui::CreateContext();
        ~ImGuiScope() { ImGui::DestroyContext(current); ImGui::SetCurrentContext(previous); }
    } imgui;
    class PreviewIds final : public renderer::IImGuiRenderer {
    public:
        renderer::ResourceHandle<renderer::RenderTargetTag> target;
        void ImGuiInit(void*) override {}
        void ImGuiShutdown() override {}
        void ImGuiNewFrame() override {}
        void ImGuiRenderDrawData() override {}
        void ImGuiRenderPlatformWindows() override {}
        void* GetImTextureID(renderer::ResourceHandle<renderer::RenderTargetTag> value, renderer::ResourceManager&, int) override
        { target = value; return this; }
        void* GetImTextureID(renderer::ResourceHandle<renderer::TextureTag>, renderer::ResourceManager&) override { return this; }
    } ids;
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.DisplaySize = {900, 700}; io.DeltaTime = 1.0f / 60.0f;
    unsigned char* font = nullptr; int fontWidth = 0, fontHeight = 0;
    io.Fonts->GetTexDataAsRGBA32(&font, &fontWidth, &fontHeight);
    asset::MaterialAssetAuthoring material;
    ASSERT_TRUE(asset::LoadMaterialAssetFromFile((m_root / "Assets/Materials/Samples/SurfaceExamples/BaseSurface.mat").generic_string(), material));
    material.params["albedo"] = {0.8f, 0.04f, 0.02f, 1.0f};
    const auto materialPath = (m_project / "Assets/PaintPreview.mat").generic_string();
    ASSERT_TRUE(asset::SaveMaterialAssetToFile(materialPath, material));
    const auto materialRef = "guid:" + asset::AssetDatabase::GuidFromPath(materialPath);
    asset::TerrainPaintAsset paint; paint.name = "Preview Cone";
    const std::array<asset::InstanceMeshPart, 1> parts{{{"primitive:cone", materialRef, 0}}};
    const std::array<float, 1> heights{0.0f};
    ASSERT_TRUE(paint.definition.Replace(parts, heights, {}));
    const auto paintPath = (m_project / "Assets/Cone.terrainpaint").generic_string();
    ASSERT_TRUE(asset::SaveTerrainPaintAsset(paintPath, paint));
    ASSERT_FALSE(asset::AssetDatabase::GuidFromPath(paintPath).empty());
    editor::EditorContext context;
    context.projectRoot = m_project.generic_string(); context.renderer = m_bundle.renderer.get();
    context.resources = m_resources.get(); context.imguiRenderer = &ids;
    editor::TerrainPaintLibrary library;
    const auto capture = [&](const char* name) {
        for (int frame = 0; frame < 3; ++frame) {
            m_resources->AdvanceFrame(); m_bundle.renderer->BeginFrame(); m_frameOpen = true;
            ImGui::NewFrame(); ImGui::SetNextWindowSize({900, 700});
            ImGui::Begin("Terrain Paint GPU Test", nullptr, ImGuiWindowFlags_NoSavedSettings);
            std::vector<std::string> selected;
            (void)library.Draw(context, {}, selected);
            ImGui::End(); ImGui::Render();
            m_bundle.renderer->EndFrame(); m_frameOpen = false;
        }
        std::vector<float> pixels;
        if (!ids.target.IsValid()) { ADD_FAILURE() << "No Paint thumbnail was rendered"; return pixels; }
        uint32_t width = 0, height = 0;
        EXPECT_TRUE(m_bundle.renderer->CaptureRenderTargetToLinearRGBA(ids.target, *m_resources, pixels, width, height));
        EXPECT_EQ(width, 128u); EXPECT_EQ(height, 128u);
        std::vector<uint8_t> png;
        if (m_bundle.renderer->CaptureRenderTargetToPng(ids.target, *m_resources, png, width, height)) {
            const auto path = m_root / "Scratch/TerrainPaintLibrary" / (std::string(name) + ".png");
            EXPECT_TRUE(util::FileSystem::EnsureParentDirectory(path));
            EXPECT_TRUE(util::FileSystem::WriteBinary(path, png.data(), png.size()));
        }
        return pixels;
    };
    const auto red = capture("Red"); ASSERT_FALSE(red.empty());
    material.params["albedo"] = {0.02f, 0.04f, 0.8f, 1.0f};
    ASSERT_TRUE(asset::SaveMaterialAssetToFile(materialPath, material));
    ASSERT_GT(asset::AssetManager::ReloadPath(materialPath), 0);
    const auto blue = capture("Blue"); ASSERT_EQ(blue.size(), red.size());
    double difference = 0.0;
    for (size_t index = 0; index < red.size(); ++index) difference += std::abs(red[index] - blue[index]);
    EXPECT_GT(difference, 1.0);
}

TEST_F(MaterialPreviewShaderTest, PaintMixThumbnailsReflectWeightsAndViewportPickingSelectsOneInstance)
{
    struct ImGuiScope{ImGuiContext* previous=ImGui::GetCurrentContext();ImGuiContext* current=ImGui::CreateContext();~ImGuiScope(){ImGui::DestroyContext(current);ImGui::SetCurrentContext(previous);}} imgui;
    class PreviewIds final:public renderer::IImGuiRenderer {
    public: renderer::ResourceHandle<renderer::RenderTargetTag> target;
        void ImGuiInit(void*)override{}void ImGuiShutdown()override{}void ImGuiNewFrame()override{}void ImGuiRenderDrawData()override{}void ImGuiRenderPlatformWindows()override{}
        void* GetImTextureID(renderer::ResourceHandle<renderer::RenderTargetTag> value,renderer::ResourceManager&,int)override{target=value;return this;}
        void* GetImTextureID(renderer::ResourceHandle<renderer::TextureTag>,renderer::ResourceManager&)override{return this;}
    } ids;
    auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.DisplaySize={900,900};io.DeltaTime=1.0f/60;io.ConfigInputTrickleEventQueue=false;
    unsigned char* font=nullptr;int width=0,height=0;io.Fonts->GetTexDataAsRGBA32(&font,&width,&height);
    std::vector<std::string> references;asset::TerrainPaintAsset cube;
    for(int i=0;i<2;++i){
        asset::MaterialAssetAuthoring material;ASSERT_TRUE(asset::LoadMaterialAssetFromFile((m_root/"Assets/Materials/Samples/SurfaceExamples/BaseSurface.mat").generic_string(),material));
        material.params["albedo"]=i==0?std::vector<float>{0.8f,0.02f,0.02f,1}:std::vector<float>{0.02f,0.02f,0.8f,1};
        const auto mat=(m_project/"Assets"/("Mix"+std::to_string(i)+".mat")).generic_string();ASSERT_TRUE(asset::SaveMaterialAssetToFile(mat,material));
        asset::TerrainPaintAsset paint;paint.name=i==0?"Red cube":"Blue cone";paint.placement.spacing=1;paint.placement.minScale=paint.placement.maxScale=1;
        const std::array<asset::InstanceMeshPart,1> parts{{{i==0?"primitive:cube":"primitive:cone","guid:"+asset::AssetDatabase::GuidFromPath(mat),0}}};const std::array<float,1> heights{0};
        ASSERT_TRUE(paint.definition.Replace(parts,heights,{}));const auto file=(m_project/"Assets"/("Mix"+std::to_string(i)+".terrainpaint")).generic_string();
        ASSERT_TRUE(asset::SaveTerrainPaintAsset(file,paint));references.push_back("guid:"+asset::AssetDatabase::GuidFromPath(file));if(i==0)cube=paint;
    }
    editor::EditorContext context;context.projectRoot=m_project.generic_string();context.renderer=m_bundle.renderer.get();context.resources=m_resources.get();context.imguiRenderer=&ids;
    editor::TerrainPaintLibrary library;
    const auto capture=[&](std::array<float,2> weights,const char* name){
        for(int frame=0;frame<3;++frame){m_resources->AdvanceFrame();m_bundle.renderer->BeginFrame();m_frameOpen=true;ImGui::NewFrame();ImGui::Begin("Mix thumbnails");
            static_cast<void>(library.MixThumbnail(context,"#testMix",references,weights));ImGui::End();ImGui::Render();m_bundle.renderer->EndFrame();m_frameOpen=false;}
        std::vector<float> pixels;uint32_t width=0,height=0;
        EXPECT_TRUE(ids.target.IsValid());EXPECT_TRUE(m_bundle.renderer->CaptureRenderTargetToLinearRGBA(ids.target,*m_resources,pixels,width,height));
        std::vector<uint8_t> png;if(m_bundle.renderer->CaptureRenderTargetToPng(ids.target,*m_resources,png,width,height)){
            const auto path=m_root/"Scratch/TerrainPaintLibrary"/(std::string(name)+".png");EXPECT_TRUE(util::FileSystem::EnsureParentDirectory(path));EXPECT_TRUE(util::FileSystem::WriteBinary(path,png.data(),png.size()));}
        return pixels;
    };
    const auto red=capture({100,0},"MixRed"),blue=capture({0,100},"MixBlue");ASSERT_EQ(red.size(),blue.size());ASSERT_FALSE(red.empty());
    EXPECT_GT(RgbEnergy(red),1.0f);EXPECT_GT(RgbEnergy(blue),1.0f);
    double difference=0;for(size_t i=0;i<red.size();++i)difference+=std::abs(red[i]-blue[i]);EXPECT_GT(difference,1.0);
    m_resources->Reset();const auto afterReset=capture({0,100},"MixBlueAfterReset");ASSERT_EQ(afterReset.size(),blue.size());EXPECT_GT(RgbEnergy(afterReset),1.0f);
    double resetDifference=0;for(size_t i=0;i<blue.size();++i)resetDifference+=std::abs(blue[i]-afterReset[i]);EXPECT_LT(resetDifference,0.01);
    scene::Scene world;context.activeScene=&world;auto& ground=world.CreateGameObject("Ground");auto& terrain=ground.AddComponent<scene::TerrainComponent>();terrain.Resize(9,9);terrain.cellSize=2;terrain.InitFlat();
    asset::MeshInstancesAsset placements;placements.terrainPaintRef=references[0];
    const std::array<renderer::InstancePlacement,2> points{{{7,{10,0,4},{},{1,1,1},true},{93,{0,0,4},{},{1,1,1},true}}};
    ASSERT_TRUE(placements.instances.Replace(cube.definition.Parts(),cube.definition.LodHeights(),points));const auto file=(m_project/"Assets/Pick.meshinstances").generic_string();ASSERT_TRUE(asset::SaveMeshInstancesAsset(file,placements));
    auto& owner=world.CreateGameObject("Painted cubes");owner.AddComponent<scene::InstancedMeshComponent>().m_instanceSetRef.path=file;
    editor::TerrainTool tool;tool.SetPaintPalette({references[0]},false);ASSERT_TRUE(tool.SelectPaintInstance(context,owner.instanceId,7));
    renderer::Camera camera;camera.m_position={0,2,-8};camera.m_aspect=1;camera.LookAt({0,0,4});
    for(int frame=0;frame<3;++frame){
        io.AddMousePosEvent(128,128);io.AddMouseButtonEvent(ImGuiMouseButton_Left,frame==1);
        m_resources->AdvanceFrame();m_bundle.renderer->BeginFrame();m_frameOpen=true;ImGui::NewFrame();ImGuizmo::BeginFrame();ImGui::Begin("Scene");
        tool.Update(context,camera,1.0f/60,true,{0,0},{256,256});ImGui::End();ImGui::Render();m_bundle.renderer->EndFrame();m_frameOpen=false;
    }
    const auto preview=tool.PaintPreviewInputs(world);ASSERT_EQ(preview.size(),1u);ASSERT_EQ(preview[0].data->placements.size(),1u);EXPECT_EQ(preview[0].data->placements[0].id,93u);
    EXPECT_TRUE(preview[0].erase);EXPECT_TRUE(preview[0].highlight);EXPECT_EQ(owner.GetComponent<scene::InstancedMeshComponent>()->Snapshot()->placements.size(),2u);
    context.activeScene=nullptr;
}

TEST_F(MaterialPreviewShaderTest, BuiltinParticleMaterialPreservesTintLightingAndOutputGain)
{
    asset::MaterialAssetAuthoring particle;
    ASSERT_TRUE(asset::LoadMaterialAssetFromFile((m_project / "Assets/Materials/Water/WaterBubbles.mat").generic_string(), particle));
    ASSERT_TRUE(particle.shaderPath.empty());
    const auto lit = Capture(particle, "Bubbles");
    ASSERT_FALSE(lit.empty());
    EXPECT_GT(RgbEnergy(lit), 1.0f);
    const auto sourcePath = m_root / "Assets/Shaders/Material/Effects/Particle.hlsl";
    std::string sourceBefore;
    ASSERT_TRUE(util::FileSystem::ReadText(sourcePath, sourceBefore));
    ASSERT_TRUE(sourceBefore.starts_with("/// @file"));
    particle.shaderPath = "guid:3d413c28f00c4f6e95937aabdbf71ee5|Assets/Shaders/Material/Effects/Particle.hlsl";
    const auto explicitShader = Capture(particle, "BubblesExplicitShader");
    ASSERT_EQ(explicitShader.size(), lit.size());
    double difference = 0.0;
    for (size_t index = 0; index < lit.size(); ++index) difference += std::abs(explicitShader[index] - lit[index]);
    EXPECT_LT(difference, 0.01);
    std::string sourceAfter;
    ASSERT_TRUE(util::FileSystem::ReadText(sourcePath, sourceAfter));
    EXPECT_EQ(sourceAfter, sourceBefore);
    particle.particle.emissiveScale = 0.0f;
    const auto dark = Capture(particle, "BubblesZeroGain");
    ASSERT_EQ(dark.size(), lit.size());
    EXPECT_LT(RgbEnergy(dark), 0.001f);
}

TEST_F(MaterialPreviewShaderTest, WaterPreviewUsesTheAuthoredWavesInsteadOfFixedSmallWaves)
{
    asset::MaterialAssetAuthoring water;
    ASSERT_TRUE(asset::LoadMaterialAssetFromFile((m_project / "Assets/Materials/Water/Lake.mat").generic_string(), water));
    ASSERT_EQ(editor::matpreview::DetectFlavor(water), editor::matpreview::Flavor::Water);
    const auto waves = Capture(water, "WaterWaves");
    ASSERT_FALSE(waves.empty());
    EXPECT_GT(RgbEnergy(waves), 1.0f);
    for (int index = 0; index < 4; ++index) water.params["wave" + std::to_string(index) + "Amplitude"] = {0.0f};
    const auto flat = Capture(water, "WaterFlat");
    ASSERT_EQ(flat.size(), waves.size());
    double difference = 0.0;
    for (size_t index = 0; index < flat.size(); ++index) difference += std::abs(flat[index] - waves[index]);
    EXPECT_GT(difference, 1.0);
}
TEST_F(MaterialPreviewShaderTest, ProjectWaterAppearanceUsesReflectedTintAndCanonicalTextureAfterSaveReload)
{
    asset::MaterialAssetAuthoring water;
    water.shaderPath = (m_root / "Assets/Shaders/Water/Examples/WaterToonExample.hlsl").generic_string();
    water.params["shallowTint"] = {0.8f, 0.05f, 0.02f};
    water.params["deepTint"] = {0.8f, 0.05f, 0.02f};
    water.params["highlightTint"] = {0, 0, 0};
    water.params["textureStrength"] = {1};
    water.params["textureScale"] = {0.2f};
    water.params["depthBands"] = {4};
    water.params["depthRange"] = {5};
    water.params["showFoam"] = {0};
    water.params["lightBands"] = {3};
    water.textures["albedo"] = (m_root / "Assets/Textures/Water/Water_Color.png").generic_string();
    ASSERT_EQ(editor::matpreview::DetectFlavor(water), editor::matpreview::Flavor::Water);
    const auto red = Capture(water, "ProjectWaterRedTextured");
    ASSERT_FALSE(red.empty());
    water.params["shallowTint"] = {0.02f, 0.05f, 0.8f};
    water.params["deepTint"] = {0.02f, 0.05f, 0.8f};
    const auto path = (m_project / "Assets/ProjectOcean.mat").generic_string();
    ASSERT_TRUE(asset::SaveMaterialAssetToFile(path, water));
    asset::MaterialAssetAuthoring reloaded;
    ASSERT_TRUE(asset::LoadMaterialAssetFromFile(path, reloaded));
    EXPECT_EQ(reloaded.params.at("depthBands"), water.params.at("depthBands"));
    EXPECT_EQ(reloaded.textures.at("albedo"), water.textures.at("albedo"));
    const auto blue = Capture(reloaded, "ProjectWaterBlueTexturedReloaded");
    reloaded.params["textureStrength"] = {0};
    const auto plain = Capture(reloaded, "ProjectWaterBluePlain");
    ASSERT_EQ(red.size(), blue.size());
    ASSERT_EQ(plain.size(), blue.size());
    double tintDifference = 0, textureDifference = 0;
    for (size_t index = 0; index < blue.size(); ++index) {
        EXPECT_TRUE(std::isfinite(blue[index]));
        tintDifference += std::abs(red[index] - blue[index]);
        textureDifference += std::abs(plain[index] - blue[index]);
    }
    EXPECT_GT(tintDifference, 1.0);
    EXPECT_GT(textureDifference, 1.0);
}

TEST_F(MaterialPreviewShaderTest, UnsupportedWaterVertexPreviewUsesSharedGeometryAfterSaveReload)
{
    asset::MaterialAssetAuthoring water;
    water.shaderPath = (m_root / "Assets/Shaders/Water/Water.hlsl").generic_string();
    water.params["unsafeColor"] = {100.0f, 0.0f, 100.0f, 1.0f};
    water.textures["albedo"] = (m_root / "Assets/Textures/Water/Water_Color.png").generic_string();
    const auto baseline = Capture(water, "SharedWaterFallbackBaseline");
    ASSERT_FALSE(baseline.empty());
    ASSERT_GT(RgbEnergy(baseline), 1.0f);
    const auto directory = m_project / "Assets/Shaders";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    ASSERT_FALSE(error);
    const std::string source = R"hlsl(
cbuffer MaterialConstants : register(b2) { float4 unsafeColor; float4 unsafePadding[12]; };
struct Vertex { float3 position : POSITION; float2 uv : TEXCOORD0; float4 spline : TEXCOORD1; };
struct Pixel { float4 position : SV_Position; };
Pixel VSMain(Vertex v) { Pixel p; p.position = float4(v.position.x + 1000.0f, v.position.y + 1000.0f, 0.5f, 1.0f); return p; }
float4 PSMain(Pixel p) : SV_Target0 { return unsafeColor; }
)hlsl";
    struct Declaration { const char* name; const char* vertex; const char* surface; };
    const Declaration declarations[] = {
        {"CustomVertex", "custom", "water_v1"},
        {"WrongVertex", "standard_surface_v1", "water_v1"},
        {"WrongSurface", "water_surface_v1", "custom"}
    };
    for (size_t index = 0; index < std::size(declarations); ++index) {
        const auto& declaration = declarations[index];
        SCOPED_TRACE(declaration.name);
        const auto path = directory / (std::string(declaration.name) + ".hlsl");
        ASSERT_TRUE(util::FileSystem::WriteText(path, source));
        const std::string metadata = "[meta]\nguid = '413223aaaf6e46858df8926edeeffff" + std::to_string(index)
            + "'\n[shader]\nversion = 1\nvertex = '" + declaration.vertex + "'\nsurface = '"
            + declaration.surface + "'\nopacity = 'custom'\nvariants = 'none'\npreview = 'water'\n";
        ASSERT_TRUE(util::FileSystem::WriteText(std::filesystem::path(path.string() + ".meta"), metadata));
        water.shaderPath = path.generic_string();
        ASSERT_EQ(editor::matpreview::DetectFlavor(water), editor::matpreview::Flavor::Water);
        ASSERT_FALSE(asset::ResolveShaderCapabilities(water.shaderPath).SupportsWaterGeometry());
        const auto materialPath = (m_project / (std::string("Assets/") + declaration.name + ".mat")).generic_string();
        ASSERT_TRUE(asset::SaveMaterialAssetToFile(materialPath, water));
        asset::MaterialAssetAuthoring reloaded;
        ASSERT_TRUE(asset::LoadMaterialAssetFromFile(materialPath, reloaded));
        /// @note The material codec canonicalizes project shader paths; preserve the authored file identity, not its absolute spelling.
        const std::string expectedShaderReference = "Assets/Shaders/" + std::string(declaration.name) + ".hlsl";
        EXPECT_EQ(reloaded.shaderPath, expectedShaderReference);
        const std::string resolvedShaderPath = asset::AssetManager::ResolveAssetPath(reloaded.shaderPath);
        ASSERT_TRUE(std::filesystem::equivalent(util::FileSystem::PathFromUtf8(resolvedShaderPath), path, error));
        ASSERT_FALSE(error);
        EXPECT_FALSE(asset::ResolveShaderCapabilities(reloaded.shaderPath).SupportsWaterGeometry());
        EXPECT_EQ(reloaded.params.at("unsafeColor"), water.params.at("unsafeColor"));
        editor::matpreview::GpuData gpu;
        ASSERT_TRUE(editor::matpreview::BuildGpuData(gpu, reloaded, *m_resources, m_project.generic_string()));
        EXPECT_TRUE(m_resources->GetShaderCapabilities(gpu.shader).SupportsWaterGeometry());
        EXPECT_EQ(gpu.paramData.size(), 48u);
        ASSERT_GT(gpu.textures.size(), 0u);
        EXPECT_FALSE(gpu.textures[0].IsValid());
        editor::matpreview::ResetGpuData(gpu, m_resources.get());
        const auto fallback = Capture(reloaded, declaration.name);
        ASSERT_EQ(fallback.size(), baseline.size());
        double maximumDifference = 0.0;
        for (size_t pixel = 0; pixel < baseline.size(); ++pixel) {
            EXPECT_TRUE(std::isfinite(fallback[pixel]));
            maximumDifference = (std::max)(maximumDifference,
                static_cast<double>(std::abs(fallback[pixel] - baseline[pixel])));
        }
        EXPECT_LE(maximumDifference, 0.000001);
    }
}

TEST_F(MaterialPreviewShaderTest, ProjectFiberFamiliesCompileSharedGeometryAcrossEveryPass)
{
    for (const char* family : {"MeadowBlade", "StripeFur", "StripeFurFin"}) {
        SCOPED_TRACE(family);
        const bool blade = std::string_view(family) == "MeadowBlade";
        const bool fin = std::string_view(family) == "StripeFurFin";
        for (int skin = 0; skin < (blade ? 1 : 2); ++skin) {
            for (const char* suffix : {"", "GBuffer", "Velocity", "Shadow", "Selection"}) {
                const auto path = m_root / "Assets/Shaders/Examples/Fiber"
                    / (std::string(family) + (skin ? "Skinned" : "") + suffix + ".hlsl");
                const auto handle = m_resources->LoadShader(path.generic_string());
                const auto* shader = m_resources->Get(handle);
                ASSERT_NE(shader, nullptr) << path;
                const auto capability = m_resources->GetShaderCapabilities(handle);
                EXPECT_EQ(capability.vertex, blade ? renderer::ShaderVertexContract::FIBER_BLADE
                    : fin ? renderer::ShaderVertexContract::FIBER_FIN : renderer::ShaderVertexContract::FIBER_SHELL);
                const auto& descriptor = shader->GetDescriptor();
                EXPECT_GE(descriptor.cbufferSize, 176u);
                ASSERT_NE(descriptor.FindVar("fiberLength"), nullptr);
                EXPECT_EQ(descriptor.FindVar("fiberLength")->offset, 32u);
                ASSERT_NE(descriptor.FindVar("colorBands"), nullptr);
                EXPECT_EQ(descriptor.FindVar("colorBands")->varType, renderer::ShaderVarType::UInt);
                ASSERT_NE(descriptor.FindVar("enablePattern"), nullptr);
                EXPECT_EQ(descriptor.FindVar("enablePattern")->varType, renderer::ShaderVarType::Bool);
                EXPECT_TRUE(std::any_of(descriptor.textures.begin(), descriptor.textures.end(),
                    [](const auto& texture) { return texture.slot == 0 && texture.constantOffset == 160; }));
                EXPECT_TRUE(std::any_of(descriptor.textures.begin(), descriptor.textures.end(),
                    [](const auto& texture) { return texture.slot == 5 && texture.constantOffset == 112; }));
            }
        }
    }
}

TEST_F(MaterialPreviewShaderTest, ProjectGrassAndFurAppearanceEditReloadPreservesCoverage)
{
    for (const char* family : {"MeadowBlade", "StripeFur"}) {
        SCOPED_TRACE(family);
        asset::MaterialAssetAuthoring material;
        ASSERT_TRUE(asset::LoadMaterialAssetFromFile(
            (m_root / "Assets/Materials/Examples/Fiber" / (std::string(family) + ".mat")).generic_string(), material));
        material.shaderPath = (m_root / "Assets/Shaders/Examples/Fiber" / (std::string(family) + ".hlsl")).generic_string();
        const auto texturePath = m_project / "Assets/Textures/Terrain/Grass/Grass_Color.png";
        ASSERT_TRUE(std::filesystem::is_regular_file(texturePath));
        material.textures["albedo"] = texturePath.generic_string();
        material.params["patternStrength"] = {1.0f};
        material.params["accentColor"] = {1.0f, 0.04f, 0.02f, 1.0f};
        const std::string redName = std::string("Project") + family + "Red";
        const auto red = Capture(material, redName.c_str());
        ASSERT_FALSE(red.empty());
        material.params["accentColor"] = {0.02f, 0.04f, 1.0f, 1.0f};
        const auto path = (m_project / (std::string(family) + ".mat")).generic_string();
        ASSERT_TRUE(asset::SaveMaterialAssetToFile(path, material));
        asset::MaterialAssetAuthoring reloaded;
        ASSERT_TRUE(asset::LoadMaterialAssetFromFile(path, reloaded));
        /// @note Small integers use the existing float-exact on-disk normalization.
        ASSERT_TRUE(reloaded.params.contains("colorBands"));
        ASSERT_TRUE(reloaded.params.contains("enablePattern"));
        EXPECT_EQ(reloaded.params.at("colorBands"), material.params.at("colorBands"));
        EXPECT_EQ(reloaded.params.at("enablePattern"), material.params.at("enablePattern"));
        EXPECT_EQ(reloaded.textures.at("albedo"), "Assets/Textures/Terrain/Grass/Grass_Color.png");
        std::error_code textureError;
        EXPECT_TRUE(std::filesystem::equivalent(
            asset::AssetManager::ResolveAssetPath(reloaded.textures.at("albedo")), texturePath, textureError));
        EXPECT_FALSE(textureError);
        const std::string blueName = std::string("Project") + family + "BlueReloaded";
        const auto blue = Capture(reloaded, blueName.c_str());
        ASSERT_EQ(red.size(), blue.size());
        double difference = 0, coverageDifference = 0;
        for (size_t index = 0; index < blue.size(); ++index) {
            ASSERT_TRUE(std::isfinite(blue[index]));
            if (index % 4 == 3) coverageDifference += std::abs(red[index] - blue[index]);
            else difference += std::abs(red[index] - blue[index]);
        }
        EXPECT_GT(difference, 1.0);
        EXPECT_LT(coverageDifference, 0.001);
    }
}

TEST_F(MaterialPreviewShaderTest, ProjectHybridPreviewUsesShellAndFinAppearanceAfterMaterialSaveReload)
{
    namespace preview = editor::matpreview;
    asset::MaterialAssetAuthoring material;
    ASSERT_TRUE(asset::LoadMaterialAssetFromFile(
        (m_root / "Assets/Materials/Examples/Fiber/StripeFur.mat").generic_string(), material));
    material.shaderPath = (m_root / "Assets/Shaders/Examples/Fiber/StripeFur.hlsl").generic_string();
    material.params["fiberLength"] = {0.20f};
    material.params["finSilhouetteWidth"] = {0.8f};
    material.params["maxBend"] = {0.0f};
    material.params["patternStrength"] = {1.0f};
    material.params["accentColor"] = {1.0f, 0.04f, 0.02f, 1.0f};
    const auto red = Capture(material, "ProjectHybridRed", preview::FiberMode::Hybrid);
    const auto shells = Capture(material, "ProjectHybridShellOnly", preview::FiberMode::Shell);
    auto finMaterial = material;
    finMaterial.shaderPath = (m_root / "Assets/Shaders/Examples/Fiber/StripeFurFin.hlsl").generic_string();
    const auto fins = Capture(finMaterial, "ProjectHybridFinOnly", preview::FiberMode::Fin);
    material.params["accentColor"] = {0.02f, 0.04f, 1.0f, 1.0f};
    const auto path = (m_project / "HybridPreview.mat").generic_string();
    ASSERT_TRUE(asset::SaveMaterialAssetToFile(path, material));
    asset::MaterialAssetAuthoring reloaded;
    ASSERT_TRUE(asset::LoadMaterialAssetFromFile(path, reloaded));
    EXPECT_EQ(reloaded.shaderPath, material.shaderPath);
    EXPECT_EQ(reloaded.params.at("accentColor"), material.params.at("accentColor"));
    const auto blue = Capture(reloaded, "ProjectHybridBlueReloaded", preview::FiberMode::Hybrid);
    ASSERT_EQ(red.size(), blue.size());
    ASSERT_EQ(red.size(), shells.size());
    ASSERT_EQ(red.size(), fins.size());
    ASSERT_FALSE(red.empty());
    double colorDifference = 0, alphaDifference = 0, shellDifference = 0, finDifference = 0;
    for (size_t index = 0; index < red.size(); ++index) {
        ASSERT_TRUE(std::isfinite(red[index]));
        ASSERT_TRUE(std::isfinite(blue[index]));
        if (index % 4 == 3) alphaDifference += std::abs(red[index] - blue[index]);
        else colorDifference += std::abs(red[index] - blue[index]);
        shellDifference += std::abs(red[index] - shells[index]);
        finDifference += std::abs(red[index] - fins[index]);
    }
    EXPECT_GT(RgbEnergy(red), 1.0f);
    EXPECT_GT(colorDifference, 1.0);
    EXPECT_LT(alphaDifference, 0.001);
    EXPECT_GT(shellDifference, 0.01);
    EXPECT_GT(finDifference, 0.01);
}

TEST_F(MaterialPreviewShaderTest, IncompleteHybridPreviewFallsBackAsOneCompatibleFamily)
{
    namespace preview = editor::matpreview;
    const auto shaderRoot = m_project / "Assets/Shaders";
    std::error_code error;
    for (const char* directory : {"Common", "Platform", "Rendering", "Fiber"}) {
        std::filesystem::create_directories(shaderRoot / directory, error); ASSERT_FALSE(error);
        std::filesystem::copy(m_root / "Assets/Shaders" / directory, shaderRoot / directory,
            std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, error);
        ASSERT_FALSE(error);
    }
    const auto familyRoot = shaderRoot / "Examples/Fiber";
    std::filesystem::create_directories(familyRoot, error); ASSERT_FALSE(error);
    for (const char* include : {"AppearanceFields.hlsli", "StripeFurAppearance.hlsli"}) {
        ASSERT_TRUE(std::filesystem::copy_file(m_root / "Assets/Shaders/Examples/Fiber" / include,
            familyRoot / include, error)); ASSERT_FALSE(error);
    }
    for (int skin = 0; skin < 2; ++skin) {
        for (const char* suffix : {"", "GBuffer", "Velocity", "Shadow", "Selection"}) {
            const std::string ending = std::string(skin ? "Skinned" : "") + suffix + ".hlsl";
            for (const char* extra : {"", ".meta"}) {
                ASSERT_TRUE(std::filesystem::copy_file(m_root / "Assets/Shaders/Examples/Fiber" / ("StripeFur" + ending + extra),
                    familyRoot / ("IncompleteFur" + ending + extra), error)); ASSERT_FALSE(error);
            }
        }
    }
    asset::MaterialAssetAuthoring material;
    ASSERT_TRUE(asset::LoadMaterialAssetFromFile(
        (m_root / "Assets/Materials/Examples/Fiber/StripeFur.mat").generic_string(), material));
    material.shaderPath = (familyRoot / "IncompleteFur.hlsl").generic_string();
    const auto fallback = Capture(material, "ProjectHybridIncompleteFallback", preview::FiberMode::Hybrid);
    auto builtin = material;
    builtin.shaderPath = (m_root / "Assets/Shaders/Fiber/FiberShell.hlsl").generic_string();
    const auto expected = Capture(builtin, "ProjectHybridBuiltin", preview::FiberMode::Hybrid);
    ASSERT_EQ(fallback.size(), expected.size());
    ASSERT_FALSE(fallback.empty());
    double maximumError = 0;
    for (size_t index = 0; index < expected.size(); ++index) {
        ASSERT_TRUE(std::isfinite(fallback[index]));
        maximumError = std::max(maximumError, std::abs(double(fallback[index]) - expected[index]));
    }
    EXPECT_GT(RgbEnergy(fallback), 1.0f);
    EXPECT_LE(maximumError, 0.000001);
}

TEST_F(MaterialPreviewShaderTest, HybridPreviewUsesTheIndependentProjectFinAppearance)
{
    namespace preview = editor::matpreview;
    const auto shaderRoot = m_project / "Assets/Shaders";
    std::error_code error;
    for (const char* directory : {"Common", "Platform", "Rendering", "Fiber", "Examples/Fiber"}) {
        std::filesystem::create_directories(shaderRoot / directory, error); ASSERT_FALSE(error);
        std::filesystem::copy(m_root / "Assets/Shaders" / directory, shaderRoot / directory,
            std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, error);
        ASSERT_FALSE(error);
    }
    const auto familyRoot = shaderRoot / "Examples/Fiber";
    const auto appearancePath = familyRoot / "PreviewFinAppearance.hlsli";
    const std::string prefix =
        "/// @file PreviewFinAppearance.hlsli\n/// @brief Independent preview Fin color.\n"
        "/// @author Hasegawa Jin\n/// @date 2026-10-21\n";
    const auto finCode = [&](const char* rgb) {
        return prefix + "float3 FiberAppearanceAlbedo(FiberPixel p, FiberStrand s, float3 c) { return float3("
            + rgb + "); }\nfloat3 FiberAppearanceLighting(FiberPixel p, FiberStrand s, float3 c) { return float3("
            + rgb + "); }\n";
    };
    ASSERT_TRUE(util::FileSystem::WriteText(appearancePath, finCode("1.0f,0.02f,0.02f")));
    for (int skin = 0; skin < 2; ++skin) {
        for (const char* suffix : {"", "GBuffer", "Velocity", "Shadow", "Selection"}) {
            const auto wrapper = familyRoot / (std::string("StripeFurFin") + (skin ? "Skinned" : "") + suffix + ".hlsl");
            std::string source;
            ASSERT_TRUE(util::FileSystem::ReadText(wrapper, source));
            const std::string original = "#define FBZZ_FIBER_APPEARANCE \"Examples/Fiber/StripeFurAppearance.hlsli\"";
            const auto offset = source.find(original);
            ASSERT_NE(offset, std::string::npos);
            source.replace(offset, original.size(), "#define FBZZ_FIBER_APPEARANCE \"Examples/Fiber/PreviewFinAppearance.hlsli\"");
            ASSERT_TRUE(util::FileSystem::WriteText(wrapper, source));
        }
    }
    asset::MaterialAssetAuthoring material;
    ASSERT_TRUE(asset::LoadMaterialAssetFromFile(
        (m_root / "Assets/Materials/Examples/Fiber/StripeFur.mat").generic_string(), material));
    material.shaderPath = (familyRoot / "StripeFur.hlsl").generic_string();
    material.params["fiberLength"] = {0.20f};
    material.params["finSilhouetteWidth"] = {0.8f};
    material.params["maxBend"] = {0.0f};
    const auto shellBefore = Capture(material, "IndependentFinShellBefore", preview::FiberMode::Shell);
    const auto hybridRed = Capture(material, "IndependentFinHybridRed", preview::FiberMode::Hybrid);
    ASSERT_TRUE(util::FileSystem::WriteText(appearancePath, finCode("0.02f,0.02f,1.0f")));
    ASSERT_TRUE(m_resources->ReloadAllShaders());
    const auto hybridBlue = Capture(material, "IndependentFinHybridBlue", preview::FiberMode::Hybrid);
    const auto shellAfter = Capture(material, "IndependentFinShellAfter", preview::FiberMode::Shell);
    ASSERT_FALSE(hybridRed.empty());
    ASSERT_EQ(hybridRed.size(), hybridBlue.size());
    ASSERT_EQ(shellBefore.size(), shellAfter.size());
    double finDifference = 0, alphaDifference = 0, shellDifference = 0;
    for (size_t index = 0; index < hybridRed.size(); ++index) {
        ASSERT_TRUE(std::isfinite(hybridRed[index]));
        ASSERT_TRUE(std::isfinite(hybridBlue[index]));
        if (index % 4 == 3) alphaDifference += std::abs(hybridRed[index] - hybridBlue[index]);
        else finDifference += std::abs(hybridRed[index] - hybridBlue[index]);
        shellDifference += std::abs(shellBefore[index] - shellAfter[index]);
    }
    EXPECT_GT(finDifference, 1.0);
    EXPECT_LT(alphaDifference, 0.001);
    EXPECT_LT(shellDifference, 0.001);
}

/// @note Insert inside namespace fbzz::tests in MaterialPreviewShaderTests.cpp after the fixture definition.

TEST_F(MaterialPreviewShaderTest, SurfaceExampleFamiliesCompileWithSharedCapabilitiesAndTypedFields)
{
    const auto root = m_root / "Assets/Shaders/Samples/SurfaceExamples";
    const auto water = m_resources->LoadShader((root / "CustomWater.hlsl").generic_string());
    ASSERT_TRUE(water.IsValid());
    EXPECT_EQ(m_resources->GetShaderCapabilities(water).vertex, renderer::ShaderVertexContract::WATER_SURFACE);
    ASSERT_NE(m_resources->Get(water), nullptr);
    const auto waterDesc = m_resources->Get(water)->GetDescriptor();
    ASSERT_NE(waterDesc.FindVar("lightBands"), nullptr);
    ASSERT_NE(waterDesc.FindVar("showFoam"), nullptr);
    EXPECT_EQ(waterDesc.FindVar("lightBands")->varType, renderer::ShaderVarType::Int);
    EXPECT_EQ(waterDesc.FindVar("showFoam")->varType, renderer::ShaderVarType::Bool);
    for (const char* family : {"GrassBlade", "SoftFur", "SoftFurFin"}) {
        const bool blade = std::string_view(family) == "GrassBlade";
        const bool fin = std::string_view(family) == "SoftFurFin";
        for (int skin = 0; skin < (blade ? 1 : 2); ++skin) {
            for (const char* pass : {"", "GBuffer", "Velocity", "Shadow", "Selection"}) {
                const auto path = root / "Fiber" / (std::string(family) + (skin ? "Skinned" : "") + pass + ".hlsl");
                SCOPED_TRACE(path.generic_string());
                const auto handle = m_resources->LoadShader(path.generic_string());
                ASSERT_TRUE(handle.IsValid());
                ASSERT_NE(m_resources->Get(handle), nullptr);
                EXPECT_EQ(m_resources->GetShaderCapabilities(handle).vertex, blade
                    ? renderer::ShaderVertexContract::FIBER_BLADE : fin
                    ? renderer::ShaderVertexContract::FIBER_FIN : renderer::ShaderVertexContract::FIBER_SHELL);
                const auto desc = m_resources->Get(handle)->GetDescriptor();
                ASSERT_NE(desc.FindVar("fiberLength"), nullptr);
                EXPECT_EQ(desc.FindVar("fiberLength")->offset, 32u);
                ASSERT_NE(desc.FindVar("accentColor"), nullptr);
                EXPECT_EQ(desc.FindVar("accentColor")->offset, 128u);
                ASSERT_NE(desc.FindVar("colorBands"), nullptr);
                EXPECT_EQ(desc.FindVar("colorBands")->varType, renderer::ShaderVarType::UInt);
                ASSERT_NE(desc.FindVar("enablePattern"), nullptr);
                EXPECT_EQ(desc.FindVar("enablePattern")->varType, renderer::ShaderVarType::Bool);
            }
        }
    }
}

TEST_F(MaterialPreviewShaderTest, SurfaceExampleMaterialEditSaveReloadChangesColorAndRetainsCoverage)
{
    namespace preview = editor::matpreview;
    for (int example = 0; example < 4; ++example) {
        const bool water = example == 0;
        const char* name = water ? "CustomWater" : example == 1 ? "GrassBlade" : "SoftFur";
        SCOPED_TRACE(example);
        asset::MaterialAssetAuthoring material;
        ASSERT_TRUE(asset::LoadMaterialAssetFromFile(
            (m_root / "Assets/Materials/Samples/SurfaceExamples" / (std::string(name) + ".mat")).generic_string(), material));
        const auto originalShader = material.shaderPath;
        const auto originalTexture = material.textures.at("albedo");
        EXPECT_TRUE(std::filesystem::is_regular_file(asset::AssetManager::ResolveAssetPath(originalTexture)));
        EXPECT_EQ(preview::DetectFlavor(material), water ? preview::Flavor::Water : preview::Flavor::Fiber);
        if (water) {
            material.params["shallowTint"] = {0.8f, 0.04f, 0.02f};
            material.params["deepTint"] = {0.8f, 0.04f, 0.02f};
            material.params["highlightTint"] = {0, 0, 0};
            material.params["showFoam"] = {0};
            material.params["textureStrength"] = {0.3f};
        } else {
            material.params["accentColor"] = {1, 0.04f, 0.02f, 1};
            material.params["patternStrength"] = {1};
        }
        const auto mode = example == 3 ? preview::FiberMode::Hybrid : example == 2
            ? preview::FiberMode::Shell : preview::FiberMode::Blade;
        const std::string prefix = "SurfaceExamples" + std::to_string(example);
        const auto red = Capture(material, (prefix + "Red").c_str(), water ? std::nullopt : std::optional{mode});
        ASSERT_FALSE(red.empty());
        EXPECT_GT(RgbEnergy(red), 1.0f);
        if (water) {
            material.params["shallowTint"] = {0.02f, 0.04f, 0.8f};
            material.params["deepTint"] = {0.02f, 0.04f, 0.8f};
        } else material.params["accentColor"] = {0.02f, 0.04f, 1, 1};
        const auto savedPath = (m_project / (prefix + ".mat")).generic_string();
        ASSERT_TRUE(asset::SaveMaterialAssetToFile(savedPath, material));
        asset::MaterialAssetAuthoring loaded;
        ASSERT_TRUE(asset::LoadMaterialAssetFromFile(savedPath, loaded));
        EXPECT_EQ(loaded.shaderPath, originalShader);
        EXPECT_EQ(loaded.textures.at("albedo"), originalTexture);
        for (const char* field : water ? std::initializer_list<const char*>{"shallowTint", "lightBands", "showFoam"}
                                      : std::initializer_list<const char*>{"accentColor", "colorBands", "enablePattern"})
            EXPECT_EQ(loaded.params.at(field), material.params.at(field));
        const auto blue = Capture(loaded, (prefix + "BlueReloaded").c_str(), water ? std::nullopt : std::optional{mode});
        ASSERT_EQ(blue.size(), red.size());
        double rgbDifference = 0, alphaDifference = 0;
        for (size_t index = 0; index < red.size(); ++index) {
            ASSERT_TRUE(std::isfinite(red[index]));
            ASSERT_TRUE(std::isfinite(blue[index]));
            if (index % 4 == 3) alphaDifference += std::abs(red[index] - blue[index]);
            else rgbDifference += std::abs(red[index] - blue[index]);
        }
        EXPECT_GT(rgbDifference, 1.0);
        EXPECT_LT(alphaDifference, 0.001);
    }
}

TEST_F(MaterialPreviewShaderTest, SurfaceExampleSceneSaveReloadRetainsSharedGeometrySettings)
{
    auto source = scene::SceneSerializer::LoadData((m_root / "Assets/Samples/SurfaceExamples/SurfaceExamples.scene").generic_string());
    ASSERT_NE(source, nullptr);
    const auto baseMaterial = asset::DecodeGuidRef(
        "guid:25b2f0a66c604b23925727eb9031d8fa|Assets/Materials/Samples/SurfaceExamples/BaseSurface.mat");
    const auto baseShader = asset::DecodeGuidRef(
        "guid:73d100e5a351425aaf52953c23425438|Assets/Shaders/Material/Surface/PBR.hlsl");
    EXPECT_FALSE(baseMaterial.starts_with("guid:"));
    EXPECT_FALSE(baseShader.starts_with("guid:"));
    const auto checkBaseMaterials = [&](scene::Scene& sample) {
        const auto meshes = sample.GetEntities<scene::MeshRenderer>();
        ASSERT_EQ(meshes.size(), 3u);
        for (const auto entity : meshes) {
            const auto* material = sample.GetComponent<scene::MaterialComponent>(entity);
            ASSERT_NE(material, nullptr);
            EXPECT_TRUE(material->enabled);
            EXPECT_EQ(material->GetMaterialPath(), baseMaterial);
            EXPECT_TRUE(std::filesystem::is_regular_file(asset::AssetManager::ResolveAssetPath(material->GetMaterialPath())));
            const auto handle = material->GetMaterialAsset();
            ASSERT_TRUE(handle.IsValid());
            const auto* materialAsset = asset::AssetManager::Get<asset::MaterialAsset>(handle);
            ASSERT_NE(materialAsset, nullptr);
            EXPECT_EQ(materialAsset->Authoring().shaderPath, baseShader);
            EXPECT_TRUE(std::filesystem::is_regular_file(asset::AssetManager::ResolveAssetPath(materialAsset->Authoring().shaderPath)));
        }
    };
    checkBaseMaterials(*source);
    const auto waterEntities = source->GetEntities<scene::WaterComponent>();
    const auto fiberEntities = source->GetEntities<scene::FiberComponent>();
    ASSERT_EQ(waterEntities.size(), 1u);
    ASSERT_EQ(fiberEntities.size(), 2u);
    const auto* water = source->GetComponent<scene::WaterComponent>(waterEntities[0]);
    ASSERT_NE(water, nullptr);
    EXPECT_TRUE(water->adaptivePatchLod);
    EXPECT_TRUE(water->enableGerstnerWaves);
    EXPECT_EQ(water->lodMaxPatches, 16u);
    EXPECT_FLOAT_EQ(water->extentX, 2.5f);
    EXPECT_FALSE(water->interactionSettings.enabled);
    EXPECT_FALSE(water->interactionSettings.bubblesEnabled);
    EXPECT_FALSE(water->interactionSettings.sedimentEnabled);
    EXPECT_TRUE(water->interactionSettings.bubbleMaterialPath.empty());
    EXPECT_TRUE(water->interactionSettings.sedimentMaterialPath.empty());
    const auto savedPath = (m_project / "SurfaceExamplesReloaded.scene").generic_string();
    ASSERT_TRUE(scene::SceneSerializer::Save(*source, savedPath));
    const auto loaded = scene::SceneSerializer::LoadData(savedPath);
    ASSERT_NE(loaded, nullptr);
    checkBaseMaterials(*loaded);
    const auto reloadedWater = loaded->GetEntities<scene::WaterComponent>();
    ASSERT_EQ(reloadedWater.size(), 1u);
    const auto* nextWater = loaded->GetComponent<scene::WaterComponent>(reloadedWater[0]);
    ASSERT_NE(nextWater, nullptr);
    EXPECT_EQ(nextWater->materialPath, water->materialPath);
    EXPECT_EQ(nextWater->lodMaxPatches, water->lodMaxPatches);
    EXPECT_FLOAT_EQ(nextWater->extentX, water->extentX);
    EXPECT_EQ(nextWater->interactionSettings.enabled, water->interactionSettings.enabled);
    EXPECT_EQ(nextWater->interactionSettings.bubblesEnabled, water->interactionSettings.bubblesEnabled);
    EXPECT_EQ(nextWater->interactionSettings.sedimentEnabled, water->interactionSettings.sedimentEnabled);
    EXPECT_EQ(nextWater->interactionSettings.bubbleMaterialPath, water->interactionSettings.bubbleMaterialPath);
    EXPECT_EQ(nextWater->interactionSettings.sedimentMaterialPath, water->interactionSettings.sedimentMaterialPath);
    bool foundBlade = false, foundHybrid = false;
    const auto reloadedFiber = loaded->GetEntities<scene::FiberComponent>();
    ASSERT_EQ(reloadedFiber.size(), 2u);
    for (auto entity : reloadedFiber) {
        const auto* fiber = loaded->GetComponent<scene::FiberComponent>(entity);
        ASSERT_NE(fiber, nullptr);
        EXPECT_FALSE(fiber->m_materialPath.empty());
        if (fiber->m_mode == renderer::FiberRenderMode::BLADE) {
            foundBlade = true;
            EXPECT_TRUE(fiber->m_chunkedPlacement);
            EXPECT_EQ(fiber->m_maxBlades, 2000);
            EXPECT_EQ(fiber->m_bladeSegments, 3);
            EXPECT_EQ(fiber->m_shadowBladeSegments, 0);
        }
        if (fiber->m_mode == renderer::FiberRenderMode::HYBRID) {
            foundHybrid = true;
            EXPECT_EQ(fiber->m_shellCount, 12);
            EXPECT_EQ(fiber->m_shadowShellCount, 4);
        }
    }
    EXPECT_TRUE(foundBlade);
    EXPECT_TRUE(foundHybrid);
}

} /// @note namespace fbzz::tests
