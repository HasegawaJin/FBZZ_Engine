// FBZZ Engine
// ScriptObjectFactory.hpp | fbzz::editor
// スクリプト 1 つから「それが動く GameObject」を丸ごと作る
//
// WHY:
//   汎用の EnemyScript を書いたとき、置くたびに RigidBody / Collider / Animator /
//   NavMeshAgent を手で選び直すのでは、スクリプトを共通化した意味が配置作業に
//   吸い取られてしまう。FBZZ_REQUIRE_COMPONENT でスクリプト側が要求を宣言している
//   なら、その宣言をそのまま組み立て手順として使える。
//
//   ここで作った GameObject をそのまま "Save As Prefab" すれば、以降は
//   プレファブ 1 個のドラッグで配置が済む。スクリプトが実行時に自分で
//   GetOrAddComponent する設計を採らないのは、Collider の寸法や Animator の
//   Controller のように「コードからは決められない値」が必ず残るため。
//   組み立てはエディタ (人が値を詰められる場所) で一度だけ行い、
//   結果を Prefab として固定するのが、破綻しない唯一の分担になる。
#pragma once

#include <Engine/Scene/Entity.hpp>
#include <string>
#include <vector>

namespace fbzz::scene { class GameObject; }

namespace fbzz::editor {

struct EditorContext;

// scriptTypeName のスクリプトと、それが FBZZ_REQUIRE_COMPONENT で宣言した
// コンポーネント一式を持つ GameObject を作る。
//
// - GameObject 名は型名から末尾の "Component" を落としたもの ("EnemyComponent" → "Enemy")
// - コンポーネントは Add Component と同じ既定値で付く (コライダーの自動フィット等)
// - 内部型 / 未登録型の要求は飛ばす (手では足せないため。Inspector が別途警告する)
//
// parent が有効ならその子として作る。Undo は呼び出し側の責務
// (Hierarchy は ExecuteSceneEditWithUndo で包む)。
// 失敗時 (シーン無し / 未登録のスクリプト型) は nullptr。
scene::GameObject* CreateScriptObject(EditorContext& ctx,
                                      const std::string& scriptTypeName,
                                      scene::EntityID parent = {});

// Add Object メニューに並べるスクリプト型の一覧。
// ScriptFactory の登録順ではなく型名の昇順で返す (メニューの並びを安定させるため)。
[[nodiscard]] std::vector<std::string> ScriptObjectTypeNames();

} // namespace fbzz::editor
