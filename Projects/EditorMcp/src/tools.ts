// FBZZ Engine
// tools.ts | EditorMcp
// Editor Query / Command を MCP ツールへ薄く写像し、権限モードを強制する
import * as z from 'zod/v4';
import type { McpServer } from '@modelcontextprotocol/sdk/server/mcp.js';
import type { CallToolResult } from '@modelcontextprotocol/sdk/types.js';
import type { EditorBus } from './busClient.js';
import type { PermissionMode } from './config.js';
import {
    EditorCommandSchema,
    AssetThumbnailResultSchema,
    JsonValueSchema,
    NodeIdSchema,
    SemanticViewportResultSchema,
    Vec3Schema,
    ViewportCaptureResultSchema,
    type EditorCommand,
} from './editorContracts.js';

const NameSchema = z.string().min(1).max(128);
const ComponentSchema = z.string().min(1).max(128);
const InputInjectionShape = {
    kind: z.enum(['key', 'axis', 'gamepadButton', 'gamepadAxis', 'mouseButton', 'mousePosition', 'mouseDelta', 'mouseScroll', 'clear']),
    key: z.string().min(1).max(32).optional(),
    buttonName: z.string().min(1).max(64).optional(),
    pressed: z.boolean().optional(),
    axis: z.string().min(1).max(64).optional(),
    button: z.number().int().min(0).max(2).optional(),
    value: z.union([z.number().finite(), z.tuple([z.number().finite(), z.number().finite()])]).optional(),
};
const InputInjectionSchema = z.object(InputInjectionShape).strict().superRefine((input, context) => {
    const isNumber = typeof input.value === 'number';
    const isVector = Array.isArray(input.value);
    const valid = input.kind === 'clear'
        || (input.kind === 'key' && input.key !== undefined && input.pressed !== undefined)
        || ((input.kind === 'axis' || input.kind === 'gamepadAxis') && input.axis !== undefined && isNumber)
        || (input.kind === 'gamepadButton' && input.buttonName !== undefined && input.pressed !== undefined)
        || (input.kind === 'mouseButton' && input.button !== undefined && input.pressed !== undefined)
        || ((input.kind === 'mousePosition' || input.kind === 'mouseDelta') && isVector)
        || (input.kind === 'mouseScroll' && isNumber);
    if (!valid) context.addIssue({ code: 'custom', message: `kind=${input.kind} に必要な入力が不足しています` });
});

type InputInjection = z.infer<typeof InputInjectionSchema>;

// MCP入力からundefinedを除き、Command Busの厳密なinput.inject契約へ変換する。
function ToInputCommand(input: InputInjection): EditorCommand {
    return {
        t: 'input.inject', kind: input.kind,
        ...(input.key === undefined ? {} : { key: input.key }),
        ...(input.buttonName === undefined ? {} : { buttonName: input.buttonName }),
        ...(input.pressed === undefined ? {} : { pressed: input.pressed }),
        ...(input.axis === undefined ? {} : { axis: input.axis }),
        ...(input.button === undefined ? {} : { button: input.button }),
        ...(input.value === undefined ? {} : { value: input.value }),
    };
}

interface SceneSnapshotLike {
    nodes?: Array<Record<string, unknown>>;
    count?: number;
}

// NodeId をキーに正規化済み Scene 状態を比較し、追加・削除・変更を小さい差分で返す。
function DiffSceneSnapshots(beforeValue: unknown, afterValue: unknown): Record<string, unknown> {
    const before = beforeValue as SceneSnapshotLike;
    const after = afterValue as SceneSnapshotLike;
    const beforeNodes = new Map((before.nodes ?? []).map((node) => [String(node.id), node]));
    const afterNodes = new Map((after.nodes ?? []).map((node) => [String(node.id), node]));
    const added: unknown[] = [];
    const removed: unknown[] = [];
    const changed: unknown[] = [];
    for (const [id, node] of afterNodes) {
        const previous = beforeNodes.get(id);
        if (previous === undefined) added.push(node);
        else if (JSON.stringify(previous) !== JSON.stringify(node)) changed.push({ id, before: previous, after: node });
    }
    for (const [id, node] of beforeNodes) {
        if (!afterNodes.has(id)) removed.push(node);
    }
    return {
        added,
        removed,
        changed,
        counts: { added: added.length, removed: removed.length, changed: changed.length },
    };
}

const Delay = (milliseconds: number): Promise<void> => new Promise((resolve) => setTimeout(resolve, milliseconds));

// engine の JSON 応答を MCP text content に変換する。
function TextResult(value: unknown): CallToolResult {
    return {
        content: [{ type: 'text', text: JSON.stringify(value, null, 2) }],
    };
}

// transport/engine の失敗をプロセス例外にせず、AI が再判断できる tool error として返す。
async function Safely(operation: () => Promise<CallToolResult>): Promise<CallToolResult> {
    try {
        return await operation();
    } catch (error) {
        const message = error instanceof Error ? error.message : '不明な Editor Command Bus エラー';
        return {
            isError: true,
            content: [{ type: 'text', text: message }],
        };
    }
}

// Stage B ではユーザー入力に関係なく dryRun を強制し、Stage C だけ実変更を許可する。
function ShouldDryRun(permission: PermissionMode): boolean {
    return permission !== 'write';
}

