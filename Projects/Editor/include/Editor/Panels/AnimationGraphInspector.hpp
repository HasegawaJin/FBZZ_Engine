// FBZZ Engine
// AnimationGraphInspector.hpp | fbzz::editor
// Animation Graph で選択した State / Transition の Inspector 描画 API
#pragma once

namespace fbzz::scene {
class GameObject;
}

namespace fbzz::editor {

struct EditorContext;

// Animation Graph の選択対象が有効なら専用 Inspector を描画する。
// WHY: Graph パネルは関係の可視化、Inspector は選択要素の詳細編集に責務を分離する。
[[nodiscard]] bool DrawAnimationGraphInspector(EditorContext& ctx, scene::GameObject& gameObject);
[[nodiscard]] bool DrawAnimationGraphAssetInspector(EditorContext& ctx);

} // namespace fbzz::editor
