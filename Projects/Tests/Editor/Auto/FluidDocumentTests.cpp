/// @file    FluidDocumentTests.cpp
/// @brief   Fluid Editor の文書 (FluidDocument) の Undo のまとめ方・保存・外からの書き換え・プレビューの表示切り替えを固定する。
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// この文書は «誰かの書き込みを黙って潰さない» ために置いた。Undo が 2 つに割れる・閉じた後の Undo が
/// どこにも書かれない・自分の Save を外からの変更と取り違える、のどれも画面ではすぐには気づけない。
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>

#include <Editor/EditorContext.hpp>
#include <Editor/Util/FluidDocument.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Asset/FluidRecipeCodec.hpp>
#include <Engine/Util/FileSystem.hpp>

#include <imgui.h>
#include <imgui_internal.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

namespace fbzz::tests {
namespace {

using editor::FluidDocument;
using editor::FluidSelectionKind;

/// ImGui の «操作中» を作る。Commit は ImGui::IsAnyItemActive() を見るが、テストではフレームを回せない。
/// ActiveId を直に立てれば «ドラッグ中» と同じ判定になる。
class ScopedImGuiContext {
public:
    ScopedImGuiContext()
        : m_previous(ImGui::GetCurrentContext())
        , m_context(ImGui::CreateContext())
    {
        ImGui::SetCurrentContext(m_context);
    }
    ~ScopedImGuiContext()
    {
        ImGui::DestroyContext(m_context);
        ImGui::SetCurrentContext(m_previous);
    }
    ScopedImGuiContext(const ScopedImGuiContext&) = delete;
    ScopedImGuiContext& operator=(const ScopedImGuiContext&) = delete;

    void SetItemActive(bool active) { m_context->ActiveId = active ? 1u : 0u; }

private:
    ImGuiContext* m_previous = nullptr;
    ImGuiContext* m_context = nullptr;
};

fluid::FluidRecipe ThreeSourcesTwoForces()
{
    fluid::FluidRecipe recipe = asset::MakeFluidPreset(asset::FluidPreset::Smoke);
    recipe.sources.resize(3);
    recipe.forces.resize(2);
    recipe.colliders.clear();
    for (auto& source : recipe.sources) source.enabled = true;
    for (auto& force : recipe.forces) force.enabled = true;
    recipe.gas.buoyancy = 1.5f;
    return recipe;
}

} // namespace

class FluidDocumentTest : public testkit::EditorFixture {
protected:
    void SetUp() override
    {
        EditorFixture::SetUp();
        Context().projectRoot = ProjectRoot().generic_string();
        Context().undoStack = &m_undo;
    }

    void TearDown() override
    {
        Context().undoStack = nullptr;
        EditorFixture::TearDown();
    }

    std::string WriteRecipe(const std::string& name, const fluid::FluidRecipe& recipe)
    {
        const std::string path = util::FileSystem::PathToUtf8(File(name));
        EXPECT_TRUE(asset::SaveFluidRecipe(path, recipe)) << path;
        return path;
    }

    static fluid::FluidRecipe LoadRecipe(const std::string& path)
    {
        fluid::FluidRecipe recipe;
        EXPECT_TRUE(asset::LoadFluidRecipe(path, recipe)) << path;
        return recipe;
    }

    /// 外の書き手 (AI・Baker) の保存を真似る。時刻の粒度は 10ms 前後あり、直後の書き込みは
    /// 同じ時刻に見えることがあるので、はっきり進めておく。
    static void WriteExternally(const std::string& path, const fluid::FluidRecipe& recipe)
    {
        ASSERT_TRUE(asset::SaveFluidRecipe(path, recipe));
        const std::filesystem::path file = util::FileSystem::PathFromUtf8(path);
        std::error_code error;
        const auto stamp = std::filesystem::last_write_time(file, error);
        ASSERT_FALSE(error);
        std::filesystem::last_write_time(file, stamp + std::chrono::seconds(5), error);
        ASSERT_FALSE(error);
    }

    static void SetBuoyancy(FluidDocument& doc, editor::EditorContext& ctx, float value)
    {
        ASSERT_TRUE(doc.Edit(ctx, "Set Buoyancy", [value](fluid::FluidRecipe& r) { r.gas.buoyancy = value; }));
    }

