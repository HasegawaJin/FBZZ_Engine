/// @file    ProjectSettingsPanel.hpp
/// @brief   プロジェクト設定とエディターの個人設定を編集するパネル。
/// @author  Hasegawa Jin
/// @date    2026-05-23
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <Engine/ProjectSettings.hpp>
#include <imgui.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace fbzz::editor {

/// @brief Project Settings パネル。保存は自動 (ProjectSettings.toml は EditorApp、Input.inputactions はパネル)。
/// @note 行は Field() で描く。ラベル列・既定値との差分印・右クリックの «既定値に戻す»・横断検索を 1 か所で持つ。
class ProjectSettingsPanel final : public IPanel {
public:
    /// @brief サイドバーの項目。Editor だけ保存先が editor_settings.toml (個人設定)。
    enum class Section {
        Application,
        Graphics,
        Physics,
        Input,
        Audio,
        TagsAndLayers,
        Import,
        Editor,
        Count,
    };

    const char* GetWindowName()        const override { return "Project Settings"; }
    bool        GetDefaultVisibility() const override { return false; }
    /// @brief 未保存の入力バインドをここで書き出す (パネルを閉じた直後に終了した場合)。
    void        OnShutdown() override;

protected:
    void OnRenderContent(EditorContext& ctx) override;

private:
    /// @name 枠
    /// @{
    void DrawSidebar();
    void DrawSaveStatus(EditorContext& ctx);
    void DrawContent(EditorContext& ctx);
    void DrawSection(EditorContext& ctx, Section section);
    void TrackUndo(EditorContext& ctx, const ProjectSettings& beforeDraw,
                   ImGuiID activeBefore, std::uint64_t generationBefore);
    /// @}

    /// @name 行の枠組み
    /// @{
    [[nodiscard]] bool Searching() const { return m_search[0] != '\0'; }
    /// @brief 検索語に当たるか。当たったら保留中の見出しを描いて true。検索中でなければ常に true。
    [[nodiscard]] bool Matches(const char* label, const char* keywords = nullptr);
    /// @brief 折りたたみ見出し。検索中は見出しを保留し、中の行が当たったときだけ描く。
    /// @param keywords 見出しに当たったとき中身を全部出すための語 (軸名・タグ名など)。
    [[nodiscard]] bool BeginGroup(const char* label, bool defaultOpen = true, const char* keywords = nullptr);
    void EndGroup();
    /// @brief 1 行の設定。draw は値ウィジェットを "##v" で 1 つ描き、変更したら true を返す。
    /// @param def 既定値。nullptr なら差分印と «既定値に戻す» を出さない (保存されない値など)。
    /// @return 編集または既定値へ戻したら true。
    template <class T, class Draw>
    bool Field(const char* label, T& value, const T* def, const char* tooltip, Draw&& draw);
    /// @brief 値を持たない表示だけの行。
    void InfoRow(const char* label, const char* text);
    void FlushPendingHeaders();
    /// @brief 既定値へ戻した・一覧を組み替えたなど、アクティブなウィジェットを伴わない編集の通知。
    void MarkStructuralEdit();
    /// @}

    /// @name 各セクション
    /// @{
    void DrawApplication(EditorContext& ctx, ProjectSettings& settings);
    void DrawCursor(EditorContext& ctx, CursorAppearance& cursor);
    void DrawGraphics(EditorContext& ctx, renderer::RenderSettings& render);
    void DrawPipelineAsset(EditorContext& ctx);
    void DrawShadows(renderer::RenderSettings& render);
    /// @brief 明るさ・描画スケール。Option 画面が持つ値なので保存されない (確認用)。
    void DrawPlayerOptionsPreview(EditorContext& ctx, renderer::RenderSettings& render);
    void DrawDebugOverlays(renderer::RenderSettings& render);
    void DrawPhysics(ProjectSettings& settings);
    /// @brief レイヤー同士がぶつかるかの表。名前の付いたレイヤーだけを並べる。
    void DrawCollisionMatrix(ProjectSettings& settings);
    /// @brief 表の見出し ("8: Boss")。シーンに保存されるのは番号なので番号を残す。
    [[nodiscard]] static std::string LayerLabel(const ProjectSettings& settings, int layer);
    /// @note エディター操作のショートカット (Hotkey Editor) とは分ける。配布物への含まれ方が違う。
    void DrawInput(EditorContext& ctx);
    void DrawInputAxes(bool& dirty);
    void DrawInputActions(bool& dirty);
    void DrawAudio(ProjectSettings& settings);
    void DrawTagsAndLayers(ProjectSettings& settings);
    void DrawTags(ProjectSettings& settings);
    void DrawLayers(ProjectSettings& settings);
    void DrawImport(EditorContext& ctx);
    void DrawEditorPreferences(EditorContext& ctx);
    /// @}

    /// @name 自動保存
    /// @{
    /// @brief Input.inputactions の自動保存。リバインド待機中と掴んでいる最中は書かない。
    void TickInputAutoSave(EditorContext& ctx);
    /// @brief editor_settings.toml 側の値 (Import / Preferences) を手を離した時点で保存要求する。
    void FlushEditorPreferences(EditorContext& ctx);

    struct UndoTracker {
        ImGuiID         activeId = 0;
        ProjectSettings before;
        bool            active  = false;
        bool            changed = false;
    };

    Section       m_currentSection = Section::Application;
    char          m_search[64] = {};
    char          m_newTag[64] = {};
    std::string   m_pipelineAssetError;
    std::uint64_t m_editGeneration = 0;
    UndoTracker   m_undo;

    /// @brief 描いている最中のセクションが ProjectSettings の値か (false なら EditorSettings)。
    bool          m_editingProjectSettings = true;
    bool          m_editorPrefsDirty = false;

    /// @name 検索中の保留見出しと当たり数
    ///@{
    Section       m_drawingSection = Section::Application;
    bool          m_pendingPage    = false;
    const char*   m_pendingGroup   = nullptr;
    bool          m_pageMatched    = false;
    bool          m_groupMatched   = false;
    int           m_matchCount     = 0;
    std::array<int, static_cast<std::size_t>(Section::Count)> m_sectionMatches{};
    std::array<int, static_cast<std::size_t>(Section::Count)> m_lastSectionMatches{};
    ///@}

    /// @name Input.inputactions の自動保存
    ///@{
    enum class InputSaveState { Saved, Pending, Failed };
    InputSaveState m_inputSaveState = InputSaveState::Saved;
    bool           m_inputDirty     = false;
    bool           m_wasRebinding   = false;
    float          m_inputIdle      = 0.0f;
    std::string    m_inputSavedClock;
    std::string    m_inputPath;
    ///@}

    /// @note 描画スケールはドラッグ中に反映すると中間 RT の再確保が走り続けるため、手を離すまで draft に溜める。
    float m_renderScaleDraft = 1.0f;
    bool  m_renderScaleDragging = false;
    /// @}
};

} /// @note namespace fbzz::editor
