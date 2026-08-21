// FBZZ Engine
// OperatorBridge.hpp | fbzz::editor::ai
// WHAT: OperatorRegistry を Editor Command Bus (AI / MCP) へ公開する薄い変換層。
// WHY:  従来 AI へ機能を出すには、EditorBusDispatcher の巨大な if/else へハンドラを書き、
//       さらに TypeScript 側 (tools.ts) へ zod スキーマを手書きし、ドキュメントの
//       ツール一覧にも追記する必要があった。1 機能につき 3 箇所で、写し損ねると
//       人が使う経路と AI が使う経路で結果が変わる。実際にその食い違いは
//       ObjectPresets / TerrainBrush / NavMeshQuery など 8 回、症状が出てから直している。
//
//       ここが公開するのは editor.op.list と editor.op.invoke の 2 種だけで、
//       操作そのものは OperatorRegistry が正本。以後どれだけ操作が増えても
//       この層のコードは増えない (MCP 側も同様)。
//       設計と移行段階: Docs/design/editor-operator-model.md
#pragma once
#include <Editor/Ai/Json.hpp>
#include <string>

namespace fbzz::editor { struct EditorContext; }

namespace fbzz::editor::ai {

// 呼び出し結果。EditorBusDispatcher 側の Outcome へそのまま写せる形にしてある。
// WHY: Outcome は dispatcher の翻訳単位内 (無名 namespace) に閉じているため、
//      ここで共有せず同形の型を返して境界で詰め替える。10,000 行のファイルへ
//      これ以上コードを足さないことが、この層を分ける目的そのものでもある。
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

// editor.op.list — 登録済み操作の目録を返す (Query)。
// payload: { search?: string, category?: string, includeUnavailable?: bool }
// 各項目に poll の評価結果 (available) を含める。
// WHY: 「なぜ実行できないのか」を実行前に知れないと、AI は失敗してから理由を探すことになる。
//      available=false の操作も既定で返すのは、「存在しない」と「今は使えない」を
//      区別できないと代替手段の探索が始まってしまうため。
OperatorBridgeResult ListOperators(editor::EditorContext& context, const JsonValue& payload);

// editor.op.invoke — 操作を 1 つ実行する (Command)。
// payload: { id: string, args?: object }
// dryRun のときは実行せず、引数検証と実行可否の判定だけを返す。
OperatorBridgeResult InvokeOperator(editor::EditorContext& context,
                                    const JsonValue&       payload,
                                    bool                   dryRun);

// editor.op.query — Query 操作だけを実行して結果データを返す (Query)。
// payload: { id: string, args?: object }
// WHY 入口を分けるか: editor.op.invoke は MCP 側で write 権限のツールとして公開される。
//     読むだけの操作までそこに閉じ込めると、read 権限で接続した AI が
//     「登録簿には出るのに 1 つも呼べない Query」を見ることになる。
//     kind == Query 以外はここでは拒否するので、書き込みの抜け道にはならない。
OperatorBridgeResult QueryOperator(editor::EditorContext& context, const JsonValue& payload);

} // namespace fbzz::editor::ai
