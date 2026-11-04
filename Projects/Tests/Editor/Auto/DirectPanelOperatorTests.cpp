/// @file    DirectPanelOperatorTests.cpp
/// @brief   画面を開かない文書編集・ログ選択・時刻固定・グリッド操作を検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include <Engine/Util/FileSystem.hpp>
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Panels/FluidEditorPanel.hpp>
#include <Editor/Panels/ConsolePanel.hpp>
#include <Editor/Panels/VFXTimelinePanel.hpp>
#include <Editor/Panels/MapEditorPanel.hpp>
#include <Editor/Profiler/ProfilerHistory.hpp>
#include <Editor/Profiler/ProfilerOperators.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Op/OperatorGroups.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Scene/Components/LightProbeVolumeComponent.hpp>
#include <Engine/Asset/FluidRecipeCodec.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/VectorFieldFile.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Components/TerrainGridComponent.hpp>
#include <Engine/Scene/Components/VFXComponent.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
namespace fbzz::tests {
class DirectPanelOperatorTest : public testkit::EditorFixture {
protected:
    void SetUp() override {
        EditorFixture::SetUp(); Context().projectRoot = ProjectRoot().generic_string(); Context().undoStack = &m_undo; AttachScene();
        m_fluid.visible = false; m_vfx.visible = false; m_map.visible = false; m_console.visible = false;
        m_fluid.OnRegisterOperators(m_registry); m_vfx.OnRegisterOperators(m_registry); m_map.OnRegisterOperators(m_registry);
        m_console.OnRegisterOperators(m_registry); editor::RegisterProfilerOperators(m_registry); editor::RegisterComponentActionOperators(m_registry); editor::ResetProfilerHistory();
        asset::AssetDatabase::Init(File("Assets").generic_string());
    }
    void TearDown() override {
        editor::OpArgs args; args.Set("discard", true); static_cast<void>(Invoke("fluid.editor.close", args));
        m_registry.Clear(); m_fluid.OnShutdown(); editor::ResetProfilerHistory(); asset::AssetDatabase::Shutdown(); EditorFixture::TearDown();
    }
    editor::OpResult Invoke(const char* id, const editor::OpArgs& args = {}) { editor::OpContext context{Context(), m_undo}; return m_registry.Invoke(id, context, args); }
    editor::OpResult Patch(const char* id, editor::OpData settings) { editor::OpArgs args; args.Set("settings", std::move(settings)); return Invoke(id, args); }
    void OpenFluid() {
        ASSERT_TRUE(asset::SaveFluidRecipe(File("Assets/Test.fluid").generic_string(), asset::MakeFluidPreset(asset::FluidPreset::Smoke)));
        editor::OpArgs args; args.Set("path", std::string("Assets/Test.fluid")); ASSERT_TRUE(Invoke("fluid.editor.open", args).ok);
    }
    editor::ConsoleSink m_sink; editor::ConsolePanel m_console{m_sink}; editor::FluidEditorPanel m_fluid;
    editor::VFXTimelinePanel m_vfx; editor::MapEditorPanel m_map; editor::OperatorRegistry m_registry; editor::UndoStack m_undo;
};
TEST_F(DirectPanelOperatorTest, FluidEditsStayUnsavedAndUndoBeforeExplicitSave)
{
    OpenFluid(); auto patch = editor::OpData::MakeObject(); patch.Set("seed", editor::OpData(42));
    ASSERT_TRUE(Patch("fluid.editor.configure", patch).ok); EXPECT_TRUE(Invoke("fluid.editor.inspect").data.Find("dirty")->AsBool());
    fluid::FluidRecipe disk; ASSERT_TRUE(asset::LoadFluidRecipe(File("Assets/Test.fluid").generic_string(), disk)); EXPECT_NE(disk.seed, 42u);
    ASSERT_TRUE(m_undo.Undo()); EXPECT_NE(Invoke("fluid.editor.inspect").data.Find("recipe")->Find("seed")->AsInt(), 42);
    ASSERT_TRUE(m_undo.Redo()); ASSERT_TRUE(Invoke("fluid.editor.save").ok);
    ASSERT_TRUE(asset::LoadFluidRecipe(File("Assets/Test.fluid").generic_string(), disk)); EXPECT_EQ(disk.seed, 42u);
}
TEST_F(DirectPanelOperatorTest, FluidClearListsAndRejectUnknownFieldsAtomically)
{
    OpenFluid(); auto patch = editor::OpData::MakeObject(); patch.Set("source", editor::OpData::MakeArray());
    ASSERT_TRUE(Patch("fluid.editor.configure", patch).ok);
    EXPECT_TRUE(Invoke("fluid.editor.inspect").data.Find("recipe")->Find("source")->AsArray().empty());
    patch.Set("seed", editor::OpData(42)); patch.Set("unknown", editor::OpData(true)); EXPECT_FALSE(Patch("fluid.editor.configure", patch).ok);
    EXPECT_NE(Invoke("fluid.editor.inspect").data.Find("recipe")->Find("seed")->AsInt(), 42);
    EXPECT_FALSE(Invoke("fluid.editor.close").ok); editor::OpArgs discard; discard.Set("discard", true); EXPECT_TRUE(Invoke("fluid.editor.close", discard).ok);
}
TEST_F(DirectPanelOperatorTest, InvalidFluidControlDoesNotPartiallyChangePlayback)
{
    OpenFluid(); auto patch = editor::OpData::MakeObject(); patch.Set("playing", editor::OpData(true)); patch.Set("quality", editor::OpData(99));
    EXPECT_FALSE(Patch("fluid.editor.control", patch).ok); EXPECT_FALSE(Invoke("fluid.editor.inspect").data.Find("playback")->Find("playing")->AsBool());
}
TEST_F(DirectPanelOperatorTest, ConsoleSelectionUsesStableSequencesAndClearRemovesRows)
{
    m_sink.OnLog({core::LogLevel::INFO, "first"}); m_sink.OnLog({core::LogLevel::WARNING, "second"});
    const auto rows = Invoke("console.inspect"); ASSERT_TRUE(rows.ok); ASSERT_EQ(rows.data.Find("total")->AsInt(), 2);
    auto sequences = editor::OpData::MakeArray(); sequences.Push(*rows.data.Find("rows")->AsArray()[1].Find("sequence"));
    editor::OpArgs args; args.Set("sequences", sequences); const auto selected = Invoke("console.select", args); ASSERT_TRUE(selected.ok);
    EXPECT_NE(selected.data.Find("selectedText")->AsString().find("second"), std::string::npos);
    ASSERT_TRUE(Invoke("console.clear").ok); EXPECT_EQ(Invoke("console.inspect").data.Find("total")->AsInt(), 0);
    EXPECT_FALSE(Invoke("console.select", args).ok);
}
TEST_F(DirectPanelOperatorTest, HiddenVfxSeekIsHeldUntilReleased)
{
    auto& root = AttachedScene()->CreateGameObject("Effect"); root.AddComponent<scene::VFXComponent>();
    editor::OpArgs args; args.Set("node", root.instanceId); args.Set("action", std::string("seek")); args.Set("time", 0.75f);
    ASSERT_TRUE(Invoke("vfx.timeline.control", args).ok); m_vfx.OnUpdate(Context(), 0.1f);
    const auto* vfx = root.GetComponent<scene::VFXComponent>(); EXPECT_FLOAT_EQ(vfx->editorScrubTime, 0.75f); EXPECT_EQ(vfx->editorScrubFrame, Time::frameCount);
    args.Set("action", std::string("release")); ASSERT_TRUE(Invoke("vfx.timeline.control", args).ok); EXPECT_FLOAT_EQ(vfx->editorScrubTime, -1);
}
TEST_F(DirectPanelOperatorTest, GridResizePreservesCoordinatesAndRejectsOccupiedTruncation)
{
    ASSERT_TRUE(Invoke("map.grid.create").ok); auto patch = editor::OpData::MakeObject(); patch.Set("cellCountX", editor::OpData(2)); patch.Set("cellCountZ", editor::OpData(1));
    patch.Set("defaultColumns", editor::OpData(3)); patch.Set("defaultRows", editor::OpData(3)); ASSERT_TRUE(Patch("map.grid.configure", patch).ok);
    editor::OpArgs args; args.Set("action", std::string("create")); args.Set("x", 1); args.Set("z", 0); ASSERT_TRUE(Invoke("map.grid.cell", args).ok);
    auto* grid = AttachedScene()->GetComponent<scene::TerrainGridComponent>(AttachedScene()->GetEntities<scene::TerrainGridComponent>().front()); const auto terrain = grid->GetCell(1, 0);
    patch = editor::OpData::MakeObject(); patch.Set("cellCountX", editor::OpData(1)); EXPECT_FALSE(Patch("map.grid.configure", patch).ok); EXPECT_EQ(grid->GetCell(1, 0), terrain);
    patch.Set("cellCountX", editor::OpData(3)); ASSERT_TRUE(Patch("map.grid.configure", patch).ok); EXPECT_EQ(grid->GetCell(1, 0), terrain);
    args.Set("action", std::string("remove")); ASSERT_TRUE(Invoke("map.grid.cell", args).ok); EXPECT_FALSE(AttachedScene()->IsValid(grid->GetCell(1, 0))); EXPECT_TRUE(AttachedScene()->IsValid(terrain));
}
TEST_F(DirectPanelOperatorTest, ProfilerHistoryCaptureWithoutFramesDoesNotFreeze)
{
    editor::OpArgs args; args.Set("action", std::string("capture")); EXPECT_FALSE(Invoke("profiler.script.history_control", args).ok);
    EXPECT_FALSE(Invoke("profiler.script.history").data.Find("frozen")->AsBool());
    args.Set("action", std::string("freeze")); ASSERT_TRUE(Invoke("profiler.script.history_control", args).ok); EXPECT_TRUE(Invoke("profiler.script.history").data.Find("frozen")->AsBool());
    args.Set("action", std::string("resume")); ASSERT_TRUE(Invoke("profiler.script.history_control", args).ok); EXPECT_FALSE(Invoke("profiler.script.history").data.Find("frozen")->AsBool());
}
TEST_F(DirectPanelOperatorTest, ControllerCreationConnectsWithGuidAndUndoKeepsAsset)
{
    auto& go = AttachedScene()->CreateGameObject("Character"); go.AddComponent<scene::AnimatorComponent>();
    editor::OpArgs args; args.Set("node", go.instanceId); args.Set("path", std::string("Assets/Character.animcontroller"));
    const auto result = Invoke("component.animator.create_controller", args); ASSERT_TRUE(result.ok);
    EXPECT_TRUE(go.GetComponent<scene::AnimatorComponent>()->controllerPath.starts_with("guid:"));
    ASSERT_TRUE(m_undo.Undo()); EXPECT_TRUE(go.GetComponent<scene::AnimatorComponent>()->controllerPath.empty());
    EXPECT_TRUE(std::filesystem::exists(File("Assets/Character.animcontroller")));
    EXPECT_FALSE(Invoke("component.animator.create_controller", args).ok);
}
TEST_F(DirectPanelOperatorTest, LightProbeBakeWithoutRendererDoesNotQueue)
{
    auto& go = AttachedScene()->CreateGameObject("Probes"); auto& probes = go.AddComponent<scene::LightProbeVolumeComponent>();
    editor::OpArgs args; args.Set("node", go.instanceId); EXPECT_FALSE(Invoke("component.light_probe.bake", args).ok);
    EXPECT_FALSE(probes.bakeRequested);
}
TEST_F(DirectPanelOperatorTest, FluidSaveAsCopiesUnsavedChangesWithoutOverwritingSource)
{
    OpenFluid(); auto patch = editor::OpData::MakeObject(); patch.Set("seed", editor::OpData(42)); ASSERT_TRUE(Patch("fluid.editor.configure", patch).ok);
    editor::OpArgs args; args.Set("path", std::string("Assets/Copy.fluid")); ASSERT_TRUE(Invoke("fluid.editor.save_as", args).ok);
    fluid::FluidRecipe original, copy;
    ASSERT_TRUE(asset::LoadFluidRecipe(File("Assets/Test.fluid").generic_string(), original)); ASSERT_TRUE(asset::LoadFluidRecipe(File("Assets/Copy.fluid").generic_string(), copy));
    EXPECT_NE(original.seed, 42u); EXPECT_EQ(copy.seed, 42u); EXPECT_FALSE(Invoke("fluid.editor.inspect").data.Find("dirty")->AsBool());
    EXPECT_FALSE(Invoke("fluid.editor.save_as", args).ok);
}
TEST_F(DirectPanelOperatorTest, VectorFieldRebakeKeepsGuidAndRejectsOverwriteWithoutRequest)
{
    editor::OpArgs args; args.Set("path", std::string("Assets/Velocity.png")); args.Set("recipe", std::string("Sphere")); args.Set("resolution", 8);
    const auto result = Invoke("asset.vector_field.bake", args); ASSERT_TRUE(result.ok);
    const auto reference = result.data.Find("reference")->AsString(); EXPECT_FALSE(reference.empty());
    EXPECT_FALSE(Invoke("asset.vector_field.bake", args).ok);
    args.Set("overwrite", true); ASSERT_TRUE(Invoke("asset.vector_field.bake", args).ok);
    editor::OpArgs inspect; inspect.Set("path", std::string("Assets/Velocity.png"));
    const auto after = Invoke("asset.vector_field.inspect", inspect); ASSERT_TRUE(after.ok); EXPECT_EQ(after.data.Find("reference")->AsString(), reference);
    EXPECT_GT(after.data.Find("sampleCount")->AsInt64(), 0);
}
TEST_F(DirectPanelOperatorTest, HistoryJumpUsesDocumentUndoAndRejectsStaleClear)
{
    OpenFluid(); auto patch = editor::OpData::MakeObject(); patch.Set("seed", editor::OpData(42)); ASSERT_TRUE(Patch("fluid.editor.configure", patch).ok);
    const int revision = Invoke("edit.history.inspect").data.Find("revision")->AsInt();
    patch.Set("seed", editor::OpData(43)); ASSERT_TRUE(Patch("fluid.editor.configure", patch).ok);
    editor::OpArgs clear; clear.Set("action", std::string("clear")); clear.Set("revision", revision); EXPECT_FALSE(Invoke("edit.history.control", clear).ok);
    editor::OpArgs jump; jump.Set("action", std::string("jump")); jump.Set("index", 1); ASSERT_TRUE(Invoke("edit.history.control", jump).ok);
    EXPECT_EQ(Invoke("fluid.editor.inspect").data.Find("recipe")->Find("seed")->AsInt(), 42);
    editor::OpArgs validClear; validClear.Set("action", std::string("clear")); ASSERT_TRUE(Invoke("edit.history.control", validClear).ok);
    EXPECT_TRUE(Invoke("edit.history.inspect").data.Find("entries")->AsArray().empty());
}
TEST_F(DirectPanelOperatorTest, InvalidClientPermissionIsRejectedBeforeExternalConfiguration)
{
    editor::OpArgs args; args.Set("permission", std::string("administrator"));
    EXPECT_FALSE(Invoke("ai.setup.register_desktop", args).ok);
}
TEST_F(DirectPanelOperatorTest, ProceduralFlipbookAssignsGuidAndMaterialUndoKeepsOutput)
{
    ASSERT_TRUE(asset::SaveMaterialAssetToFile(File("Assets/Particle.mat").generic_string(), asset::MaterialAssetAuthoring{}));
    auto settings = editor::OpData::MakeObject(); settings.Set("preset", editor::OpData("Smoke")); settings.Set("frameSize", editor::OpData(32));
    settings.Set("columns", editor::OpData(2)); settings.Set("rows", editor::OpData(1)); settings.Set("outputDirectory", editor::OpData("Assets/Generated"));
    editor::OpArgs args; args.Set("settings", settings); args.Set("material", std::string("Assets/Particle.mat"));
    const auto generated = Invoke("asset.flipbook.generate", args); ASSERT_TRUE(generated.ok);
    asset::MaterialAssetAuthoring material; ASSERT_TRUE(asset::LoadMaterialAssetFromFile(File("Assets/Particle.mat").generic_string(), material));
    std::string savedMaterial;
    ASSERT_TRUE(util::FileSystem::ReadText(File("Assets/Particle.mat").generic_string(), savedMaterial));
    EXPECT_NE(savedMaterial.find(generated.data.Find("reference")->AsString()), std::string::npos);
    EXPECT_EQ(material.particle.flipbook.spriteColumns, 2);
    ASSERT_TRUE(m_undo.Undo()); ASSERT_TRUE(asset::LoadMaterialAssetFromFile(File("Assets/Particle.mat").generic_string(), material));
    EXPECT_TRUE(material.textures["albedo"].empty()); EXPECT_TRUE(std::filesystem::exists(generated.data.Find("path")->AsString()));
}
TEST_F(DirectPanelOperatorTest, SequenceScanOrdersNumbersAndAtlasRejectsSingleFrameBeforeWriting)
{
    fluid::VectorFieldAsset field; fluid::BakeVectorField(fluid::VectorFieldRecipe::Sphere, 8, {1, 1, 1}, 1, 1, field);
    ASSERT_TRUE(asset::SaveVectorField(File("Assets/Fx_2.png").generic_string(), field)); ASSERT_TRUE(asset::SaveVectorField(File("Assets/Fx_10.png").generic_string(), field));
    editor::OpArgs scan; scan.Set("firstFrame", std::string("Assets/Fx_2.png")); const auto found = Invoke("asset.flipbook.scan_sequence", scan); ASSERT_TRUE(found.ok);
    ASSERT_EQ(found.data.Find("frames")->AsArray().size(), 2u); EXPECT_TRUE(found.data.Find("frames")->AsArray()[0].AsString().ends_with("Fx_2.png"));
    auto settings = editor::OpData::MakeObject(); auto frames = editor::OpData::MakeArray(); frames.Push(editor::OpData("Assets/Fx_2.png"));
    settings.Set("frames", frames); settings.Set("output", editor::OpData("Assets/Atlas.png")); EXPECT_FALSE(Patch("asset.flipbook.bake_atlas", settings).ok); EXPECT_FALSE(std::filesystem::exists(File("Assets/Atlas.png")));
    settings.Set("frames", *found.data.Find("frames")); const auto baked = Patch("asset.flipbook.bake_atlas", settings); ASSERT_TRUE(baked.ok);
    EXPECT_EQ(baked.data.Find("frameCount")->AsInt(), 2); EXPECT_TRUE(std::filesystem::exists(File("Assets/Atlas.png")));
}
} /// @note namespace fbzz::tests
