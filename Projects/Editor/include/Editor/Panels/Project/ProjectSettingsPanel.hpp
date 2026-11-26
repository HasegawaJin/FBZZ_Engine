/// @file    ProjectSettingsPanel.hpp
/// @brief   プロジェクト設定とエディターの個人設定を編集するパネル。
/// @author  Hasegawa Jin
/// @date    2026-05-23
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <Engine/Project/ProjectSettings.hpp>
#include <Engine/Input/InputBindingsDocument.hpp>
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
    void TrackInputUndo(EditorContext& ctx, const input::InputBindingsDocument& beforeDraw, ImGuiID activeBefore);
    /// @}

    /// @name 行の枠組み
    /// @{
    [[nodiscard]] bool Searching() const { return m_search[0] != '\0'; }
    /// @brief 検索語に当たるか。当たったら保留中の見出しを描いて true。検索中でなければ常に true。
    [[nodiscard]] bool Matches(const char* label, const char* keywords = nullptr);
    /// @brief 折りたたみ見出し。検索中は見出しを保留し、中の行が当たったときだけ描く。
    /// @param[in] keywords 見出しに当たったとき中身を全部出すための語 (軸名・タグ名など)。
    [[nodiscard]] bool BeginGroup(const char* label, bool defaultOpen = true, const char* keywords = nullptr);
    void EndGroup();
    /// @brief 1 行の設定。draw は値ウィジェットを "##v" で 1 つ描き、変更したら true を返す。
    /// @param[in] def 既定値。nullptr なら差分印と «既定値に戻す» を出さない (保存されない値など)。
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
        ImGuiID         activeId = 0; ///< 編集前の値を取り始めた ImGui 入力項目の ID。
        ProjectSettings before; ///< 現在の入力操作を Undo で戻すために控えた設定全体。
        bool            active  = false; ///< 入力操作の開始から終了まで設定変更を追跡しているか。
        bool            changed = false; ///< 現在追跡中の入力操作が設定値を変更したか。
    };

    Section       m_currentSection = Section::Application; ///< 設定パネルで現在表示しているカテゴリ。
    char          m_search[64] = {}; ///< プロジェクト設定をカテゴリ横断で絞る入力文字列。
    char          m_newTag[64] = {}; ///< タグ一覧に追加する名前を編集中のバッファ。
    std::string   m_pipelineAssetError; ///< レンダーパイプラインアセットの読み込みや作成に失敗した理由。
    std::uint64_t m_editGeneration = 0; ///< 設定の構造を変更したときに進めるリビジョン。構造変更を通常の入力操作と区別する。
    UndoTracker   m_undo; ///< 設定ウィジェットの連続編集を一回の Undo にまとめるための状態。
    input::InputBindingsDocument m_inputUndoBefore; ///< キー割り当てを変更する操作の Undo に保存する編集前の入力定義。
    ImGuiID m_inputUndoActiveId = 0; ///< キー割り当ての編集前状態を取得した ImGui 項目 ID。
    bool m_inputUndoActive = false; ///< 入力定義への変更を一回の操作として記録中か。

    bool          m_editingProjectSettings = true; ///< 描いている最中のセクションが ProjectSettings の値か (false なら EditorSettings)。
    bool          m_editorPrefsDirty = false; ///< Editor の永続設定に保存が必要な変更があるか。

    /// @name 検索中の保留見出しと当たり数
    ///@{
    Section       m_drawingSection = Section::Application; ///< 検索に一致した項目を集計する、現在描画中のカテゴリ。
    bool          m_pendingPage    = false; ///< 検索に一致した項目が見つかり次第、カテゴリ見出しを描くか。
    const char*   m_pendingGroup   = nullptr; ///< 最初の一致項目を描くまで遅延している借用のグループ見出し。
    bool          m_pageMatched    = false; ///< カテゴリ名自体が検索語に一致し、中の項目をすべて表示するか。
    bool          m_groupMatched   = false; ///< グループ見出しやキーワードが検索に一致し、配下を表示するか。
    int           m_matchCount     = 0; ///< 現在の検索で表示対象になった設定項目の総数。
    std::array<int, static_cast<std::size_t>(Section::Count)> m_sectionMatches{}; ///< 今フレームのカテゴリごとの一致件数。添字は Section。
    std::array<int, static_cast<std::size_t>(Section::Count)> m_lastSectionMatches{}; ///< 前フレームのカテゴリ別集計。サイドバーの描画時点で表示する。
    ///@}

    /// @name Input.inputactions の自動保存
    ///@{
    enum class InputSaveState { Saved, Pending, Failed };
    InputSaveState m_inputSaveState = InputSaveState::Saved; ///< 入力バインディング自動保存の待機、成功、失敗の状態。
    bool           m_inputDirty     = false; ///< 入力バインディングに未保存の変更があるか。
    bool           m_wasRebinding   = false; ///< 前フレームで入力待ちのキー割り当て操作を実行中だったか。
    float          m_inputIdle      = 0.0f; ///< 入力バインディングの変更操作が止まってからの経過時間 [s]。
    std::string    m_inputSavedClock; ///< 最後に入力バインディングを保存したローカル時刻の表示文字列。
    std::string    m_inputPath; ///< 現在編集中の入力バインディング設定の保存先。
    ///@}

    float m_renderScaleDraft = 1.0f; ///< 描画スケールはドラッグ中に反映すると中間 RT の再確保が走り続けるため、手を離すまで draft に溜める。
    bool  m_renderScaleDragging = false; ///< 描画解像度の倍率を調整する入力をドラッグ中か。
    /// @}
};

} // namespace fbzz::editor
