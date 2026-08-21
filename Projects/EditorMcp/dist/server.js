// FBZZ Engine
// server.ts | EditorMcp
// transport 非依存の MCP サーバと Editor ツール集合を生成する
import { McpServer } from '@modelcontextprotocol/sdk/server/mcp.js';
import { ToDraft2020ToolsResult } from './jsonSchemaDraft2020.js';
import { RegisterEditorTools } from './tools.js';
// tools/list の応答を draft 2020-12 へ正規化する。
// WHY: SDK は zod → JSON Schema 変換の target を draft-7 に固定しており
//      (server/zod-json-schema-compat.js)、Anthropic API は draft 2020-12 で
//      input_schema を検証するため z.tuple() などがそのままだと 400 になる。
//      SDK に target を渡す口が無いので、登録済みハンドラを一段包んで変換する。
//      transport ではなくここで包むことで stdio / in-memory の双方に効く。
function InstallDraft2020ToolSchemas(server) {
    const handlers = server.server._requestHandlers;
    const original = handlers?.get('tools/list');
    if (!handlers || !original) {
        // SDK の内部構造が変わった場合。ツール一覧自体は動くので落とさず警告に留める。
        console.error('[FBZZ Editor MCP] warn: tools/list ハンドラを包めず、JSON Schema は draft-07 のままです');
        return;
    }
    handlers.set('tools/list', async (request, extra) => ToDraft2020ToolsResult(await original(request, extra)));
}
// stdio 以外の transport を後日追加しても同じツール実装を再利用できる factory を提供する。
export function CreateEditorMcpServer(bus, permission) {
    const server = new McpServer({
        name: 'fbzz-editor',
        version: '0.1.0',
    }, {
        instructions: [
            'FBZZ Editor をローカル Command Bus 経由で操作します。',
            `現在の権限モードは ${permission} です。`,
            '編集前に editor_catalog で正確なコンポーネント型・フィールド契約を確認し、scene_find / scene_get_tree と node_get_components で対象を特定してください。',
            '自律作業は editor_perceive → dry-run/write Command → editor_perceive を反復し、見た目と Scene 状態の両方で完了条件を確認してください。',
            'VFX制作は vfx_knowledge_catalog → vfx_candidate_fork → schema駆動編集 → vfx_candidate_evaluate → vfx_candidate_accept → vfx_knowledge_promote の閉ループを使い、元アセットを直接試行錯誤で上書きしないでください。',
            '実行確認は可能なら playtest_run を使い、今回発生したログ、性能、意味付きviewportを一つのレポートで検証してください。',
        ].join(' '),
    });
    RegisterEditorTools(server, bus, permission);
    InstallDraft2020ToolSchemas(server);
    return server;
}
//# sourceMappingURL=server.js.map