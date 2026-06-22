// FBZZ Engine
// PostProcessInspectorWidgets.hpp | fbzz::editor
// Project Settings と .fzpp で共有するポストプロセス編集 UI
#pragma once

namespace fbzz::renderer { struct PostProcessSettings; }

namespace fbzz::editor {

struct PostProcessInspectorResult {
    bool changed = false;
    bool structureChanged = false;
};

// PostProcessSettings の既存効果とカスタムパスを一貫した UI で編集する。
PostProcessInspectorResult DrawPostProcessInspector(renderer::PostProcessSettings& settings);

} // namespace fbzz::editor
