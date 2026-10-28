/// @file    ManagedDataAssetAuthoringTests.cpp
/// @brief   実 Host の DataAsset create/edit/save と conflict/IO failure による checked file Undo を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-06
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Util/ManagedDataAssetEditor.hpp>
#include <Editor/Panels/InspectorPanel.hpp>
#include <Editor/Util/ManagedSettingsCodec.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Scene/Managed/ManagedScriptRuntime.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <toml++/toml.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <fstream>
#include <sstream>
#include <cstring>
#include <limits>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace fbzz::tests {
class ManagedDataAssetAuthoringTest : public testkit::EditorFixture {
protected:
    editor::OperatorRegistry m_registry;
    editor::UndoStack m_undo;
    void SetUp() override
    {
        EditorFixture::SetUp(); Context().projectRoot = util::FileSystem::PathToUtf8(ProjectRoot());
        ASSERT_TRUE(util::FileSystem::EnsureDirectory(ProjectRoot() / "Assets"));
        asset::AssetDatabase::Init(util::FileSystem::PathToUtf8(ProjectRoot() / "Assets"));
        Context().operators = &m_registry; Context().undoStack = &m_undo; editor::RegisterManagedDataAssetOperators(m_registry);
        auto& runtime = scene::ManagedScriptRuntime::Instance(); ASSERT_TRUE(runtime.Initialize(FBZZ_MANAGED_HOST_TEST_DIRECTORY));
        ASSERT_TRUE(runtime.Load(FBZZ_MANAGED_FIXTURE_TEST_ASSEMBLY, true)) << runtime.LastDiagnostic();
    }
    void TearDown() override
    {
        editor::CloseManagedDataAssetEditorDocuments(); m_undo.Clear(); m_registry.Clear();
        scene::ManagedScriptRuntime::Instance().Unload(nullptr); EXPECT_TRUE(scene::ManagedScriptRuntime::Instance().Shutdown());
        asset::AssetDatabase::Shutdown(); EditorFixture::TearDown();
    }
    editor::OpResult Invoke(const char* name, const editor::OpArgs& args = {})
    { editor::OpContext context{Context(), m_undo}; return m_registry.Invoke(name, context, args); }
    std::string Path(const char* name) { return util::FileSystem::PathToUtf8(ProjectRoot() / "Assets" / name); }
    editor::OpResult Preview(const std::string& path, bool create, int amount = 7, const char* type = "cs-internal:ManagedFixtures.PortableProfileDataAsset")
    {
        editor::OpData fields = editor::OpData::MakeObject(); fields.Set("amount", amount);
        editor::OpArgs args; args.Set("path", path); args.Set("fields", fields); if (create) args.Set("type", std::string(type));
        return Invoke(create ? "asset.data.create.preview" : "asset.data.configure.preview", args);
    }
    editor::OpResult CreateTextAsset(const std::string& path, const std::string& text)
    {
        editor::OpData fields = editor::OpData::MakeObject(); fields.Set("amount", 7); fields.Set("text", text);
        editor::OpArgs args; args.Set("path", path); args.Set("type", std::string("cs-internal:ManagedFixtures.PortableProfileDataAsset")); args.Set("fields", fields);
        return Invoke("asset.data.create.preview", args);
    }
    editor::OpResult Commit(const editor::OpResult& preview)
    { editor::OpArgs args; args.Set("plan", preview.data.Find("plan")->AsString()); return Invoke("asset.data.commit", args); }
    editor::OpData Document(const std::string& path)
    {
        std::string bytes; EXPECT_TRUE(util::FileSystem::ReadText(path, bytes)); auto data = toml::parse(bytes); EXPECT_TRUE(data);
        editor::OpData value; EXPECT_TRUE(editor::ReadManagedSettingsJson(data.table()["json"].value_or(std::string{}), value)); return value;
    }
    std::string Bytes(const std::string& path)
    {
        std::ifstream file(util::FileSystem::PathFromUtf8(path), std::ios::binary);
        EXPECT_TRUE(file.good());
        std::ostringstream output;
        output << file.rdbuf();
        return output.str();
    }
};
class ManagedDataAssetInspectorTest : public ManagedDataAssetAuthoringTest {
protected:
    void SetUp() override
    {
        ManagedDataAssetAuthoringTest::SetUp();
        editor::AssetDirtyRegistry::DiscardAll();
        m_previous = ImGui::GetCurrentContext();
        m_imgui = ImGui::CreateContext();
        ImGui::SetCurrentContext(m_imgui);
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = {1000, 800};
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }
    void TearDown() override
    {
        ManagedDataAssetAuthoringTest::TearDown();
        editor::AssetDirtyRegistry::DiscardAll();
        ImGui::DestroyContext(m_imgui);
        ImGui::SetCurrentContext(m_previous);
    }
    void Draw(const std::string& path, bool activateAmount = false)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({900, 700});
        ImGui::Begin("DataAsset Inspector Test");
        ImGuiID amountId = 0;
        if (activateAmount) {
            ImGui::PushID("/amount");
            ImGui::PushID("amount");
            amountId = ImGui::GetID("##primitive");
            GImGui->NavActivateId = amountId;
            GImGui->NavActivateFlags = ImGuiActivateFlags_PreferInput;
            ImGui::PopID();
            ImGui::PopID();
        }
        if (activateAmount) ImGui::LogToBuffer();
        EXPECT_TRUE(editor::DrawManagedDataAssetInspector(Context(), path));
        if (activateAmount) {
            EXPECT_EQ(GImGui->ActiveId, amountId) << GImGui->LogBuffer.c_str();
            ImGui::LogFinish();
        }
        ImGui::End();
        ImGui::EndFrame();
    }
    std::string DrawTextPage(const std::string& path, const char* action = nullptr, uint64_t textStart = 0)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize({900, 700}); ImGui::Begin("DataAsset Inspector Test");
        ImGuiID activate = 0;
        if (action) {
            ImGui::PushID("/text"); const ImGuiID node = ImGui::GetID("##node");
            if (std::strcmp(action, "open") == 0) activate = node;
            else {
                ImGui::PushOverrideID(node);
                if (std::strcmp(action, "next") == 0) activate = ImGui::GetID("次へ");
                else if (std::strcmp(action, "input") == 0) {
                    const std::string pageId = std::to_string(textStart);
                    ImGui::PushID(pageId.c_str()); ImGui::PushID("text"); activate = ImGui::GetID("##v");
                    ImGui::PopID(); ImGui::PopID();
                }
                ImGui::PopID();
            }
            ImGui::PopID();
            GImGui->NavActivateId = activate; GImGui->NavActivateDownId = activate; GImGui->NavActivatePressedId = activate;
            GImGui->NavActivateFlags = std::strcmp(action, "input") == 0 ? ImGuiActivateFlags_PreferInput : ImGuiActivateFlags_None;
        }
        const bool capture = !action || std::strcmp(action, "open") != 0;
        if (capture) ImGui::LogToBuffer();
        EXPECT_TRUE(editor::DrawManagedDataAssetInspector(Context(), path));
        const std::string log = capture ? GImGui->LogBuffer.c_str() : std::string{};
        if (capture) ImGui::LogFinish();
        ImGui::End(); ImGui::EndFrame();
        if (action && std::strcmp(action, "input") == 0) EXPECT_EQ(GImGui->ActiveId, activate) << log;
        return log;
    }
    void SelectAllTextPage(const std::string& path, uint64_t textStart)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.AddKeyEvent(ImGuiKey_LeftCtrl, true); io.AddKeyEvent(ImGuiMod_Ctrl, true); DrawTextPage(path, nullptr, textStart);
        io.AddKeyEvent(ImGuiKey_A, true); DrawTextPage(path, nullptr, textStart);
        io.AddKeyEvent(ImGuiKey_A, false); DrawTextPage(path, nullptr, textStart);
        io.AddKeyEvent(ImGuiKey_LeftCtrl, false); io.AddKeyEvent(ImGuiMod_Ctrl, false); DrawTextPage(path, nullptr, textStart);
        const ImGuiInputTextState& state = GImGui->InputTextState;
        ASSERT_EQ(state.ID, GImGui->ActiveId);
        ASSERT_TRUE(state.HasSelection());
        EXPECT_EQ(state.GetSelectionStart(), 0);
        EXPECT_EQ(state.GetSelectionEnd(), state.TextLen);
    }
    void EditAmount(const std::string& path, const char* value)
    {
        Draw(path);
        Draw(path, true);
        ImGui::GetIO().AddInputCharactersUTF8(value);
        Draw(path);
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
        Draw(path);
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
        Draw(path);
        ASSERT_TRUE(editor::AssetDirtyRegistry::IsDirty(path));
    }
    ImGuiContext* m_previous = nullptr;
    ImGuiContext* m_imgui = nullptr;
};

