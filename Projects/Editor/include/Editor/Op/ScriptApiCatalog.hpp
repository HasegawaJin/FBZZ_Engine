/// @file    ScriptApiCatalog.hpp
/// @brief   公開 ScriptApi の読み取り catalog を共通操作へ登録する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#pragma once
namespace fbzz::editor {
class OperatorRegistry;
/// @note Query の結果 catalogJson は SDK 自身が生成した JSON。ゲーム callback は呼ばない。
void RegisterScriptApiCatalogOperator(OperatorRegistry& registry);
} /// @note namespace fbzz::editor
