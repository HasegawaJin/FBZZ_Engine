/// @file    ScriptTools.ts
/// @brief   C# SDK と Script の読取専用 MCP 操作。
/// @author  Hasegawa Jin
/// @date    2026-11-06
import * as z from 'zod/v4';
import { NodeIdSchema } from '../editorContracts.js';
import { TextResult, Safely } from './ToolSupport.js';
export function RegisterScriptQueryTools(server, bus) {
    server.registerTool('script_inspect', {
        description: 'node_get_components の scriptId で設定・観測の軽量 summary を取得します。大きな値は deferred path を返します。'
            + '応答の savedScriptId は保存・再読込でも保持される ID で、入力の scriptId は現在の個体 handle です。'
            + 'path と start/count を指定すると選択 subtree の不変 snapshot を作成し、observationId でページを再利用します。'
            + '文字列の start/count は UTF-8 byte 境界、返された nextStart を使用してください。close:true で保持資源を解放します。',
        inputSchema: {
            id: NodeIdSchema, scriptId: z.string().min(1).max(128), path: z.string().optional(),
            start: z.number().int().min(0).max(Number.MAX_SAFE_INTEGER).optional(), count: z.number().int().min(1).max(2147483647).optional(),
            wireBytes: z.number().int().min(2048).max(2147483647).optional(), observationId: z.string().regex(/^[1-9][0-9]*$/).optional(), close: z.boolean().optional()
        },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ id, scriptId, path, start, count, wireBytes, observationId, close }) => Safely(async () => TextResult(await bus.Query({
        t: 'script.inspect', id, scriptId,
        ...(path === undefined ? {} : { path }), ...(start === undefined ? {} : { start }), ...(count === undefined ? {} : { count }),
        ...(wireBytes === undefined ? {} : { wireBytes }), ...(observationId === undefined ? {} : { observationId }), ...(close === undefined ? {} : { close }),
    }))));
    server.registerTool('script_api_catalog', {
        description: '公開済み C# SDK の型・メソッド・property・引数・既定値・nullability・契約コメントを返します。'
            + 'Script の保存 field catalog は script_catalog。API の読取はメソッドの実行権限を与えません。',
        inputSchema: {
            type: z.string().min(1).max(256).optional().describe('SDK 型名または完全名。省略で全公開型'),
            member: z.string().min(1).max(128).optional().describe('指定 member の全 overload を返す'),
        },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ type, member }) => Safely(async () => TextResult(await bus.Query({
        t: 'script.api.catalog', ...(type === undefined ? {} : { type }), ...(member === undefined ? {} : { member }),
    }))));
    server.registerTool('script_catalog', {
        description: '登録 Script のフィールド型・保存対象・依存 Component を取得します。観測 getter は実行しません。現在値は script_inspect で取得します。',
        inputSchema: { type: z.string().min(1).max(128).optional() },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ type }) => Safely(async () => TextResult(await bus.Query({ t: 'script.catalog', ...(type === undefined ? {} : { type }) }))));
}
//# sourceMappingURL=ScriptTools.js.map