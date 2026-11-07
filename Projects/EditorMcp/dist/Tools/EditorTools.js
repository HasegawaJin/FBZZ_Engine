/// @file    EditorTools.ts
/// @brief   Editor の観測・Operator・履歴・複合操作を MCP へ公開する。
/// @author  Hasegawa Jin
/// @date    2026-11-06
import * as z from 'zod/v4';
import { EditorCommandSchema, SemanticViewportResultSchema, ViewportCaptureResultSchema } from '../editorContracts.js';
import { DiffSceneSnapshots, Delay, TextResult, Safely, CreateCommandContext } from './ToolSupport.js';
export function RegisterEditorQueryTools(server, bus) {
    server.registerTool('editor_catalog', {
        description: '登録済みコンポーネントとcomponent_add/setで使える正確な型名・フィールド型・enumラベル・range・既定値を取得します。編集前に必ず参照してください。',
        inputSchema: {},
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, () => Safely(async () => TextResult(await bus.Query({ t: 'editor.catalog' }))));
    /// @note 操作の引数と実行可否は Editor の登録簿を正本とし、MCP へ個別に複製しない。
    /// @see Docs/design/EditorDirectApi.md
    server.registerTool('editor_op_list', {
        description: 'Editor に登録された操作 (Operator) の目録を返します。'
            + 'メニュー・ホットキー・コマンドパレットが読むのと同じ登録簿なので、'
            + '「人が UI からできること」と一致します。'
            + '各項目の available は実行可能条件 (poll) の評価結果で、'
            + '実行前に「今できない理由がある」ことを判別できます。'
            + 'kind は query / action / mutation で、mutation だけが Undo 履歴に残ります。'
            + 'params が付いている操作は editor_op_invoke の args にその名前で渡します。',
        inputSchema: {
            search: z.string().min(1).max(128).optional()
                .describe('id / label / desc の部分一致 (大小無視)'),
            category: z.string().min(1).max(64).optional()
                .describe('File / Edit / Selection / Viewport / Gizmo / Play / Tools / Panels'),
            includeUnavailable: z.boolean().optional()
                .describe('既定 true。false にすると今実行できる操作だけを返す'),
        },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ search, category, includeUnavailable }) => Safely(async () => TextResult(await bus.Query({
        t: 'editor.op.list',
        ...(search === undefined ? {} : { search }),
        ...(category === undefined ? {} : { category }),
        ...(includeUnavailable === undefined ? {} : { includeUnavailable }),
    }))));
    /// @note 読み取り側の Operator。invoke (write) と入口を分けてあるので read 権限でも呼べる。
    server.registerTool('editor_op_query', {
        description: 'kind=query の Operator を実行し、結果データを返します。'
            + 'editor_op_list で id と params を調べてから呼びます。'
            + '書き込み系 (action / mutation) はここでは拒否され、editor_op_invoke を使います。',
        inputSchema: {
            id: z.string().min(1).max(128)
                .describe('operator の id (例: "panel.list")'),
            args: z.record(z.string(), z.unknown()).optional()
                .describe('editor_op_list の params に対応する引数'),
        },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ id, args }) => Safely(async () => TextResult(await bus.Query({
        t: 'editor.op.query',
        id,
        ...(args === undefined ? {} : { args }),
    }))));
    server.registerTool('editor_catalog_search', {
        description: 'Component API を型名・表示名・カテゴリ・フィールド名・tooltipから検索し、必要な項目だけ返します。',
        inputSchema: {
            query: z.string().min(1).max(128).optional(),
            category: z.string().min(1).max(128).optional(),
            limit: z.number().int().min(1).max(100).default(25),
        },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ query, category, limit }) => Safely(async () => TextResult(await bus.Query({
        t: 'editor.catalog.search',
        ...(query === undefined ? {} : { query }),
        ...(category === undefined ? {} : { category }),
        limit,
    }))));
    server.registerTool('editor_get_state', {
        description: 'Play/Pause/停止状態、復元待ち、現在のシーンを取得します。実行制御後の確認に使います。',
        inputSchema: {},
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, () => Safely(async () => TextResult(await bus.Query({ t: 'editor.state' }))));
    server.registerTool('editor_get_undo_history', {
        description: 'Undo/Redoスタックを取得します。canUndo/canRedo、cursor、次にundo/redoされる操作の説明、履歴エントリ(index・ラベル・applied)を新しい順で返します。自分の編集が期待どおりのラベルで残ったか検証したり、何回undoで戻れるか判断するのに使います。',
        inputSchema: { limit: z.number().int().min(1).max(128).default(50) },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ limit }) => Safely(async () => TextResult(await bus.Query({ t: 'editor.undoHistory', limit }))));
    server.registerTool('console_get_logs', {
        description: 'Editorログを新しい順に取得します。応答cursorを次回afterSequenceへ渡すと、その後に発生したログだけを取得できます。',
        inputSchema: {
            minLevel: z.enum(['debug', 'info', 'warning', 'error']).default('debug'),
            contains: z.string().min(1).max(256).optional(),
            limit: z.number().int().min(1).max(512).default(100),
            afterSequence: z.number().int().nonnegative().max(Number.MAX_SAFE_INTEGER).optional(),
        },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ minLevel, contains, limit, afterSequence }) => Safely(async () => TextResult(await bus.Query({
        t: 'console.logs',
        minLevel,
        ...(contains === undefined ? {} : { contains }),
        ...(afterSequence === undefined ? {} : { afterSequence }),
        limit,
    }))));
    server.registerTool('editor_perceive', {
        description: 'Scene、選択、現在の viewport PNG を1つの知覚スナップショットとして取得します。自律ループの各反復の先頭と末尾で使います。',
        inputSchema: {
            w: z.number().int().min(160).max(1920).default(960),
            h: z.number().int().min(90).max(1080).default(540),
        },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ w, h }) => Safely(async () => {
        const [state, tree, selection, logs, rawCapture] = await Promise.all([
            bus.Query({ t: 'editor.state' }),
            bus.Query({ t: 'scene.tree' }),
            bus.Query({ t: 'scene.selection' }),
            bus.Query({ t: 'console.logs', minLevel: 'warning', limit: 50 }),
            bus.Query({ t: 'viewport.capture', w, h }),
        ]);
        const capture = ViewportCaptureResultSchema.parse(rawCapture);
        return {
            content: [
                { type: 'text', text: JSON.stringify({ state, tree, selection, logs }, null, 2) },
                { type: 'image', data: capture.base64, mimeType: capture.mimeType },
            ],
            structuredContent: { state, tree, selection, logs, width: capture.width, height: capture.height },
        };
    }));
    server.registerTool('profiler_get_snapshot', {
        description: 'FPS/フレーム時間、draw call、三角形、メモリ、重いCPU sampleを取得します。',
        inputSchema: { limit: z.number().int().min(1).max(100).default(20) },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ limit }) => Safely(async () => TextResult(await bus.Query({ t: 'profiler.snapshot', limit }))));
    server.registerTool('editor_wait', {
        description: 'Editorを停止させずに、Play状態・ロード完了・ログ・衝突条件をポーリングして待機します。',
        inputSchema: {
            event: z.enum(['playState', 'sceneReady', 'log', 'collision']),
            value: z.string().max(256).optional().describe('playState名、ログ部分文字列、衝突NodeId'),
            timeoutMs: z.number().int().min(100).max(60000).default(10000),
            pollMs: z.number().int().min(25).max(1000).default(100),
            afterSequence: z.number().int().nonnegative().max(Number.MAX_SAFE_INTEGER).optional()
                .describe('log待機の開始カーソル。省略時は呼び出し時点の最新ログから待つ'),
        },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ event, value, timeoutMs, pollMs, afterSequence }) => Safely(async () => {
        const deadline = Date.now() + timeoutMs;
        let logCursor = afterSequence;
        if (event === 'log' && logCursor === undefined) {
            const baseline = await bus.Query({ t: 'console.logs', minLevel: 'debug', limit: 1 });
            logCursor = Number(baseline.cursor ?? 0);
        }
        do {
            if (event === 'playState' || event === 'sceneReady') {
                const state = await bus.Query({ t: 'editor.state' });
                const matched = event === 'playState'
                    ? state.playState === value
                    : state.managedBuildBusy === false && state.restorePending === false;
                if (matched)
                    return TextResult({ matched: true, event, state });
            }
            else if (event === 'log') {
                if (value === undefined)
                    throw new Error('log 待機には value（部分文字列）が必要です');
                const logs = await bus.Query({
                    t: 'console.logs', minLevel: 'debug', contains: value, limit: 20,
                    ...(logCursor === undefined ? {} : { afterSequence: logCursor }),
                });
                if (Number(logs.count ?? 0) > 0)
                    return TextResult({ matched: true, event, logs });
                logCursor = Number(logs.cursor ?? logCursor ?? 0);
            }
            else {
                const events = await bus.Query({ t: 'physics.events' });
                const all = [...(events.enter ?? []), ...(events.stay ?? []), ...(events.exit ?? [])];
                const match = all.find((item) => value === undefined || item.a === value || item.b === value);
                if (match !== undefined)
                    return TextResult({ matched: true, event, match });
            }
            await Delay(pollMs);
        } while (Date.now() < deadline);
        return TextResult({ matched: false, event, timeoutMs, ...(logCursor === undefined ? {} : { cursor: logCursor }) });
    }));
    server.registerTool('editor_bus_list', {
        description: 'Editor が受け付ける Command Bus の型名と種別 (query / command / undoable) を返します。'
            + 'undoable と transaction=true の型だけが editor_transaction に混ぜられます。'
            + 'ツールが UNKNOWN_COMMAND / UNKNOWN_QUERY で失敗したら、Editor のビルドが MCP より古い可能性をここで確かめます。',
        inputSchema: {},
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, () => Safely(async () => TextResult(await bus.Query({ t: 'editor.bus.list' }))));
}
export function RegisterEditorCommandTools(server, bus, permission) {
    const { dryRun, run, writeAnnotations } = CreateCommandContext(bus, permission);
    /// @note ── Operator ゲートウェイ ──
    /// @note editor_op_list で見つけた操作をそのまま実行する。実行可否の判定は Editor 側の
    /// @note poll が持つので、ホットキーやメニューでグレーアウトされる状況ではここも拒否される
    /// @note (AI にだけできる操作、という抜け道を作らない)。
    server.registerTool('editor_op_invoke', {
        description: 'editor_op_list に載っている操作を実行します。'
            + 'メニューやホットキーが呼ぶのと同一の実体を通るため、人が UI で行った場合と結果が一致します。'
            + '実行可能条件を満たさない場合は NOT_AVAILABLE で拒否され、シーンには触れません。'
            + 'args は operator が宣言した params の名前で渡します (未宣言のキーはエラー)。'
            + 'dry-run 権限では引数検証と実行可否の判定だけを行います。',
        inputSchema: {
            id: z.string().min(1).max(128)
                .describe('operator の id (例: "scene.save" / "gizmo.move")'),
            args: z.record(z.string(), z.unknown()).optional()
                .describe('editor_op_list の params に対応する引数。引数なしの操作では省略する'),
        },
        annotations: writeAnnotations,
    }, ({ id, args }) => run({
        t: 'editor.op.invoke',
        id,
        ...(args === undefined ? {} : { args }),
    }));
    server.registerTool('editor_undo', {
        description: '直前の Editor Command を取り消します。',
        inputSchema: {},
        annotations: writeAnnotations,
    }, () => run({ t: 'editor.undo' }));
    server.registerTool('editor_redo', {
        description: '取り消した Editor Command を再適用します。',
        inputSchema: {},
        annotations: writeAnnotations,
    }, () => run({ t: 'editor.redo' }));
    server.registerTool('run_transaction', {
        description: '対応する複数 Command を1つの Undo 単位として実行します。legacy Command の一般的な失敗時atomic保証はありません。',
        inputSchema: {
            label: z.string().min(1).max(128),
            cmds: z.array(EditorCommandSchema).min(1).max(128),
        },
        annotations: writeAnnotations,
    }, ({ label, cmds }) => run({ t: 'editor.transaction', label, cmds }));
    server.registerTool('run_transaction_and_observe', {
        description: '複数 Command を1 Undo単位で実行し、Scene差分・lint・状態・ログ・意味付きviewportを返します。知覚→操作→自己検証ループ用です。',
        inputSchema: {
            label: z.string().min(1).max(128),
            cmds: z.array(EditorCommandSchema).min(1).max(128),
            w: z.number().int().min(160).max(1920).default(960),
            h: z.number().int().min(90).max(1080).default(540),
        },
        annotations: writeAnnotations,
    }, ({ label, cmds, w, h }) => Safely(async () => {
        const before = await bus.Query({ t: 'scene.snapshot' });
        const commandResult = await bus.Command(EditorCommandSchema.parse({ t: 'editor.transaction', label, cmds }), dryRun);
        const [after, validation, state, logs, rawCapture] = await Promise.all([
            bus.Query({ t: 'scene.snapshot' }),
            bus.Query({ t: 'scene.validate' }),
            bus.Query({ t: 'editor.state' }),
            bus.Query({ t: 'console.logs', minLevel: 'warning', limit: 100 }),
            bus.Query({ t: 'viewport.semantic', w, h }),
        ]);
        const capture = SemanticViewportResultSchema.parse(rawCapture);
        const diff = DiffSceneSnapshots(before, after);
        return {
            content: [
                { type: 'text', text: JSON.stringify({ commandResult, diff, validation, state, logs, objects: capture.objects }, null, 2) },
                { type: 'image', data: capture.base64, mimeType: capture.mimeType },
            ],
            structuredContent: {
                commandResult, diff, validation, state, logs,
                width: capture.width, height: capture.height,
                cameraPosition: capture.cameraPosition, objects: capture.objects,
            },
        };
    }));
}
//# sourceMappingURL=EditorTools.js.map