    editor::UndoStack m_undo;
};

TEST_F(FluidDocumentTest, OpenEditSaveRoundTrip)
{
    const std::string path = WriteRecipe("Smoke.fluid", ThreeSourcesTwoForces());

    FluidDocument doc;
    std::string error;
    ASSERT_TRUE(doc.Open(path, error)) << error;
    EXPECT_TRUE(doc.IsOpen());
    EXPECT_FALSE(doc.IsDirty());
    EXPECT_EQ(doc.Path(), path);
    EXPECT_EQ(doc.Recipe().sources.size(), 3u);
    EXPECT_FLOAT_EQ(doc.Recipe().gas.buoyancy, 1.5f);

    SetBuoyancy(doc, Context(), 3.5f);
    EXPECT_TRUE(doc.IsDirty());

    Context().requestAssetBrowserRefresh = false;
    ASSERT_TRUE(doc.Save(Context(), error)) << error;
    EXPECT_FALSE(doc.IsDirty());
    EXPECT_TRUE(Context().requestAssetBrowserRefresh);
    EXPECT_FLOAT_EQ(LoadRecipe(path).gas.buoyancy, 3.5f);
}

TEST_F(FluidDocumentTest, OpenFailsForMissingFile)
{
    FluidDocument doc;
    std::string error;
    EXPECT_FALSE(doc.Open(util::FileSystem::PathToUtf8(File("Missing.fluid")), error));
    EXPECT_FALSE(doc.IsOpen());
    EXPECT_FALSE(error.empty());
}

TEST_F(FluidDocumentTest, EditPushesOneUndoAndUndoRestores)
{
    const std::string path = WriteRecipe("Smoke.fluid", ThreeSourcesTwoForces());
    FluidDocument doc;
    std::string error;
    ASSERT_TRUE(doc.Open(path, error)) << error;

    const std::uint64_t revision = doc.Revision();
    SetBuoyancy(doc, Context(), 3.5f);
    EXPECT_GT(doc.Revision(), revision);
    EXPECT_EQ(m_undo.GetHistorySize(), 1u);
    EXPECT_EQ(m_undo.GetUndoDescription(), "Set Buoyancy");

    /// @note 何も変えない Edit は積まない (履歴が «何も起きない Undo» で埋まる)。
    EXPECT_FALSE(doc.Edit(Context(), "No-op", [](fluid::FluidRecipe&) {}));
    EXPECT_EQ(m_undo.GetHistorySize(), 1u);

    m_undo.Undo();
    EXPECT_FLOAT_EQ(doc.Recipe().gas.buoyancy, 1.5f);
    EXPECT_TRUE(doc.IsDirty());
    m_undo.Redo();
    EXPECT_FLOAT_EQ(doc.Recipe().gas.buoyancy, 3.5f);
}

/// 細かい値の差も «変わった» と見る (to_string の 6 桁で比べると丸まって積まれない)。
TEST_F(FluidDocumentTest, EditDetectsTinyFloatChanges)
{
    const std::string path = WriteRecipe("Smoke.fluid", ThreeSourcesTwoForces());
    FluidDocument doc;
    std::string error;
    ASSERT_TRUE(doc.Open(path, error)) << error;

    EXPECT_TRUE(doc.Edit(Context(), "Nudge", [](fluid::FluidRecipe& r) { r.gas.buoyancy += 1.0e-6f; }));
    EXPECT_EQ(m_undo.GetHistorySize(), 1u);
}

TEST_F(FluidDocumentTest, CommitGroupsChangesWhileAnItemIsActive)
{
    const std::string path = WriteRecipe("Smoke.fluid", ThreeSourcesTwoForces());
    FluidDocument doc;
    std::string error;
    ASSERT_TRUE(doc.Open(path, error)) << error;

    ScopedImGuiContext imgui;
    fluid::FluidRecipe working = doc.Recipe();

    /// @note ドラッグ中の 3 フレーム: 値は毎フレーム取り込むが、Undo はまだ積まない。
    imgui.SetItemActive(true);
    for (const float value : { 2.0f, 2.5f, 3.0f }) {
        working.gas.buoyancy = value;
        doc.Commit(Context(), "Drag Buoyancy", working, true);
        EXPECT_FLOAT_EQ(doc.Recipe().gas.buoyancy, value);
    }
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
    EXPECT_TRUE(doc.IsDirty());

    /// @note 押したままで値が来ないフレームも積まない。
    doc.Commit(Context(), "Drag Buoyancy", doc.Recipe(), false);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);