// Stage A で公開する副作用なし Query とオンデマンド viewport capture を登録する。
function RegisterQueryTools(server: McpServer, bus: EditorBus): void {
    const sceneSnapshots = new Map<string, unknown>();
    let snapshotSequence = 0;
    server.registerTool('editor_catalog', {
        description: '登録済みコンポーネントとcomponent_add/setで使える正確な型名・フィールド型・enumラベル・range・既定値を取得します。編集前に必ず参照してください。',
        inputSchema: {},
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, () => Safely(async () => TextResult(await bus.Query({ t: 'editor.catalog' }))));

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

    server.registerTool('scene_get_tree', {
        description: '現在のシーン階層を取得します。副作用はありません。',
        inputSchema: {},
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, () => Safely(async () => TextResult(await bus.Query({ t: 'scene.tree' }))));

    server.registerTool('scene_find', {
        description: '名前・タグ・active状態・複数コンポーネント・反射プロパティ値をAND条件で一括検索します。',
        inputSchema: {
            name: z.string().min(1).max(128).optional().describe('大文字小文字を無視する名前部分一致'),
            tag: z.string().min(1).max(128).optional().describe('タグの完全一致'),
            active: z.boolean().optional().describe('activeSelf の一致'),
            comp: ComponentSchema.optional().describe('editor_catalogが返す正確なコンポーネント型名'),
            components: z.array(ComponentSchema).min(1).max(32).optional().describe('すべて装着されている必要があるコンポーネント型名'),
            properties: z.array(z.object({
                comp: ComponentSchema,
                field: z.string().min(1).max(128),
                op: z.enum(['equals', 'notEquals', 'contains', 'greater', 'less']).default('equals'),
                value: JsonValueSchema,
            }).strict()).min(1).max(32).optional().describe('反射フィールドに対するANDフィルタ'),
            limit: z.number().int().min(1).max(500).default(100),
        },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ name, tag, active, comp, components, properties, limit }) => Safely(async () => TextResult(await bus.Query({
        t: 'scene.find',
        ...(name === undefined ? {} : { name }),
        ...(tag === undefined ? {} : { tag }),
        ...(active === undefined ? {} : { active }),
        ...(comp === undefined ? {} : { comp }),
        ...(components === undefined ? {} : { components }),
        ...(properties === undefined ? {} : { properties }),
        limit,
    }))));

    server.registerTool('scene_snapshot', {
        description: '現在の Scene を安定NodeId・Transform・Component値で記録し、後の scene_diff 用IDを返します。',
        inputSchema: { label: z.string().min(1).max(128).optional() },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ label }) => Safely(async () => {
        const snapshot = await bus.Query({ t: 'scene.snapshot' });
        const id = `snapshot-${Date.now()}-${++snapshotSequence}`;
        sceneSnapshots.set(id, snapshot);
        while (sceneSnapshots.size > 16) sceneSnapshots.delete(sceneSnapshots.keys().next().value as string);
        const value = snapshot as SceneSnapshotLike;
        return TextResult({ id, label, count: value.count ?? value.nodes?.length ?? 0 });
    }));

    server.registerTool('scene_diff', {
        description: 'scene_snapshot の記録と現在状態を比較し、追加・削除・変更ノードを返します。',
        inputSchema: { snapshotId: z.string().min(1) },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ snapshotId }) => Safely(async () => {
        const before = sceneSnapshots.get(snapshotId);
        if (before === undefined) throw new Error(`Scene snapshot が見つかりません: ${snapshotId}`);
        const after = await bus.Query({ t: 'scene.snapshot' });
        return TextResult({ snapshotId, ...DiffSceneSnapshots(before, after) });
    }));

    server.registerTool('scene_validate', {
        description: '重複名/ID、NaN・ゼロscale、Prefab・Material・Animator参照切れをまとめて検出します。',
        inputSchema: {},
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, () => Safely(async () => TextResult(await bus.Query({ t: 'scene.validate' }))));

    server.registerTool('scene_get_selection', {
        description: '現在選択されている NodeId の一覧を取得します。',
        inputSchema: {},
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, () => Safely(async () => TextResult(await bus.Query({ t: 'scene.selection' }))));

    server.registerTool('node_get_components', {
        description: '指定ノードのコンポーネントと反射可能なフィールドを取得します。',
        inputSchema: { id: NodeIdSchema.describe('対象 NodeId') },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ id }) => Safely(async () => TextResult(await bus.Query({ t: 'node.components', id }))));

    server.registerTool('asset_list', {
        description: 'プロジェクト内のアセット一覧を取得します。ファイルは変更しません。',
        inputSchema: { dir: z.string().min(1).optional().describe('プロジェクト相対ディレクトリ') },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ dir }) => Safely(async () => TextResult(await bus.Query({
        t: 'asset.list',
        ...(dir === undefined ? {} : { dir }),
    }))));

    server.registerTool('asset_inspect', {
        description: 'アセットのサイズ・更新時刻・参照先・参照元・直接サムネイル可否を取得します。',
        inputSchema: { path: z.string().min(1).describe('projectRoot 相対パス') },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ path }) => Safely(async () => TextResult(await bus.Query({ t: 'asset.inspect', path }))));

    server.registerTool('asset_find_unused', {
        description: 'Scene/Prefab/Material等のテキスト参照を走査し、未参照候補を返します。結果はheuristicです。',
        inputSchema: { limit: z.number().int().min(1).max(500).default(100) },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ limit }) => Safely(async () => TextResult(await bus.Query({ t: 'asset.findUnused', limit }))));

    server.registerTool('asset_thumbnail', {
        description: 'PNG/JPEGアセットをMCP画像として直接取得します。モデル等の生成プレビューは対象外です。',
        inputSchema: { path: z.string().min(1) },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ path }) => Safely(async () => {
        const thumbnail = AssetThumbnailResultSchema.parse(await bus.Query({ t: 'asset.thumbnail', path }));
        return {
            content: [{ type: 'image', data: thumbnail.base64, mimeType: thumbnail.mimeType }],
            structuredContent: { path: thumbnail.path },
        };
    }));

    server.registerTool('vfx_inspect_graph', {
        description: '.vfxのノード、イベントリンク、SubGraph、Particle実行方式、Burst数、リソースbudgetと検証結果を構造化して取得します。',
        inputSchema: { path: z.string().min(1).describe('projectRoot相対または絶対の.vfxパス') },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ path }) => Safely(async () => TextResult(await bus.Query({ t: 'vfx.graph', path }))));

    server.registerTool('vfx_get_params', {
        description: '公開パラメーター、binding、defaultと、任意VFXGraphComponentのvariant・sparse override・解決済み値を取得します。',
        inputSchema: { path: z.string().min(1), id: NodeIdSchema.optional() },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ path, id }) => Safely(async () => TextResult(await bus.Query({
        t: 'vfx.params', path, ...(id === undefined ? {} : { id }),
    }))));

    server.registerTool('vfx_get_schema', {
        description: 'VFXノードのauthoring schemaを取得します。setFieldとparameter bindingに使えるpath・型・range・exposableが同じ定義から返ります。',
        inputSchema: {},
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, () => Safely(async () => TextResult(await bus.Query({ t: 'vfx.schema' }))));

    server.registerTool('vfx_preview', {
        description: '専用VFX WorldをrandomSeedから指定時刻へscrubし、UIやSceneを含まない決定論的PNGを返します。',
        inputSchema: {
            path: z.string().min(1),
            time: z.number().finite().nonnegative(),
            w: z.number().int().min(160).max(1920).default(960),
            h: z.number().int().min(90).max(1080).default(540),
        },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ path, time, w, h }) => Safely(async () => {
        const prepared = await bus.Query({ t: 'vfx.preview', path, time, w, h });
        // EditorのUpdate/LateUpdate/Renderへ最低2フレーム渡してから同じ専用RTを読み戻す。
        await Delay(80);
        const capture = ViewportCaptureResultSchema.parse(await bus.Query({ t: 'viewport.capture', w, h, view: 'vfx' }));
        return {
            content: [
                { type: 'text', text: JSON.stringify(prepared, null, 2) },
                { type: 'image', data: capture.base64, mimeType: capture.mimeType },
            ],
            structuredContent: { prepared, width: capture.width, height: capture.height, view: 'vfx' },
        };
    }));

    server.registerTool('material_inspect', {
        description: 'NodeのMaterialパス、解決済みShader、インスタンス別parameter overrideを取得します。',
        inputSchema: { id: NodeIdSchema },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ id }) => Safely(async () => TextResult(await bus.Query({ t: 'material.inspect', id }))));

    server.registerTool('animation_get_state', {
        description: 'Animatorの再生/停止、clip、秒・正規化時間、遷移状態、ロード済みclip一覧を取得します。',
        inputSchema: { id: NodeIdSchema },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ id }) => Safely(async () => TextResult(await bus.Query({ t: 'animation.state', id }))));

    server.registerTool('animation_get_graph', {
        description: 'Animatorステートマシンの構造(states/transitions/parameters・BlendTree駆動パラメーター・anyState遷移・遷移条件)を、パラメーターのライブ値と現在ステート付きで取得します。どのパラメーターがどの遷移を発火させるか把握するのに使います。',
        inputSchema: { id: NodeIdSchema },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ id }) => Safely(async () => TextResult(await bus.Query({ t: 'animation.graph', id }))));

    server.registerTool('animation_get_blend_tree', {
        description: '指定ステートのBlendTree構成を掘り下げ、駆動パラメーターとそのライブ値、各Motionのsource/clip/threshold(1D)またはposX/posY(2D)/speed/IK、直近フレームのランタイムWeightを返します。Clipステートやステート不在はエラーになります。',
        inputSchema: {
            id: NodeIdSchema,
            state: z.string().min(1).max(128).describe('animation_get_graph が返す BlendTree ステート名'),
        },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ id, state }) => Safely(async () => TextResult(await bus.Query({ t: 'animation.blendTree', id, state }))));

    server.registerTool('animation_get_pose', {
        description: '直近フレームで評価済みのSkeleton poseを、ノード名・親index・bone index・4x4 global matrixで取得します。',
        inputSchema: { id: NodeIdSchema, limit: z.number().int().min(1).max(512).default(128) },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ id, limit }) => Safely(async () => TextResult(await bus.Query({ t: 'animation.pose', id, limit }))));

    server.registerTool('profiler_get_snapshot', {
        description: 'FPS/フレーム時間、draw call、三角形、メモリ、重いCPU sampleを取得します。',
        inputSchema: { limit: z.number().int().min(1).max(100).default(20) },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ limit }) => Safely(async () => TextResult(await bus.Query({ t: 'profiler.snapshot', limit }))));

    server.registerTool('physics_raycast', {
        description: '物理 World へ Raycast または SphereCast を行い、ヒット位置・法線・距離・NodeId を数値で返します。',
        inputSchema: {
            origin: Vec3Schema,
            direction: Vec3Schema.describe('正規化前でも可。ゼロベクトルは不可'),
            maxDistance: z.number().finite().positive().max(100000),
            sphereRadius: z.number().finite().positive().max(10000).optional(),
        },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ origin, direction, maxDistance, sphereRadius }) => Safely(async () => TextResult(await bus.Query({
        t: 'physics.raycast', origin, direction, maxDistance,
        ...(sphereRadius === undefined ? {} : { sphereRadius }),
    }))));

    server.registerTool('physics_overlap_sphere', {
        description: '指定球と重なる Collider を検索し、対応する NodeId 一覧を返します。',
        inputSchema: { center: Vec3Schema, radius: z.number().finite().positive().max(10000) },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ center, radius }) => Safely(async () => TextResult(await bus.Query({
        t: 'physics.overlapSphere', center, radius,
    }))));

    server.registerTool('physics_get_events', {
        description: '直近物理フレームのCollision/Trigger enter・stay・exitをNodeId付きで取得します。',
        inputSchema: {},
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, () => Safely(async () => TextResult(await bus.Query({ t: 'physics.events' }))));

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
            const baseline = await bus.Query({ t: 'console.logs', minLevel: 'debug', limit: 1 }) as Record<string, unknown>;
            logCursor = Number(baseline.cursor ?? 0);
        }
        do {
            if (event === 'playState' || event === 'sceneReady') {
                const state = await bus.Query({ t: 'editor.state' }) as Record<string, unknown>;
                const matched = event === 'playState'
                    ? state.playState === value
                    : state.scriptReloadBusy === false && state.restorePending === false;
                if (matched) return TextResult({ matched: true, event, state });
            } else if (event === 'log') {
                if (value === undefined) throw new Error('log 待機には value（部分文字列）が必要です');
                const logs = await bus.Query({
                    t: 'console.logs', minLevel: 'debug', contains: value, limit: 20,
                    ...(logCursor === undefined ? {} : { afterSequence: logCursor }),
                }) as Record<string, unknown>;
                if (Number(logs.count ?? 0) > 0) return TextResult({ matched: true, event, logs });
                logCursor = Number(logs.cursor ?? logCursor ?? 0);
            } else {
                const events = await bus.Query({ t: 'physics.events' }) as Record<string, unknown[]>;
                const all = [...(events.enter ?? []), ...(events.stay ?? []), ...(events.exit ?? [])] as Array<Record<string, unknown>>;
                const match = all.find((item) => value === undefined || item.a === value || item.b === value);
                if (match !== undefined) return TextResult({ matched: true, event, match });
            }
            await Delay(pollMs);
        } while (Date.now() < deadline);
        return TextResult({ matched: false, event, timeoutMs, ...(logCursor === undefined ? {} : { cursor: logCursor }) });
    }));

    server.registerTool('viewport_capture', {
        description: '現在のネイティブ viewport を PNG として取得します。w/h は希望サイズで、応答には実RTサイズが返ります。',
        inputSchema: {
            w: z.number().int().min(160).max(1920).default(960).describe('PNG 幅'),
            h: z.number().int().min(90).max(1080).default(540).describe('PNG 高さ'),
            view: z.enum(['scene', 'game', 'vfx']).default('scene').describe('Scene / Game / 装飾なし VFX Preview'),
            cameraId: NodeIdSchema.optional().describe('省略時は active editor camera'),
        },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ w, h, view, cameraId }) => Safely(async () => {
        const raw = await bus.Query({
            t: 'viewport.capture',
            w,
            h,
            view,
            ...(cameraId === undefined ? {} : { cameraId }),
        });
        const capture = ViewportCaptureResultSchema.parse(raw);
        return {
            content: [{ type: 'image', data: capture.base64, mimeType: capture.mimeType }],
            structuredContent: {
                width: capture.width,
                height: capture.height,
                view: capture.view ?? view,
                ...(capture.cameraId === undefined ? {} : { cameraId: capture.cameraId }),
            },
        };
    }));

    server.registerTool('viewport_capture_semantic', {
        description: 'Viewport PNGと、各GameObjectのNodeId・ワールド座標・画像内ピクセル位置・可視判定を同時取得します。',
        inputSchema: {
            w: z.number().int().min(160).max(1920).default(960),
            h: z.number().int().min(90).max(1080).default(540),
            view: z.enum(['scene', 'game']).default('scene'),
            visibleOnly: z.boolean().default(true),
        },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ w, h, view, visibleOnly }) => Safely(async () => {
        const capture = SemanticViewportResultSchema.parse(await bus.Query({ t: 'viewport.semantic', w, h, view }));
        const objects = visibleOnly ? capture.objects.filter((object) => object.visible) : capture.objects;
        return {
            content: [
                { type: 'text', text: JSON.stringify({ width: capture.width, height: capture.height, view: capture.view ?? view, cameraPosition: capture.cameraPosition, objects }, null, 2) },
                { type: 'image', data: capture.base64, mimeType: capture.mimeType },
            ],
            structuredContent: { width: capture.width, height: capture.height, view: capture.view ?? view, cameraPosition: capture.cameraPosition, objects },
        };
    }));
}

