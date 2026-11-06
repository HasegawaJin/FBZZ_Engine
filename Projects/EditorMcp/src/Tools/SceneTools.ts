/// @file    SceneTools.ts
/// @brief   Scene・Node・Component・Prefab の MCP 操作。
/// @author  Hasegawa Jin
/// @date    2026-11-06
import * as z from 'zod/v4';
import type { McpServer } from '@modelcontextprotocol/sdk/server/mcp.js';
import type { EditorBus } from '../busClient.js';
import type { PermissionMode } from '../config.js';
import { JsonValueSchema, NodeIdSchema, Vec3Schema } from '../editorContracts.js';
import { NameSchema, type SceneSnapshotLike, DiffSceneSnapshots, TextResult, Safely, CreateCommandContext } from './ToolSupport.js';

const ComponentSchema = z.string().min(1).max(128);

export function RegisterSceneQueryTools(server: McpServer, bus: EditorBus): void {
    const sceneSnapshots = new Map<string, unknown>();
    let snapshotSequence = 0;

    server.registerTool('scene_get_tree', {
        description: '現在のシーン階層を取得します。副作用はありません。'
            + '既定ではシステムが実行時に生成したオブジェクト(VFX Graphのノード実体、'
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
        description: '名前・タグ・active状態・複数コンポーネント・反射プロパティをAND検索します。最大500件ずつ返し、nextOffsetをoffsetへ渡すと全件へ到達できます。ページ間のScene変更は同時点保証しません。',
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
            offset: z.number().int().min(0).max(Number.MAX_SAFE_INTEGER).optional().describe('一致結果の先頭からの件数。応答nextOffsetを次の要求へ渡してください'),
        },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ name, tag, active, comp, components, properties, limit, offset }) => Safely(async () => TextResult(await bus.Query({
        t: 'scene.find',
        ...(name === undefined ? {} : { name }),
        ...(tag === undefined ? {} : { tag }),
        ...(active === undefined ? {} : { active }),
        ...(comp === undefined ? {} : { comp }),
        ...(components === undefined ? {} : { components }),
        ...(properties === undefined ? {} : { properties }),
        limit,
        ...(offset === undefined ? {} : { offset }),
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
        description: '指定ノードのコンポーネント値と Script 一覧を取得します。scriptId は現在の個体を読む handle、savedScriptId は owner NodeId と組み合わせる保存 ID です。再生成後は保存 ID が一致する一覧行から新しい scriptId を取得します。',
        inputSchema: { id: NodeIdSchema.describe('対象 NodeId') },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ id }) => Safely(async () => TextResult(await bus.Query({ t: 'node.components', id }))));

    server.registerTool('scene_list', {
        description: 'プロジェクト内の .scene を列挙します。現在開いているシーン(isCurrent)、未保存かどうか(dirty)も返します。'
            + 'scene_open の前に必ず確認してください。dirty=true のまま scene_open を呼ぶと拒否されます。'
            + 'Library / Baked は生成物なので除外します。',
        inputSchema: {},
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, () => Safely(async () => TextResult(await bus.Query({ t: 'scene.list' }))));

    server.registerTool('preset_catalog', {
        description: 'Hierarchy の Add Object メニューと同じ GameObject プリセット一覧を返します。'
            + 'id / カテゴリ / 表示名 / 「何が付くか」の説明を持ち、preset_create の preset にはこの id を渡します。'
            + 'node_create + component_add でオブジェクトを組み立てる前に必ず見てください — '
            + '組み立て方は毎回変わるので、人がメニューから置いたものと中身の違うオブジェクトがシーンに混ざります。',
        inputSchema: { category: z.string().min(1).max(64).optional().describe('3D Object / Light / Environment などで絞る') },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ category }) => Safely(async () => TextResult(await bus.Query({
        t: 'preset.catalog', ...(category === undefined ? {} : { category }),
    }))));
}


export function RegisterSceneCommandTools(server: McpServer, bus: EditorBus, permission: PermissionMode): void {
    const { dryRun, run, writeAnnotations } = CreateCommandContext(bus, permission);

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

    /// @note ── ワールドオーサリングの編集 ──
    server.registerTool('preset_create', {
        description: 'Add Object プリセットから GameObject を生成します。preset は preset_catalog の id です。'
            + 'Hierarchy メニューと同じ生成関数を通るので、人が置いたものと中身が完全に一致します'
            + '(Cube なら MeshRenderer + Lit マテリアル + 実寸に合わせた Box Collider まで含む)。'
            + 'position はローカル座標で、parent を指定した場合は親からの相対になります。'
            + '応答の id が生成された NodeId です。1 回の Undo で丸ごと取り消せます。'
            + '空の GameObject が欲しいだけなら node_create でも構いません。',
        inputSchema: {
            preset: z.string().min(1).max(64).describe('preset_catalog が返す id (例: "3d.cube")'),
            parent: NodeIdSchema.optional(),
            name: z.string().min(1).max(128).optional().describe('省略でプリセット既定名'),
            position: Vec3Schema.optional(),
        },
        annotations: writeAnnotations,
    }, ({ preset, parent, name, position }) => run({
        t: 'preset.create', preset,
        ...(parent === undefined ? {} : { parent }),
        ...(name === undefined ? {} : { name }),
        ...(position === undefined ? {} : { position }),
    }));

    server.registerTool('scene_open', {
        description: '別のシーンを開きます。scene_list の path を渡してください。'
            + '未保存の変更があるときは既定で拒否します — 捨ててよい場合だけ discardUnsaved=true を指定します'
            + '(Editor の確認ダイアログは AI 経由では出せないため、判断を引数として先に受け取ります)。'
            + 'Play 中と Prefab 編集モード中は拒否されます。開くと Undo スタックは破棄されます'
            + '(別シーンの EntityID を持つ Undo は復元できないため)。',
        inputSchema: {
            path: z.string().min(1).describe('projectRoot 相対の .scene パス'),
            discardUnsaved: z.boolean().default(false).describe('未保存の変更を捨てて開く'),
        },
        annotations: writeAnnotations,
    }, ({ path, discardUnsaved }) => run({ t: 'scene.open', path, discardUnsaved }));

    server.registerTool('scene_save', {
        description: '現在のシーンを保存します。path 省略で上書き保存、指定すると別名保存してカレントもそのパスになります。'
            + 'Undo には載りません。scene_open の前や、AI が加えた編集を確定させるときに使います。',
        inputSchema: { path: z.string().min(1).optional().describe('projectRoot 相対の .scene パス (省略で上書き)') },
        annotations: writeAnnotations,
    }, ({ path }) => run({ t: 'scene.save', ...(path === undefined ? {} : { path }) }));
}
