// FBZZ Engine
// server.ts | EditorMcp
// transport 非依存の MCP サーバと Editor ツール集合を生成する
import { McpServer } from '@modelcontextprotocol/sdk/server/mcp.js';
import type { EditorBus } from './busClient.js';
import type { PermissionMode } from './config.js';
import { RegisterEditorTools } from './tools.js';

// stdio 以外の transport を後日追加しても同じツール実装を再利用できる factory を提供する。
export function CreateEditorMcpServer(bus: EditorBus, permission: PermissionMode): McpServer {
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
    return server;
}
