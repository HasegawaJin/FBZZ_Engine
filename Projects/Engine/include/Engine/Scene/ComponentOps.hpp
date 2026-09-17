/// @file    ComponentOps.hpp
/// @brief   型名 (文字列) からコンポーネントを操作するための実行時テーブル。
/// @author  Hasegawa Jin
/// @date    2026-08-19
///
/// @note ComponentRegistry はコンパイル時の表で型を知っているコードからしか使えないが、スクリプト DLL の FBZZ_REQUIRE_COMPONENT は DLL 境界越しに「型名の文字列」でしか運べない (DLL 側でテンプレート実体を作ると同じ型で別実体が生まれ所有権と ABI の話が増える)。
/// @note ForEachRegisteredComponent を一度だけ畳んで名前引きの表を作り、検証側は型を知らないまま「持っているか」を問い合わせる。
#pragma once

#include <Engine/Scene/ComponentRegistry.hpp>
#include <span>
#include <string_view>

namespace fbzz::scene {

class GameObject;

/// @brief 1 コンポーネント型ぶんの名前引きエントリ。
struct ComponentOps {
    const char* typeName = ""; ///< ComponentRegistry の serializedName (= 型名そのもの)。検証キーになる。
    const char* displayName = ""; ///< Inspector / Console に出す人間向けの名前 ("Rigid Body" 等)。
    ComponentCategory category = ComponentCategory::Misc;
    /// @brief Add Component メニューに出る型か (内部型は false)。
    /// @note 要求として宣言されていても false の型はユーザーが手で足せないため、検証側は「Fix ボタンを出さない」判断にこれを使う。
    bool addable = false;
    bool (*has)(GameObject& go) = nullptr; ///< go がこの型を持っているか。型を知らない呼び出し側の唯一の入口。
};

/// @brief 登録順のテーブル。ComponentRegistry と 1:1 で対応する。
[[nodiscard]] std::span<const ComponentOps> ComponentOpsTable();

/// @brief 型名からエントリを引く。未登録なら nullptr。
/// @note typeName は名前空間修飾を含まない短縮名 (SplitComponentNames が正規化済み)。
[[nodiscard]] const ComponentOps* FindComponentOps(std::string_view typeName);

} // namespace fbzz::scene
