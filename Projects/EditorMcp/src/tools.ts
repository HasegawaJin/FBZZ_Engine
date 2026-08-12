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
    VFXPreviewCameraSchema,
    ViewportCaptureResultSchema,
    type EditorCommand,
    type VFXPreviewCamera,
} from './editorContracts.js';

const NameSchema = z.string().min(1).max(128);
// ステート/遷移/Motion 編集の対象グラフを選ぶ。省略で Base Layer。
// WHY: 上半身レイヤーに独自の遷移グラフを組むには、既存の編集ツールが
//      「どのレイヤーのグラフか」を受け取れる必要がある。
const LayerOptionSchema = z.string().min(1).max(128).optional()
    .describe('対象レイヤー名。省略でBase Layer。animation_add_layerで作ったレイヤーの独自ステートマシンを編集する場合に指定');
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

// preview を組んでから RT を読み戻すまでに必要な待ち時間 (ms)。
// Editor の Update / LateUpdate / Render を数フレーム通す必要があるため、応答直後には読めない。
const PREVIEW_SETTLE_MS = 80;

type PreviewView = 'normal' | 'overdraw' | 'gizmos';

interface PreviewRequest {
    path: string;
    time: number;
    w: number;
    h: number;
    view: PreviewView;
    camera?: VFXPreviewCamera | undefined;
}

// preview を指定時刻で組み直し、落ち着くまで待つ。返り値は duration などを含む prepared 応答。
// WHY: Preview World は randomSeed から決定論的に再シミュレートされるので、
//      毎回組み直しても同じ時刻なら同じ絵になる。呼び出し側はこの前提に乗ってよい。
async function PreparePreview(bus: EditorBus, request: PreviewRequest): Promise<Record<string, unknown>> {
    const prepared = await bus.Query({
        t: 'vfx.preview', path: request.path, time: request.time,
        w: request.w, h: request.h, view: request.view,
        ...(request.camera === undefined ? {} : { camera: request.camera }),
    });
    await Delay(PREVIEW_SETTLE_MS);
    return prepared as Record<string, unknown>;
}

// preview を組んで、画像ではなく指標を取る。
async function MeasurePreview(bus: EditorBus, request: PreviewRequest): Promise<{
    prepared: Record<string, unknown>;
    metrics: Record<string, unknown>;
}> {
    const prepared = await PreparePreview(bus, request);
    const metrics = await bus.Query({ t: 'vfx.previewMetrics', path: request.path, view: request.view });
    return { prepared, metrics: metrics as Record<string, unknown> };
}

// duration を取得して等間隔サンプル時刻を作る。duration が 0 なら [0] だけ返す。
async function ResolveSampleTimes(bus: EditorBus, request: PreviewRequest,
                                  explicit: number[] | undefined, count: number): Promise<number[]> {
    if (explicit !== undefined) return explicit;
    const first = await bus.Query({
        t: 'vfx.preview', path: request.path, time: 0, w: request.w, h: request.h, view: request.view,
        ...(request.camera === undefined ? {} : { camera: request.camera }),
    });
    const duration = Number((first as { duration?: number }).duration ?? 0);
    if (!(duration > 0)) return [0];
    return Array.from({ length: count }, (_, index) => (duration * index) / (count - 1));
}

// 指標の入れ子から、時系列として並べたい代表値だけを抜く。
function FlattenMetrics(metrics: Record<string, unknown>): Record<string, number> {
    const exposure = (metrics.exposure ?? {}) as Record<string, number>;
    const occupancy = (metrics.occupancy ?? {}) as Record<string, number>;
    const motion = (metrics.motion ?? {}) as Record<string, number>;
    return {
        luminanceMean: Number(exposure.luminanceMean ?? 0),
        coveredLuminanceMean: Number(exposure.coveredLuminanceMean ?? 0),
        luminanceP99: Number(exposure.luminanceP99 ?? 0),
        clippedRatio: Number(exposure.clippedRatio ?? 0),
        blownOutRatio: Number(exposure.blownOutRatio ?? 0),
        coverage: Number(occupancy.coverage ?? 0),
        centroidX: Number(occupancy.centroidX ?? 0.5),
        centroidY: Number(occupancy.centroidY ?? 0.5),
        motion: Number(motion.meanLuminanceDelta ?? 0),
        changedRatio: Number(motion.changedRatio ?? 0),
    };
}