    /// @note 手を離したフレームで 1 つ積む。
    imgui.SetItemActive(false);
    doc.Commit(Context(), "Drag Buoyancy", doc.Recipe(), false);
    ASSERT_EQ(m_undo.GetHistorySize(), 1u);
    EXPECT_EQ(m_undo.GetUndoDescription(), "Drag Buoyancy");

    /// @note 操作中でない変更 (チェックボックス) はその場で 1 つ。
    working = doc.Recipe();
    working.gas.floor = !working.gas.floor;
    doc.Commit(Context(), "Toggle Floor", working, true);
    EXPECT_EQ(m_undo.GetHistorySize(), 2u);

    m_undo.Undo();
    m_undo.Undo();
    EXPECT_FLOAT_EQ(doc.Recipe().gas.buoyancy, 1.5f);
}

/// ドラッグして元の値へ戻して離しただけなら積まない。
TEST_F(FluidDocumentTest, CommitSkipsAGroupThatEndsWhereItStarted)
{
    const std::string path = WriteRecipe("Smoke.fluid", ThreeSourcesTwoForces());
    FluidDocument doc;
    std::string error;
    ASSERT_TRUE(doc.Open(path, error)) << error;

    ScopedImGuiContext imgui;
    fluid::FluidRecipe working = doc.Recipe();
    imgui.SetItemActive(true);
    working.gas.buoyancy = 4.0f;
    doc.Commit(Context(), "Drag", working, true);
    working.gas.buoyancy = 1.5f;
    doc.Commit(Context(), "Drag", working, true);
    imgui.SetItemActive(false);
    doc.Commit(Context(), "Drag", doc.Recipe(), false);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
}

TEST_F(FluidDocumentTest, InteractiveEditPushesOnceAtEnd)
{
    const std::string path = WriteRecipe("Smoke.fluid", ThreeSourcesTwoForces());
    FluidDocument doc;
    std::string error;
    ASSERT_TRUE(doc.Open(path, error)) << error;

    const math::Vector3 original = doc.Recipe().sources[0].center;
    doc.BeginInteractiveEdit();
    EXPECT_TRUE(doc.InInteractiveEdit());
    fluid::FluidRecipe working = doc.Recipe();
    for (const float x : { 0.1f, 0.2f, 0.3f }) {
        working.sources[0].center.x = x;
        doc.ApplyInteractive(working);
    }
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
    EXPECT_FLOAT_EQ(doc.Recipe().sources[0].center.x, 0.3f);

    doc.EndInteractiveEdit(Context(), "Move Source");
    EXPECT_FALSE(doc.InInteractiveEdit());
    ASSERT_EQ(m_undo.GetHistorySize(), 1u);

    m_undo.Undo();
    EXPECT_FLOAT_EQ(doc.Recipe().sources[0].center.x, original.x);
}

/// 閉じた後の Undo はファイルへ書く (どこにも反映されない Undo が一番まずい)。
TEST_F(FluidDocumentTest, UndoAfterCloseWritesTheFile)
{
    const std::string path = WriteRecipe("Smoke.fluid", ThreeSourcesTwoForces());
    {
        FluidDocument doc;
        std::string error;
        ASSERT_TRUE(doc.Open(path, error)) << error;
        SetBuoyancy(doc, Context(), 3.5f);
        ASSERT_TRUE(doc.Save(Context(), error)) << error;
        doc.Close();
        EXPECT_FALSE(doc.IsOpen());
    }
    ASSERT_FLOAT_EQ(LoadRecipe(path).gas.buoyancy, 3.5f);

    Context().requestAssetBrowserRefresh = false;
    m_undo.Undo();
    EXPECT_FLOAT_EQ(LoadRecipe(path).gas.buoyancy, 1.5f);
    EXPECT_TRUE(Context().requestAssetBrowserRefresh);

    m_undo.Redo();
    EXPECT_FLOAT_EQ(LoadRecipe(path).gas.buoyancy, 3.5f);
}

