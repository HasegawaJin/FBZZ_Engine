/// @file    server.ts
/// @brief   transport 非依存の MCP サーバと Editor ツール集合を生成する。
/// @author  Hasegawa Jin
/// @date    2026-07-20
import { McpServer } from '@modelcontextprotocol/sdk/server/mcp.js';
import { ToDraft2020ToolsResult } from './jsonSchemaDraft2020.js';
import { RegisterEditorTools } from './tools.js';
/// @brief tools/list の応答を draft 2020-12 へ正規化する。
/// @note SDK 側の draft-07 を 2020-12 の固定長配列などに変換し、両方の transport で共有する。
function InstallDraft2020ToolSchemas(server) {
    const handlers = server.server._requestHandlers;
    const original = handlers?.get('tools/list');
    if (!handlers || !original) {
        // SDK の内部構造が変わった場合も既定形式の一覧を利用できる。
        console.error('[FBZZ Editor MCP] warn: tools/list ハンドラを包めず、JSON Schema は draft-07 のままです');
        return;
    }
    handlers.set('tools/list', async (request, extra) => ToDraft2020ToolsResult(await original(request, extra)));
}
export function CreateEditorMcpServer(bus, permission) {
    const server = new McpServer({
        name: 'fbzz-editor',
        version: '0.1.0',
    }, {
        instructions: [
            'FBZZ Editor をローカル Command Bus 経由で操作します。',
            `現在の権限モードは ${permission} です。`,
            'editor_op_list で操作の種類・必須引数・ダイアログの要否を調べ、照会操作は editor_op_query で呼び出します。',
            'Prefab を含む一括移動・改名は asset.move_batch.preview で本体と .meta の新旧パス・GUID を確認します。',
            'GUID の参照先・影響する参照元と参照切れは asset_dependencies_scan と asset_dependencies_status で調べます。',
            'コンポーネントの型とフィールドは editor_catalog、対象の ID と状態は scene_find と node_get_components で確認します。',
            'VFX の操作は editor_op_list で検索し、流体の設定項目は fluid_schema で確認します。',
            'シーンの状態は scene_validate、見た目は editor_perceive、Playtest の一覧と結果は scenario_list と scenario_status で確認します。',
            'Scene View の Collider などのオーバーレイ表示は editor_op_query の viewport.overlays.inspect で確認します。',
            'viewport_capture と viewport_capture_semantic の provenance で、描画元のシーン・カメラ・パス・フレームを確認します。',
            ...(permission === 'read' ? [] : [
                '変更操作は editor_op_invoke の dryRun=true で引数と実行条件を確認します。ファイルを直接指定してシーンを開くには scene_open、保存には scene_save を使います。',
                'オーバーレイ表示は editor_op_invoke の viewport.overlays.configure で指定した項目だけ設定します。Collider は args.colliders に true / false を渡します。',
            ]),
            ...(permission === 'write' ? [
                '確認済みの一括移動・改名は asset.move_batch.apply に直近の preview が返した receipt を渡して実行します。',
                'ファイルに保存された Playtest は scenario_run で開始し、scenario_status で今回の合否を確認します。',
                '新規の流体エフェクトは fluid_create_effect で作り、fluid_preview と fluid_job_status で見た目を確認してから fluid_bake を実行します。',
            ] : []),
        ].join('\n'),
    });
    RegisterEditorTools(server, bus, permission);
    InstallDraft2020ToolSchemas(server);
    return server;
}
//# sourceMappingURL=server.js.map