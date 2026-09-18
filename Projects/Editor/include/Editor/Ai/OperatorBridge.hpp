/// @file    OperatorBridge.hpp
/// @brief   OperatorRegistry を Editor Command Bus (AI / MCP) へ公開する薄い変換層。
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// @note 従来は EditorBusDispatcher の if/else・TypeScript 側の zod スキーマ・ドキュメントの 3 箇所へ機能ごとに書いており、経路間で結果が食い違う事故が繰り返し起きていた。ここは editor.op.list / editor.op.invoke の 2 種のみを公開し、操作そのものの正本は OperatorRegistry。
/// @see Docs/design/editor-operator-model.md
#pragma once
#include <Editor/Ai/Json.hpp>
#include <string>

namespace fbzz::editor { struct EditorContext; }

namespace fbzz::editor::ai {

/// 呼び出し結果。EditorBusDispatcher 側の Outcome へそのまま写せる形にしてある。
/// @note Outcome は dispatcher の無名 namespace に閉じているため共有できず、同形の型を返して境界で詰め替える。
struct OperatorBridgeResult {
    bool        ok = false;
    JsonValue   result;
    std::string code;
    std::string message;

    static OperatorBridgeResult Ok(JsonValue value)
    {
        OperatorBridgeResult r;
        r.ok     = true;
        r.result = std::move(value);
        return r;
    }
    static OperatorBridgeResult Err(std::string code, std::string message)
    {
        OperatorBridgeResult r;
        r.code    = std::move(code);
        r.message = std::move(message);
        return r;
    }
};

/// editor.op.list — 登録済み操作の目録を返す (Query)。
/// payload: { search?: string, category?: string, includeUnavailable?: bool }
/// 各項目に poll の評価結果 (available) を含める。
/// @note available=false の操作も既定で返す。「存在しない」と「今は使えない」を AI が区別できるようにするため。
OperatorBridgeResult ListOperators(editor::EditorContext& context, const JsonValue& payload);

/// editor.op.invoke — 操作を 1 つ実行する (Command)。
/// payload: { id: string, args?: object }
/// dryRun のときは実行せず、引数検証と実行可否の判定だけを返す。
OperatorBridgeResult InvokeOperator(editor::EditorContext& context,
                                    const JsonValue&       payload,
                                    bool                   dryRun);

/// editor.op.query — Query 操作だけを実行して結果データを返す (Query)。
/// payload: { id: string, args?: object }
/// @note editor.op.invoke は write 権限のツールとして公開されるため、読むだけの操作までそこに閉じ込めると read 権限の AI が 1 つも呼べなくなる。kind == Query 以外はここで拒否するので書き込みの抜け道にはならない。
OperatorBridgeResult QueryOperator(editor::EditorContext& context, const JsonValue& payload);

} // namespace fbzz::editor::ai
