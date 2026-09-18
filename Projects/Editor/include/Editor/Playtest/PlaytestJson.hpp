/// @file    PlaytestJson.hpp
/// @brief   Playtest シナリオが Command Bus の応答を表明するための JSON パス解決と比較演算。
/// @author  Hasegawa Jin
/// @date    2026-09-17
/// @see     Docs/design/ai-verification-loop.md «2. Playtest»
#pragma once
#include <Editor/Ai/Json.hpp>

#include <string>
#include <string_view>

namespace fbzz::editor::playtest {

/// @brief ドット区切りのパス ("nodes.0.name") で値を引く。配列は 10 進の添字。
/// @param path 空文字なら root そのもの。
/// @return 途中で見つからなければ nullptr。
[[nodiscard]] const ai::JsonValue* ResolveJsonPath(const ai::JsonValue& root, std::string_view path);

/// @brief 実値と期待値を演算子で比べる。
/// @param actual 解決できなかった値は nullptr で渡す (exists / missing が扱う)。
/// @param op exists / missing / == / != / < / <= / > / >= / contains / length== / length>= / length<=。
/// @param expected 比較しない演算子では nullptr 可。
/// @param detail 不成立または演算子が不正なときに理由を書く。
/// @return 成立なら true。未知の演算子は false。
[[nodiscard]] bool EvaluateJsonCondition(const ai::JsonValue* actual, std::string_view op,
                                         const ai::JsonValue* expected, std::string& detail);

} // namespace fbzz::editor::playtest