/// 開き直した文書へは Undo がメモリで戻る (ファイルを書き換えて手元と食い違わせない)。
TEST_F(FluidDocumentTest, UndoFindsAReopenedDocument)
{
    const std::string path = WriteRecipe("Smoke.fluid", ThreeSourcesTwoForces());
    FluidDocument first;
    std::string error;
    ASSERT_TRUE(first.Open(path, error)) << error;
    SetBuoyancy(first, Context(), 3.5f);
    ASSERT_TRUE(first.Save(Context(), error)) << error;
    first.Close();

    FluidDocument second;
    ASSERT_TRUE(second.Open(path, error)) << error;
    m_undo.Undo();
    EXPECT_FLOAT_EQ(second.Recipe().gas.buoyancy, 1.5f);
    EXPECT_TRUE(second.IsDirty());
    EXPECT_FLOAT_EQ(LoadRecipe(path).gas.buoyancy, 3.5f);
}

TEST_F(FluidDocumentTest, UndoKeepsTheSelectionInRange)
{
    const std::string path = WriteRecipe("Smoke.fluid", ThreeSourcesTwoForces());
    FluidDocument doc;
    std::string error;
    ASSERT_TRUE(doc.Open(path, error)) << error;

    ASSERT_TRUE(doc.Edit(Context(), "Add Source", [](fluid::FluidRecipe& r) { r.sources.emplace_back(); }));
    doc.selection = { FluidSelectionKind::Source, 3 };
    m_undo.Undo();
    ASSERT_EQ(doc.Recipe().sources.size(), 3u);
    EXPECT_EQ(doc.selection.kind, FluidSelectionKind::Source);
    EXPECT_EQ(doc.selection.index, 2);
}

TEST_F(FluidDocumentTest, ExternalChangeReloadsWhenClean)
{
    const std::string path = WriteRecipe("Smoke.fluid", ThreeSourcesTwoForces());
    FluidDocument doc;
    std::string error;
    ASSERT_TRUE(doc.Open(path, error)) << error;
    const std::uint64_t revision = doc.Revision();

    fluid::FluidRecipe external = ThreeSourcesTwoForces();
    external.gas.buoyancy = 4.0f;
    WriteExternally(path, external);

    doc.PollExternalChange();
    EXPECT_FALSE(doc.HasExternalConflict());
    EXPECT_FALSE(doc.IsDirty());
    EXPECT_FLOAT_EQ(doc.Recipe().gas.buoyancy, 4.0f);
    EXPECT_GT(doc.Revision(), revision);
}

TEST_F(FluidDocumentTest, ExternalChangeConflictsWhenDirty)
{
    const std::string path = WriteRecipe("Smoke.fluid", ThreeSourcesTwoForces());
    FluidDocument doc;
    std::string error;
    ASSERT_TRUE(doc.Open(path, error)) << error;
    SetBuoyancy(doc, Context(), 3.5f);

    fluid::FluidRecipe external = ThreeSourcesTwoForces();
    external.gas.buoyancy = 4.0f;
    WriteExternally(path, external);

    doc.PollExternalChange();
    ASSERT_TRUE(doc.HasExternalConflict());
    EXPECT_FLOAT_EQ(doc.Recipe().gas.buoyancy, 3.5f);

    /// @note 手元を残す: 衝突は下り、次の Save がディスクを上書きする。
    doc.KeepLocal();
    EXPECT_FALSE(doc.HasExternalConflict());
    EXPECT_TRUE(doc.IsDirty());
    doc.PollExternalChange();
    EXPECT_FALSE(doc.HasExternalConflict());
    ASSERT_TRUE(doc.Save(Context(), error)) << error;
    EXPECT_FLOAT_EQ(LoadRecipe(path).gas.buoyancy, 3.5f);

    /// @note もう一度外から書かれたら、今度はディスクで置き換える。
    SetBuoyancy(doc, Context(), 2.0f);
    external.gas.buoyancy = 5.0f;
    WriteExternally(path, external);
    doc.PollExternalChange();
    ASSERT_TRUE(doc.HasExternalConflict());
    doc.ReloadFromDisk();
    EXPECT_FALSE(doc.HasExternalConflict());
    EXPECT_FALSE(doc.IsDirty());
    EXPECT_FLOAT_EQ(doc.Recipe().gas.buoyancy, 5.0f);
}

