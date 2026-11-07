/// @file    SqlTools.ts
/// @brief   Scene と既存アセット参照を読み取り専用 SQL で検索する MCP 操作。
/// @author  Hasegawa Jin
/// @date    2026-11-06
import * as z from 'zod/v4';
import type { McpServer } from '@modelcontextprotocol/sdk/server/mcp.js';
import type { EditorBus } from '../busClient.js';
import { SceneSqlIndex } from '../Analysis/SceneSqlIndex.js';
import { SQL_LIMITS } from '../Analysis/SqlSchema.js';
import { Safely, TextResult } from './ToolSupport.js';

/// @note read権限でも利用できる。索引の作成とSQLはSceneやアセットの保存値を書き換えない。
export function RegisterSqlTools(server: McpServer, bus: EditorBus): void {
    const index = new SceneSqlIndex();
    const annotations = { readOnlyHint: true, openWorldHint: false };
    server.registerTool('sql_index_refresh', {
        description: '現在のScene snapshotをSQLite検索用索引へ取得します。ScriptはinspectNodeLimit件までのノードを順に照会します。'
            + 'referenceJobsは既存asset.references.scanのjobです。新しい参照scanは開始しません。'
            + 'generation・coverage・warningsを確認してください。全プロジェクトや動的参照の網羅は保証しません。',
        inputSchema: {
            inspectNodeLimit: z.number().int().min(0).max(512).default(128),
            referenceJobs: z.array(z.number().int().positive().max(Number.MAX_SAFE_INTEGER)).max(16).optional(),
        }, annotations,
    }, ({ inspectNodeLimit, referenceJobs }) => Safely(async () => TextResult(await index.Refresh(bus, { inspectNodeLimit, referenceJobs }))));
    server.registerTool('sql_index_status', {
        description: 'このMCPセッションの索引世代・取得元・範囲・欠測・表件数を返します。現在内容への自動更新はしません。',
        inputSchema: {}, annotations,
    }, () => Safely(async () => TextResult(index.Status())));
    server.registerTool('sql_index_schema', {
        description: 'SQLの表・列・制限・JOIN例を返します。実際の列名はこのschemaを使用してください。',
        inputSchema: {}, annotations,
    }, () => Safely(async () => TextResult(index.Schema())));
    server.registerTool('sql_query', {
        description: '取得済みgenerationへの単一SELECT/CTEを実行します。位置パラメーター(?)にparametersを渡します。'
            + 'DDL/DML/PRAGMA/ATTACH/拡張読込/複数文は拒否します。実行は3秒以内、結果は最大500行/256 KiBです。'
            + 'truncated・coverageを確認してください。Sceneの内容変更による古さは自動検出しません。',
        inputSchema: {
            generation: z.string().uuid(),
            sql: z.string().min(1).max(SQL_LIMITS.queryBytes),
            parameters: z.array(z.union([z.string().max(SQL_LIMITS.cellBytes), z.number().finite(), z.null()])).max(128).default([]),
            maxRows: z.number().int().min(1).max(SQL_LIMITS.resultRows).default(100),
            maxBytes: z.number().int().min(1024).max(SQL_LIMITS.resultBytes).default(SQL_LIMITS.resultBytes),
        }, annotations,
    }, ({ generation, sql, parameters, maxRows, maxBytes }) => Safely(async () => TextResult(await index.Query(bus, generation, sql, parameters, maxRows, maxBytes))));
}