// 時系列から「時間の形」を読み取る。AAA の判断はここでしか下せない。
// WHY: 静止画を何枚並べても「立ち上がりが鈍い」は言えない。ピークがいつ来て、
//      どれだけの速さで立ち上がり、どう消えるかを数値の形にしておく。
function SummarizeCurve(samples: Array<{ time: number } & Record<string, number>>,
                        key: 'luminanceMean' | 'coverage'): Record<string, number | string> {
    if (samples.length === 0) return {};
    let peakIndex = 0;
    for (let index = 1; index < samples.length; index += 1) {
        if ((samples[index]?.[key] ?? 0) > (samples[peakIndex]?.[key] ?? 0)) peakIndex = index;
    }
    const peak = samples[peakIndex];
    const last = samples[samples.length - 1];
    const first = samples[0];
    const duration = Math.max(1e-6, (last?.time ?? 0) - (first?.time ?? 0));
    const peakValue = peak?.[key] ?? 0;
    // ピーク位置を再生全長で正規化する。爆発なら 0.15 より手前、煙なら中盤が目安。
    const peakNormalized = ((peak?.time ?? 0) - (first?.time ?? 0)) / duration;
    // 消え際: 最終サンプルがピークの何割まで落ちたか。1 に近ければ「消えていない」。
    const tailRatio = peakValue > 0 ? (last?.[key] ?? 0) / peakValue : 0;
    return {
        metric: key,
        peakTime: Number((peak?.time ?? 0).toFixed(4)),
        peakValue: Number(peakValue.toFixed(6)),
        peakNormalized: Number(peakNormalized.toFixed(4)),
        tailRatio: Number(tailRatio.toFixed(4)),
    };
}

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
        description: '現在のシーン階層を取得します。副作用はありません。'
            + '既定ではシステムが実行時に生成したオブジェクト(VFX Graphのノード実体、foliage bake、'
            + 'water splash)を除外します。これらは編集してもシーンへ保存されないため、'
            + '編集対象を探す用途では常に除外したままで構いません。'
            + '除外した件数は親ノードの hiddenGeneratedChildren に出ます。'
            + '再生中の実体を調べたいときだけ includeGenerated=true を指定してください。',
        inputSchema: {
            includeGenerated: z.boolean().optional()
                .describe('実行時生成オブジェクトを含める(既定false)。デバッグ用途のみ'),
        },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ includeGenerated }) => Safely(async () => TextResult(await bus.Query({
        t: 'scene.tree', ...(includeGenerated === undefined ? {} : { includeGenerated }),
    }))));

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
        description: '.vfxのノード、イベントリンク、SubGraph、Particle実行方式、Burst数、リソースbudgetと検証結果を構造化して取得します。'
            + '既定の detail="summary" では、ノードのtransform・キャンバス座標・group・signalNodeの中身を返しません。'
            + '構造の把握にはこれで足り、応答量は半分以下になります。'
            + '空間配置やキャンバス配置を編集するときだけ detail="full" を指定してください。'
            + '個々のフィールドの現在値が知りたいだけなら vfx_node_get_field のほうが小さく済みます。',
        inputSchema: {
            path: z.string().min(1).describe('projectRoot相対または絶対の.vfxパス'),
            detail: z.enum(['summary', 'full']).default('summary')
                .describe('summary=構造のみ / full=transform・group・signalNodeも含む'),
        },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ path, detail }) => Safely(async () => TextResult(
        await bus.Query({ t: 'vfx.graph', path, detail }))));

    server.registerTool('vfx_node_get_field', {
        description: 'VFXノードのフィールドの現在値を読みます。vfx_node_set_field の対になる取得系です。'
            + 'blendMode / texturePath / colorGradient / sizeCurve に「今何が入っているか」を'
            + '確認する手段はこれだけです (node_get_components は Scene のノード用で、'
            + 'VFX Graph の nodeId とは別空間なので使えません)。'
            + '返る value は vfx_node_set_field の value へそのまま渡せる形なので、'
            + '読んで一部だけ変えて書き戻せます。'
            + 'Curve / Gradient は {interp, keys} で返ります。enum は enumName に現在の名前が付きます。'
            + 'schemaPath を指定すると 1 つだけ、省略すると全 leaf を返します。'
            + 'Particle ノードの全 leaf は 100 個を超えるため、'
            + 'prefix="particle." のように部分木で絞ってください。',
        inputSchema: {
            path: z.string().min(1).describe('projectRoot相対または絶対の.vfxパス'),
            nodeId: z.number().int().positive(),
            schemaPath: z.string().min(1).max(256).optional()
                .describe('vfx_get_schema に存在する型付きpath。省略すると全leaf'),
            prefix: z.string().min(1).max(256).optional()
                .describe('schemaPath省略時の絞り込み。例 "particle."'),
        },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ path, nodeId, schemaPath, prefix }) => Safely(async () => TextResult(await bus.Query({
        t: 'vfx.nodeField', path, nodeId,
        ...(schemaPath === undefined ? {} : { schemaPath }),
        ...(prefix === undefined ? {} : { prefix }),
    }))));

    server.registerTool('vfx_lint', {
        description: '.vfxを静的診断します。循環、Entryから到達できない(=実行時に起動しない)ノード、'
            + '参照アセットの欠落、budget超過、公開パラメーターbindingの不正、消えないMesh、'
            + '実体ノード無しを severity/code/message/nodeId で返します。'
            + '各issueには直し方が fix (呼ぶべきツールと引数) と autoFixable (vfx_repairで直せるか) '
            + 'として付きます。修正はfixに従ってください。推測は不要です。'
            + 'cautionがある場合は、その修正で失われるものを確認してから実行してください。'
            + 'エフェクトを編集したら、プレビュー画像を見る前にまずこれを実行してください。',
        inputSchema: { path: z.string().min(1).describe('projectRoot相対または絶対の.vfxパス') },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ path }) => Safely(async () => TextResult(await bus.Query({ t: 'vfx.lint', path }))));

    server.registerTool('vfx_guide', {
        description: 'このエンジンで見られるエフェクトを作るためのオーサリング規約と、'
            + 'Fire/Explosion/Impact/Smoke の層構成レシピを返します。'
            + '「全部を加算にすると白飽和する」「回転と非等方サイズは排他」「上昇加速度の出所は1つ」など、'
            + 'DAG検証もlintも通るのに見た目が破綻する落とし穴をまとめてあります。'
            + '.vfxを新規作成または大きく変更する前に必ず1度読んでください。'
            + 'lintCodeを持つ規約はvfx_lintが機械的に検査します。',
        inputSchema: {},
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, () => Safely(async () => TextResult(await bus.Query({ t: 'vfx.guide' }))));

    server.registerTool('vfx_knowledge_catalog', {
        description: '組み込みとプロジェクト固有のVFX Templateを同じカタログから検索します。'
            + 'vfx_knowledge_promoteで採択した成果もここへ現れるため、次の制作ではゼロから作らず'
            + '最も近い成功例をvfx_candidate_forkしてください。',
        inputSchema: {
            query: z.string().min(1).max(128).optional()
                .describe('名前・カテゴリ・タグ・説明・ノード構成の部分一致。省略すると全件'),
            limit: z.number().int().min(1).max(256).default(64),
        },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ query, limit }) => Safely(async () => TextResult(await bus.Query({
        t: 'vfx.templateCatalog', ...(query === undefined ? {} : { query }), limit,
    }))));

    // ── Behavior Tree ──
    // WHY vfx_* と同じ形にするか: 「アセットを読む → 規約を読む → 編集する → 検証する」
    //     という流れは VFX と同一で、面の作り方を変える理由が無い。
    server.registerTool('bt_inspect_tree', {
        description: '.behaviortree の木構造・Blackboard・検証結果を返します。'
            + 'ノードは parentId と order で並べて返るため、配列の順序がそのまま優先順位です。'
            + 'order は Selector で「やりたいことの優先順位」そのものなので、'
            + '木の形だけから推測せずここを読んでください。',
        inputSchema: { path: z.string().min(1).describe('projectRoot 相対の .behaviortree') },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ path }) => Safely(async () => TextResult(await bus.Query({ t: 'bt.tree', path }))));

    server.registerTool('bt_lint', {
        description: '.behaviortree の「保存はできるが意図どおり動かない」構成を返します。'
            + 'valid=false は保存が拒否される致命的な不整合 (ルートが 0/2 個・循環・子数超過)。',
        inputSchema: { path: z.string().min(1) },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ path }) => Safely(async () => TextResult(await bus.Query({ t: 'bt.lint', path }))));

    server.registerTool('bt_guide', {
        description: 'Behavior Tree を組む前に読むオーサリング規約と、'
            + '代表的な骨格 (Guard / Chaser / Turret)、全ノード種別の子数上限と'
            + 'abortMode を付けられるかの一覧を返します。'
            + '木を作る前に必ず読んでください。とくに abortMode=lowerPriority を'
            + '付けないと「巡回中に敵を見つけても着くまで反応しない」AI になります。',
        inputSchema: {},
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, () => Safely(async () => TextResult(await bus.Query({ t: 'bt.guide' }))));

    server.registerTool('vfx_curve_presets', {
        description: '名前付きの時間カーブプリセット一覧を返します。'
            + 'Spike(閃光)/Breathe(炎の呼吸)/Ease Out(減衰)/Blink(点滅)など。'
            + 'vfx_node_set_field の value へ {"preset":"Spike","scale":1.0} を渡すと適用されます。'
            + '生のキー列を書くより意図した形になり、外したときの原因も特定しやすくなります。',
        inputSchema: {},
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, () => Safely(async () => TextResult(await bus.Query({ t: 'vfx.curvePresets' }))));

    server.registerTool('vfx_analyze_texture', {
        description: 'VFX素材テクスチャ(png/tga/dds等)の中身を解析し、設定の根拠になる特徴量と'
            + '推奨オーサリング値を返します。'
            + '.vfxでテクスチャを割り当てる前に必ず1度実行してください。'
            + 'blendMode / alphaSource / spriteColumns / spriteRows / softParticles は'
            + '素材の中身で正解が変わり、ファイル名からは判断できません。'
            + 'recommendations[].schemaPath と .value は vfx_node_set_field へそのまま渡せます。'
            + 'alpha.isMeaningful=false なら alphaSource=Luminance が必須(そうしないと矩形の板になる)、'
            + 'alpha.likelyPremultiplied=true なら blendMode=Premultiplied 以外で縁が黒く縁取られます。'
            + 'flipbookCandidates が空でなければアトラス素材で、先頭が最有力の候補です。'
            + 'classification (glow/smoke/spark/flipbook/mask) は vfx_guide の層構成recipeと突き合わせます。',
        inputSchema: {
            path: z.string().min(1).max(1024).describe('projectRoot相対または絶対のテクスチャパス'),
        },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ path }) => Safely(async () => TextResult(await bus.Query({ t: 'vfx.textureAnalyze', path }))));

    server.registerTool('shader_inspect', {
        description: 'シェーダーが公開する変数とテクスチャスロットの目録を返します。'
            + '.matのparamsやVFX MeshノードのanimatedParamは「シェーダー変数名」を要求しますが、'
            + 'その一覧を知る手段はこれだけです。'
            + '**ここに無い名前を書いても保存は通り、実行時に黙って無視されます**'
            + '(値を変えても絵が変わらない、という形でしか現れません)。'
            + 'componentsは渡す値の個数で、float3の変数へ1個だけ渡すと残りは0になります。'
            + '未使用変数はコンパイル時に消えるため、HLSLに宣言があってもここに出ないことがあります'
            + '(実際に効くのはコンパイル済みバイトコードの実体です)。'
            + 'pathは.matのshaderフィールド(guid:形式も可)またはシェーダーパスです。',
        inputSchema: {
            path: z.string().min(1).max(1024).describe('シェーダーパスまたはguid:参照'),
        },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ path }) => Safely(async () => TextResult(await bus.Query({ t: 'shader.inspect', path }))));

    server.registerTool('shader_get_compile_diagnostics', {
        description: 'DX11/DX12ランタイムコンパイルと外部HLSLビルドのエラー・警告を取得します。'
            + 'VFXEditorの赤いShader Compile Errorバナーと同じ診断を返します。'
            + 'path/entryPoint/target/messageを読み、修正後は再コンパイルして一覧から消えたことを確認してください。',
        inputSchema: {},
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, () => Safely(async () => TextResult(await bus.Query({ t: 'shader.diagnostics' }))));

    server.registerTool('vfx_analyze_material', {
        description: '.matとそのalbedoテクスチャを併せて解析します。'
            + 'ParticleEmitterにmaterialPathを設定すると、実行時にblendModeが.matの値で'
            + '**上書きされます**。つまりEmitter側のblendModeを変えても効きません。'
            + 'ブレンドを変えたい場合は.matのblend_modeを編集してください。'
            + 'findings は .mat 自体を直すべき問題(blend_mode/render_path/albedo未設定)、'
            + 'recommendations は .mat では表現できずEmitter側にしか無い設定'
            + '(alphaSource/spriteColumns/sortMode)で vfx_node_set_field へ渡せます。'
            + 'blendModeConflictsWithTexture=true は、albedoテクスチャの中身が要求するブレンドと'
            + '.matの宣言が食い違っている状態です。',
        inputSchema: {
            path: z.string().min(1).max(1024).describe('projectRoot相対または絶対の.matパス'),
        },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ path }) => Safely(async () => TextResult(await bus.Query({ t: 'vfx.materialAnalyze', path }))));

    server.registerTool('vfx_survey_assets', {
        description: 'プロジェクトの素材テクスチャを分類し、エフェクトの層構成に対して'
            + '何が足りないかを返します。'
            + '.vfxをゼロから組む前、またはvfx_guideのrecipeに沿う前に1度実行してください。'
            + 'recipeは「煙/外炎/芯/火の粉/陽炎」のような層を要求しますが、'
            + 'その層を作れる素材が手元にあるかは別問題です。'
            + 'missingRolesにある役割は手持ちでは作れないため、'
            + 'そのままrecipeを再現しようとすると破綻します(代替案はhintに含まれます)。'
            + '解析は1枚あたり数十msかかるため、既定は120枚で打ち切ります(truncatedで判ります)。'
            + '分類はsize+mtimeでキャッシュされ、2回目以降は解析し直しません'
            + '(freshlyAnalyzed / fromCache で内訳が判ります)。'
            + '既定の detail="summary" は1枚あたりpathだけを返します。'
            + '個別の素材の中身は vfx_analyze_texture で見てください。'
            + '**全素材の棚卸しが要るのは新しくグラフを組むときだけ**です。'
            + '既存.vfxの確認や修正では呼ばず、directoryで対象を絞るかlimitを下げてください。',
        inputSchema: {
            directory: z.string().min(1).max(1024).optional()
                .describe('projectRoot相対の走査ディレクトリ(既定 "Assets")。'
                    + '"Assets/Textures/Particles" のように絞ると応答も時間も大きく減ります'),
            limit: z.number().int().min(1).max(400).optional().describe('解析する最大枚数(既定120)'),
            detail: z.enum(['summary', 'full']).default('summary')
                .describe('summary=pathのみ / full=1枚ごとの解析文も含む'),
            refresh: z.boolean().optional()
                .describe('外部ツールで素材を差し替えたのに分類が変わらないときだけtrue'),
        },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ directory, limit, detail, refresh }) => Safely(async () => TextResult(await bus.Query({
        t: 'vfx.assetSurvey',
        ...(directory === undefined ? {} : { directory }),
        ...(limit === undefined ? {} : { limit }),
        detail,
        ...(refresh === undefined ? {} : { refresh }),
    }))));

    server.registerTool('vfx_preview_ensure', {
        description: 'VFX Preview Worldを起動し、プレビュー系を呼べる状態かを返します。'
            + 'vfx_preview / vfx_preview_metrics / vfx_preview_curve / vfx_runtime_state は'
            + 'すべてこのWorldを前提にしますが、Worldを所有するのはEditor本体ではなく'
            + '独立プロセスのFBZZVFXEditorです。'
            + 'このツールは必要ならそのプロセスを起動し、初期化完了まで待ってから状態を返します。'
            + 'preview系が NO_PREVIEW_WORLD で失敗したときは、まずこれを1度呼んでください。'
            + 'readyForPreview=false の場合、原因は rendererReady に出ます'
            + '(false なら描画デバイス未取得。console_logs でシェーダーコンパイル失敗を確認)。'
            + 'prepared.hasGraph=false は「まだ一度もvfx_previewを実行していない」だけで異常ではありません。'
            + 'プロセスが未起動だった場合、初回だけ起動と初期化に数秒かかります'
            + '(BUS_TIMEOUTになる場合は環境変数 FBZZ_EDITOR_BUS_TIMEOUT_MS を上げてください)。',
        inputSchema: {},
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, () => Safely(async () => TextResult(await bus.Query({ t: 'vfx.previewEnsure' }))));

    server.registerTool('vfx_runtime_state', {
        description: '直前のvfx_previewが構築した実行状態と実測コストを、その時刻のまま読み出します。'
            + 'ノードが画に出ない原因のうち「起動していない」「イベント待ちのまま起動しない」を'
            + '画像を見ずに切り分けられます。'
            + 'waitingForEvent=trueのノードはOnCollision/OnDeath待ちで、'
            + 'source側のParticleが衝突・死亡しない限り永久に起動しません。'
            + 'active=trueなのに見えない場合だけ、サイズ・色・カメラ画角を疑ってください。'
            + 'simulation.effectiveは実際に走った経路で、requestedがGpuなのにeffectiveがCpuなら'
            + 'fallbackFieldの設定が原因で黙って縮退しています(粒子数を増やしても性能は使われません)。'
            + 'cost.particlePassGpuMsはParticleパスの実測GPU時間、'
            + 'cost.overdrawは重なり枚数(vfx_previewをview="overdraw"で実行したときだけ計測)。'
            + '先にvfx_previewを実行し、readyAfterFrameまで待ってから呼びます。'
            + 'NO_PREVIEW_WORLDで失敗する場合はvfx_preview_ensureを1度呼んでください。',
        inputSchema: {},
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, () => Safely(async () => TextResult(await bus.Query({ t: 'vfx.runtime' }))));

    server.registerTool('vfx_diff', {
        description: '2つの.vfxをノード/リンク/公開パラメーター単位で比較し、変わった箇所だけを返します。'
            + '編集前後の確認や、テンプレートとの差分把握に使ってください。'
            + 'ファイル全文を2回読むより情報量が少なく、判断も速くなります。',
        inputSchema: {
            base: z.string().min(1).describe('比較元の.vfxパス'),
            target: z.string().min(1).describe('比較先の.vfxパス'),
        },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ base, target }) => Safely(async () => TextResult(await bus.Query({ t: 'vfx.diff', base, target }))));

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

    const PreviewViewSchema = z.enum(['normal', 'overdraw', 'gizmos']).default('normal')
        .describe("normal=評価用のクリーンな絵 / overdraw=半透明の重なり枚数 "
            + "/ gizmos=力場の半径・向きとエミッター形状・初速。"
            + "見た目の原因が分からないときだけ診断用へ切り替えてください");
    const CameraDescription = '視点。省略すると Editor の固定視点(正面・約5m)になります。'
        + 'ビルボードは横から見ると平面なので、シルエットの破綻は yaw:90 でしか判りません。'
        + 'ゲーム内距離で読めるかは distance を振って確認してください。';

    server.registerTool('vfx_preview', {
        description: '専用VFX WorldをrandomSeedから指定時刻へscrubし、UIやSceneを含まない決定論的PNGを返します。'
            + '画像の良し悪しを目で判断する前に、vfx_preview_metricsで機械的に判る破綻'
            + '(白飛び・覆いすぎ・何も出ていない)を潰しておくと反復が収束します。',
        inputSchema: {
            path: z.string().min(1),
            time: z.number().finite().nonnegative(),
            w: z.number().int().min(160).max(1920).default(960),
            h: z.number().int().min(90).max(1080).default(540),
            view: PreviewViewSchema,
            camera: VFXPreviewCameraSchema.optional().describe(CameraDescription),
        },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ path, time, w, h, view, camera }) => Safely(async () => {
        const prepared = await PreparePreview(bus, { path, time, w, h, view, camera });
        const capture = ViewportCaptureResultSchema.parse(await bus.Query({ t: 'viewport.capture', w, h, view: 'vfx' }));
        return {
            content: [
                { type: 'text', text: JSON.stringify(prepared, null, 2) },
                { type: 'image', data: capture.base64, mimeType: capture.mimeType },
            ],
            structuredContent: { prepared, width: capture.width, height: capture.height, view: 'vfx' },
        };
    }));

    server.registerTool('vfx_preview_metrics', {
        description: '直前と同じ手順でプレビューを組み、画像ではなく数値で評価します。'
            + '輝度ヒストグラム/白飛び率/画面占有率/重心/前サンプルからの変化量を返し、'
            + '機械的に判る破綻(BLOWN_OUT・SCREEN_FLOODED・EMPTY_FRAME・STATIC_FRAME・OFF_CENTER)を'
            + 'issuesとして名指しします。'
            + '「少し暗い」の“少し”に基準が無いまま画像だけで直すと反復が振動するため、'
            + 'まずこれでissuesを空にしてから、残った「らしさ」を画像で詰めてください。',
        inputSchema: {
            path: z.string().min(1),
            time: z.number().finite().nonnegative(),
            w: z.number().int().min(160).max(1920).default(960),
            h: z.number().int().min(90).max(1080).default(540),
            view: PreviewViewSchema,
            camera: VFXPreviewCameraSchema.optional().describe(CameraDescription),
        },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ path, time, w, h, view, camera }) => Safely(async () => {
        const measured = await MeasurePreview(bus, { path, time, w, h, view, camera });
        return TextResult(measured);
    }));

    server.registerTool('vfx_preview_curve', {
        description: 'エフェクトを全区間サンプルし、指標の時系列(=時間の形)を返します。画像は返しません。'
            + 'エフェクトの質は静止画ではなく立ち上がりの速さ・ピークの位置・消え際の粘りで決まりますが、'
            + 'それは3枚の静止画からは判定できません。'
            + 'peakNormalized(ピーク位置を全長で正規化した値)が判るので、'
            + '「爆発なのにピークが t=0.62 にある」のような時間設計の誤りを直接指摘できます。'
            + 'AAAの爆発はピークが概ね0.15より手前、煙や炎は中盤〜後半が目安です。',
        inputSchema: {
            path: z.string().min(1),
            samples: z.number().int().min(3).max(16).default(8)
                .describe('再生全長を等分するサンプル数'),
            times: z.array(z.number().finite().nonnegative()).min(2).max(16).optional()
                .describe('秒。指定するとsamplesより優先する'),
            w: z.number().int().min(160).max(1280).default(480),
            h: z.number().int().min(90).max(720).default(270),
            camera: VFXPreviewCameraSchema.optional().describe(CameraDescription),
        },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ path, samples, times, w, h, camera }) => Safely(async () => {
        const request: PreviewRequest = { path, time: 0, w, h, view: 'normal', camera };
        const sampleTimes = await ResolveSampleTimes(bus, request, times, samples);
        const series: Array<{ time: number } & Record<string, number>> = [];
        const issues: Array<{ time: number; issues: string[] }> = [];
        for (const time of sampleTimes) {
            const measured = await MeasurePreview(bus, { ...request, time });
            series.push({ time: Number(time.toFixed(4)), ...FlattenMetrics(measured.metrics) });
            const frameIssues = (measured.metrics.issues ?? []) as string[];
            if (frameIssues.length > 0) issues.push({ time: Number(time.toFixed(4)), issues: frameIssues });
        }
        return TextResult({
            path,
            series,
            shape: {
                luminance: SummarizeCurve(series, 'luminanceMean'),
                coverage: SummarizeCurve(series, 'coverage'),
            },
            issuesByTime: issues,
            hint: 'peakNormalized は 0=開始 / 1=終了。tailRatio が 0.5 を超えていれば、'
                + '再生終了時点でまだピークの半分が残っている = 消え際が切れていません。'
                + 'motion がほぼ 0 の区間が続くなら、そこはエフェクトが止まって見えています。',
        });
    }));

    server.registerTool('vfx_preview_compare', {
        description: '2つの.vfx(または編集前後)を同条件でサンプルし、指標の差を返します。'
            + '画像2枚からは「良くなった」のか「変わっただけ」なのかを言えませんが、'
            + 'ここでは変化量が符号付きで出ます。'
            + '例: emitRateを上げてcoverageが1.8倍・luminanceがほぼ同じ=コストだけ増えた、が判ります。',
        inputSchema: {
            base: z.string().min(1).describe('比較元の.vfxパス'),
            target: z.string().min(1).describe('比較先の.vfxパス'),
            samples: z.number().int().min(2).max(12).default(5),
            w: z.number().int().min(160).max(1280).default(480),
            h: z.number().int().min(90).max(720).default(270),
            camera: VFXPreviewCameraSchema.optional().describe(CameraDescription),
        },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ base, target, samples, w, h, camera }) => Safely(async () => {
        // 両者を同じ時刻列で測る。base 側の duration を基準にしないと、
        // 「長さが違うだけ」の差を「形が変わった」と読み違える。
        const baseRequest: PreviewRequest = { path: base, time: 0, w, h, view: 'normal', camera };
        const sampleTimes = await ResolveSampleTimes(bus, baseRequest, undefined, samples);
        const Collect = async (path: string) => {
            const collected: Array<{ time: number } & Record<string, number>> = [];
            for (const time of sampleTimes) {
                const measured = await MeasurePreview(bus, { path, time, w, h, view: 'normal', camera });
                collected.push({ time: Number(time.toFixed(4)), ...FlattenMetrics(measured.metrics) });
            }
            return collected;
        };
        const baseSeries = await Collect(base);
        const targetSeries = await Collect(target);
        // 各指標の平均で比較する。1 サンプルの偶然の差に引きずられないため。
        const keys = ['luminanceMean', 'coveredLuminanceMean', 'clippedRatio', 'blownOutRatio',
                      'coverage', 'motion'] as const;
        const Average = (series: Array<Record<string, number>>, key: string): number =>
            series.reduce((sum, sample) => sum + Number(sample[key] ?? 0), 0) / Math.max(1, series.length);
        const delta: Record<string, { base: number; target: number; delta: number; ratio: number | null }> = {};
        for (const key of keys) {
            const baseValue = Average(baseSeries, key);
            const targetValue = Average(targetSeries, key);
            delta[key] = {
                base: Number(baseValue.toFixed(6)),
                target: Number(targetValue.toFixed(6)),
                delta: Number((targetValue - baseValue).toFixed(6)),
                ratio: baseValue > 1e-9 ? Number((targetValue / baseValue).toFixed(3)) : null,
            };
        }
        return TextResult({
            base, target, times: sampleTimes,
            averages: delta,
            shape: {
                base: SummarizeCurve(baseSeries, 'luminanceMean'),
                target: SummarizeCurve(targetSeries, 'luminanceMean'),
            },
            hint: 'coverage だけが増えて luminance が変わらない変更は、コストを払って見た目が変わっていません。'
                + 'peakTime がずれていれば、時間設計そのものが変わっています。',
        });
    }));

    server.registerTool('vfx_candidate_evaluate', {
        description: '候補.vfxを固定した品質契約で全区間評価し、採択・棄却・意味評価待ちを返します。'
            + 'lint、白飛び、画面占有、ピーク位置、tailを一度に検査するため、反復ごとに評価基準が変わりません。'
            + '機械判定を通過した後、vfx_preview画像を見てsemanticScoreを付けて再実行してください。'
            + 'semanticScoreなしでAAA品質を自動承認することはありません。',
        inputSchema: {
            path: z.string().min(1).describe('評価する候補.vfx'),
            samples: z.number().int().min(3).max(16).default(8),
            w: z.number().int().min(160).max(1280).default(480),
            h: z.number().int().min(90).max(720).default(270),
            camera: VFXPreviewCameraSchema.optional().describe(CameraDescription),
            objective: z.object({
                maxLintErrors: z.number().int().min(0).default(0),
                maxLintWarnings: z.number().int().min(0).default(0),
                minCoverage: z.number().min(0).max(1).default(0.01),
                maxCoverage: z.number().min(0).max(1).default(0.65),
                maxClippedRatio: z.number().min(0).max(1).default(0.02),
                peakBefore: z.number().min(0).max(1).default(0.35),
                maxTailRatio: z.number().min(0).max(1).default(0.5),
                minSemanticScore: z.number().min(0).max(1).default(0.8),
            }).describe('反復中に変更しない品質契約。Smoke/Fireなど遅い効果ではpeakBeforeを明示調整する'),
            semanticAssessment: z.object({
                score: z.number().min(0).max(1),
                rationale: z.string().min(1).max(2000),
            }).optional().describe('画像を見たAI/アートディレクターの意味・らしさ評価'),
        },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ path, samples, w, h, camera, objective, semanticAssessment }) => Safely(async () => {
        const lint = await bus.Query({ t: 'vfx.lint', path }) as Record<string, unknown>;
        const request: PreviewRequest = { path, time: 0, w, h, view: 'normal', camera };
        const times = await ResolveSampleTimes(bus, request, undefined, samples);
        const series: Array<{ time: number } & Record<string, number>> = [];
        const previewIssues: Array<{ time: number; issues: string[] }> = [];
        for (const time of times) {
            const measured = await MeasurePreview(bus, { ...request, time });
            series.push({ time: Number(time.toFixed(4)), ...FlattenMetrics(measured.metrics) });
            const issues = Array.isArray(measured.metrics.issues)
                ? measured.metrics.issues.filter((value): value is string => typeof value === 'string')
                : [];
            if (issues.length > 0) previewIssues.push({ time: Number(time.toFixed(4)), issues });
        }

        const luminanceShape = SummarizeCurve(series, 'luminanceMean');
        const coverageShape = SummarizeCurve(series, 'coverage');
        const average = (key: string): number =>
            series.reduce((sum, sample) => sum + Number(sample[key] ?? 0), 0) / Math.max(1, series.length);
        const lintErrors = Number(lint.errors ?? 0);
        const lintWarnings = Number(lint.warnings ?? 0);
        const peakCoverage = Math.max(...series.map((sample) => Number(sample.coverage ?? 0)));
        const clippedRatio = Math.max(...series.map((sample) => Number(sample.clippedRatio ?? 0)));
        const violations: string[] = [];
        if (lintErrors > objective.maxLintErrors) violations.push(`lint.errors ${lintErrors} > ${objective.maxLintErrors}`);
        if (lintWarnings > objective.maxLintWarnings) violations.push(`lint.warnings ${lintWarnings} > ${objective.maxLintWarnings}`);
        if (peakCoverage < objective.minCoverage) violations.push(`coverage.peak ${peakCoverage} < ${objective.minCoverage}`);
        if (peakCoverage > objective.maxCoverage) violations.push(`coverage.peak ${peakCoverage} > ${objective.maxCoverage}`);
        if (clippedRatio > objective.maxClippedRatio) violations.push(`clippedRatio.max ${clippedRatio} > ${objective.maxClippedRatio}`);
        if (Number(luminanceShape.peakNormalized ?? 0) > objective.peakBefore)
            violations.push(`luminance.peakNormalized ${luminanceShape.peakNormalized} > ${objective.peakBefore}`);
        if (Number(luminanceShape.tailRatio ?? 0) > objective.maxTailRatio)
            violations.push(`luminance.tailRatio ${luminanceShape.tailRatio} > ${objective.maxTailRatio}`);

        let decision: 'reject' | 'needs_semantic_review' | 'accept' = violations.length > 0
            ? 'reject' : 'needs_semantic_review';
        if (violations.length === 0 && semanticAssessment !== undefined) {
            if (semanticAssessment.score >= objective.minSemanticScore) decision = 'accept';
            else {
                decision = 'reject';
                violations.push(`semanticScore ${semanticAssessment.score} < ${objective.minSemanticScore}`);
            }
        }
        return TextResult({
            path,
            decision,
            objective,
            violations,
            lint,
            previewIssues,
            metrics: {
                luminance: luminanceShape,
                coverage: coverageShape,
                peakCoverage: Number(peakCoverage.toFixed(6)),
                averageCoverage: Number(average('coverage').toFixed(6)),
                maxClippedRatio: Number(clippedRatio.toFixed(6)),
            },
            semanticAssessment: semanticAssessment ?? null,
            next: decision === 'accept'
                ? 'vfx_candidate_acceptで本番へ採択し、vfx_knowledge_promoteで成功例を知識化してください。'
                : decision === 'needs_semantic_review'
                    ? 'vfx_previewでピーク前後の画像を確認し、semanticAssessmentを付けて再評価してください。'
                    : 'violationsとlint.issuesを直し、同じobjectiveのまま再評価してください。',
        });
    }));

    server.registerTool('vfx_preview_sweep', {
        description: '同じ時刻のエフェクトを複数の距離から評価し、距離ごとの指標と画像を返します。'
            + '近接で作り込んだディテールは10m先では消え、逆に近くでは板が透けて見えます。'
            + 'lodNearDistance/lodFarDistanceを設定しても、距離を変えなければ切り替わりを一度も見られません。'
            + 'LOD検証と「ゲーム内距離で読めるか」の確認はこれ1回で済みます。',
        inputSchema: {
            path: z.string().min(1),
            time: z.number().finite().nonnegative().describe('評価する時刻。通常はピーク付近'),
            distances: z.array(z.number().finite().min(0.05).max(500)).min(1).max(6)
                .default([2, 5, 12, 30]).describe('注視点からの距離 m'),
            yaw: z.number().finite().min(-360).max(360).default(25).describe('度。90で真横=シルエット確認'),
            pitch: z.number().finite().min(-89).max(89).default(12),
            w: z.number().int().min(160).max(1280).default(480),
            h: z.number().int().min(90).max(720).default(270),
            includeImages: z.boolean().default(true).describe('falseにすると指標だけを返す(高速)'),
        },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ path, time, distances, yaw, pitch, w, h, includeImages }) => Safely(async () => {
        const content: Array<{ type: 'text'; text: string } | { type: 'image'; data: string; mimeType: string }> = [];
        const series: Array<{ distance: number } & Record<string, number>> = [];
        for (const distance of distances) {
            const camera: VFXPreviewCamera = { distance, yaw, pitch };
            const measured = await MeasurePreview(bus, { path, time, w, h, view: 'normal', camera });
            series.push({ distance, ...FlattenMetrics(measured.metrics) });
            if (!includeImages) continue;
            const capture = ViewportCaptureResultSchema.parse(
                await bus.Query({ t: 'viewport.capture', w, h, view: 'vfx' }));
            content.push({ type: 'text', text: `distance = ${distance}m` });
            content.push({ type: 'image', data: capture.base64, mimeType: capture.mimeType });
        }
        content.unshift({
            type: 'text',
            text: JSON.stringify({
                path, time, yaw, pitch, series,
                hint: '遠距離で coverage が急に 0 近くまで落ちていれば、そこで LOD がエフェクトを消しています。'
                    + '逆に距離を離しても coverage がほとんど減らないなら、'
                    + 'そのエフェクトは遠景でも画面を占有し続けている = 引きの絵で邪魔になります。',
            }, null, 2),
        });
        return { content, structuredContent: { path, time, series } };
    }));

    server.registerTool('vfx_preview_sequence', {
        description: '.vfxを複数時刻でプレビューし、連番画像をまとめて返します。'
            + '静止画1枚では「動いているか」「タイミングが合っているか」が判断できないため、'
            + 'エフェクトの見た目を確認するときはこちらを使ってください。'
            + 'times未指定なら再生全長を等間隔にサンプルします。'
            + '時間の形を数値で評価したいときは vfx_preview_curve を使ってください。',
        inputSchema: {
            path: z.string().min(1),
            times: z.array(z.number().finite().nonnegative()).min(1).max(8).optional()
                .describe('秒。未指定なら duration を等分した6点'),
            w: z.number().int().min(160).max(1280).default(640),
            h: z.number().int().min(90).max(720).default(360),
            view: PreviewViewSchema,
            camera: VFXPreviewCameraSchema.optional().describe(CameraDescription),
        },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ path, times, w, h, view, camera }) => Safely(async () => {
        const request: PreviewRequest = { path, time: 0, w, h, view, camera };
        const sampleTimes = await ResolveSampleTimes(bus, request, times, 6);
        const content: Array<{ type: 'text'; text: string } | { type: 'image'; data: string; mimeType: string }> = [];
        const frames: Array<{ time: number; width: number; height: number }> = [];
        for (const time of sampleTimes) {
            // 各時刻ごとに preview を作り直す。Preview World は randomSeed から
            // 決定論的に再シミュレートされるので、同じ時刻なら常に同じ絵になる。
            await PreparePreview(bus, { ...request, time });
            const capture = ViewportCaptureResultSchema.parse(
                await bus.Query({ t: 'viewport.capture', w, h, view: 'vfx' }));
            content.push({ type: 'text', text: `t = ${time.toFixed(3)}s` });
            content.push({ type: 'image', data: capture.base64, mimeType: capture.mimeType });
            frames.push({ time, width: capture.width, height: capture.height });
        }
        return { content, structuredContent: { path, frames } };
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
        description: 'Animatorステートマシンの構造(states/transitions/parameters・BlendTree駆動パラメーター・anyState遷移・遷移条件)を、パラメーターのライブ値と現在ステート付きで取得します。layersにレイヤー一覧(weight/mode/maskPath/現在ステート/Slot状態)とbaseLayerMaskPathも常に含まれるため、上半身レイヤーの構成確認もこれ1本で行えます。layer引数でそのレイヤー独自のステートマシンを覗けます。',
        inputSchema: {
            id: NodeIdSchema,
            layer: z.string().min(1).max(128).optional()
                .describe('レイヤー名。指定でそのレイヤー独自のステートマシンを返す。省略でBase Layer'),
        },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ id, layer }) => Safely(async () => TextResult(await bus.Query({
        t: 'animation.graph', id, ...(layer === undefined ? {} : { layer }),
    }))));

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

    // ── Behavior Tree の編集 ──
    server.registerTool('bt_node_add', {
        description: 'Behavior Tree へノードを追加します。parentId 省略はルート作成で、'
            + '既にルートがあると拒否されます (木にルートは 1 つだけ)。'
            + '子数上限・循環はここで拒否されるため、保存時まで気付かない失敗になりません。',
        inputSchema: {
            path: z.string().min(1),
            nodeType: z.string().min(1).max(64)
                .describe('bt_guide の nodeTypes に載っている種別名 (Sequence / Selector / MoveTo …)'),
            parentId: z.number().int().min(1).optional(),
            name: z.string().min(1).max(128).optional(),
        }, annotations: writeAnnotations,
    }, ({ path, nodeType, parentId, name }) => run({
        t: 'bt.node.add', path, nodeType,
        ...(parentId === undefined ? {} : { parentId }),
        ...(name === undefined ? {} : { name }),
    }));

    server.registerTool('bt_node_remove', {
        description: 'ノードとその子孫をまとめて削除します。'
            + 'WHY 子孫ごとか: 親だけ消すと枝が浮き、残された枝が何のためのものか判らなくなります。',
        inputSchema: { path: z.string().min(1), nodeId: z.number().int().min(1) },
        annotations: writeAnnotations,
    }, ({ path, nodeId }) => run({ t: 'bt.node.remove', path, nodeId }));

    server.registerTool('bt_node_set_parent', {
        description: 'ノードの親を張り替えます。循環・子数上限・葉への子付けは拒否されます。',
        inputSchema: {
            path: z.string().min(1), nodeId: z.number().int().min(1),
            parentId: z.number().int().min(0).describe('0 で親なし (ルート)'),
        }, annotations: writeAnnotations,
    }, ({ path, nodeId, parentId }) => run({ t: 'bt.node.setParent', path, nodeId, parentId }));

    server.registerTool('bt_node_set_order', {
        description: '同じ親の中での評価順 (優先度) を変えます。小さいほど先に評価されます。'
            + 'Selector ではこれが「やりたいことの優先順位」そのもので、'
            + '並べ替えを怠ると巡回が先に成功して戦闘へ入らない、という形で静かに壊れます。',
        inputSchema: {
            path: z.string().min(1), nodeId: z.number().int().min(1),
            order: z.number().int().min(0).max(4096),
        }, annotations: writeAnnotations,
    }, ({ path, nodeId, order }) => run({ t: 'bt.node.setOrder', path, nodeId, order }));

    server.registerTool('bt_node_set_field', {
        description: 'ノードのフィールドを 1 つ変更します。'
            + 'abortMode は純粋条件ノードにしか設定できず、それ以外へ渡すと理由付きで拒否されます。'
            + 'keyName / moveTargetKey は Blackboard に実在するキーだけを受け付けます '
            + '(存在しない名前は保存も通り、実行時に黙って無視されるため)。',
        inputSchema: {
            path: z.string().min(1), nodeId: z.number().int().min(1),
            field: z.string().min(1).max(64)
                .describe('name / duration / durationRandom / repeatCount / range / keyName / '
                    + 'moveTargetKey / animatorTrigger / scriptMethod / abortMode / compareOp など'),
            value: JsonValueSchema,
        }, annotations: writeAnnotations,
    }, ({ path, nodeId, field, value }) => run({ t: 'bt.node.setField', path, nodeId, field, value }));

    server.registerTool('bt_blackboard_add', {
        description: 'Blackboard キーを追加します。ノードがキーを参照する前にこれで作ってください。',
        inputSchema: {
            path: z.string().min(1), name: z.string().min(1).max(64),
            type: z.enum(['bool', 'int', 'float', 'vector3', 'entity', 'string']).optional(),
        }, annotations: writeAnnotations,
    }, ({ path, name, type }) => run({
        t: 'bt.blackboard.add', path, name, ...(type === undefined ? {} : { type }),
    }));

    server.registerTool('bt_blackboard_remove', {
        description: 'Blackboard キーを削除します。予約キー (reserved=true) は拒否されます。',
        inputSchema: { path: z.string().min(1), name: z.string().min(1).max(64) },
        annotations: writeAnnotations,
    }, ({ path, name }) => run({ t: 'bt.blackboard.remove', path, name }));

    server.registerTool('bt_auto_layout', {
        description: '木の深さを列にして並べ直します。ノードを追加した後に呼ぶと、'
            + '人が開いたときに構造が読める配置になります。',
        inputSchema: { path: z.string().min(1) },
        annotations: writeAnnotations,
    }, ({ path }) => run({ t: 'bt.autoLayout', path }));

    server.registerTool('vfx_template_apply', {
        description: '組み込みまたはvfx_knowledge_catalogで見つけた任意のTemplateを適用します。'
            + 'mode=replace(既定)は新しい.vfxへ複製、mode=mergeは既存.vfxへ層を追記、'
            + 'mode=subgraphは複製せずSub Graphノードとして参照します。'
            + 'mergeはノードを1個ずつaddするより確実で、パラメーター・binding・Variant・'
            + 'Signal Graphまで欠落なく持ち込みます。応答のdetailに改名されたパラメーター・'
            + '引き上げられたbudget・不足素材が載るため、次の手で存在しない名前を指さずに済みます。'
            + 'dry-run・Undoに対応します。',
        inputSchema: {
            template: z.string().min(1).max(260)
                .describe('Template名、またはprojectRoot相対の.vfxパス'),
            path: z.string().min(1)
                .describe('projectRoot相対の.vfx。replaceでは出力先、merge/subgraphでは取り込み先(既存必須)'),
            name: z.string().min(1).max(128).optional(),
            mode: z.enum(['replace', 'merge', 'subgraph']).optional()
                .describe('replace=複製 / merge=追記 / subgraph=参照(Template更新が伝播)'),
            groups: z.array(z.number().int().min(1)).max(64).optional()
                .describe('取り込む層のgroupId。vfx_knowledge_catalogのlayersで確認する。省略で全体'),
            anchorNodeId: z.number().int().min(1).optional()
                .describe('取り込んだ塊の発火元ノード。省略でEntry'),
            trigger: z.enum(['onComplete', 'onStart', 'onCollision', 'onDeath']).optional()
                .describe('anchorNodeIdからの発火条件。onCollision/onDeathはParticleのみ'),
            delay: z.number().min(0).max(600).optional(),
            parentNodeId: z.number().int().min(1).optional()
                .describe('空間の親(Transform階層)。発火順ではない'),
            variant: z.string().min(1).max(128).optional()
                .describe('適用するVariant Set名。公開パラメーターの既定値へ焼き込む'),
            raiseBudget: z.boolean().optional()
                .describe('取り込み後の実使用量へbudget上限を合わせる(既定true)。'
                    + 'falseにすると検証は通るのに実行時だけ粒子が出ない状態を作れる'),
        }, annotations: writeAnnotations,
    }, ({ template, path, name, mode, groups, anchorNodeId, trigger, delay, parentNodeId,
          variant, raiseBudget }) => run({
        t: 'vfx.template.apply', template, path,
        ...(name === undefined ? {} : { name }),
        ...(mode === undefined ? {} : { mode }),
        ...(groups === undefined ? {} : { groups }),
        ...(anchorNodeId === undefined ? {} : { anchorNodeId }),
        ...(trigger === undefined ? {} : { trigger }),
        ...(delay === undefined ? {} : { delay }),
        ...(parentNodeId === undefined ? {} : { parentNodeId }),
        ...(variant === undefined ? {} : { variant }),
        ...(raiseBudget === undefined ? {} : { raiseBudget }),
    }));

    server.registerTool('vfx_candidate_fork', {
        description: '既存.vfxまたは成功Templateを変更せず、反復専用の候補.vfxへ分岐します。'
            + '候補だけをコード差分で編集するため、各試行を独立比較でき、失敗時も元へ戻す操作が不要です。',
        inputSchema: {
            source: z.string().min(1).max(260).describe('元の.vfxパスまたはTemplate名'),
            candidatePath: z.string().min(1).describe('projectRoot相対の候補.vfx出力先'),
            name: z.string().min(1).max(128).optional(),
        },
        annotations: writeAnnotations,
    }, ({ source, candidatePath, name }) => run({
        t: 'vfx.template.apply', template: source, path: candidatePath,
        ...(name === undefined ? {} : { name }),
    }));

    server.registerTool('vfx_candidate_accept', {
        description: 'vfx_candidate_evaluateでacceptになった候補だけを本番.vfxへ採択します。'
            + '本番の旧内容はUndoへ保持されるため、生成と採択を分離したまま安全に反復できます。',
        inputSchema: {
            candidatePath: z.string().min(1).max(260),
            targetPath: z.string().min(1).describe('projectRoot相対の本番.vfx'),
            name: z.string().min(1).max(128).optional(),
        },
        annotations: writeAnnotations,
    }, ({ candidatePath, targetPath, name }) => run({
        t: 'vfx.template.apply', template: candidatePath, path: targetPath,
        ...(name === undefined ? {} : { name }),
    }));

    server.registerTool('vfx_knowledge_promote', {
        description: '採択済み候補をプロジェクト固有の再利用可能Templateへ昇格します。'
            + '用途・成功条件はdescriptionとtagsへ保存され、Editorのカタログと'
            + 'vfx_knowledge_catalogの両方から同じ文言で検索できます。'
            + '評価がacceptになる前には呼ばないでください。',
        inputSchema: {
            path: z.string().min(1).max(260).describe('採択済み.vfx'),
            name: z.string().regex(/^[A-Za-z0-9_-]+$/).min(1).max(96)
                .describe('Templateファイル名。パス区切り不可'),
            summary: z.string().min(1).max(512)
                .describe('用途・演出意図・成功条件。Templateのdescriptionとして保存される'),
            category: z.string().regex(/^[A-Za-z0-9_-]+$/).max(64).optional()
                .describe('Templates直下のサブフォルダ。カタログではカテゴリとして畳まれる'),
            tags: z.array(z.string().min(1).max(48)).max(16).optional()
                .describe('検索用のタグ。descriptionと合わせて部分一致の対象になる'),
        },
        annotations: writeAnnotations,
    }, ({ path, name, summary, category, tags }) => run({
        t: 'vfx.template.apply',
        template: path,
        // 名前はファイル名。説明を名前へ押し込むと、カタログの表示名が説明文になる。
        path: category === undefined || category === ''
            ? `Assets/VFX/Templates/${name}.vfx`
            : `Assets/VFX/Templates/${category}/${name}.vfx`,
        name,
        description: summary,
        ...(tags === undefined ? {} : { tags }),
    }));

    server.registerTool('vfx_optimize_budget', {
        description: 'エフェクトのコストを下げます。適用前dry-runとUndoに対応します。'
            + '削る対象はstrategyで選びます。'
            + 'particles=発生数・上限を比例で下げる(従来の挙動)。'
            + 'fillRate=粒を大きくして枚数を減らし、寄与の大きい層を遠距離で間引く。'
            + 'both=両方。'
            + '重要: パーティクルの実コストは粒子数ではなく塗った画素数(fill rate)で決まります。'
            + '原因がfill rateのときに粒子数だけ減らすと、効きが悪いうえ見た目だけが痩せます。'
            + 'どちらが効くかはvfx_runtime_stateのcost.overdrawとcost.particlePassGpuMsで判断してください。'
            + 'overdraw.meanLayersが大きい(5枚超)ならfillRate、'
            + '粒子数がbudget超過なだけならparticlesです。',
        inputSchema: {
            path: z.string().min(1),
            targetParticles: z.number().int().min(1).max(10000000).optional()
                .describe('strategy=fillRate のときだけ省略できます'),
            strategy: z.enum(['particles', 'fillRate', 'both']).optional()
                .describe('省略時は particles (従来の挙動)'),
        },
        annotations: writeAnnotations,
    }, ({ path, targetParticles, strategy }) => run({
        t: 'vfx.optimize', path,
        ...(targetParticles === undefined ? {} : { targetParticles }),
        ...(strategy === undefined ? {} : { strategy }),
    }));

    server.registerTool('vfx_repair', {
        description: 'vfx_lintが autoFixable=true を返した不備を自動修正します。対応する code は '
            + 'UNREACHABLE_NODE / MISSING_ASSET / SHEARED_SPRITE / LIGHTING_SATURATED / '
            + 'ALPHA_NO_SORT / MESH_NO_FADE / PARENT_HAS_NO_TRANSFORM です。'
            + '既定では全て有効なので、通常は path だけ渡してください。'
            + '直し方が一意に決まるものだけを扱い、設計判断(何を出すか)には触れません。'
            + 'lint → repair → lint の順で使い、残ったissueをfixに従って手で直します。'
            + 'dry-runとUndoに対応します。',
        inputSchema: {
            path: z.string().min(1),
            connectOrphans: z.boolean().optional().describe('孤立ノードをEntryへ接続する (既定true)'),
            fixAssets: z.boolean().optional().describe('欠落アセットパスを近い候補へ置換する (既定true)'),
            fixSprites: z.boolean().optional().describe('回転と併用された非等方sizeAxisScaleを等方へ戻す (既定true)'),
            fixLighting: z.boolean().optional().describe('1.0超のlightingStrengthを0.8へ落とす (既定true)'),
            fixSorting: z.boolean().optional().describe('半透明エミッターのsortModeをBackToFrontにする (既定true)'),
            fixMeshFade: z.boolean().optional().describe('Meshノードのmesh.colorEnd RGBを0にする (既定true)'),
            fixParents: z.boolean().optional().describe('Entry/Delayを親にした無効なparentNodeIdを外す (既定true)'),
        },
        annotations: writeAnnotations,
    }, ({ path, connectOrphans, fixAssets, fixSprites, fixLighting, fixSorting, fixMeshFade, fixParents }) => run({
        t: 'vfx.repair', path,
        ...(connectOrphans === undefined ? {} : { connectOrphans }),
        ...(fixAssets === undefined ? {} : { fixAssets }),
        ...(fixSprites === undefined ? {} : { fixSprites }),
        ...(fixLighting === undefined ? {} : { fixLighting }),
        ...(fixSorting === undefined ? {} : { fixSorting }),
        ...(fixMeshFade === undefined ? {} : { fixMeshFade }),
        ...(fixParents === undefined ? {} : { fixParents }),
    }));

    server.registerTool('vfx_variant_upsert', {
        description: '公開パラメーター値の名前付きVariant Setを一括作成・置換します。複数styleのAI量産に使います。',
        inputSchema: { path: z.string().min(1), name: z.string().min(1).max(128), values: z.record(z.string(), JsonValueSchema) },
        annotations: writeAnnotations,
    }, ({ path, name, values }) => run({ t: 'vfx.variant.upsert', path, name, values }));

    server.registerTool('vfx_variant_remove', {
        description: '名前付きVariant Setを削除します。Undo可能です。',
        inputSchema: { path: z.string().min(1), name: z.string().min(1).max(128) },
        annotations: writeAnnotations,
    }, ({ path, name }) => run({ t: 'vfx.variant.remove', path, name }));

    server.registerTool('vfx_graph_set', {
        description: 'Graph名と粒子・Light・Audioのbudget上限を変更します。UIのGraph Settingsと同じ保存面をUndo可能に編集します。',
        inputSchema: {
            path: z.string().min(1),
            name: z.string().min(1).max(128).optional(),
            maxParticles: z.number().int().min(1).max(10000000).optional(),
            maxLights: z.number().int().min(0).max(1024).optional(),
            maxAudioVoices: z.number().int().min(0).max(1024).optional(),
        },
        annotations: writeAnnotations,
    }, ({ path, name, maxParticles, maxLights, maxAudioVoices }) => run({
        t: 'vfx.graph.set', path,
        ...(name === undefined ? {} : { name }),
        ...(maxParticles === undefined ? {} : { maxParticles }),
        ...(maxLights === undefined ? {} : { maxLights }),
        ...(maxAudioVoices === undefined ? {} : { maxAudioVoices }),
    }));

    server.registerTool('vfx_node_add', {
        description: '検証済み.vfxへEffectノードを追加し、任意のsourceから接続します。budget/DAG超過は拒否されUndo可能です。',
        inputSchema: {
            path: z.string().min(1),
            // C++ の ParseVFXNodeType / editorContracts.ts と3箇所そろえること。
            nodeType: z.enum(['particle', 'trail', 'meshTrail', 'light', 'audio', 'decal', 'delay',
                'subGraph', 'forceField', 'mesh', 'screenEffect', 'cameraShake', 'timeScale', 'wind',
                'reroute'])
                .describe('forceField=風/吸引/渦/乱流, mesh=衝撃波シェル, screenEffect=画面フラッシュ, '
                    + 'cameraShake=カメラ揺れ, timeScale=ヒットストップ, wind=風域, '
                    + 'reroute=配線の中継点(実行のタイミングには影響しない。Canvas の見た目専用)'),
            name: z.string().min(1).max(128).optional(),
            from: z.number().int().positive().optional(),
            assetPath: z.string().min(1).optional(),
        }, annotations: writeAnnotations,
    }, ({ path, nodeType, name, from, assetPath }) => run({
        t: 'vfx.node.add', path, nodeType,
        ...(name === undefined ? {} : { name }), ...(from === undefined ? {} : { from }),
        ...(assetPath === undefined ? {} : { assetPath }),
    }));

    server.registerTool('vfx_node_duplicate', {
        description: 'VFXノードの全設定を複製し、新しいNodeIdを割り当てます。任意の名前・Canvas座標を指定でき、Undo可能です。',
        inputSchema: {
            path: z.string().min(1), nodeId: z.number().int().positive(),
            name: z.string().min(1).max(128).optional(),
            editorX: z.number().finite().optional(), editorY: z.number().finite().optional(),
        },
        annotations: writeAnnotations,
    }, ({ path, nodeId, name, editorX, editorY }) => run({
        t: 'vfx.node.duplicate', path, nodeId,
        ...(name === undefined ? {} : { name }),
        ...(editorX === undefined ? {} : { editorX }),
        ...(editorY === undefined ? {} : { editorY }),
    }));

    server.registerTool('vfx_node_remove', {
        description: 'VFXノードと関連link/bindingを検証付きで削除します。',
        inputSchema: { path: z.string().min(1), nodeId: z.number().int().positive() }, annotations: writeAnnotations,
    }, ({ path, nodeId }) => run({ t: 'vfx.node.remove', path, nodeId }));

    server.registerTool('vfx_node_set_enabled', {
        description: 'VFXノードを配線を保ったまま有効化・無効化します。無効ノードはpreviewとbudgetから除外され、Undo可能です。',
        inputSchema: { path: z.string().min(1), nodeId: z.number().int().positive(), enabled: z.boolean() },
        annotations: writeAnnotations,
    }, ({ path, nodeId, enabled }) => run({ t: 'vfx.node.setEnabled', path, nodeId, enabled }));

    server.registerTool('vfx_node_set_metadata', {
        description: 'ノードの表示名とCanvas座標を更新します。実行パラメーターではないEditor情報もAIから完全に編集できます。',
        inputSchema: {
            path: z.string().min(1), nodeId: z.number().int().positive(),
            name: z.string().min(1).max(128).optional(),
            editorX: z.number().finite().optional(), editorY: z.number().finite().optional(),
        },
        annotations: writeAnnotations,
    }, ({ path, nodeId, name, editorX, editorY }) => run({
        t: 'vfx.node.setMetadata', path, nodeId,
        ...(name === undefined ? {} : { name }),
        ...(editorX === undefined ? {} : { editorX }),
        ...(editorY === undefined ? {} : { editorY }),
    }));

    server.registerTool('vfx_node_set_parent', {
        description: 'VFXノードの「空間の親」を設定します。親のPosition/Rotation/Scaleがこのノードへ合成され、'
            + '親を動かすと子もついてきます。'
            + 'これはgraph link(いつ発火するか)とは完全に別の軸です。'
            + 'linkで繋いだだけでは位置は継承されず、親子にしただけでは発火順は変わりません。'
            + 'parentNodeIdを省略または-1にすると親を外します。'
            + '親にできるのは実体を持つノード(Particle/Mesh/Light/Trail等)だけで、'
            + 'Entry/Delayを指定するとPARENT_HAS_NO_TRANSFORMで拒否されます。'
            + '循環はPARENT_CYCLEで拒否されます。Undo可能です。',
        inputSchema: {
            path: z.string().min(1),
            nodeId: z.number().int().positive(),
            parentNodeId: z.number().int().optional()
                .describe('親ノードid。省略または-1でowner直下へ戻す'),
        },
        annotations: writeAnnotations,
    }, ({ path, nodeId, parentNodeId }) => run({
        t: 'vfx.node.setParent', path, nodeId,
        ...(parentNodeId === undefined ? {} : { parentNodeId }),
    }));

    server.registerTool('vfx_node_set_field', {
        description: 'vfx_get_schemaに存在する型付きschemaPathだけを変更します。保存前にbudgetとDAGを再検証します。'
            + 'Curve型(sizeCurve/velocityCurve/rotationCurve/dragCurve/light.intensityCurve等)は '
            + '{"preset":"Spike","scale":1.0} または {"interp":0|1|2,"keys":[[t,v],...]} で、'
            + 'Gradient型は {"interp":0|1|2,"keys":[[t,r,g,b,a],...]} で指定します(最大8キー)。'
            + '利用できるpreset名は vfx_curve_presets を参照してください。',
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

    server.registerTool('vfx_link_update', {
        description: '既存イベントlinkの接続先・trigger・delayを更新します。削除して作り直さず1回のUndoで変更できます。',
        inputSchema: {
            path: z.string().min(1), index: z.number().int().nonnegative(),
            from: z.number().int().positive().optional(), to: z.number().int().positive().optional(),
            trigger: z.enum(['onComplete', 'onStart', 'onCollision', 'onDeath']).optional(),
            delay: z.number().finite().nonnegative().optional(),
        },
        annotations: writeAnnotations,
    }, ({ path, index, from, to, trigger, delay }) => run({
        t: 'vfx.link.update', path, index,
        ...(from === undefined ? {} : { from }), ...(to === undefined ? {} : { to }),
        ...(trigger === undefined ? {} : { trigger }), ...(delay === undefined ? {} : { delay }),
    }));

    server.registerTool('vfx_link_remove', {
        description: 'index指定でVFXイベントlinkを削除します。',
        inputSchema: { path: z.string().min(1), index: z.number().int().nonnegative() }, annotations: writeAnnotations,
    }, ({ path, index }) => run({ t: 'vfx.link.remove', path, index }));

    server.registerTool('vfx_generate_motion_vectors', {
        description: 'フリップブックアトラスを解析してモーションベクターアトラス(<name>_mv.png)を生成します。'
            + 'columns/rowsはアトラスの分割数です。生成後はvfx_node_set_fieldで'
            + 'particle.motionVectorTexturePathへ割り当て、particle.motionVectorFlipbookを有効にしてください。',
        inputSchema: {
            texturePath: z.string().min(1).max(1024),
            columns: z.number().int().min(1).max(64),
            rows: z.number().int().min(1).max(64),
            searchRadius: z.number().int().min(1).max(64).optional(),
            loop: z.boolean().optional(),
        },
        annotations: writeAnnotations,
    }, ({ texturePath, columns, rows, searchRadius, loop }) => run({
        t: 'vfx.generateMotionVectors', texturePath, columns, rows,
        ...(searchRadius === undefined ? {} : { searchRadius }),
        ...(loop === undefined ? {} : { loop }),
    }));

    server.registerTool('vfx_param_declare', {
        description: '意味名・型・default・任意rangeを持つ公開VFXパラメーターを宣言します。',
        inputSchema: { path: z.string().min(1), name: z.string().min(1).max(128), paramType: z.enum(['float', 'int', 'bool', 'color', 'vector3', 'asset']), defaultValue: JsonValueSchema, minimum: z.number().finite().optional(), maximum: z.number().finite().optional() },
        annotations: writeAnnotations,
    }, ({ path, name, paramType, defaultValue, minimum, maximum }) => run({
        t: 'vfx.param.declare', path, name, paramType, defaultValue,
        ...(minimum === undefined ? {} : { minimum }), ...(maximum === undefined ? {} : { maximum }),
    }));

    server.registerTool('vfx_param_remove', {
        description: '公開VFXパラメーターと、そのbinding・Variant overrideをまとめて削除します。Undo可能です。',
        inputSchema: { path: z.string().min(1), name: z.string().min(1).max(128) },
        annotations: writeAnnotations,
    }, ({ path, name }) => run({ t: 'vfx.param.remove', path, name }));

    server.registerTool('vfx_param_bind', {
        description: '公開パラメーターをexposableかつ型互換なVFXノードschema leafへbindingします。',
        inputSchema: { path: z.string().min(1), name: z.string().min(1).max(128), nodeId: z.number().int().positive(), schemaPath: z.string().min(1).max(256) },
        annotations: writeAnnotations,
    }, ({ path, name, nodeId, schemaPath }) => run({ t: 'vfx.param.bind', path, name, nodeId, schemaPath }));

    server.registerTool('vfx_param_unbind', {
        description: '公開パラメーターのbindingを解除します。nodeId/schemaPathを省略すると、そのパラメーターの全bindingを解除します。',
        inputSchema: {
            path: z.string().min(1), name: z.string().min(1).max(128),
            nodeId: z.number().int().positive().optional(),
            schemaPath: z.string().min(1).max(256).optional(),
        },
        annotations: writeAnnotations,
    }, ({ path, name, nodeId, schemaPath }) => run({
        t: 'vfx.param.unbind', path, name,
        ...(nodeId === undefined ? {} : { nodeId }),
        ...(schemaPath === undefined ? {} : { schemaPath }),
    }));

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

    const VFXGroupFields = {
        title: z.string().min(1).max(128).optional(),
        note: z.string().max(2048).optional(),
        x: z.number().finite().optional(), y: z.number().finite().optional(),
        width: z.number().finite().min(80).max(10000).optional(),
        height: z.number().finite().min(60).max(10000).optional(),
        color: z.tuple([z.number().finite(), z.number().finite(), z.number().finite(), z.number().finite()]).optional(),
    };
    server.registerTool('vfx_group_add', {
        description: 'Graph CanvasへGroup/Noteを追加します。構成意図をAIと人間の双方へ残すEditor情報で、Undo可能です。',
        inputSchema: { path: z.string().min(1), ...VFXGroupFields },
        annotations: writeAnnotations,
    }, ({ path, ...fields }) => run({ t: 'vfx.group.add', path, ...fields }));
    server.registerTool('vfx_group_update', {
        description: 'Group/Noteのタイトル・説明・位置・大きさ・色を更新します。Undo可能です。',
        inputSchema: { path: z.string().min(1), groupId: z.number().int().positive(), ...VFXGroupFields },
        annotations: writeAnnotations,
    }, ({ path, groupId, ...fields }) => run({ t: 'vfx.group.update', path, groupId, ...fields }));
    server.registerTool('vfx_group_remove', {
        description: 'Group/Noteを削除します。内包ノード自体は削除せず、Undo可能です。',
        inputSchema: { path: z.string().min(1), groupId: z.number().int().positive() },
        annotations: writeAnnotations,
    }, ({ path, groupId }) => run({ t: 'vfx.group.remove', path, groupId }));

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
            layer: LayerOptionSchema,
            from: z.string().min(1).max(128).optional().describe('遷移元ステート名。省略で Any State 遷移'),
            to: z.string().min(1).max(128).describe('遷移先ステート名'),
            hasExitTime: z.boolean().optional(),
            exitTime: z.number().finite().min(0).max(1).optional().describe('0..1 正規化再生位置'),
            fixedDuration: z.boolean().optional().describe('true=秒、false=遷移元Length比'),
            transitionDuration: z.number().finite().min(0).max(60).optional(),
        },
        annotations: writeAnnotations,
    }, ({ id, from, to, hasExitTime, exitTime, fixedDuration, transitionDuration, layer }) => run({
        t: 'animation.addTransition', id, ...(layer === undefined ? {} : { layer }), to,
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
            layer: LayerOptionSchema,
            from: z.string().min(1).max(128).optional().describe('遷移元ステート名。省略で Any State 遷移'),
            transitionIndex: z.number().int().nonnegative().describe('対象ソースの遷移リスト内index'),
            action: z.enum(['add', 'update', 'remove', 'clear']),
            parameter: z.string().min(1).max(128).optional().describe('add/update で必須。既存パラメーター名'),
            op: z.enum(['greater', 'less', 'equal', 'notEqual', 'true', 'false']).optional().describe('add/update で必須'),
            threshold: z.number().finite().optional().describe('Float/Int比較の閾値。true/falseでは無視'),
            conditionIndex: z.number().int().nonnegative().optional().describe('update/remove で必須'),
        },
        annotations: writeAnnotations,
    }, ({ id, from, transitionIndex, action, parameter, op, threshold, conditionIndex, layer }) => run({
        t: 'animation.setCondition', id, ...(layer === undefined ? {} : { layer }), transitionIndex, action,
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
            layer: LayerOptionSchema,
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
         blendParameter, blendParameterX, blendParameterY, setAsDefault, layer }) => run({
        t: 'animation.addState', id, ...(layer === undefined ? {} : { layer }), name,
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
            layer: LayerOptionSchema,
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
         blendParameter, blendParameterX, blendParameterY, blend2DType, setAsDefault, layer }) => run({
        t: 'animation.setState', id, ...(layer === undefined ? {} : { layer }), state,
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
            layer: LayerOptionSchema,
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
    }, ({ id, state, sourcePath, clipName, clipIndex, threshold, posX, posY, speed, ikWeight, layer }) => run({
        t: 'animation.addMotion', id, ...(layer === undefined ? {} : { layer }), state,
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
            layer: LayerOptionSchema,
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
    }, ({ id, state, motionIndex, sourcePath, clipName, clipIndex, threshold, posX, posY, speed, ikWeight, layer }) => run({
        t: 'animation.setMotion', id, ...(layer === undefined ? {} : { layer }), state, motionIndex,
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
            layer: LayerOptionSchema,
            state: z.string().min(1).max(128).describe('削除するステート名'),
        },
        annotations: writeAnnotations,
    }, ({ id, state, layer }) => run({ t: 'animation.removeState', id, ...(layer === undefined ? {} : { layer }), state }));

    server.registerTool('animation_remove_transition', {
        description: dryRun
            ? 'Animator遷移削除の入力を検証します。'
            : '指定遷移をUndo可能に削除します。fromで遷移元ステート(省略でAny State)、transitionIndexはanimation_get_graphの並び順です。',
        inputSchema: {
            id: NodeIdSchema,
            layer: LayerOptionSchema,
            from: z.string().min(1).max(128).optional().describe('遷移元ステート名。省略で Any State'),
            transitionIndex: z.number().int().nonnegative().describe('対象ソースの遷移リスト内index'),
        },
        annotations: writeAnnotations,
    }, ({ id, from, transitionIndex, layer }) => run({
        t: 'animation.removeTransition', id, ...(layer === undefined ? {} : { layer }), transitionIndex,
        ...(from === undefined ? {} : { from }),
    }));

    server.registerTool('animation_remove_motion', {
        description: dryRun
            ? 'BlendTree Motion削除の入力を検証します。'
            : '指定BlendTreeステートのMotionをmotionIndexでUndo可能に削除します。motionIndexはanimation_get_blend_treeの並び順です。',
        inputSchema: {
            id: NodeIdSchema,
            layer: LayerOptionSchema,
            state: z.string().min(1).max(128).describe('BlendTree ステート名'),
            motionIndex: z.number().int().nonnegative().describe('motions リスト内 index'),
        },
        annotations: writeAnnotations,
    }, ({ id, state, motionIndex, layer }) => run({ t: 'animation.removeMotion', id, ...(layer === undefined ? {} : { layer }), state, motionIndex }));

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

    // ── Animator レイヤー / Slot ────────────────────────────────────────────
    // 上半身だけ・下半身だけといった部分制御はレイヤーが入口になる。
    // レイヤーを作る → マスクを割り当てる → そのレイヤーにステートを足す、の順で使う。
    const LayerNameSchema = z.string().min(1).max(128);

    server.registerTool('avatar_mask_get', {
        description: '.mask アセット(Avatar Mask)の内容を返します。defaultInclude と、ボーンごとの weight / includeChildren / blendDepth の一覧が取れます。animation_set_layer で maskPath に割り当てる前の確認に使います。',
        inputSchema: {
            path: z.string().min(1).max(512).describe('.mask アセットのパス (例 Assets/Animation/UpperBody.mask)'),
        },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ path }) => Safely(async () => TextResult(await bus.Query({ t: 'avatarMask.get', path }))));

    server.registerTool('avatar_mask_write', {
        description: dryRun
            ? 'Avatar Mask 編集の入力を検証します。'
            : '.mask アセットを作成・編集します。action=create/setBone/removeBone/clear/setDefaultInclude。上半身レイヤーなら「defaultInclude=false + Spine を weight 1 / blendDepth 3 で setBone」が基本形です。blendDepth はそのボーンから下へ何階層かけて weight を立ち上げるかで、腰の継ぎ目でポーズが折れるのを防ぎます。Undo対象外(アセットファイルへ直接書き込み)。',
        inputSchema: {
            path: z.string().min(1).max(512).describe('.mask アセットのパス'),
            action: z.enum(['create', 'setBone', 'removeBone', 'clear', 'setDefaultInclude'])
                .describe('create=新規作成 / setBone=ボーン追加更新 / removeBone=削除 / clear=全消去 / setDefaultInclude=既定の切替'),
            name: z.string().min(1).max(128).optional().describe('create 時の表示名'),
            overwrite: z.boolean().optional().describe('create で既存を上書きする'),
            defaultInclude: z.boolean().optional()
                .describe('false=一覧にないボーンは無効 (上半身だけ有効などに使う) / true=一覧にないボーンは有効 (指だけ除外などに使う)'),
            skeletonSourcePath: z.string().min(1).max(512).optional().describe('参照スケルトンの .fbx'),
            bone: z.string().min(1).max(256).optional()
                .describe('setBone/removeBone の対象。ボーン名 ("Spine1") またはパス ("Hips/Spine/Spine1")'),
            weight: z.number().finite().min(0).max(1).optional().describe('setBone のウェイト 0..1'),
            includeChildren: z.boolean().optional().describe('setBone で子孫にも適用するか'),
            blendDepth: z.number().int().min(0).max(8).optional()
                .describe('0=配下一律。1以上でこのボーンから下へ段階的に weight を立ち上げる'),
        },
        annotations: writeAnnotations,
    }, ({ path, action, name, overwrite, defaultInclude, skeletonSourcePath,
          bone, weight, includeChildren, blendDepth }) => run({
        t: 'avatarMask.write', path, action,
        ...(name === undefined ? {} : { name }),
        ...(overwrite === undefined ? {} : { overwrite }),
        ...(defaultInclude === undefined ? {} : { defaultInclude }),
        ...(skeletonSourcePath === undefined ? {} : { skeletonSourcePath }),
        ...(bone === undefined ? {} : { bone }),
        ...(weight === undefined ? {} : { weight }),
        ...(includeChildren === undefined ? {} : { includeChildren }),
        ...(blendDepth === undefined ? {} : { blendDepth }),
    }));

    server.registerTool('animation_add_layer', {
        description: dryRun
            ? 'Animatorレイヤー追加の入力を検証します。'
            : 'AnimatorにUndo可能なレイヤーを追加します。上半身/下半身の部分制御はここから始めます。maskPathに.maskアセットを指定するとそのボーンだけに効きます。mode=additiveで加算レイヤーになります。追加後はanimation_add_stateにlayerを渡してレイヤー専用のステートマシンを組めます。',
        inputSchema: {
            id: NodeIdSchema,
            name: NameSchema.describe('新規レイヤー名。既存と重複不可'),
            weight: z.number().finite().min(0).max(1).optional().describe('レイヤー適用量 0..1'),
            mode: z.enum(['override', 'additive']).optional().describe('省略でoverride'),
            enabled: z.boolean().optional(),
            maskPath: z.string().min(1).max(512).optional().describe('.mask アセットのパス。省略で全身'),
            stateName: z.string().min(1).max(128).optional()
                .describe('レイヤー専用ステートマシンを使わない場合に再生する共有ステート名'),
            additiveSourcePath: z.string().min(1).max(512).optional()
                .describe('mode=additive の基準ポーズを取るアニメーションソース'),
            additiveClipName: z.string().min(1).max(128).optional(),
            additiveTime: z.number().finite().min(0).optional().describe('基準ポーズの秒位置'),
        },
        annotations: writeAnnotations,
    }, ({ id, name, weight, mode, enabled, maskPath, stateName,
          additiveSourcePath, additiveClipName, additiveTime }) => run({
        t: 'animation.addLayer', id, name,
        ...(weight === undefined ? {} : { weight }),
        ...(mode === undefined ? {} : { mode }),
        ...(enabled === undefined ? {} : { enabled }),
        ...(maskPath === undefined ? {} : { maskPath }),
        ...(stateName === undefined ? {} : { stateName }),
        ...(additiveSourcePath === undefined ? {} : { additiveSourcePath }),
        ...(additiveClipName === undefined ? {} : { additiveClipName }),
        ...(additiveTime === undefined ? {} : { additiveTime }),
    }));

    server.registerTool('animation_set_layer', {
        description: dryRun
            ? 'Animatorレイヤー更新の入力を検証します。'
            : '既存Animatorレイヤーのプロパティを部分的にUndo可能で更新します。layerで対象を指定し、name指定でリネーム。weight/mode/enabled/maskPath/stateName/defaultStateName/加算基準ポーズを個別に変更できます。上半身レイヤーのマスク差し替えやフェードイン量の調整に使います。',
        inputSchema: {
            id: NodeIdSchema,
            layer: LayerNameSchema.describe('更新対象の現在のレイヤー名'),
            name: z.string().min(1).max(128).optional().describe('新しい名前 (リネーム)'),
            weight: z.number().finite().min(0).max(1).optional(),
            mode: z.enum(['override', 'additive']).optional(),
            enabled: z.boolean().optional(),
            maskPath: z.string().max(512).optional().describe('.mask アセットのパス。空文字列でマスク解除'),
            stateName: z.string().max(128).optional(),
            defaultStateName: z.string().max(128).optional()
                .describe('レイヤー専用ステートマシンの初期ステート'),
            additiveSourcePath: z.string().max(512).optional(),
            additiveClipName: z.string().max(128).optional(),
            additiveTime: z.number().finite().min(0).optional(),
        },
        annotations: writeAnnotations,
    }, ({ id, layer, name, weight, mode, enabled, maskPath, stateName, defaultStateName,
          additiveSourcePath, additiveClipName, additiveTime }) => run({
        t: 'animation.setLayer', id, layer,
        ...(name === undefined ? {} : { name }),
        ...(weight === undefined ? {} : { weight }),
        ...(mode === undefined ? {} : { mode }),
        ...(enabled === undefined ? {} : { enabled }),
        ...(maskPath === undefined ? {} : { maskPath }),
        ...(stateName === undefined ? {} : { stateName }),
        ...(defaultStateName === undefined ? {} : { defaultStateName }),
        ...(additiveSourcePath === undefined ? {} : { additiveSourcePath }),
        ...(additiveClipName === undefined ? {} : { additiveClipName }),
        ...(additiveTime === undefined ? {} : { additiveTime }),
    }));

    server.registerTool('animation_remove_layer', {
        description: dryRun
            ? 'Animatorレイヤー削除の入力を検証します。'
            : 'Animatorレイヤーを名前でUndo可能に削除します。そのレイヤーが持つステートマシンとマスク参照も一緒に消えます。',
        inputSchema: {
            id: NodeIdSchema,
            layer: LayerNameSchema.describe('削除するレイヤー名'),
        },
        annotations: writeAnnotations,
    }, ({ id, layer }) => run({ t: 'animation.removeLayer', id, layer }));

    server.registerTool('animation_play_slot', {
        description: dryRun
            ? 'Slot再生の入力を検証します。'
            : '指定レイヤーへワンショットのクリップを割り込ませます。下半身の移動を流したまま上半身だけ攻撃モーションを差し込む、といった用途です。クリップ終端で自動的にフェードアウトして元のステートマシン出力へ戻ります。',
        inputSchema: {
            id: NodeIdSchema,
            layer: LayerNameSchema.describe('割り込ませる対象レイヤー名'),
            sourcePath: z.string().min(1).max(512).optional().describe('アニメーションソース'),
            clipName: z.string().min(1).max(128).optional(),
            fadeIn: z.number().finite().min(0).optional().describe('立ち上げ秒数 (既定0.15)'),
            fadeOut: z.number().finite().min(0).optional().describe('終了フェード秒数 (既定0.15)'),
            speed: z.number().finite().optional(),
            loop: z.boolean().optional().describe('trueで明示stopまでループ'),
        },
        annotations: writeAnnotations,
    }, ({ id, layer, sourcePath, clipName, fadeIn, fadeOut, speed, loop }) => run({
        t: 'animation.playSlot', id, layer,
        ...(sourcePath === undefined ? {} : { sourcePath }),
        ...(clipName === undefined ? {} : { clipName }),
        ...(fadeIn === undefined ? {} : { fadeIn }),
        ...(fadeOut === undefined ? {} : { fadeOut }),
        ...(speed === undefined ? {} : { speed }),
        ...(loop === undefined ? {} : { loop }),
    }));

    server.registerTool('animation_stop_slot', {
        description: dryRun
            ? 'Slot停止の入力を検証します。'
            : '指定レイヤーで再生中のSlotをフェードアウトさせます。loop指定で再生したSlotを止めるときに使います。',
        inputSchema: {
            id: NodeIdSchema,
            layer: LayerNameSchema.describe('対象レイヤー名'),
            fadeOut: z.number().finite().min(0).optional().describe('省略でPlay時に指定した値'),
        },
        annotations: writeAnnotations,
    }, ({ id, layer, fadeOut }) => run({
        t: 'animation.stopSlot', id, layer,
        ...(fadeOut === undefined ? {} : { fadeOut }),
    }));

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