/// 自分の Save は外からの変更ではない (読み直しで Revision が進むとプレビューが無駄に解き直される)。
TEST_F(FluidDocumentTest, OwnSaveIsNotAnExternalChange)
{
    const std::string path = WriteRecipe("Smoke.fluid", ThreeSourcesTwoForces());
    FluidDocument doc;
    std::string error;
    ASSERT_TRUE(doc.Open(path, error)) << error;
    SetBuoyancy(doc, Context(), 3.5f);
    ASSERT_TRUE(doc.Save(Context(), error)) << error;

    const std::uint64_t revision = doc.Revision();
    doc.PollExternalChange();
    EXPECT_FALSE(doc.HasExternalConflict());
    EXPECT_EQ(doc.Revision(), revision);
}

TEST_F(FluidDocumentTest, PreviewRecipeHonoursHideAndSolo)
{
    const std::string path = WriteRecipe("Smoke.fluid", ThreeSourcesTwoForces());
    FluidDocument doc;
    std::string error;
    ASSERT_TRUE(doc.Open(path, error)) << error;

    doc.SetHidden(FluidSelectionKind::Source, 1, true);
    EXPECT_TRUE(doc.IsHidden(FluidSelectionKind::Source, 1));
    EXPECT_FALSE(doc.IsHidden(FluidSelectionKind::Force, 1));
    fluid::FluidRecipe preview = doc.PreviewRecipe();
    EXPECT_TRUE(preview.sources[0].enabled);
    EXPECT_FALSE(preview.sources[1].enabled);
    EXPECT_TRUE(preview.sources[2].enabled);
    /// @note 表示の切り替えはレシピ本体に触らない (保存・焼きには効かない)。
    EXPECT_TRUE(doc.Recipe().sources[1].enabled);
    EXPECT_FALSE(doc.IsDirty());

    doc.ToggleSolo(FluidSelectionKind::Force, 0);
    EXPECT_TRUE(doc.IsSolo(FluidSelectionKind::Force, 0));
    preview = doc.PreviewRecipe();
    EXPECT_TRUE(preview.forces[0].enabled);
    EXPECT_FALSE(preview.forces[1].enabled);
    /// @note ソロは同じリストだけに効く。
    EXPECT_TRUE(preview.sources[0].enabled);
    EXPECT_FALSE(preview.sources[1].enabled);

    doc.ToggleSolo(FluidSelectionKind::Force, 0);
    EXPECT_FALSE(doc.IsSolo(FluidSelectionKind::Force, 0));
    EXPECT_TRUE(doc.PreviewRecipe().forces[1].enabled);

    doc.SetHidden(FluidSelectionKind::Source, 1, false);
    EXPECT_TRUE(doc.PreviewRecipe().sources[1].enabled);
}

/// 部品を消した後に残った隠し・ソロの記録で落ちない (消えた部品を指すソロは無視する)。
TEST_F(FluidDocumentTest, StaleHiddenAndSoloEntriesAreHarmless)
{
    const std::string path = WriteRecipe("Smoke.fluid", ThreeSourcesTwoForces());
    FluidDocument doc;
    std::string error;
    ASSERT_TRUE(doc.Open(path, error)) << error;

    doc.SetHidden(FluidSelectionKind::Source, 2, true);
    doc.ToggleSolo(FluidSelectionKind::Source, 2);
    ASSERT_TRUE(doc.Edit(Context(), "Remove Source", [](fluid::FluidRecipe& r) { r.sources.pop_back(); }));

    const fluid::FluidRecipe preview = doc.PreviewRecipe();
    ASSERT_EQ(preview.sources.size(), 2u);
    EXPECT_TRUE(preview.sources[0].enabled);
    EXPECT_TRUE(preview.sources[1].enabled);
}

} // namespace fbzz::tests
