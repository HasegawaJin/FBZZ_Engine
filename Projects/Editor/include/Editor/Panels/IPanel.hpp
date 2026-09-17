/// @file    IPanel.hpp
/// @brief   エディターパネルの基底インターフェース。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#pragma once
#include <Editor/Util/HotkeyScope.hpp>
#include <Editor/Util/Localization.hpp>
#include <imgui.h>

namespace fbzz::editor { struct EditorContext; struct EditorSettings; }

namespace fbzz::editor {

/// EditorContext::focusedPanelScope へ書く唯一の口。
/// @note IPanel.hpp は EditorContext を前方宣言しか持たない。パネルの基底がコンテキストの全定義を
///       引くと、コンテキストを触るたび全パネルが再コンパイルされるため、関数として切り出す。
void PublishPanelScope(EditorContext& ctx, HotkeyScope scope);

class IPanel {
public:
    virtual ~IPanel() = default;

    virtual const char* GetWindowName() const = 0;
    virtual const char* GetViewMenuName() const { return GetWindowName(); }
    /// View > Panels メニューに表示するかどうか。
    virtual bool ShowInViewMenu() const { return true; }
    /// 起動時の初期表示状態。設定・ツールなどの非常時パネルは false を返すことで
    /// EditorApp の individual visible = false 設定を不要にする。
    virtual bool GetDefaultVisibility() const { return true; }
    /// View > Panels のサブメニュー名。nullptr の場合はルートに並べる。
    virtual const char* GetMenuCategory() const { return nullptr; }
    /// このパネルにフォーカスがある間、どの «面» のキーを効かせるか。None ならキーの文脈を
    /// 持たない (Global なキーだけが効く)。
    /// @note フォーカス位置を決められるのは Begin している当人だけなので名乗りは各パネルに置き、
    ///       記録・解決は基底と HotkeyManager (EditorContext::focusedPanelScope) に集約する。
    virtual HotkeyScope GetHotkeyScope() const { return HotkeyScope::None; }

    virtual void OnInit(EditorContext& ctx) { (void)ctx; }
    virtual void OnShutdown() {}

    /// パネル固有の設定を editor_settings.toml と往復させるフック。
    /// OnLoadSettings はプロジェクトを開いた直後 (設定ロード後)、OnSaveSettings は終了時 (設定保存前) に
    /// EditorApp が呼ぶ。
    /// @note OnInit は projectRoot が決まる前に走り EditorContext もまだ既定値のため、パネルが
    ///       「前回の状態」を受け取れる唯一の点がここ。
    virtual void OnLoadSettings(const EditorSettings& settings) { (void)settings; }
    virtual void OnSaveSettings(EditorSettings& settings) const { (void)settings; }

    /// テンプレートメソッド。フック呼び出し順序:
    ///  OnBeforeBegin  → ImGui::Begin → OnAfterBegin → OnRenderContent → ImGui::End → OnAfterEnd
    /// OnBeforeBegin : SetNextWindowPos/Size など Begin より前に必要な ImGui 設定を行う場所
    /// OnAfterBegin  : Begin 内部で消費された PushStyleVar を PopStyleVar で戻す場所
    /// OnAfterEnd    : ウィンドウが閉じられた後のクリーンアップ
    void OnRender(EditorContext& ctx)
    {
        OnBeforeBegin(ctx);
        /// @note タイトルだけを訳す。LOC は "訳###原文" を返すので ImGui の ID は原文のままで、
        ///       保存済みのドッキング配置 (imgui_layout.ini) も、名前で引く
        ///       DockBuilderDockWindow / panelVisibility も、言語を切り替えても効き続ける。
        bool open = ImGui::Begin(LOC(GetWindowName()), CanClose() ? &visible : nullptr,
                                 GetWindowFlags());
        OnAfterBegin(ctx);
        if (!open || !visible) {
            m_contentRendered = false;
            ImGui::End();
            OnAfterEnd(ctx);
            return;
        }

        m_contentRendered = true;

        /// @note キーの文脈はウィンドウの内側でしか判定できないので、ここで名乗る。
        ///       中身より先に立てるのは、パネル自身のキー処理 (Ctrl+C など) が
        ///       同じフレームのうちに PanelScopeFocused() を読めるようにするため。
        if (const HotkeyScope scope = GetHotkeyScope();
            scope != HotkeyScope::None &&
            ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
            PublishPanelScope(ctx, scope);
        }

        OnRenderContent(ctx);
        ImGui::End();
        OnAfterEnd(ctx);
    }

    bool visible = true;

    /// 直近の OnRender で中身を実際に描いたか。
    /// @note ドッキングされたタブが非アクティブな場合、visible は true のまま ImGui::Begin が false を
    ///       返し中身は描かれない。重い描画を丸ごと省きたいパネルはこの区別を見る必要がある。
    /// @note visible == false のパネルは OnRender 自体が呼ばれず値が更新されない。参照側は必ず visible と併せて判定する。
    [[nodiscard]] bool WasContentRendered() const { return m_contentRendered; }

protected:
    virtual void OnRenderContent(EditorContext& ctx) = 0;
    virtual bool CanClose() const { return true; }
    virtual ImGuiWindowFlags GetWindowFlags() const { return ImGuiWindowFlags_None; }
    virtual void OnBeforeBegin(EditorContext& ctx) { (void)ctx; }
    virtual void OnAfterBegin(EditorContext& ctx) { (void)ctx; }
    virtual void OnAfterEnd(EditorContext& ctx) { (void)ctx; }

private:
    /// 初期値 true: 起動直後の 1 フレーム目からビューポートを描かせる。
    bool m_contentRendered = true;
};

} // namespace fbzz::editor
