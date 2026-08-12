// FBZZ Engine
// ProjectSettingsPanel.hpp | fbzz::editor
// タグ・レイヤー名の編集パネル
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <cstdint>

namespace fbzz { struct ProjectSettings; }
namespace fbzz::renderer { struct RenderSettings; }

namespace fbzz::editor {

class ProjectSettingsPanel final : public IPanel {
public:
    // サイドバーの項目。
    // WHY 統合したか: 以前は 10 項目あり、うち Screen (2 フィールド) /
    //      Audio (2 スライダー) / Physics (3 フィールド) のように
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
    };

    const char* GetWindowName()        const override { return "Project Settings"; }
    bool        GetDefaultVisibility() const override { return false; }

protected:
    void OnRenderContent(EditorContext& ctx) override;

private:
    void DrawSidebar();
    void DrawSection(EditorContext& ctx);

    void DrawApplication(ProjectSettings& settings);
    void DrawImport(EditorContext& ctx);

    // Graphics セクション。中を 3 つの折りたたみに分ける。
    // WHY 分けるか: 旧 Render は「描画設定 (Pipeline/Shadow)」と
    //      「エディタのデバッグ表示 (Collider/NavMesh/Pass Viewer)」が
    //      同じ画面に混ざっており、目的の項目を探しにくかった。
    void DrawGraphics(renderer::RenderSettings& render);
    void DrawRenderCore(renderer::RenderSettings& render);   // Pipeline / Shadow / ViewMode
    void DrawRenderDebug(renderer::RenderSettings& render);  // デバッグ表示・アウトライン
    void DrawPostProcess(renderer::RenderSettings& render);

    void DrawPhysics(ProjectSettings& settings);

    // 入力バインド (.inputactions) の編集。
    // WHY エディタのショートカット (HotkeyEditorPanel) と分けるか:
    //     同じ UI に並べると「今どちらを編集しているのか」が判別できなくなる。
    //     ゲーム入力はプレイヤー向けのキーコンフィグ、エディタ操作は開発者向けで
    //     配布物への含まれ方も異なるため、責務ごと分離する。
    void DrawInput(EditorContext& ctx);

    void DrawAudio(ProjectSettings& settings);

    void DrawTagsAndLayers(ProjectSettings& settings);
    void DrawTags(ProjectSettings& settings);
    void DrawLayers(ProjectSettings& settings);

    Section m_currentSection = Section::Application;
    char m_newTag[64] = {};
    std::uint64_t m_editGeneration = 0;
};

} // namespace fbzz::editor
