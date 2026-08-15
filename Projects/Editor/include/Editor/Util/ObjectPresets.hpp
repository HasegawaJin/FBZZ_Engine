// FBZZ Engine
// ObjectPresets.hpp | fbzz::editor
// 「Add Object」で置ける GameObject プリセットの単一登録表。
//
// WHY 表にするか:
//   プリセットは「どのコンポーネントをどの初期値で組み合わせるか」という知識で、
//   これまで SceneHierarchyPanel.cpp の ImGui メニュー本体に直接埋め込まれていた。
//   そのため (1) メニューを描く経路からしか作れず、AI (Command Bus) からは同じものを
//   1 個も作れない、(2) プリセットを増やすたびにメニューのネスト構造を手で書き足す、
//   という 2 つの問題があった。
//
//   ここでは生成関数まで含めて 1 つの表にする。表に載せた時点でメニューにも AI にも
//   同時に現れ、片方だけに存在するプリセットを作れない。AI 側が「Cube を置く」ために
//   MeshRenderer + MaterialComponent + BoxCollider の組み合わせを推測する必要も無くなる
//   (推測させると、人がメニューから置いたものと中身の違うオブジェクトが混ざる)。
#pragma once
#include <Engine/Scene/Entity.hpp>
#include <span>
#include <string_view>

namespace fbzz::scene { class GameObject; }

namespace fbzz::editor {

struct EditorContext;

// 1 プリセットの定義。id は AI が指す安定キーで、label (表示名) を変えても壊れない。
struct ObjectPreset {
    std::string_view id;          // "3d.cube" 形式の安定 ID
    std::string_view category;    // メニューの見出し。空文字なら Add Object 直下
    std::string_view label;       // メニュー表示名
    std::string_view description; // 何が付くか (AI がプリセットを選ぶ根拠になる)

    // 生成本体。ctx.activeScene 上に GameObject を作って返す。
    // 選択の更新と親付けは CreateObjectFromPreset がまとめて行うため、ここではしない。
    scene::GameObject* (*create)(EditorContext& ctx);
};

// 登録順のプリセット一覧。メニューはこの順にカテゴリを積む。
[[nodiscard]] std::span<const ObjectPreset> ObjectPresetCatalog();

// id からプリセットを引く。見つからなければ nullptr。
[[nodiscard]] const ObjectPreset* FindObjectPreset(std::string_view id);

// プリセットを生成し、親付け (parent が有効なら) と選択更新まで行う。
// 失敗時 (未知の id / シーン無し / 生成失敗) は nullptr。
// Undo は呼び出し側の責務 (パネルは ExecuteSceneEditWithUndo、AI はディスパッチャ側で包む)。
scene::GameObject* CreateObjectFromPreset(EditorContext& ctx, std::string_view id,
                                          scene::EntityID parent = {});

} // namespace fbzz::editor
