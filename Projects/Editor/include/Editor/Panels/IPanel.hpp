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
    virtual void OnInit(EditorContext& ctx) { (void)ctx; }
    virtual void OnShutdown() {}

    void OnRender(EditorContext& ctx)
    {
        OnBeforeBegin(ctx);
        bool open = ImGui::Begin(GetWindowName(), nullptr, GetWindowFlags());
        OnAfterBegin(ctx);
        if (!open) {
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
    virtual ImGuiWindowFlags GetWindowFlags() const { return ImGuiWindowFlags_None; }
    virtual void OnBeforeBegin(EditorContext& ctx) { (void)ctx; }
    virtual void OnAfterBegin(EditorContext& ctx) { (void)ctx; }
    virtual void OnAfterEnd(EditorContext& ctx) { (void)ctx; }
};

} // namespace fbzz::editor
