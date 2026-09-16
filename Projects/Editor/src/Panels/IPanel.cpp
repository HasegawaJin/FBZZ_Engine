/// @file    IPanel.cpp
/// @brief   パネル基底が EditorContext へ触る数少ない口。
/// @author  Hasegawa Jin
/// @date    2026-09-12
#include <Editor/Panels/IPanel.hpp>
#include <Editor/EditorContext.hpp>

namespace fbzz::editor {

void PublishPanelScope(EditorContext& ctx, HotkeyScope scope)
{
    ctx.focusedPanelScope = scope;
}

} // namespace fbzz::editor
