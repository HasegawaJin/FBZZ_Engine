/// @file    SchemaInspector.hpp
/// @brief   ITypeSchema を走査して型ごとのウィジェットを自動で描く汎用 Inspector。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// WHY: authoring フィールドは「スキーマ」「TOML codec」「手書き Inspector」「AI」の
/// 複数面に散らばりがちで、追加のたびにどこかが取り残される。スキーマから UI を
/// 生成できるようにしておけば、新しい leaf は宣言しただけで Inspector にも現れる。
/// 既存の手書き UI は情報量が多くスーパーセットなので置き換えず、共存させる
/// (手書きが担当済みの leaf は skipPaths で除外する)。
#pragma once

#include <Engine/Reflection/TypeSchema.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::editor::widgets {

// スキーマを走査してウィジェットを描く。
//   owner       : schema が記述している型のインスタンス
//   projectRoot : AssetRef の "..." ピッカーが検索する起点
//   skipPaths   : 既に手書き UI が担当している leaf の schemaPath (ルートからの相対)。
//                 前方一致で判定するため "particle.colorGradient" のような親指定もできる。
//   pathPrefix  : 再帰用。呼び出し側は既定の空文字のままでよい。
// @return true if any value was changed
// leaf の schemaPath 一覧が要るときは reflection::CollectLeafPaths() を使うこと
// (Inspector・AI・テストで唯一の走査実装を共有する)。
bool DrawSchemaProperties(const reflection::ITypeSchema& schema, void* owner,
                          const std::string& projectRoot,
                          const std::vector<std::string>& skipPaths = {},
                          std::string_view pathPrefix = {});

} // namespace fbzz::editor::widgets
