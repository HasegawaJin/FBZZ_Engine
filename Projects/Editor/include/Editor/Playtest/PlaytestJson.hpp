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

/// @brief ドット区切り、または '/' 開始の JSON Pointer で値を引く。
/// @param path 空文字なら root。JSON Pointer は ~0/~1 を復号し、配列添字の先頭ゼロを許さない。
/// @return 未解決または不正な JSON Pointer なら nullptr。
/// @see https://www.rfc-editor.org/rfc/rfc6901#section-4 RFC 6901 Section 4: Evaluation.
[[nodiscard]] const ai::JsonValue* ResolveJsonPath(const ai::JsonValue& root, std::string_view path);

/// @brief 実値と期待値を演算子で比べる。
/// @param actual 解決できなかった値は nullptr で渡す (exists / missing が扱う)。
/// @param op exists / missing / == / != / < / <= / > / >= / contains / length== / length>= / length<=。
/// @param expected 比較しない演算子では nullptr 可。
/// @param detail 不成立または演算子が不正なときに理由を書く。
/// @return 成立なら true。未知の演算子は false。
[[nodiscard]] bool EvaluateJsonCondition(const ai::JsonValue* actual, std::string_view op,
                                         const ai::JsonValue* expected, std::string& detail);

} /// @note namespace fbzz::editor::playtest