// Stage B/C でのみ Command を登録し、MCP から engine の Undo 対応 Command Bus へ転送する。
function RegisterCommandTools(server: McpServer, bus: EditorBus, permission: PermissionMode): void {
    const dryRun = ShouldDryRun(permission);
    const run = (command: EditorCommand) => Safely(async () => TextResult(await bus.Command(EditorCommandSchema.parse(command), dryRun)));
    const writeAnnotations = {
        readOnlyHint: false,
        destructiveHint: permission === 'write',
        idempotentHint: false,
        openWorldHint: false,
    };

    server.registerTool('vfx_template_apply', {
        description: '検証済みExplosion/Fire/Smoke/Impact/Magicテンプレートを新しい.vfxへ複製します。dry-run・Undoに対応します。',
        inputSchema: {
            template: z.enum(['explosion', 'fire', 'smoke', 'impact', 'magic']),
            path: z.string().min(1).describe('projectRoot相対の出力.vfx'),
            name: z.string().min(1).max(128).optional(),
        }, annotations: writeAnnotations,
    }, ({ template, path, name }) => run({
        t: 'vfx.template.apply', template, path, ...(name === undefined ? {} : { name }),
    }));

    server.registerTool('vfx_optimize_budget', {
        description: '目標particle budgetへ発生数・上限を比例最適化し、距離LODと安全なGPU simulationを設定します。適用前dry-runとUndoに対応します。',
        inputSchema: { path: z.string().min(1), targetParticles: z.number().int().min(1).max(10000000) },
        annotations: writeAnnotations,
    }, ({ path, targetParticles }) => run({ t: 'vfx.optimize', path, targetParticles }));

    server.registerTool('vfx_variant_upsert', {
        description: '公開パラメーター値の名前付きVariant Setを一括作成・置換します。複数styleのAI量産に使います。',
        inputSchema: { path: z.string().min(1), name: z.string().min(1).max(128), values: z.record(z.string(), JsonValueSchema) },
        annotations: writeAnnotations,
    }, ({ path, name, values }) => run({ t: 'vfx.variant.upsert', path, name, values }));

    server.registerTool('vfx_node_add', {
        description: '検証済み.vfxへEffectノードを追加し、任意のsourceから接続します。budget/DAG超過は拒否されUndo可能です。',
        inputSchema: {
            path: z.string().min(1),
            nodeType: z.enum(['particle', 'trail', 'meshTrail', 'light', 'audio', 'decal', 'delay', 'subGraph']),
            name: z.string().min(1).max(128).optional(),
            from: z.number().int().positive().optional(),
            assetPath: z.string().min(1).optional(),
        }, annotations: writeAnnotations,
    }, ({ path, nodeType, name, from, assetPath }) => run({
        t: 'vfx.node.add', path, nodeType,
        ...(name === undefined ? {} : { name }), ...(from === undefined ? {} : { from }),
        ...(assetPath === undefined ? {} : { assetPath }),
    }));

    server.registerTool('vfx_node_remove', {
        description: 'VFXノードと関連link/bindingを検証付きで削除します。',
        inputSchema: { path: z.string().min(1), nodeId: z.number().int().positive() }, annotations: writeAnnotations,
    }, ({ path, nodeId }) => run({ t: 'vfx.node.remove', path, nodeId }));

    server.registerTool('vfx_node_set_field', {
        description: 'vfx_get_schemaに存在する型付きschemaPathだけを変更します。保存前にbudgetとDAGを再検証します。',
        inputSchema: { path: z.string().min(1), nodeId: z.number().int().positive(), schemaPath: z.string().min(1).max(256), value: JsonValueSchema },
        annotations: writeAnnotations,
    }, ({ path, nodeId, schemaPath, value }) => run({ t: 'vfx.node.setField', path, nodeId, schemaPath, value }));

    server.registerTool('vfx_link_add', {
        description: 'VFXイベントlinkを追加します。循環・重複・無効なcollision sourceは拒否します。',
        inputSchema: { path: z.string().min(1), from: z.number().int().positive(), to: z.number().int().positive(), trigger: z.enum(['onComplete', 'onStart', 'onCollision', 'onDeath']).optional(), delay: z.number().finite().nonnegative().optional() },
        annotations: writeAnnotations,
    }, ({ path, from, to, trigger, delay }) => run({
        t: 'vfx.link.add', path, from, to,
        ...(trigger === undefined ? {} : { trigger }), ...(delay === undefined ? {} : { delay }),
    }));

    server.registerTool('vfx_link_remove', {
        description: 'index指定でVFXイベントlinkを削除します。',
        inputSchema: { path: z.string().min(1), index: z.number().int().nonnegative() }, annotations: writeAnnotations,
    }, ({ path, index }) => run({ t: 'vfx.link.remove', path, index }));

    server.registerTool('vfx_param_declare', {
        description: '意味名・型・default・任意rangeを持つ公開VFXパラメーターを宣言します。',
        inputSchema: { path: z.string().min(1), name: z.string().min(1).max(128), paramType: z.enum(['float', 'int', 'bool', 'color', 'vector3', 'asset']), defaultValue: JsonValueSchema, minimum: z.number().finite().optional(), maximum: z.number().finite().optional() },
        annotations: writeAnnotations,
    }, ({ path, name, paramType, defaultValue, minimum, maximum }) => run({
        t: 'vfx.param.declare', path, name, paramType, defaultValue,
        ...(minimum === undefined ? {} : { minimum }), ...(maximum === undefined ? {} : { maximum }),
    }));

    server.registerTool('vfx_param_bind', {
        description: '公開パラメーターをexposableかつ型互換なVFXノードschema leafへbindingします。',
        inputSchema: { path: z.string().min(1), name: z.string().min(1).max(128), nodeId: z.number().int().positive(), schemaPath: z.string().min(1).max(256) },
        annotations: writeAnnotations,
    }, ({ path, name, nodeId, schemaPath }) => run({ t: 'vfx.param.bind', path, name, nodeId, schemaPath }));

    server.registerTool('vfx_param_set_default', {
        description: '公開VFXパラメーターのdefault値ソースを型検証して更新します。',
        inputSchema: { path: z.string().min(1), name: z.string().min(1).max(128), value: JsonValueSchema }, annotations: writeAnnotations,
    }, ({ path, name, value }) => run({ t: 'vfx.param.setDefault', path, name, value }));

    server.registerTool('vfx_instance_set', {
        description: 'Scene上のVFXGraphComponentへ型付きsparse overrideを設定します。',
        inputSchema: { id: NodeIdSchema, name: z.string().min(1).max(128), value: JsonValueSchema }, annotations: writeAnnotations,
    }, ({ id, name, value }) => run({ t: 'vfx.instance.set', id, name, value }));

    server.registerTool('vfx_instance_clear', {
        description: 'Scene上のVFX parameter overrideを消し、variant/defaultへ戻します。',
        inputSchema: { id: NodeIdSchema, name: z.string().min(1).max(128) }, annotations: writeAnnotations,
    }, ({ id, name }) => run({ t: 'vfx.instance.clear', id, name }));

    server.registerTool('node_create', {
        description: dryRun ? 'ノード作成の差分を試算します。シーンは変更しません。' : 'Undo 可能なノードを作成します。',
        inputSchema: { parent: NodeIdSchema.optional(), name: NameSchema.optional() },
        annotations: writeAnnotations,
    }, ({ parent, name }) => run({
        t: 'node.create',
        ...(parent === undefined ? {} : { parent }),
        ...(name === undefined ? {} : { name }),
    }));

    server.registerTool('node_duplicate', {
        description: dryRun ? 'ノード階層複製の影響を試算します。' : 'ノードと子階層・コンポーネントを複製し、1回のUndoで取り消せます。',
        inputSchema: {
            id: NodeIdSchema,
            parent: NodeIdSchema.optional().describe('省略時は元ノードと同じ親'),
            name: NameSchema.optional().describe('省略時は「元名 (Clone)」'),
        },
        annotations: writeAnnotations,
    }, ({ id, parent, name }) => run({
        t: 'node.duplicate',
        id,
        ...(parent === undefined ? {} : { parent }),
        ...(name === undefined ? {} : { name }),
    }));

    server.registerTool('node_delete', {
        description: dryRun ? 'ノード削除の影響を試算します。' : 'ノードを Undo 可能な形で削除します。',
        inputSchema: { id: NodeIdSchema },
        annotations: writeAnnotations,
    }, ({ id }) => run({ t: 'node.delete', id }));

    server.registerTool('node_reparent', {
        description: 'ノードの親と兄弟順序を変更します。',
        inputSchema: { id: NodeIdSchema, parent: NodeIdSchema, index: z.number().int().nonnegative() },
        annotations: writeAnnotations,
    }, ({ id, parent, index }) => run({ t: 'node.reparent', id, parent, index }));

    server.registerTool('node_rename', {
        description: 'ノード名を変更します。',
        inputSchema: { id: NodeIdSchema, name: NameSchema },
        annotations: writeAnnotations,
    }, ({ id, name }) => run({ t: 'node.rename', id, name }));

    server.registerTool('node_set_active', {
        description: 'GameObjectのactiveSelf(有効/無効)をUndo可能に切り替えます。無効化すると子孫ごと非表示・停止になります。現在値はscene_get_tree/scene_find/node_get_componentsのactiveで確認できます。',
        inputSchema: { id: NodeIdSchema, active: z.boolean() },
        annotations: writeAnnotations,
    }, ({ id, active }) => run({ t: 'node.setActive', id, active }));

    server.registerTool('node_set_tag', {
        description: 'GameObjectのtagをUndo可能に設定します。scene_findのtagフィルタで検索できるようになります。',
        inputSchema: { id: NodeIdSchema, tag: z.string().min(1).max(128) },
        annotations: writeAnnotations,
    }, ({ id, tag }) => run({ t: 'node.setTag', id, tag }));

    server.registerTool('node_set_layer', {
        description: 'GameObjectのlayer(0〜31)をUndo可能に設定します。描画・物理のレイヤー分けに使います。',
        inputSchema: { id: NodeIdSchema, layer: z.number().int().min(0).max(31) },
        annotations: writeAnnotations,
    }, ({ id, layer }) => run({ t: 'node.setLayer', id, layer }));

    server.registerTool('selection_set', {
        description: 'Editor の選択ノードを変更します。',
        inputSchema: { ids: z.array(NodeIdSchema).max(256) },
        annotations: writeAnnotations,
    }, ({ ids }) => run({ t: 'selection.set', ids }));

    server.registerTool('transform_set', {
        description: 'Transform の位置・回転・スケールを変更します。少なくとも1項目が必要です。',
        inputSchema: {
            id: NodeIdSchema,
            pos: Vec3Schema.optional(),
            rot: Vec3Schema.optional(),
            scale: Vec3Schema.optional(),
        },
        annotations: writeAnnotations,
    }, ({ id, pos, rot, scale }) => run({
        t: 'transform.set',
        id,
        ...(pos === undefined ? {} : { pos }),
        ...(rot === undefined ? {} : { rot }),
        ...(scale === undefined ? {} : { scale }),
    }));

    server.registerTool('component_add', {
        description: '指定ノードへコンポーネントを追加します。',
        inputSchema: { id: NodeIdSchema, comp: ComponentSchema },
        annotations: writeAnnotations,
    }, ({ id, comp }) => run({ t: 'component.add', id, comp }));

    server.registerTool('component_set', {
        description: 'JsonReflector で公開されたコンポーネントフィールドを変更します。',
        inputSchema: {
            id: NodeIdSchema,
            comp: ComponentSchema,
            field: z.string().min(1).max(128),
            value: JsonValueSchema,
        },
        annotations: writeAnnotations,
    }, ({ id, comp, field, value }) => run({ t: 'component.set', id, comp, field, value }));

    server.registerTool('component_remove', {
        description: '指定ノードからコンポーネントを削除します。',
        inputSchema: { id: NodeIdSchema, comp: ComponentSchema },
        annotations: writeAnnotations,
    }, ({ id, comp }) => run({ t: 'component.remove', id, comp }));

    server.registerTool('asset_import', {
        description: 'プロジェクト内の src を Assets 起点の dst へインポートします。',
        inputSchema: { src: z.string().min(1), dst: z.string().min(1) },
        annotations: writeAnnotations,
    }, ({ src, dst }) => run({ t: 'asset.import', src, dst }));

    server.registerTool('prefab_instantiate', {
        description: dryRun ? 'Prefab インスタンス化の入力を検証します。' : 'プロジェクト内の .prefab から階層を生成し、Undo 可能にします。',
        inputSchema: {
            path: z.string().min(1).describe('projectRoot 相対パス。例: Assets/Prefabs/Enemy.prefab'),
            parent: NodeIdSchema.optional(),
        },
        annotations: writeAnnotations,
    }, ({ path, parent }) => run({
        t: 'prefab.instantiate', path,
        ...(parent === undefined ? {} : { parent }),
    }));

    server.registerTool('prefab_create', {
        description: dryRun
            ? 'Prefab 作成の入力を検証します。'
            : '選択中 (または ids 指定) の GameObject 階層を .prefab として保存し、ソースをそのインスタンスへ接続します。Undo 可能。',
        inputSchema: {
            path: z.string().min(1).describe('保存先 projectRoot 相対パス。例: Assets/Prefabs/Enemy.prefab'),
            ids: z.array(NodeIdSchema).max(256).optional().describe('対象ノード。省略時は現在の選択を使用。'),
        },
        annotations: writeAnnotations,
    }, ({ path, ids }) => run({
        t: 'prefab.create', path,
        ...(ids === undefined ? {} : { ids }),
    }));

    server.registerTool('prefab_apply', {
        description: dryRun
            ? 'Prefab Apply の入力を検証します。'
            : 'Prefab インスタンスの現在状態を元の .prefab アセットへ書き戻します。Undo 可能。',
        inputSchema: { id: NodeIdSchema.describe('Prefab インスタンスのルート NodeId') },
        annotations: writeAnnotations,
    }, ({ id }) => run({ t: 'prefab.apply', id }));

    server.registerTool('prefab_revert', {
        description: dryRun
            ? 'Prefab Revert の入力を検証します。'
            : 'Prefab インスタンスを元の .prefab アセットの定義へ戻します (Transform は保持)。Undo 可能。',
        inputSchema: { id: NodeIdSchema.describe('Prefab インスタンスのルート NodeId') },
        annotations: writeAnnotations,
    }, ({ id }) => run({ t: 'prefab.revert', id }));

    server.registerTool('material_assign', {
        description: 'GameObjectへマテリアルアセットを割り当てます。MaterialComponentがなければ追加します。',
        inputSchema: { id: NodeIdSchema, path: z.string().min(1) },
        annotations: writeAnnotations,
    }, ({ id, path }) => run({ t: 'material.assign', id, path }));

    server.registerTool('material_set_parameter', {
        description: 'GameObject単位のマテリアルインスタンスへfloat/vec2/vec3/vec4パラメーターを上書きします。',
        inputSchema: {
            id: NodeIdSchema,
            parameter: z.string().min(1).max(128),
            value: z.array(z.number().finite()).min(1).max(4),
        },
        annotations: writeAnnotations,
    }, ({ id, parameter, value }) => run({ t: 'material.override', id, parameter, value }));

    server.registerTool('material_set_shader', {
        description: 'プロジェクト内の.matアセットが参照するシェーダーパスを変更します。',
        inputSchema: { path: z.string().min(1), shaderPath: z.string().min(1) },
        annotations: writeAnnotations,
    }, ({ path, shaderPath }) => run({ t: 'material.asset.setShader', path, shaderPath }));

    server.registerTool('animation_control', {
        description: 'Animatorのクリップ/ステート再生、一時停止、停止、秒またはフレーム位置へのシークを行います。',
        inputSchema: {
            id: NodeIdSchema,
            action: z.enum(['play', 'pause', 'stop', 'seek']),
            clipName: z.string().min(1).max(128).optional(),
            clipIndex: z.number().int().nonnegative().optional(),
            state: z.string().min(1).max(128).optional(),
            time: z.number().finite().nonnegative().optional(),
            frame: z.number().finite().nonnegative().optional(),
        },
        annotations: writeAnnotations,
    }, ({ id, action, clipName, clipIndex, state, time, frame }) => run({
        t: 'animation.control', id, action,
        ...(clipName === undefined ? {} : { clipName }),
        ...(clipIndex === undefined ? {} : { clipIndex }),
        ...(state === undefined ? {} : { state }),
        ...(time === undefined ? {} : { time }),
        ...(frame === undefined ? {} : { frame }),
    }));

    server.registerTool('animation_set_parameter', {
        description: 'Animatorパラメーターを名前で設定し、遷移やBlendTreeを駆動します。value型は宣言型に従います(float/int=数値、bool=真偽、trigger=真偽で発火/リセット、省略時は発火)。animation_get_graphで名前と型を確認してください。',
        inputSchema: {
            id: NodeIdSchema,
            name: z.string().min(1).max(128).describe('animation_get_graph が返すパラメーター名'),
            value: z.union([z.number().finite(), z.boolean()]).optional()
                .describe('float/int は数値、bool/trigger は真偽。trigger は省略で発火'),
        },
        annotations: writeAnnotations,
    }, ({ id, name, value }) => run({
        t: 'animation.setParameter', id, name,
        ...(value === undefined ? {} : { value }),
    }));

    server.registerTool('animation_add_transition', {
        description: dryRun
            ? 'Animator遷移追加の入力を検証します。'
            : 'Animatorステート間(またはAny State→ステート)にUndo可能な遷移を追加します。from省略でAny State遷移。遷移先が既にある場合はエラー。追加後はanimation_set_conditionで条件を付けます。',
        inputSchema: {
            id: NodeIdSchema,
            from: z.string().min(1).max(128).optional().describe('遷移元ステート名。省略で Any State 遷移'),
            to: z.string().min(1).max(128).describe('遷移先ステート名'),
            hasExitTime: z.boolean().optional(),
            exitTime: z.number().finite().min(0).max(1).optional().describe('0..1 正規化再生位置'),
            fixedDuration: z.boolean().optional().describe('true=秒、false=遷移元Length比'),
            transitionDuration: z.number().finite().min(0).max(60).optional(),
        },
        annotations: writeAnnotations,
    }, ({ id, from, to, hasExitTime, exitTime, fixedDuration, transitionDuration }) => run({
        t: 'animation.addTransition', id, to,
        ...(from === undefined ? {} : { from }),
        ...(hasExitTime === undefined ? {} : { hasExitTime }),
        ...(exitTime === undefined ? {} : { exitTime }),
        ...(fixedDuration === undefined ? {} : { fixedDuration }),
        ...(transitionDuration === undefined ? {} : { transitionDuration }),
    }));

    server.registerTool('animation_set_condition', {
        description: dryRun
            ? 'Animator遷移条件の編集入力を検証します。'
            : '指定遷移の発火条件をUndo可能に編集します。action=add/update/remove/clear。add/updateはparameterとopが必要(true/false以外はthresholdも)、update/removeはconditionIndexが必要。transitionIndexはanimation_get_graphのstates[].transitionsまたはanyStateTransitionsの並び順です。',
        inputSchema: {
            id: NodeIdSchema,
            from: z.string().min(1).max(128).optional().describe('遷移元ステート名。省略で Any State 遷移'),
            transitionIndex: z.number().int().nonnegative().describe('対象ソースの遷移リスト内index'),
            action: z.enum(['add', 'update', 'remove', 'clear']),
            parameter: z.string().min(1).max(128).optional().describe('add/update で必須。既存パラメーター名'),
            op: z.enum(['greater', 'less', 'equal', 'notEqual', 'true', 'false']).optional().describe('add/update で必須'),
            threshold: z.number().finite().optional().describe('Float/Int比較の閾値。true/falseでは無視'),
            conditionIndex: z.number().int().nonnegative().optional().describe('update/remove で必須'),
        },
        annotations: writeAnnotations,
    }, ({ id, from, transitionIndex, action, parameter, op, threshold, conditionIndex }) => run({
        t: 'animation.setCondition', id, transitionIndex, action,
        ...(from === undefined ? {} : { from }),
        ...(parameter === undefined ? {} : { parameter }),
        ...(op === undefined ? {} : { op }),
        ...(threshold === undefined ? {} : { threshold }),
        ...(conditionIndex === undefined ? {} : { conditionIndex }),
    }));

    // BlendTree ステートを追加する際の駆動パラメーターと座標系スキーマ (add/set 共通)。
    const StateModeSchema = z.enum(['clip', 'blendTree1D', 'blendTree2D']);
    server.registerTool('animation_add_state', {
        description: dryRun
            ? 'Animatorステート追加の入力を検証します。'
            : 'AnimatorにUndo可能なステートを追加します。mode省略でclip。最初のステートやsetAsDefault指定でデフォルトステートになります。BlendTreeはblendParameter(1D)またはblendParameterX/Y(2D)で駆動パラメーターを指定し、Motionはanimation_add_motionで足します。',
        inputSchema: {
            id: NodeIdSchema,
            name: NameSchema.describe('新規ステート名。既存と重複不可'),
            mode: StateModeSchema.optional(),
            sourcePath: z.string().min(1).max(512).optional().describe('clip モードのアニメーションソース'),
            clipName: z.string().min(1).max(128).optional(),
            clipIndex: z.number().int().nonnegative().optional(),
            speed: z.number().finite().optional(),
            loop: z.boolean().optional(),
            ikWeight: z.number().finite().min(0).max(1).optional(),
            blendParameter: z.string().min(1).max(128).optional().describe('BlendTree1D 駆動パラメーター'),
            blendParameterX: z.string().min(1).max(128).optional().describe('BlendTree2D X 軸パラメーター'),
            blendParameterY: z.string().min(1).max(128).optional().describe('BlendTree2D Y 軸パラメーター'),
            setAsDefault: z.boolean().optional(),
        },
        annotations: writeAnnotations,
    }, ({ id, name, mode, sourcePath, clipName, clipIndex, speed, loop, ikWeight,
         blendParameter, blendParameterX, blendParameterY, setAsDefault }) => run({
        t: 'animation.addState', id, name,
        ...(mode === undefined ? {} : { mode }),
        ...(sourcePath === undefined ? {} : { sourcePath }),
        ...(clipName === undefined ? {} : { clipName }),
        ...(clipIndex === undefined ? {} : { clipIndex }),
        ...(speed === undefined ? {} : { speed }),
        ...(loop === undefined ? {} : { loop }),
        ...(ikWeight === undefined ? {} : { ikWeight }),
        ...(blendParameter === undefined ? {} : { blendParameter }),
        ...(blendParameterX === undefined ? {} : { blendParameterX }),
        ...(blendParameterY === undefined ? {} : { blendParameterY }),
        ...(setAsDefault === undefined ? {} : { setAsDefault }),
    }));

    server.registerTool('animation_set_state', {
        description: dryRun
            ? 'Animatorステート更新の入力を検証します。'
            : '既存ステートのプロパティをUndo可能に更新します。stateで対象を指定し、name指定でリネーム(全遷移参照・defaultStateを追従)。mode/clip/speed/loop/ikWeight/BlendTree駆動パラメーター/blend2DType/setAsDefaultを部分更新できます。',
        inputSchema: {
            id: NodeIdSchema,
            state: z.string().min(1).max(128).describe('更新対象の現在のステート名'),
            name: z.string().min(1).max(128).optional().describe('新しい名前 (リネーム。参照追従)'),
            mode: StateModeSchema.optional(),
            sourcePath: z.string().min(1).max(512).optional(),
            clipName: z.string().min(1).max(128).optional(),
            clipIndex: z.number().int().nonnegative().optional(),
            speed: z.number().finite().optional(),
            loop: z.boolean().optional(),
            ikWeight: z.number().finite().min(0).max(1).optional(),
            blendParameter: z.string().min(1).max(128).optional(),
            blendParameterX: z.string().min(1).max(128).optional(),
            blendParameterY: z.string().min(1).max(128).optional(),
            blend2DType: z.enum(['simpleDirectional', 'freeformCartesian']).optional(),
            setAsDefault: z.boolean().optional(),
        },
        annotations: writeAnnotations,
    }, ({ id, state, name, mode, sourcePath, clipName, clipIndex, speed, loop, ikWeight,
         blendParameter, blendParameterX, blendParameterY, blend2DType, setAsDefault }) => run({
        t: 'animation.setState', id, state,
        ...(name === undefined ? {} : { name }),
        ...(mode === undefined ? {} : { mode }),
        ...(sourcePath === undefined ? {} : { sourcePath }),
        ...(clipName === undefined ? {} : { clipName }),
        ...(clipIndex === undefined ? {} : { clipIndex }),
        ...(speed === undefined ? {} : { speed }),
        ...(loop === undefined ? {} : { loop }),
        ...(ikWeight === undefined ? {} : { ikWeight }),
        ...(blendParameter === undefined ? {} : { blendParameter }),
        ...(blendParameterX === undefined ? {} : { blendParameterX }),
        ...(blendParameterY === undefined ? {} : { blendParameterY }),
        ...(blend2DType === undefined ? {} : { blend2DType }),
        ...(setAsDefault === undefined ? {} : { setAsDefault }),
    }));

    server.registerTool('animation_add_motion', {
        description: dryRun
            ? 'BlendTree Motion追加の入力を検証します。'
            : '指定BlendTreeステートにUndo可能なMotionを追加します。1Dはthreshold、2Dはpos X/Yで配置します。対象がClipステートの場合はエラー。animation_get_blend_treeで構成を確認できます。',
        inputSchema: {
            id: NodeIdSchema,
            state: z.string().min(1).max(128).describe('BlendTree ステート名'),
            sourcePath: z.string().min(1).max(512).optional(),
            clipName: z.string().min(1).max(128).optional(),
            clipIndex: z.number().int().nonnegative().optional(),
            threshold: z.number().finite().optional().describe('BlendTree1D の位置'),
            posX: z.number().finite().optional().describe('BlendTree2D の X'),
            posY: z.number().finite().optional().describe('BlendTree2D の Y'),
            speed: z.number().finite().optional(),
            ikWeight: z.number().finite().min(0).max(1).optional(),
        },
        annotations: writeAnnotations,
    }, ({ id, state, sourcePath, clipName, clipIndex, threshold, posX, posY, speed, ikWeight }) => run({
        t: 'animation.addMotion', id, state,
        ...(sourcePath === undefined ? {} : { sourcePath }),
        ...(clipName === undefined ? {} : { clipName }),
        ...(clipIndex === undefined ? {} : { clipIndex }),
        ...(threshold === undefined ? {} : { threshold }),
        ...(posX === undefined ? {} : { posX }),
        ...(posY === undefined ? {} : { posY }),
        ...(speed === undefined ? {} : { speed }),
        ...(ikWeight === undefined ? {} : { ikWeight }),
    }));

    server.registerTool('animation_set_motion', {
        description: dryRun
            ? 'BlendTree Motion更新の入力を検証します。'
            : '指定BlendTreeステートのMotionをmotionIndexで指定してUndo可能に更新します。motionIndexはanimation_get_blend_treeのmotions並び順です。',
        inputSchema: {
            id: NodeIdSchema,
            state: z.string().min(1).max(128),
            motionIndex: z.number().int().nonnegative().describe('motions リスト内 index'),
            sourcePath: z.string().min(1).max(512).optional(),
            clipName: z.string().min(1).max(128).optional(),
            clipIndex: z.number().int().nonnegative().optional(),
            threshold: z.number().finite().optional(),
            posX: z.number().finite().optional(),
            posY: z.number().finite().optional(),
            speed: z.number().finite().optional(),
            ikWeight: z.number().finite().min(0).max(1).optional(),
        },
        annotations: writeAnnotations,
    }, ({ id, state, motionIndex, sourcePath, clipName, clipIndex, threshold, posX, posY, speed, ikWeight }) => run({
        t: 'animation.setMotion', id, state, motionIndex,
        ...(sourcePath === undefined ? {} : { sourcePath }),
        ...(clipName === undefined ? {} : { clipName }),
        ...(clipIndex === undefined ? {} : { clipIndex }),
        ...(threshold === undefined ? {} : { threshold }),
        ...(posX === undefined ? {} : { posX }),
        ...(posY === undefined ? {} : { posY }),
        ...(speed === undefined ? {} : { speed }),
        ...(ikWeight === undefined ? {} : { ikWeight }),
    }));

    // ── Animator 構造の削除系 + パラメーター CRUD ──
    server.registerTool('animation_remove_state', {
        description: dryRun
            ? 'Animatorステート削除の入力を検証します。'
            : '指定ステートをUndo可能に削除します。他ステート/Any Stateからこのステートへの遷移も併せて除去し、defaultStateだった場合は先頭ステートへ付け替えます。',
        inputSchema: {
            id: NodeIdSchema,
            state: z.string().min(1).max(128).describe('削除するステート名'),
        },
        annotations: writeAnnotations,
    }, ({ id, state }) => run({ t: 'animation.removeState', id, state }));

    server.registerTool('animation_remove_transition', {
        description: dryRun
            ? 'Animator遷移削除の入力を検証します。'
            : '指定遷移をUndo可能に削除します。fromで遷移元ステート(省略でAny State)、transitionIndexはanimation_get_graphの並び順です。',
        inputSchema: {
            id: NodeIdSchema,
            from: z.string().min(1).max(128).optional().describe('遷移元ステート名。省略で Any State'),
            transitionIndex: z.number().int().nonnegative().describe('対象ソースの遷移リスト内index'),
        },
        annotations: writeAnnotations,
    }, ({ id, from, transitionIndex }) => run({
        t: 'animation.removeTransition', id, transitionIndex,
        ...(from === undefined ? {} : { from }),
    }));

    server.registerTool('animation_remove_motion', {
        description: dryRun
            ? 'BlendTree Motion削除の入力を検証します。'
            : '指定BlendTreeステートのMotionをmotionIndexでUndo可能に削除します。motionIndexはanimation_get_blend_treeの並び順です。',
        inputSchema: {
            id: NodeIdSchema,
            state: z.string().min(1).max(128).describe('BlendTree ステート名'),
            motionIndex: z.number().int().nonnegative().describe('motions リスト内 index'),
        },
        annotations: writeAnnotations,
    }, ({ id, state, motionIndex }) => run({ t: 'animation.removeMotion', id, state, motionIndex }));

    server.registerTool('animation_add_parameter', {
        description: dryRun
            ? 'Animatorパラメーター追加の入力を検証します。'
            : 'AnimatorパラメーターをUndo可能に追加します。type省略でfloat。valueで初期値を設定(float/int=数値、bool/trigger=真偽、省略で既定)。追加後はanimation_set_conditionで遷移条件に使えます。',
        inputSchema: {
            id: NodeIdSchema,
            name: NameSchema.describe('新規パラメーター名。既存と重複不可'),
            type: z.enum(['float', 'int', 'bool', 'trigger']).optional(),
            value: z.union([z.number().finite(), z.boolean()]).optional().describe('初期値'),
        },
        annotations: writeAnnotations,
    }, ({ id, name, type, value }) => run({
        t: 'animation.addParameter', id, name,
        ...(type === undefined ? {} : { type }),
        ...(value === undefined ? {} : { value }),
    }));

    server.registerTool('animation_remove_parameter', {
        description: dryRun
            ? 'Animatorパラメーター削除の入力を検証します。'
            : 'Animatorパラメーターを名前でUndo可能に削除します。このパラメーターを参照する遷移条件は残ります(発火しなくなるだけ)。参照状況はanimation_get_graphで確認してください。',
        inputSchema: {
            id: NodeIdSchema,
            name: z.string().min(1).max(128).describe('削除するパラメーター名'),
        },
        annotations: writeAnnotations,
    }, ({ id, name }) => run({ t: 'animation.removeParameter', id, name }));

    server.registerTool('input_inject', {
        description: 'Play Modeへキー、仮想軸、ゲームパッドボタン/軸、マウス入力を注入します。clearで全注入状態を解除します。',
        inputSchema: InputInjectionShape,
        annotations: writeAnnotations,
    }, (input) => run(ToInputCommand(InputInjectionSchema.parse(input))));

    server.registerTool('play_control', {
        description: 'Play Mode の開始・停止・一時停止・再開・1フレームステップを制御します。',
        inputSchema: { action: z.enum(['start', 'stop', 'pause', 'resume', 'step']) },
        annotations: writeAnnotations,
    }, ({ action }) => run({ t: 'play.control', action }));

    server.registerTool('playtest_run', {
        description: dryRun
            ? '自動プレイテストの入力列と判定条件を検証します。待機や実行は行いません。'
            : 'Play開始、時系列入力、待機、今回のログ・性能・衝突・意味付き画像の収集、元のPlay状態への復元を1回で行います。',
        inputSchema: {
            startPlay: z.boolean().default(true).describe('editor状態ならPlay開始、pausedならresumeする'),
            steps: z.array(z.object({
                delayMs: z.number().int().min(0).max(10000).default(0).describe('直前stepからの待機時間'),
                input: InputInjectionSchema,
            }).strict()).max(64).default([]),
            settleMs: z.number().int().min(0).max(10000).default(250).describe('最終入力後の観測待機時間'),
            restorePlayState: z.boolean().default(true).describe('終了時に呼び出し前のeditor/paused/playingへ戻す'),
            view: z.enum(['scene', 'game']).default('game').describe('判定と画像取得に使うViewport'),
            assertions: z.object({
                noErrors: z.boolean().default(true).describe('今回発生したerrorログを失敗にする'),
                noWarnings: z.boolean().default(false).describe('今回発生したwarningログも失敗にする'),
                minFps: z.number().finite().positive().max(1000).optional(),
                maxFrameMs: z.number().finite().positive().max(10000).optional(),
                logContains: z.string().min(1).max(256).optional(),
                visibleNodeIds: z.array(NodeIdSchema).max(64).default([]),
            }).strict().default({ noErrors: true, noWarnings: false, visibleNodeIds: [] }),
            w: z.number().int().min(160).max(1920).default(960),
            h: z.number().int().min(90).max(1080).default(540),
        },
        annotations: writeAnnotations,
    }, ({ startPlay, steps, settleMs, restorePlayState, view, assertions, w, h }) => Safely(async () => {
        const totalWaitMs = settleMs + steps.reduce((total, step) => total + step.delayMs, 0);
        if (totalWaitMs > 60000) throw new Error('playtest_run の合計待機時間は60000ms以下にしてください');

        const inputCommands = steps.map((step) => ({
            delayMs: step.delayMs,
            command: EditorCommandSchema.parse(ToInputCommand(step.input)),
        }));
        const initialState = await bus.Query({ t: 'editor.state' }) as Record<string, unknown>;
        const initialPlayState = String(initialState.playState ?? 'editor');

        // dry-runでは時系列待機を省略し、全CommandをEditor側のdryRun検証へ通す。
        if (dryRun) {
            const previews: unknown[] = [];
            if (startPlay && initialPlayState === 'editor')
                previews.push(await bus.Command({ t: 'play.control', action: 'start' }, true));
            else if (startPlay && initialPlayState === 'paused')
                previews.push(await bus.Command({ t: 'play.control', action: 'resume' }, true));
            for (const step of inputCommands) previews.push(await bus.Command(step.command, true));
            previews.push(await bus.Command({ t: 'input.inject', kind: 'clear' }, true));
            if (restorePlayState && startPlay && initialPlayState === 'editor')
                previews.push(await bus.Command({ t: 'play.control', action: 'stop' }, true));
            else if (restorePlayState && startPlay && initialPlayState === 'paused')
                previews.push(await bus.Command({ t: 'play.control', action: 'pause' }, true));
            return TextResult({ dryRun: true, initialPlayState, totalWaitMs, steps: inputCommands.length, previews });
        }

        const baselineLogs = await bus.Query({ t: 'console.logs', minLevel: 'debug', limit: 1 }) as Record<string, unknown>;
        const baselineCursor = Number(baselineLogs.cursor ?? 0);
        const cleanupFailures: string[] = [];
        let operationError: unknown;
        let state: Record<string, unknown> = {};
        let logs: Record<string, unknown> = {};
        let profiler: Record<string, unknown> = {};
        let physicsEvents: unknown = {};
        let capture: z.infer<typeof SemanticViewportResultSchema> | undefined;

        const waitForState = async (expected: string, requireReady: boolean): Promise<void> => {
            const deadline = Date.now() + 10000;
            do {
                const current = await bus.Query({ t: 'editor.state' }) as Record<string, unknown>;
                const ready = !requireReady || (current.restorePending === false && current.scriptReloadBusy === false);
                if (current.playState === expected && ready) return;
                await Delay(50);
            } while (Date.now() < deadline);
            throw new Error(`Play状態が ${expected} へ遷移しませんでした`);
        };

        try {
            if (startPlay && initialPlayState === 'editor') {
                await bus.Command({ t: 'play.control', action: 'start' }, false);
                await waitForState('playing', false);
            } else if (startPlay && initialPlayState === 'paused') {
                await bus.Command({ t: 'play.control', action: 'resume' }, false);
                await waitForState('playing', false);
            }

            for (const step of inputCommands) {
                if (step.delayMs > 0) await Delay(step.delayMs);
                await bus.Command(step.command, false);
            }
            if (settleMs > 0) await Delay(settleMs);

            const [rawState, rawLogs, rawProfiler, rawPhysicsEvents, rawCapture] = await Promise.all([
                bus.Query({ t: 'editor.state' }),
                bus.Query({ t: 'console.logs', minLevel: 'debug', limit: 512, afterSequence: baselineCursor }),
                bus.Query({ t: 'profiler.snapshot', limit: 20 }),
                bus.Query({ t: 'physics.events' }),
                bus.Query({ t: 'viewport.semantic', w, h, view }),
            ]);
            state = rawState as Record<string, unknown>;
            logs = rawLogs as Record<string, unknown>;
            profiler = rawProfiler as Record<string, unknown>;
            physicsEvents = rawPhysicsEvents;
            capture = SemanticViewportResultSchema.parse(rawCapture);
        } catch (error) {
            operationError = error;
        } finally {
            try {
                await bus.Command({ t: 'input.inject', kind: 'clear' }, false);
            } catch (error) {
                cleanupFailures.push(`input clear: ${error instanceof Error ? error.message : String(error)}`);
            }

            if (restorePlayState) {
                try {
                    const current = await bus.Query({ t: 'editor.state' }) as Record<string, unknown>;
                    if (initialPlayState === 'editor' && current.playState !== 'editor') {
                        await bus.Command({ t: 'play.control', action: 'stop' }, false);
                        await waitForState('editor', true);
                    } else if (initialPlayState === 'paused' && current.playState === 'playing') {
                        await bus.Command({ t: 'play.control', action: 'pause' }, false);
                        await waitForState('paused', false);
                    } else if (initialPlayState === 'playing' && current.playState === 'paused') {
                        await bus.Command({ t: 'play.control', action: 'resume' }, false);
                        await waitForState('playing', false);
                    }
                } catch (error) {
                    cleanupFailures.push(`play state restore: ${error instanceof Error ? error.message : String(error)}`);
                }
            }
        }

        if (operationError !== undefined) throw operationError;
        if (capture === undefined) throw new Error('意味付きviewportを取得できませんでした');

        const failures: string[] = [...cleanupFailures];
        const entries = Array.isArray(logs.entries) ? logs.entries as Array<Record<string, unknown>> : [];
        const errorEntries = entries.filter((entry) => entry.level === 'error');
        const warningEntries = entries.filter((entry) => entry.level === 'warning');
        if (logs.dropped === true) failures.push('ログリングバッファが溢れ、テスト中ログの一部を失いました');
        if (assertions.noErrors && errorEntries.length > 0) failures.push(`errorログ: ${errorEntries.length}件`);
        if (assertions.noWarnings && warningEntries.length > 0) failures.push(`warningログ: ${warningEntries.length}件`);

        const fps = Number(profiler.fps ?? 0);
        const frameMs = Number(profiler.frameMs ?? 0);
        if (assertions.minFps !== undefined && fps < assertions.minFps)
            failures.push(`FPS ${fps.toFixed(2)} < ${assertions.minFps}`);
        if (assertions.maxFrameMs !== undefined && frameMs > assertions.maxFrameMs)
            failures.push(`frameMs ${frameMs.toFixed(2)} > ${assertions.maxFrameMs}`);
        if (assertions.logContains !== undefined) {
            const needle = assertions.logContains.toLowerCase();
            const found = entries.some((entry) => String(entry.message ?? '').toLowerCase().includes(needle));
            if (!found) failures.push(`ログに「${assertions.logContains}」がありません`);
        }

        const visibleIds = new Set(capture.objects.filter((object) => object.visible).map((object) => object.id));
        for (const id of assertions.visibleNodeIds) {
            if (!visibleIds.has(id)) failures.push(`Node ${id} がviewportに表示されていません`);
        }

        const restoredState = await bus.Query({ t: 'editor.state' });
        const report = {
            passed: failures.length === 0,
            failures,
            initialState,
            observedState: state,
            restoredState,
            baselineCursor,
            logs,
            profiler,
            physicsEvents,
            semantic: {
                width: capture.width,
                height: capture.height,
                view: capture.view ?? view,
                cameraPosition: capture.cameraPosition,
                objects: capture.objects,
            },
        };
        return {
            content: [
                { type: 'text', text: JSON.stringify(report, null, 2) },
                { type: 'image', data: capture.base64, mimeType: capture.mimeType },
            ],
            structuredContent: report,
        };
    }));

    server.registerTool('viewport_camera_set', {
        description: 'Scene View カメラを座標へ移動し、座標または NodeId を注視させます。',
        inputSchema: {
            position: Vec3Schema.optional(),
            lookAt: Vec3Schema.optional(),
            targetId: NodeIdSchema.optional(),
        },
        annotations: writeAnnotations,
    }, ({ position, lookAt, targetId }) => run({
        t: 'viewport.camera',
        ...(position === undefined ? {} : { position }),
        ...(lookAt === undefined ? {} : { lookAt }),
        ...(targetId === undefined ? {} : { targetId }),
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
        description: '複数 Command を1つの Undo 単位として原子的に実行します。',
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

// 権限モードから公開面そのものを変え、Stage A では書き込みツールを発見不能にする。
export function RegisterEditorTools(server: McpServer, bus: EditorBus, permission: PermissionMode): void {
    RegisterQueryTools(server, bus);
    if (permission !== 'read') {
        RegisterCommandTools(server, bus, permission);
    }
}
