/// @file    ScriptObjectFactory.hpp
/// @brief   スクリプト 1 つから、それが要求するコンポーネントを揃えた GameObject を作る。
/// @author  Hasegawa Jin
/// @date    2026-08-19
///
/// FBZZ_REQUIRE_COMPONENT の宣言をそのまま組み立て手順として使う。作ったものを
/// "Save As Prefab" すれば、以降の配置はプレファブ 1 個のドラッグで済む。
#pragma once

#include <string>
#include <vector>

namespace fbzz::scene { class GameObject; }

namespace fbzz::editor {

struct EditorContext;

/// @brief スクリプトと、それが宣言した要求コンポーネント一式を持つ GameObject をルートに作る。
/// @note 名前は型名から末尾の "Component" を落としたもの。要求は Add Component と同じ既定値で付く。
/// @note 親付け・命名の一意化・配置・選択・Undo は ObjectCreation.hpp が行う。
/// @return シーンが無いか、型が未登録なら nullptr (空の GameObject は残さない)。
scene::GameObject* CreateScriptObject(EditorContext& ctx, const std::string& scriptTypeName);

/// @brief Create メニューに並べるスクリプト型。
/// @note 登録順ではなく型名の昇順 (メニューの並びを安定させる)。
[[nodiscard]] std::vector<std::string> ScriptObjectTypeNames();

/// @brief スクリプト型名から GameObject 名を作る ("EnemyComponent" → "Enemy")。
[[nodiscard]] std::string ScriptObjectName(const std::string& scriptTypeName);

} // namespace fbzz::editor
