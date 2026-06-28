// FBZZ Engine
// IPanel.hpp | fbzz::editor
// エディターパネルの基底インターフェース
#pragma once
#include <imgui.h>

namespace fbzz::editor { struct EditorContext; }

namespace fbzz::editor {

class IPanel {
public:
    virtual ~IPanel() = default;

    virtual const char* GetWindowName() const = 0;
    virtual const char* GetViewMenuName() const { return GetWindowName(); }
    // View > Panels メニューに表示するかどうか。
    virtual bool ShowInViewMenu() const { return true; }
    // 起動時の初期表示状態。設定・ツールなどの非常時パネルは false を返すことで
    // EditorApp の individual visible = false 設定を不要にする。
    virtual bool GetDefaultVisibility() const { return true; }
    // View > Panels のサブメニュー名。nullptr の場合はルートに並べる。
    virtual const char* GetMenuCategory() const { return nullptr; }
    virtual void OnInit(EditorContext& ctx) { (void)ctx; }
    virtual void OnShutdown() {}

    // テンプレートメソッド。フック呼び出し順序:
    //  OnBeforeBegin  → ImGui::Begin → OnAfterBegin → OnRenderContent → ImGui::End → OnAfterEnd
    // OnBeforeBegin : SetNextWindowPos/Size など Begin より前に必要な ImGui 設定を行う場所
    // OnAfterBegin  : Begin 内部で消費された PushStyleVar を PopStyleVar で戻す場所
    // OnAfterEnd    : ウィンドウが閉じられた後のクリーンアップ
    void OnRender(EditorContext& ctx)
    {
        OnBeforeBegin(ctx);
        bool open = ImGui::Begin(GetWindowName(), CanClose() ? &visible : nullptr, GetWindowFlags());
        OnAfterBegin(ctx);
        if (!open || !visible) {
            ImGui::End();
            OnAfterEnd(ctx);
            return;
        }

        OnRenderContent(ctx);
        ImGui::End();
        OnAfterEnd(ctx);
    }

    bool visible = true;

protected:
    virtual void OnRenderContent(EditorContext& ctx) = 0;
    virtual bool CanClose() const { return true; }
    virtual ImGuiWindowFlags GetWindowFlags() const { return ImGuiWindowFlags_None; }
    virtual void OnBeforeBegin(EditorContext& ctx) { (void)ctx; }
    virtual void OnAfterBegin(EditorContext& ctx) { (void)ctx; }
    virtual void OnAfterEnd(EditorContext& ctx) { (void)ctx; }
};

} // namespace fbzz::editor
