/// @file    ProjectSettingsPanel.hpp
/// @brief   タグ・レイヤー名の編集パネル。
/// @author  Hasegawa Jin
/// @date    2026-05-23
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <cstdint>
#include <string>

namespace fbzz { struct ProjectSettings; }
namespace fbzz { struct CursorAppearance; }
namespace fbzz::renderer { struct RenderSettings; }

namespace fbzz::editor {

class ProjectSettingsPanel final : public IPanel {
public:
    // サイドバーの項目。
    // WHY 統合したか: 以前は 10 項目あり、うち Screen (2 フィールド) /
    //      Audio (音量スライダーのみ) / Physics (3 フィールド) のように
    //      「1 画面に数行しかない項目」が並んで一覧性を落としていた。
    //      同じ関心事どうしをまとめ、各項目の中を折りたたみで整理する方が探しやすい。
    enum class Section {
        Application,    // Target FPS / 画面解像度 / 起動シーン
        Graphics,       // Rendering + Post Process + デバッグ表示
        Physics,
        Input,
        Audio,
        TagsAndLayers,  // タグとレイヤーはどちらも ID テーブルなので同居させる
        Import,
        // エディター自身の設定。値の保存先は ProjectSettings ではなく
        // editor_settings.toml (EditorSettings) ─ 表示言語のような «誰が触っているか»
        // で決まる設定は、配布ビルドまで運ばれる ProjectSettings には置けない。
        Editor,
    };

    const char* GetWindowName()        const override { return "Project Settings"; }
    bool        GetDefaultVisibility() const override { return false; }

protected:
    void OnRenderContent(EditorContext& ctx) override;

private:
    void DrawSidebar();
    void DrawSection(EditorContext& ctx);

    void DrawApplication(EditorContext& ctx, ProjectSettings& settings);
    // カーソルの絵 (種類ごとの画像とホットスポット)。拘束と表示はスクリプトが持つ。
    void DrawCursor(EditorContext& ctx, CursorAppearance& cursor);
    void DrawImport(EditorContext& ctx);

    // Graphics セクション。中を 3 つの折りたたみに分ける。
    // WHY 分けるか: 旧 Render は「描画設定 (Pipeline/Shadow)」と
    //      「エディタのデバッグ表示 (Collider/NavMesh/Pass Viewer)」が
    //      同じ画面に混ざっており、目的の項目を探しにくかった。
    void DrawGraphics(EditorContext& ctx, renderer::RenderSettings& render);
    void DrawRenderCore(renderer::RenderSettings& render);   // Pipeline / Shadow / ViewMode
    // 明るさ・描画スケール。Option 画面が持つ値なので保存されない (確認用)。
    void DrawRenderUserSettings(EditorContext& ctx, renderer::RenderSettings& render);
    void DrawRenderDebug(renderer::RenderSettings& render);  // デバッグ表示・アウトライン

    void DrawPhysics(ProjectSettings& settings);
    /// レイヤー同士がぶつかるかの表。名前の付いたレイヤーだけを並べる。
    void DrawCollisionMatrix(ProjectSettings& settings);
    /// 表の見出し。"8: Boss" のように番号を残す ─ シーンに保存されるのは番号なので、
    /// 名前だけだと «どの番号を GameObject へ入れるのか» が読めない。
    [[nodiscard]] static std::string LayerLabel(const ProjectSettings& settings, int layer);

    // 入力バインド (.inputactions) の編集。
    // WHY エディタのショートカット (HotkeyEditorPanel) と分けるか:
    //     同じ UI に並べると「今どちらを編集しているのか」が判別できなくなる。
    //     ゲーム入力はプレイヤー向けのキーコンフィグ、エディタ操作は開発者向けで
    //     配布物への含まれ方も異なるため、責務ごと分離する。
    void DrawInput(EditorContext& ctx);

    void DrawAudio(ProjectSettings& settings);

    void DrawEditor(EditorContext& ctx);

    void DrawTagsAndLayers(ProjectSettings& settings);
    void DrawTags(ProjectSettings& settings);
    void DrawLayers(ProjectSettings& settings);

    Section m_currentSection = Section::Application;
    char m_newTag[64] = {};
    std::uint64_t m_editGeneration = 0;
    // 描画スケールはドラッグ中に反映すると中間 RT の再確保が走り続けるため、
    // 手を離すまで draft に溜める。
    float m_renderScaleDraft = 1.0f;
    bool  m_renderScaleDragging = false;
};

} // namespace fbzz::editor
