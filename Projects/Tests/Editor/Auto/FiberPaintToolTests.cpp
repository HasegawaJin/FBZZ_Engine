/// @file    FiberPaintToolTests.cpp
/// @brief   草の一括追加、ストローク Undo とアセット保存の契約を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-11
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Op/OperatorGroups.hpp>
#include <Editor/Tools/FiberPaintTool.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Renderer/Camera.hpp>
#include <Engine/Scene/Components/FiberComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <imgui.h>

namespace fbzz::tests {
namespace {
/// @note CPU 専用のアセット・編集テスト。描画資源は作成しない。
class FiberCpuRenderer final : public renderer::IRenderer {
public:
    void Shutdown() override {}
    void BeginFrame() override {}
    void EndFrame() override {}
    void Clear(const math::Vector4&) override {}
    void Submit(const renderer::DrawCall&,renderer::ResourceManager&) override {}
    void Dispatch(const renderer::ComputeCall&,renderer::ResourceManager&) override {}
    void Resize(uint32_t,uint32_t) override {}
    uint32_t GetWidth() const override { return 1; }
    uint32_t GetHeight() const override { return 1; }
    void SetRenderTarget(renderer::ResourceHandle<renderer::RenderTargetTag>,renderer::ResourceManager&) override {}
    void ClearDepth() override {}
    void SetViewport(uint32_t,uint32_t,uint32_t,uint32_t) override {}
    std::unique_ptr<renderer::IShader> CreateNativeShader(const std::string&) override { return {}; }
    std::unique_ptr<renderer::IConstantBuffer> CreateNativeConstantBuffer(size_t) override { return {}; }
    std::unique_ptr<renderer::IBuffer> CreateNativeVertexBuffer(const void*,size_t,uint32_t) override { return {}; }
    std::unique_ptr<renderer::IBuffer> CreateNativeIndexBuffer(const void*,uint32_t) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTexture(const std::string&) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromData(const uint8_t*,uint32_t,uint32_t) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTexture3DFromData(const uint8_t*,uint32_t,uint32_t,uint32_t) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeTextureFromRenderTarget(renderer::IRenderTarget&,uint32_t,renderer::RenderTargetTextureKind) override { return {}; }
    std::unique_ptr<renderer::IPipelineState> CreateNativePipelineState(const renderer::PipelineStateDesc&) override { return {}; }
    std::unique_ptr<renderer::IRenderTarget> CreateNativeRenderTarget(uint32_t,uint32_t,const renderer::RenderTargetDesc&) override { return {}; }
    std::unique_ptr<renderer::ITexture> CreateNativeComputeTexture(uint32_t,uint32_t) override { return {}; }
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeStructuredBuffer(const void*,uint32_t,uint32_t) override { return {}; }
    std::unique_ptr<renderer::IStructuredBuffer> CreateNativeRWStructuredBuffer(const void*,uint32_t,uint32_t) override { return {}; }
};
} /// @note namespace
class FiberPaintToolTest : public testkit::EditorFixture {
protected:
    FiberCpuRenderer m_renderer;
    renderer::ResourceManager m_resources{m_renderer};
    renderer::Mesh m_mesh;
    editor::OperatorRegistry m_registry;
    editor::UndoStack m_undo;
    scene::EntityID m_id;
    renderer::Camera m_camera;
    editor::FiberPaintTool m_tool;
    ImGuiContext* m_imgui = nullptr;
    ImGuiContext* m_previous = nullptr;
    void SetUp() override
    {
        EditorFixture::SetUp();
        editor::AssetDirtyRegistry::DiscardAll(); editor::SynchronizeFiberPaintDrafts();
        Context().projectRoot = util::FileSystem::PathToUtf8(ProjectRoot());
        asset::AssetManager::Init(m_resources,util::FileSystem::PathToUtf8(ProjectRoot()/"Assets"));
        auto& scene = AttachScene();
        m_mesh.cpuVertices.resize(4);
        m_mesh.cpuVertices[0].position = {-2,0,-2}; m_mesh.cpuVertices[1].position = {2,0,-2};
        m_mesh.cpuVertices[2].position = {-2,0,2}; m_mesh.cpuVertices[3].position = {2,0,2};
        for (auto& vertex : m_mesh.cpuVertices) vertex.normal = {0,1,0};
        m_mesh.cpuIndices = {0,2,1,1,2,3}; m_mesh.ComputeBounds();
        auto& go = scene.CreateGameObject("Ground"); m_id = go.GetID();
        go.AddComponent<scene::MeshRenderer>().mesh = &m_mesh;
        Context().selectedEntities = {m_id}; Context().operators = &m_registry; Context().undoStack = &m_undo;
        editor::RegisterFiberOperators(m_registry);
    }
    void TearDown() override
    {
        if (m_imgui) { ImGui::DestroyContext(m_imgui); ImGui::SetCurrentContext(m_previous); }
        m_undo.Clear(); editor::AssetDirtyRegistry::DiscardAll(); editor::SynchronizeFiberPaintDrafts();
        asset::AssetManager::UnloadAll(); EditorFixture::TearDown();
    }
    scene::FiberComponent* Fiber() { return AttachedScene()->GetComponent<scene::FiberComponent>(m_id); }
    asset::FiberPaintAsset* Paint() { return asset::AssetManager::Get(asset::AssetManager::Load<asset::FiberPaintAsset>(Fiber()->m_paintPath)); }
    editor::OpResult Add(bool empty = true)
    {
        editor::OpArgs args; args.Set("painted",empty); return editor::InvokeOperator(Context(),"fiber.add",args);
    }
    editor::OpResult Stroke(float target = 1)
    {
        editor::OpArgs args; args.Set("from",math::Vector3{}); args.Set("to",math::Vector3{1,0,0});
        args.Set("radius",1.0f); args.Set("target",target); return editor::InvokeOperator(Context(),"fiber.stroke",args);
    }
    void BeginUi()
    {
        m_previous = ImGui::GetCurrentContext(); m_imgui = ImGui::CreateContext();
        auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.DisplaySize = {800,600}; io.DeltaTime = 1.0f/60;
        io.ConfigInputTrickleEventQueue = false;
        unsigned char* pixels = nullptr; int width = 0, height = 0; io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
        m_camera.m_position = {0,6,-6}; m_camera.LookAt({0,0,0});
        m_camera.m_projection = renderer::ProjectionMode::Orthographic; m_camera.m_orthoHeight = 6; m_camera.m_aspect = 800.0f/600;
        Context().editorCamera = &m_camera; Context().fiberPainting = true; m_tool.radius = 1;
    }
    void Frame(bool down, bool editable = true, bool escape = false)
    {
        auto& io = ImGui::GetIO(); io.AddMousePosEvent(400,280); io.AddMouseButtonEvent(ImGuiMouseButton_Left,down);
        io.AddKeyEvent(ImGuiKey_Escape,escape);
        ImGui::NewFrame(); ImGui::SetNextWindowPos({0,0}); ImGui::SetNextWindowSize({800,600});
        ImGui::Begin("Fiber test",nullptr,ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings);
        editor::DrawFiberPaintTool(Context(),m_tool,{0,0},{800,600},true,editable);
        ImGui::End(); ImGui::Render();
    }
};
TEST_F(FiberPaintToolTest, RegistersMeshesWithGuidPaintAndSupportsUndoRedo)
{
    ASSERT_TRUE(Add().ok); ASSERT_TRUE(Fiber()); ASSERT_TRUE(Paint());
    EXPECT_TRUE(Fiber()->m_chunkedPlacement); EXPECT_EQ(Fiber()->m_mode,scene::FiberRenderMode::BLADE);
    EXPECT_TRUE(asset::AssetDatabase::IsGuidRef(Fiber()->m_paintPath));
    EXPECT_FLOAT_EQ(Paint()->data.defaults.density,0);
    const auto path = Fiber()->m_paintPath;
    ASSERT_TRUE(m_undo.Undo()); EXPECT_FALSE(Fiber());
    ASSERT_TRUE(m_undo.Redo()); ASSERT_TRUE(Fiber()); EXPECT_EQ(Fiber()->m_paintPath,path);
}
TEST_F(FiberPaintToolTest, AddsChildMeshesAsOneCommand)
{
    auto& root = AttachedScene()->CreateGameObject("Model"); const auto rootId = root.GetID();
    AttachedScene()->GetGameObject(m_id)->SetParent(root);
    auto& other = AttachedScene()->CreateGameObject("Other"); const auto otherId = other.GetID();
    other.AddComponent<scene::MeshRenderer>().mesh = &m_mesh;
    other.SetParent(*AttachedScene()->GetGameObject(rootId));
    Context().selectedEntities = {rootId};
    ASSERT_TRUE(Add().ok); ASSERT_TRUE(Fiber()); EXPECT_TRUE(other.GetComponent<scene::FiberComponent>());
    EXPECT_EQ(m_undo.GetHistorySize(),1u);
    ASSERT_TRUE(m_undo.Undo()); EXPECT_FALSE(Fiber()); EXPECT_FALSE(AttachedScene()->GetComponent<scene::FiberComponent>(otherId));
}
TEST_F(FiberPaintToolTest, ScaledMeshesKeepUsefulWorldBrushResolutionAndChunkSize)
{
    auto* go = AttachedScene()->GetGameObject(m_id);
    go->transform.scale = {20,1,20}; go->transform.worldScale = {20,1,20};
    ASSERT_TRUE(Add().ok); ASSERT_TRUE(Paint());
    EXPECT_NEAR(Paint()->data.cellSize*20,0.5f,1.0e-6f);
    EXPECT_NEAR(Fiber()->m_chunkSize*20,4.0f,1.0e-6f);
    EXPECT_LT(Fiber()->m_bladeDensity,200);
    ASSERT_TRUE(Stroke().ok); EXPECT_FALSE(Paint()->data.cells.empty());
}
TEST_F(FiberPaintToolTest, StrokeIsOneUndoAndSavesActualPaintAcrossReload)
{
    ASSERT_TRUE(Add().ok); const auto before = Paint()->data;
    ASSERT_TRUE(Stroke().ok); const auto after = Paint()->data;
    EXPECT_FALSE(after == before); EXPECT_EQ(m_undo.GetHistorySize(),2u);
    const auto disk = editor::ToProjectAssetDiskPath(Context().projectRoot,Fiber()->m_paintPath);
    EXPECT_TRUE(editor::AssetDirtyRegistry::IsDirty(disk));
    ASSERT_TRUE(m_undo.Undo()); EXPECT_TRUE(Paint()->data == before);
    ASSERT_TRUE(m_undo.Redo()); EXPECT_TRUE(Paint()->data == after);
    ASSERT_EQ(editor::AssetDirtyRegistry::SaveAll(),0);
    EXPECT_FALSE(editor::AssetDirtyRegistry::IsDirty(disk));
    ASSERT_GT(asset::AssetManager::ReloadPath(disk),0); EXPECT_TRUE(Paint()->data == after);
}
TEST_F(FiberPaintToolTest, InvalidStrokeAndChangedTopologyDoNotMutateOrCreateHistory)
{
    ASSERT_TRUE(Add().ok); const auto before = Paint()->data;
    EXPECT_FALSE(Stroke(2).ok); EXPECT_TRUE(Paint()->data == before); EXPECT_EQ(m_undo.GetHistorySize(),1u);
    m_mesh.cpuVertices[0].position.x += 1;
    EXPECT_FALSE(Stroke().ok); EXPECT_TRUE(Paint()->data == before); EXPECT_EQ(m_undo.GetHistorySize(),1u);
}
TEST_F(FiberPaintToolTest, FillPreservesHeightAndClearIsReversible)
{
    ASSERT_TRUE(Add(false).ok);
    editor::OpArgs height; height.Set("mode",std::string("length")); height.Set("target",0.25f); height.Set("radius",2.0f);
    ASSERT_TRUE(editor::InvokeOperator(Context(),"fiber.stroke",height).ok);
    const auto before = Paint()->data;
    editor::OpArgs clear; clear.Set("density",0.0f);
    ASSERT_TRUE(editor::InvokeOperator(Context(),"fiber.fill",clear).ok);
    EXPECT_FLOAT_EQ(Paint()->data.defaults.density,0);
    for (const auto& [key,value] : Paint()->data.cells) {
        EXPECT_FLOAT_EQ(value.density,0); EXPECT_FLOAT_EQ(value.length,before.cells.at(key).length);
    }
    ASSERT_TRUE(m_undo.Undo()); EXPECT_TRUE(Paint()->data == before);
}
TEST_F(FiberPaintToolTest, DiscardRestoresDiskAndRejectsStaleUndo)
{
    ASSERT_TRUE(Add().ok); const auto saved = Paint()->data;
    ASSERT_TRUE(Stroke().ok);
    editor::AssetDirtyRegistry::DiscardAll(); editor::SynchronizeFiberPaintDrafts();
    EXPECT_TRUE(Paint()->data == saved); EXPECT_FALSE(m_undo.Undo());
}
TEST_F(FiberPaintToolTest, VisibleOnlyDoesNotPaintThroughAnOccludingMesh)
{
    ASSERT_TRUE(Add().ok); const auto before = Paint()->data;
    auto& blocker = AttachedScene()->CreateGameObject("Bridge");
    blocker.AddComponent<scene::MeshRenderer>().mesh = &m_mesh;
    blocker.transform.position = {0,1,0}; blocker.transform.worldPosition = {0,1,0};
    renderer::Camera camera; camera.m_position = {0,4,-4}; camera.LookAt({0,0,0}); Context().editorCamera = &camera;
    editor::OpArgs args; args.Set("radius",0.5f); args.Set("visible",true);
    ASSERT_TRUE(editor::InvokeOperator(Context(),"fiber.stroke",args).ok);
    EXPECT_TRUE(Paint()->data == before); EXPECT_EQ(m_undo.GetHistorySize(),1u);
}
TEST_F(FiberPaintToolTest, MouseStrokeCommitsOnceOnReleaseAndUndoRestoresIt)
{
    ASSERT_TRUE(Add().ok); const auto before = Paint()->data; BeginUi();
    Frame(true); ASSERT_TRUE(m_tool.dragging); EXPECT_TRUE(Paint()->data == before);
    Frame(true); EXPECT_EQ(m_undo.GetHistorySize(),1u);
    Frame(false); EXPECT_FALSE(m_tool.dragging); EXPECT_FALSE(Paint()->data == before);
    EXPECT_EQ(m_undo.GetHistorySize(),2u);
    ASSERT_TRUE(m_undo.Undo()); EXPECT_TRUE(Paint()->data == before);
}
TEST_F(FiberPaintToolTest, EscapeAndPlayInterruptionDiscardUncommittedStrokes)
{
    ASSERT_TRUE(Add().ok); const auto before = Paint()->data; BeginUi();
    Frame(true); ASSERT_TRUE(m_tool.dragging);
    Frame(false,true,true); EXPECT_FALSE(m_tool.dragging); EXPECT_TRUE(Paint()->data == before);
    Frame(false); Frame(true); ASSERT_TRUE(m_tool.dragging);
    Frame(false,false); EXPECT_FALSE(m_tool.dragging); EXPECT_FALSE(Context().fiberPainting);
    EXPECT_TRUE(Paint()->data == before); EXPECT_EQ(m_undo.GetHistorySize(),1u);
}
} /// @note namespace fbzz::tests
