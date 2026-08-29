/// @file    ComponentOps.hpp
/// @brief   型名 (文字列) からコンポーネントを操作するための実行時テーブル。
/// @author  Hasegawa Jin
/// @date    2026-08-19
///
/// WHY 文字列で引く経路が要るか:
/// ComponentRegistry は完全にコンパイル時の表で、型を知っているコードからしか使えない。
/// しかしスクリプト DLL が宣言する必須コンポーネント (FBZZ_REQUIRE_COMPONENT) は
/// DLL 境界を越えて渡す都合上「型名の文字列」でしか運べない。テンプレート実体を
/// DLL 側で作らせると、同じ型なのに別実体が生まれて所有権と ABI の話が増える。
/// ここで一度だけ ForEachRegisteredComponent を畳んで名前引きの表を作り、
/// 検証側は型を一切知らないまま「持っているか」を問い合わせられるようにする。
#pragma once

#include <Engine/Scene/ComponentRegistry.hpp>
#include <span>
#include <string_view>

namespace fbzz::scene {

class GameObject;

// 1 コンポーネント型ぶんの名前引きエントリ。
struct ComponentOps {
    // ComponentRegistry の serializedName (= 型名そのもの)。検証キーになる。
    const char* typeName = "";
    // Inspector / Console に出す人間向けの名前 ("Rigid Body" 等)。
    const char* displayName = "";
    ComponentCategory category = ComponentCategory::Misc;
    // Add Component メニューに出る型か (内部型は false)。
    // 要求として宣言されていても false の型はユーザーが手で足せないため、
    // 検証側は「Fix ボタンを出さない」判断にこれを使う。
    bool addable = false;
    // go がこの型を持っているか。型を知らない呼び出し側の唯一の入口。
    bool (*has)(GameObject& go) = nullptr;
};

// 登録順のテーブル。ComponentRegistry と 1:1 で対応する。
[[nodiscard]] std::span<const ComponentOps> ComponentOpsTable();

// 型名からエントリを引く。未登録なら nullptr。
// typeName は名前空間修飾を含まない短縮名 (SplitComponentNames が正規化済み)。
[[nodiscard]] const ComponentOps* FindComponentOps(std::string_view typeName);

} // namespace fbzz::scene