TEST_F(ManagedDataAssetInspectorTest, SelectionChangesPreserveBothDraftsAndSaveAllWritesTheirValues)
{
    const auto first = Path("First.fzdata"), second = Path("Second.fzdata");
    ASSERT_TRUE(Commit(Preview(first, true)).ok);
    ASSERT_TRUE(Commit(Preview(second, true)).ok);
    EditAmount(first, "55");
    EditAmount(second, "66");
    Draw(first);
    EXPECT_EQ(editor::AssetDirtyRegistry::GetAll().size(), 2U);
    EXPECT_EQ(Document(first).Find("fields")->Find("amount")->AsInt64(), 7);
    EXPECT_EQ(Document(second).Find("fields")->Find("amount")->AsInt64(), 7);
    ASSERT_EQ(editor::AssetDirtyRegistry::SaveAll(), 0);
    EXPECT_FALSE(editor::AssetDirtyRegistry::HasAny());
    EXPECT_EQ(Document(first).Find("fields")->Find("amount")->AsInt64(), 55);
    EXPECT_EQ(Document(second).Find("fields")->Find("amount")->AsInt64(), 66);
}

TEST_F(ManagedDataAssetInspectorTest, FailedSaveSurvivesSelectionChangeAndCanRetryAfterUnlock)
{
    const auto first = Path("LockedDraft.fzdata"), second = Path("Other.fzdata");
    ASSERT_TRUE(Commit(Preview(first, true)).ok);
    ASSERT_TRUE(Commit(Preview(second, true)).ok);
    EditAmount(first, "55");
    const auto original = Bytes(first);
    HANDLE lock = CreateFileW(util::FileSystem::PathFromUtf8(first).c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(lock, INVALID_HANDLE_VALUE);
    const bool saved = editor::AssetDirtyRegistry::Save(first);
    CloseHandle(lock);
    EXPECT_FALSE(saved);
    EXPECT_EQ(Bytes(first), original);
    Draw(second);
    Draw(first);
    EXPECT_TRUE(editor::AssetDirtyRegistry::IsDirty(first));
    ASSERT_TRUE(editor::AssetDirtyRegistry::Save(first));
    EXPECT_EQ(Document(first).Find("fields")->Find("amount")->AsInt64(), 55);
}

TEST_F(ManagedDataAssetInspectorTest, LongTextUsesObservedPageBoundariesAndKeepsHeadTailDraftsAcrossRetryUndoRedo)
{
    const auto path = Path("PagedText.fzdata"), other = Path("PagedTextOther.fzdata");
    const std::string originalText = std::string(31, 'a') + "😀" + std::string(11000, 'b');
    auto created = CreateTextAsset(path, originalText); ASSERT_TRUE(created.ok) << created.message; ASSERT_TRUE(Commit(created).ok);
    ASSERT_TRUE(Commit(Preview(other, true)).ok);
    const std::string originalBytes = Bytes(path);

    DrawTextPage(path, "open");
    std::string log = DrawTextPage(path);
    EXPECT_NE(log.find("0 +31 / 11033"), std::string::npos) << log;
    DrawTextPage(path, "input");
    SelectAllTextPage(path, 0);
    ImGui::GetIO().AddInputCharactersUTF8("HEAD"); log = DrawTextPage(path);
    ASSERT_TRUE(editor::AssetDirtyRegistry::IsDirty(path)) << log
        << " activeId=" << GImGui->ActiveId << " input=" << GImGui->InputTextState.GetText();

    constexpr uint64_t tailStart = 11007;
    uint64_t start = 0;
    for (size_t page = 0; start < tailStart && page < 400; ++page) {
        log = DrawTextPage(path, "next");
        start = start == 0 ? 31 : start + 32;
        EXPECT_NE(log.find(std::to_string(start) + " +"), std::string::npos) << "page start " << start;
    }
    ASSERT_EQ(start, tailStart);
    DrawTextPage(path, "input", tailStart);
    SelectAllTextPage(path, tailStart);
    ImGui::GetIO().AddInputCharactersUTF8("TAIL"); DrawTextPage(path);

    HANDLE lock = CreateFileW(util::FileSystem::PathFromUtf8(path).c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(lock, INVALID_HANDLE_VALUE);
    EXPECT_FALSE(editor::AssetDirtyRegistry::Save(path));
    CloseHandle(lock);
    EXPECT_EQ(Bytes(path), originalBytes);
    Draw(other); DrawTextPage(path);
    EXPECT_TRUE(editor::AssetDirtyRegistry::IsDirty(path));
    ASSERT_TRUE(editor::AssetDirtyRegistry::Save(path));

    const std::string expected = "HEAD😀" + std::string(10974, 'b') + "TAIL";
    EXPECT_EQ(Document(path).Find("fields")->Find("text")->AsString(), expected);
    const std::string savedBytes = Bytes(path);
    ASSERT_TRUE(m_undo.Undo()) << m_undo.LastDiagnostic(); EXPECT_EQ(Bytes(path), originalBytes);
    ASSERT_TRUE(m_undo.Redo()) << m_undo.LastDiagnostic(); EXPECT_EQ(Bytes(path), savedBytes);
    EXPECT_EQ(Document(path).Find("fields")->Find("text")->AsString(), expected);
}

TEST_F(ManagedDataAssetInspectorTest, ExplicitDiscardAndShutdownDoNotResumeOldDraftSaves)
{
    const auto path = Path("Discarded.fzdata");
    ASSERT_TRUE(Commit(Preview(path, true)).ok);
    const auto original = Bytes(path);
    EditAmount(path, "55");
    editor::AssetDirtyRegistry::DiscardAll();
    Draw(path);
    EXPECT_FALSE(editor::AssetDirtyRegistry::HasAny());
    EXPECT_EQ(Bytes(path), original);
    EditAmount(path, "66");
    editor::CloseManagedDataAssetEditorDocuments();
    EXPECT_FALSE(editor::AssetDirtyRegistry::HasAny());
    EXPECT_EQ(Bytes(path), original);
}

TEST_F(ManagedDataAssetInspectorTest, ProjectCloseReleasesDraftBeforeRuntimeShutdownAndReopensCurrentSchema)
{
    const auto first = Path("ClosingProject.fzdata"), second = Path("ReopenedProject.fzdata");
    ASSERT_TRUE(Commit(Preview(first, true)).ok);
    ASSERT_TRUE(Commit(Preview(second, true)).ok);
    const auto original = Bytes(first);
    EditAmount(first, "55");
    editor::InspectorPanel::ResetPendingDataAssetSaves();
    EXPECT_FALSE(editor::AssetDirtyRegistry::HasAny());
    EXPECT_EQ(Bytes(first), original);

    auto& runtime = scene::ManagedScriptRuntime::Instance();
    runtime.Unload(nullptr);
    ASSERT_TRUE(runtime.Shutdown());
    ASSERT_TRUE(runtime.Initialize(FBZZ_MANAGED_HOST_TEST_DIRECTORY));
    ASSERT_TRUE(runtime.Load(FBZZ_MANAGED_FIXTURE_TEST_ASSEMBLY, true)) << runtime.LastDiagnostic();
    EditAmount(second, "66");
    ASSERT_TRUE(editor::AssetDirtyRegistry::Save(second));
    EXPECT_EQ(Document(second).Find("fields")->Find("amount")->AsInt64(), 66);
    EXPECT_EQ(Bytes(first), original);
    EXPECT_FALSE(editor::AssetDirtyRegistry::HasAny());
}

TEST_F(ManagedDataAssetAuthoringTest, SavesCrlfAssetsAndUndoRestoresExactResidentAndSpooledBytes)
{
    for (const bool large : {false, true}) {
        const auto path = Path(large ? "LargeCrlf.fzdata" : "Crlf.fzdata");
        auto created = Preview(path, true);
        ASSERT_TRUE(created.ok) << created.message;
        ASSERT_TRUE(Commit(created).ok);
        std::string crlf;
        for (const char character : Bytes(path)) {
            if (character == '\n') crlf += '\r';
            crlf += character;
        }
        if (large) crlf += "\r\n#" + std::string(1024U * 1024U, 'x') + "\r\n";
        ASSERT_TRUE(util::FileSystem::WriteTextAtomic(path, crlf));
        const auto edited = Preview(path, false, 55);
        ASSERT_TRUE(edited.ok) << edited.message;
        ASSERT_TRUE(Commit(edited).ok);
        const auto after = Bytes(path);
        EXPECT_EQ(Document(path).Find("fields")->Find("amount")->AsInt64(), 55);
        ASSERT_TRUE(m_undo.Undo()) << m_undo.LastDiagnostic();
        EXPECT_EQ(Bytes(path), crlf);
        ASSERT_TRUE(m_undo.Redo()) << m_undo.LastDiagnostic();
        EXPECT_EQ(Bytes(path), after);
    }
}
TEST_F(ManagedDataAssetAuthoringTest, CreatesNormalizesReopensAndRestoresExactFileBytesWithoutRerunningCallbacks)
{
    const std::string path = Path("profile.fzdata"); auto created = Preview(path, true, 900); ASSERT_TRUE(created.ok) << created.message;
    ASSERT_TRUE(Commit(created).ok); const auto guid = asset::AssetDatabase::TryGetGuidFromPath(path); ASSERT_FALSE(guid.empty());
    std::string first; ASSERT_TRUE(util::FileSystem::ReadText(path, first)); auto data = Document(path);
    EXPECT_EQ(data.Find("fields")->Find("amount")->AsInt64(), 100); EXPECT_EQ(data.Find("fields")->Find("resetCount")->AsInt64(), 1);
    auto edited = Preview(path, false, -20); ASSERT_TRUE(edited.ok) << edited.message; ASSERT_TRUE(Commit(edited).ok);
    std::string second; ASSERT_TRUE(util::FileSystem::ReadText(path, second)); data = Document(path);
    EXPECT_EQ(data.Find("fields")->Find("amount")->AsInt64(), 0); EXPECT_EQ(data.Find("fields")->Find("validateCount")->AsInt64(), 2);
    ASSERT_TRUE(m_undo.Undo()) << m_undo.LastDiagnostic(); std::string actual; ASSERT_TRUE(util::FileSystem::ReadText(path, actual)); EXPECT_EQ(actual, first);
    ASSERT_TRUE(m_undo.Redo()) << m_undo.LastDiagnostic(); ASSERT_TRUE(util::FileSystem::ReadText(path, actual)); EXPECT_EQ(actual, second);
    EXPECT_EQ(asset::AssetDatabase::TryGetGuidFromPath(path), guid);
    editor::OpArgs inspect; inspect.Set("path", "guid:" + guid); auto page = Invoke("asset.data.inspect", inspect); ASSERT_TRUE(page.ok) << page.message;
    EXPECT_EQ(page.data.Find("type")->AsString(), "cs-internal:ManagedFixtures.PortableProfileDataAsset");
}
TEST_F(ManagedDataAssetAuthoringTest, FileAndMetaConflictsPreserveDiskAndHistoryCursor)
{
    const auto path = Path("conflict.fzdata"); auto create = Preview(path, true); ASSERT_TRUE(create.ok); ASSERT_TRUE(Commit(create).ok);
    auto edit = Preview(path, false, 80); ASSERT_TRUE(edit.ok); std::string original; ASSERT_TRUE(util::FileSystem::ReadText(path, original));
    ASSERT_TRUE(util::FileSystem::WriteText(path, original + "\n# external editor\n")); const auto revision = m_undo.GetRevision();
    EXPECT_FALSE(Commit(edit).ok); EXPECT_EQ(m_undo.GetRevision(), revision); EXPECT_FALSE(m_undo.Undo()); EXPECT_EQ(m_undo.GetRevision(), revision);
    std::string actual; ASSERT_TRUE(util::FileSystem::ReadText(path, actual)); EXPECT_EQ(actual, original + "\n# external editor\n");
    const auto other = Path("other.fzdata"); auto planned = Preview(other, true); ASSERT_TRUE(planned.ok);
    const std::string foreign = "[meta]\nguid = 'foreign'\n"; ASSERT_TRUE(util::FileSystem::WriteText(other + ".meta", foreign));
    EXPECT_FALSE(Commit(planned).ok); EXPECT_FALSE(util::FileSystem::Exists(other)); ASSERT_TRUE(util::FileSystem::ReadText(other + ".meta", actual)); EXPECT_EQ(actual, foreign);
}
TEST_F(ManagedDataAssetAuthoringTest, LockedMetaAndFailedAtomicReplacementPreserveOriginalFilesAndCursor)
{
    const auto path = Path("locked.fzdata"); auto create = Preview(path, true); ASSERT_TRUE(create.ok); ASSERT_TRUE(Commit(create).ok);
    std::string original, meta; ASSERT_TRUE(util::FileSystem::ReadText(path, original)); ASSERT_TRUE(util::FileSystem::ReadText(path + ".meta", meta));
    const auto revision = m_undo.GetRevision();
    HANDLE lock = CreateFileW(util::FileSystem::PathFromUtf8(path + ".meta").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(lock, INVALID_HANDLE_VALUE); EXPECT_FALSE(m_undo.Undo()); CloseHandle(lock); EXPECT_EQ(m_undo.GetRevision(), revision);
    std::string actual; ASSERT_TRUE(util::FileSystem::ReadText(path, actual)); EXPECT_EQ(actual, original); ASSERT_TRUE(util::FileSystem::ReadText(path + ".meta", actual)); EXPECT_EQ(actual, meta);
    auto edit = Preview(path, false, 55); ASSERT_TRUE(edit.ok);
    lock = CreateFileW(util::FileSystem::PathFromUtf8(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(lock, INVALID_HANDLE_VALUE); EXPECT_FALSE(Commit(edit).ok); CloseHandle(lock); EXPECT_EQ(m_undo.GetRevision(), revision);
    ASSERT_TRUE(util::FileSystem::ReadText(path, actual)); EXPECT_EQ(actual, original);
    ASSERT_TRUE(m_undo.Undo()) << m_undo.LastDiagnostic(); EXPECT_FALSE(util::FileSystem::Exists(path)); EXPECT_FALSE(util::FileSystem::Exists(path + ".meta"));
    ASSERT_TRUE(m_undo.Redo()) << m_undo.LastDiagnostic(); ASSERT_TRUE(util::FileSystem::ReadText(path, actual)); EXPECT_EQ(actual, original);
}
TEST_F(ManagedDataAssetAuthoringTest, IdenticalDataWithNewMetaGuidRejectsStaleDisplayedIdentity)
{
    const auto path = Path("identity.fzdata"); auto create = Preview(path, true); ASSERT_TRUE(create.ok); ASSERT_TRUE(Commit(create).ok);
    const auto guid = asset::AssetDatabase::TryGetGuidFromPath(path); std::string before; ASSERT_TRUE(util::FileSystem::ReadText(path, before));
    ASSERT_TRUE(util::FileSystem::WriteText(path + ".meta", "[meta]\nguid = 'new-identity'\n"));
    editor::OpData fields = editor::OpData::MakeObject(); fields.Set("amount", 5); editor::OpArgs request;
    request.Set("path", path); request.Set("fields", fields); request.Set("expectedAssetGuid", guid);
    const auto revision = m_undo.GetRevision(); const auto rejected = Invoke("asset.data.configure.preview", request);
    EXPECT_FALSE(rejected.ok); EXPECT_NE(rejected.message.find("ASSET_IDENTITY_CHANGED"), std::string::npos); EXPECT_EQ(m_undo.GetRevision(), revision);
    std::string actual; ASSERT_TRUE(util::FileSystem::ReadText(path, actual)); EXPECT_EQ(actual, before);
}
TEST_F(ManagedDataAssetAuthoringTest, LargeAssetSaveReopenLaterLeafAndUndoRetainBeyond16MiB)
{
    const auto path = Path("large.fzdata"); auto create = Preview(path, true, 7, "cs-internal:ManagedFixtures.LargePortableProfileDataAsset"); ASSERT_TRUE(create.ok) << create.message; ASSERT_TRUE(Commit(create).ok);
    ASSERT_GT(std::filesystem::file_size(util::FileSystem::PathFromUtf8(path)), 18U * 1024U * 1024U);
    editor::OpData page = editor::OpData::MakeObject(); page.Set("path", "/values"); page.Set("start", 40000); page.Set("count", 1);
    editor::OpArgs inspect; inspect.Set("path", path); inspect.Set("page", page); auto tail = Invoke("asset.data.inspect", inspect); ASSERT_TRUE(tail.ok); EXPECT_EQ(tail.data.Find("items")->AsArray()[0].AsInt64(), 40000);
    editor::OpData patches = editor::OpData::MakeArray(); editor::OpData patch = editor::OpData::MakeObject(); patch.Set("path", "/values/40000"); patch.Set("value", 77); patches.Push(patch);
    editor::OpArgs edit; edit.Set("path", path); edit.Set("patches", patches); auto planned = Invoke("asset.data.configure.preview", edit); ASSERT_TRUE(planned.ok) << planned.message; ASSERT_TRUE(Commit(planned).ok);
    ASSERT_TRUE(m_undo.Undo()); auto restored = Document(path); const auto* fields = restored.Find("fields"); EXPECT_EQ(fields->Find("values")->AsArray().back().AsInt64(), 40000);
    EXPECT_TRUE(fields->Find("text")->AsString().ends_with("終端")); ASSERT_TRUE(m_undo.Redo()); restored = Document(path); EXPECT_EQ(restored.Find("fields")->Find("values")->AsArray().back().AsInt64(), 77);
    editor::OpArgs bounds; bounds.Set("start", std::numeric_limits<int>::max()); bounds.Set("count", 64); EXPECT_FALSE(Invoke("asset.data.catalog", bounds).ok);
}
} /// @note namespace fbzz::tests
