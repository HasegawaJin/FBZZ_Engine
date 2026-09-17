// FBZZ Engine
// editorContracts.ts | EditorMcp
// Editor Command Bus と MCP が共有する wire 契約と入力検証
import * as z from 'zod/v4';

// プロトコル識別子。C++ Editor 側 (EditorBusProtocol.hpp) と一字一句一致させ、
// 世代を跨いだ非互換要求を弾く単一の真実にする。
export const EDITOR_PROTOCOL = 'fbzz.editor.v1';

// NodeId は Scene の UUID v4 を使い、配列移動や世代更新を越えて安定させる。
export const NodeIdSchema = z.string().uuid();
export const Vec3Schema = z.tuple([z.number().finite(), z.number().finite(), z.number().finite()]);
// 地形ブラシの op。並びは C++ の TerrainSculptOp と同じ (追加は末尾)。
export const TerrainSculptOpSchema = z.enum([
    'raise', 'lower', 'smooth', 'flatten', 'stamp', 'noise', 'thermalErosion', 'hydraulicErosion', 'terrace',
]);

// component.set の value は任意 JSON。再帰型は lazy で自己参照させる。
export type JsonValue =
    | null
    | boolean
    | number
    | string
    | JsonValue[]
    | { [key: string]: JsonValue };
export const JsonValueSchema: z.ZodType<JsonValue> = z.lazy(() =>
    z.union([
        z.null(),
        z.boolean(),
        z.number().finite(),
        z.string(),
        z.array(JsonValueSchema),
        z.record(z.string(), JsonValueSchema),
    ]),
);

export const ScenePropertyFilterSchema = z.object({
    comp: z.string().min(1).max(128),
    field: z.string().min(1).max(128),
    op: z.enum(['equals', 'notEquals', 'contains', 'greater', 'less']).default('equals'),
    value: JsonValueSchema,
}).strict();
export type ScenePropertyFilter = z.infer<typeof ScenePropertyFilterSchema>;

// .fluid はレシピ本体。拡張子を先に弾くのは、.mat や .vfx を渡されても C++ 側では
// FLUID_READ_FAILED としか言えず、AI が「中身が壊れている」と誤読するため。
export const FluidPathSchema = z.string().min(1).max(1024).regex(/\.fluid$/i, '.fluid のパスを指定してください');
// FluidBakeService のジョブ id は uint32 で 0 が「受け付けなかった」を意味する。
export const FluidJobIdSchema = z.number().int().min(1).max(0xFFFFFFFF);
// 既定値は C++ 側が持つ (MakeFluidPreset の識別子)。ここで enum に固定すると、
// エンジンにプリセットを 1 つ足すたびに TypeScript も直すことになる。
export const FluidPresetSchema = z.string().min(1).max(64);
// fluid.set / fluid.createEffect の部分レシピ。形は fluid.schema が正本なので中身は見ない。
export const FluidFieldsSchema = z.record(z.string().min(1).max(128), JsonValueSchema)
    .refine((fields) => Object.keys(fields).length > 0, { message: 'fields に 1 つ以上の項目が必要です' });
export const FluidEffectNameSchema = z.string().min(1).max(64)
    .regex(/^[^\\/:*?"<>|.][^\\/:*?"<>|]*$/, 'name はファイル名 1 つ分 (区切り文字・先頭の . は不可)');
// 部品リストの名前は .fluid の配列キー (recipe.source / recipe.force / recipe.collider) と同じ。
export const FluidOperatorListSchema = z.enum(['source', 'force', 'collider']);
// 上限 (source 16 / force 8 / collider 8) の判定は C++ 側 (OPERATOR_LIMIT)。ここは添字として読める範囲だけ見る。
export type FluidOperatorList = z.infer<typeof FluidOperatorListSchema>;
export const FluidOperatorIndexSchema = z.number().int().min(0).max(15);
// 形 (source: sphere / box / cone / ring / texture、collider: sphere / box / plane) か
// 力の種類 (wind / attract / ...) のラベル。一覧は fluid.schema が正本。
export const FluidOperatorTypeSchema = z.string().min(1).max(32);


// 副作用のない読み取り要求。Editor の状態を観測するだけで Scene を変更しない。
export type EditorQuery =
    | { t: 'editor.catalog' }
    | { t: 'editor.catalog.search'; query?: string | undefined; category?: string | undefined; limit?: number | undefined }
    // Operator モデルの目録。メニュー・ホットキー・コマンドパレットが読むのと同じ登録簿。
    // Docs/design/editor-operator-model.md
    | { t: 'editor.op.list'; search?: string | undefined; category?: string | undefined; includeUnavailable?: boolean | undefined }
    // kind=query の Operator を実行して結果データを読む。
    // WHY editor.op.invoke と分けるか: invoke は write 権限のツールとして公開される。
    //      読むだけの操作までそこに閉じ込めると、read 権限の接続からは
    //      「目録には出るのに 1 つも呼べない Query」に見える。
    | { t: 'editor.op.query'; id: string; args?: Record<string, unknown> | undefined }
    | { t: 'editor.state' }
    | { t: 'editor.undoHistory'; limit?: number | undefined }
    | { t: 'console.logs'; minLevel?: 'debug' | 'info' | 'warning' | 'error' | undefined; contains?: string | undefined; limit?: number | undefined; afterSequence?: number | undefined }
    // includeGenerated 省略時は実行時生成オブジェクトを除外する (C++ 側の既定と一致)。
    | { t: 'scene.tree'; includeGenerated?: boolean | undefined }
    | { t: 'scene.snapshot' }
    | { t: 'scene.validate' }
    | { t: 'scene.find'; name?: string | undefined; tag?: string | undefined; active?: boolean | undefined; comp?: string | undefined; components?: string[] | undefined; properties?: ScenePropertyFilter[] | undefined; limit?: number | undefined }
    | { t: 'scene.selection' }
    | { t: 'node.components'; id: string }
    | { t: 'asset.list'; dir?: string | undefined }
    | { t: 'asset.inspect'; path: string }
    | { t: 'asset.findUnused'; limit?: number | undefined }
    | { t: 'asset.thumbnail'; path: string }
    // ── Sprite (Texture のサブアセット) ──
    // Sprite は独立したファイルではないので asset.list に出ない。一覧と切り抜き画像を
    // 出せないと、AI は .meta の UUID を書き写す以外に割り当てる手段が無い。
    | { t: 'sprite.list'; path: string }
    | { t: 'sprite.thumbnail'; path: string; sprite: string }
    // ── Behavior Tree ──
    // 木の構造・Blackboard・検証結果。order が優先度そのものなので必ず含まれる。
    | { t: 'bt.tree'; path: string }
    | { t: 'bt.lint'; path: string }
    | { t: 'bt.guide' }
    // ノード種別ごとの編集可能フィールド目録。bt.node.setField の対で、
    // 「実在するがその種別では読まれない」フィールドを書いて黙って無視されるのを防ぐ。
    | { t: 'bt.schema'; nodeType?: string | undefined }
    // ノードの現在値。書く手段はあるのに読む手段が無かった非対称の解消。
    | { t: 'bt.nodeField'; path: string; nodeId: number; field?: string | undefined }
    // Play 中の実行状態。走っている枝と Blackboard の実値は木からは判らない。
    | { t: 'bt.runtime'; path?: string | undefined; id?: string | undefined }
    | { t: 'bt.diff'; base: string; target: string }
    | { t: 'bt.templateCatalog' }
    // シェーダーが公開する変数とテクスチャスロットの目録。書ける名前の唯一の正本。
    | { t: 'shader.inspect'; path: string }
    | { t: 'shader.diagnostics' }
    | { t: 'material.inspect'; id: string }
    | { t: 'animation.state'; id: string }
    | { t: 'animation.graph'; id: string; layer?: string | undefined }
    | { t: 'avatarMask.get'; path: string }
    | { t: 'animation.blendTree'; id: string; state: string }
    | { t: 'animation.pose'; id: string; limit?: number | undefined }
    | { t: 'profiler.snapshot'; limit?: number | undefined }
    | { t: 'physics.raycast'; origin: [number, number, number]; direction: [number, number, number]; maxDistance: number; sphereRadius?: number | undefined }
    | { t: 'physics.overlapSphere'; center: [number, number, number]; radius: number }
    | { t: 'physics.events' }
    | { t: 'viewport.capture'; w: number; h: number; cameraId?: string | undefined; view?: 'scene' | 'game' | undefined }
    | { t: 'viewport.semantic'; w: number; h: number; view?: 'scene' | 'game' | undefined }
    // ── ワールドオーサリング ──
    // プロジェクト内の .scene 一覧。AI が「今開いている 1 枚」の外へ出るための入口。
    | { t: 'scene.list' }
    // Hierarchy の Add Object と同じプリセット表。node_create + component_add の組み立てを推測させない。
    | { t: 'preset.catalog'; category?: string | undefined }
    // 地形のグリッド・レイヤー・高さ統計。生の heightData は返さない (点は terrain.sample)。
    | { t: 'terrain.inspect'; id?: string | undefined }
    | { t: 'terrain.sample'; points: Array<number[]>; id?: string | undefined }
    // NavMesh: Surface の設定・ベイク状態と Agent の実行状態を並べて返す。
    | { t: 'navmesh.state'; id?: string | undefined }
    // Agent が実際に使う A* + Funnel で経路を引く。歩けるかどうかの唯一の確実な検証手段。
    | { t: 'navmesh.path'; from?: [number, number, number] | undefined; to?: [number, number, number] | undefined;
        fromId?: string | undefined; toId?: string | undefined; surfaceId?: string | undefined;
        agentTypeId?: number | undefined; areaMask?: number | undefined }
    | { t: 'navmesh.sample'; points: Array<number[]>; surfaceId?: string | undefined; agentTypeId?: number | undefined }
    // 空・太陽・霧・IBL・雲・ポストプロセスとライト一覧をまとめて読む。
    | { t: 'environment.inspect' }
    | { t: 'audio.inspect'; id?: string | undefined }
    | { t: 'ui.inspect'; id?: string | undefined }
    // スクリプト DLL / HLSL のビルド結果と診断 (shader.diagnostics の Script 版)。
    | { t: 'build.status'; limit?: number | undefined }
    // ── 流体 (.fluid) ──
    // 焼きとプレビューは非同期ジョブ。fluid.jobStatus が唯一の完了通知で、
    // プレビュー画像もここに載る (includeImage 省略 = 載せる、は C++ 側の既定)。
    | { t: 'fluid.schema' }
    | { t: 'fluid.get'; path: string }
    | { t: 'fluid.jobStatus'; job: number; includeImage?: boolean | undefined }
    // ── 検証ループ (Docs/design/ai-verification-loop.md) ──
    | { t: 'playtest.status' }
    | { t: 'playtest.list' }
    // 登録済みの型名一覧。契約と Editor 実装の食い違いを調べる。
    | { t: 'editor.bus.list' };
export const EditorQuerySchema: z.ZodType<EditorQuery> = z.discriminatedUnion('t', [
    z.object({ t: z.literal('editor.catalog') }).strict(),
    z.object({
        t: z.literal('editor.op.list'),
        search: z.string().min(1).max(128).optional(),
        category: z.string().min(1).max(64).optional(),
        includeUnavailable: z.boolean().optional(),
    }).strict(),
    z.object({
        t: z.literal('editor.op.query'),
        id: z.string().min(1).max(128),
        // 引数は Editor 側の params 宣言だけで検証する。ここで形を固定すると
        // 「Operator を 1 つ足すたび TypeScript も直す」という重複が復活する。
        args: z.record(z.string(), z.unknown()).optional(),
    }).strict(),
    z.object({
        t: z.literal('editor.catalog.search'),
        query: z.string().min(1).max(128).optional(),
        category: z.string().min(1).max(128).optional(),
        limit: z.number().int().min(1).max(100).optional(),
    }).strict().refine((value) => value.query !== undefined || value.category !== undefined, {
        message: 'query、category のいずれかが必要です',
    }),
    z.object({ t: z.literal('editor.state') }).strict(),
    z.object({ t: z.literal('editor.undoHistory'), limit: z.number().int().min(1).max(128).optional() }).strict(),
    z.object({
        t: z.literal('console.logs'),
        minLevel: z.enum(['debug', 'info', 'warning', 'error']).optional(),
        contains: z.string().min(1).max(256).optional(),
        limit: z.number().int().min(1).max(512).optional(),
        afterSequence: z.number().int().nonnegative().max(Number.MAX_SAFE_INTEGER).optional(),
    }).strict(),
    z.object({ t: z.literal('scene.tree'), includeGenerated: z.boolean().optional() }).strict(),
    z.object({ t: z.literal('scene.snapshot') }).strict(),
    z.object({ t: z.literal('scene.validate') }).strict(),
    z.object({
        t: z.literal('scene.find'),
        name: z.string().min(1).max(128).optional(),
        tag: z.string().min(1).max(128).optional(),
        active: z.boolean().optional(),
        comp: z.string().min(1).max(128).optional(),
        components: z.array(z.string().min(1).max(128)).min(1).max(32).optional(),
        properties: z.array(ScenePropertyFilterSchema).min(1).max(32).optional(),
        limit: z.number().int().min(1).max(500).optional(),
    })
        .strict()
        .refine((value) => value.name !== undefined || value.tag !== undefined || value.active !== undefined
            || value.comp !== undefined || value.components !== undefined || value.properties !== undefined, {
            message: '検索条件を1つ以上指定してください',
        }),
    z.object({ t: z.literal('scene.selection') }).strict(),
    z.object({ t: z.literal('node.components'), id: NodeIdSchema }).strict(),
    z.object({ t: z.literal('asset.list'), dir: z.string().min(1).optional() }).strict(),
    z.object({ t: z.literal('asset.inspect'), path: z.string().min(1) }).strict(),
    z.object({ t: z.literal('asset.findUnused'), limit: z.number().int().min(1).max(500).optional() }).strict(),
    z.object({ t: z.literal('asset.thumbnail'), path: z.string().min(1) }).strict(),
    z.object({ t: z.literal('sprite.list'), path: z.string().min(1).max(1024) }).strict(),
    z.object({ t: z.literal('sprite.thumbnail'), path: z.string().min(1).max(1024),
        sprite: z.string().min(1).max(256) }).strict(),
    z.object({ t: z.literal('bt.tree'), path: z.string().min(1).max(1024) }).strict(),
    z.object({ t: z.literal('bt.lint'), path: z.string().min(1).max(1024) }).strict(),
    z.object({ t: z.literal('bt.guide') }).strict(),
    z.object({ t: z.literal('bt.schema'),
        nodeType: z.string().min(1).max(64).optional() }).strict(),
    z.object({ t: z.literal('bt.nodeField'), path: z.string().min(1).max(1024),
        nodeId: z.number().int(),
        field: z.string().min(1).max(64).optional() }).strict(),
    z.object({ t: z.literal('bt.runtime'),
        path: z.string().min(1).max(1024).optional(),
        id: z.string().min(1).max(128).optional() }).strict(),
    z.object({ t: z.literal('bt.diff'), base: z.string().min(1).max(1024),
        target: z.string().min(1).max(1024) }).strict(),
    z.object({ t: z.literal('bt.templateCatalog') }).strict(),
    z.object({ t: z.literal('shader.inspect'), path: z.string().min(1).max(1024) }).strict(),
    z.object({ t: z.literal('shader.diagnostics') }).strict(),
    z.object({ t: z.literal('material.inspect'), id: NodeIdSchema }).strict(),
    z.object({ t: z.literal('animation.state'), id: NodeIdSchema }).strict(),
    z.object({ t: z.literal('animation.graph'), id: NodeIdSchema, layer: z.string().min(1).max(128).optional() }).strict(),
    z.object({ t: z.literal('avatarMask.get'), path: z.string().min(1).max(512) }).strict(),
    z.object({ t: z.literal('animation.blendTree'), id: NodeIdSchema, state: z.string().min(1).max(128) }).strict(),
    z.object({ t: z.literal('animation.pose'), id: NodeIdSchema, limit: z.number().int().min(1).max(512).optional() }).strict(),
    z.object({ t: z.literal('profiler.snapshot'), limit: z.number().int().min(1).max(100).optional() }).strict(),
    z.object({
        t: z.literal('physics.raycast'),
        origin: Vec3Schema,
        direction: Vec3Schema,
        maxDistance: z.number().finite().positive().max(100000),
        sphereRadius: z.number().finite().positive().max(10000).optional(),
    }).strict(),
    z.object({
        t: z.literal('physics.overlapSphere'),
        center: Vec3Schema,
        radius: z.number().finite().positive().max(10000),
    }).strict(),
    z.object({ t: z.literal('physics.events') }).strict(),
    z.object({
        t: z.literal('viewport.capture'),
        w: z.number().int().min(160).max(1920),
        h: z.number().int().min(90).max(1080),
        cameraId: NodeIdSchema.optional(),
        view: z.enum(['scene', 'game']).optional(),
    }).strict(),
    z.object({
        t: z.literal('viewport.semantic'),
        w: z.number().int().min(160).max(1920),
        h: z.number().int().min(90).max(1080),
        view: z.enum(['scene', 'game']).optional(),
    }).strict(),
    // ── ワールドオーサリング ──
    z.object({ t: z.literal('scene.list') }).strict(),
    z.object({ t: z.literal('preset.catalog'), category: z.string().min(1).max(64).optional() }).strict(),
    z.object({ t: z.literal('terrain.inspect'), id: NodeIdSchema.optional() }).strict(),
    z.object({
        t: z.literal('terrain.sample'),
        // [x, y, z] でも [x, z] でも受ける (高さを問うのに y は要らない)。
        points: z.array(z.array(z.number().finite()).min(2).max(3)).min(1).max(256),
        id: NodeIdSchema.optional(),
    }).strict(),
    z.object({ t: z.literal('navmesh.state'), id: NodeIdSchema.optional() }).strict(),
    z.object({
        t: z.literal('navmesh.path'),
        from: Vec3Schema.optional(),
        to: Vec3Schema.optional(),
        fromId: NodeIdSchema.optional(),
        toId: NodeIdSchema.optional(),
        surfaceId: NodeIdSchema.optional(),
        agentTypeId: z.number().int().min(0).max(31).optional(),
        areaMask: z.number().int().optional(),
    }).strict(),
    z.object({
        t: z.literal('navmesh.sample'),
        points: z.array(z.array(z.number().finite()).length(3)).min(1).max(256),
        surfaceId: NodeIdSchema.optional(),
        agentTypeId: z.number().int().min(0).max(31).optional(),
    }).strict(),
    z.object({ t: z.literal('environment.inspect') }).strict(),
    z.object({ t: z.literal('audio.inspect'), id: NodeIdSchema.optional() }).strict(),
    z.object({ t: z.literal('ui.inspect'), id: NodeIdSchema.optional() }).strict(),
    z.object({ t: z.literal('build.status'), limit: z.number().int().min(1).max(20).optional() }).strict(),
    z.object({ t: z.literal('fluid.schema') }).strict(),
    z.object({ t: z.literal('fluid.get'), path: FluidPathSchema }).strict(),
    z.object({ t: z.literal('fluid.jobStatus'), job: FluidJobIdSchema, includeImage: z.boolean().optional() }).strict(),
    z.object({ t: z.literal('playtest.status') }).strict(),
    z.object({ t: z.literal('playtest.list') }).strict(),
    z.object({ t: z.literal('editor.bus.list') }).strict(),
]);

const CommandNameSchema = z.string().min(1).max(128);
const ComponentNameSchema = z.string().min(1).max(128);

// Scene を変更しうる要求。engine 側で Undo 可能な Command Bus へ転送される。
export type EditorCommand =
    // Operator を 1 つ実行する。実行可否 (poll) の判定は Editor 側の登録簿にあり、
    // メニューやホットキーと同じ述語が効く。args は operator が宣言した params に従う。
    | { t: 'editor.op.invoke'; id: string; args?: Record<string, unknown> | undefined }
    | { t: 'node.create'; parent?: string | undefined; name?: string | undefined }
    | { t: 'node.duplicate'; id: string; parent?: string | undefined; name?: string | undefined }
    | { t: 'node.delete'; id: string }
    | { t: 'node.reparent'; id: string; parent: string; index: number }
    | { t: 'node.rename'; id: string; name: string }
    | { t: 'node.setActive'; id: string; active: boolean }
    | { t: 'node.setTag'; id: string; tag: string }
    | { t: 'node.setLayer'; id: string; layer: number }
    | { t: 'selection.set'; ids: string[] }
    | {
          t: 'transform.set';
          id: string;
          pos?: [number, number, number] | undefined;
          rot?: [number, number, number] | undefined;
          scale?: [number, number, number] | undefined;
      }
    | { t: 'component.add'; id: string; comp: string }
    | { t: 'component.set'; id: string; comp: string; field: string; value: JsonValue }
    | { t: 'component.remove'; id: string; comp: string }
    | { t: 'asset.import'; src: string; dst: string }
    | { t: 'prefab.instantiate'; path: string; parent?: string | undefined }
    | { t: 'prefab.create'; path: string; ids?: string[] | undefined }
    | { t: 'prefab.apply'; id: string }
    | { t: 'prefab.revert'; id: string }
    | { t: 'material.assign'; id: string; path: string }
    | { t: 'material.override'; id: string; parameter: string; value: number[] }
    | { t: 'material.asset.setShader'; path: string; shaderPath: string }
    // ── Behavior Tree ──
    // parentId 省略は「ルートとして作る」。既にルートがあれば拒否される。
    | { t: 'bt.node.add'; path: string; nodeType: string; parentId?: number | undefined; name?: string | undefined }
    | { t: 'bt.node.remove'; path: string; nodeId: number }
    // 部分木ごと複製する。parentId 省略で元と同じ親の末尾へ兄弟として並ぶ。
    | { t: 'bt.node.duplicate'; path: string; nodeId: number; parentId?: number | undefined }
    | { t: 'bt.node.setParent'; path: string; nodeId: number; parentId: number }
    // order = 同じ親の中での優先度。小さいほど先に評価される。
    | { t: 'bt.node.setOrder'; path: string; nodeId: number; order: number }
    | { t: 'bt.node.setField'; path: string; nodeId: number; field: string; value: JsonValue }
    | { t: 'bt.blackboard.add'; path: string; name: string; type?: 'bool' | 'int' | 'float' | 'vector3' | 'entity' | 'string' | undefined }
    | { t: 'bt.blackboard.remove'; path: string; name: string }
    | { t: 'bt.autoLayout'; path: string }
    // 修復フラグは全て「省略 = 有効」。個別に false を渡したときだけその修復を止める。
    | { t: 'bt.repair'; path: string; fixAborts?: boolean | undefined;
        fixDurations?: boolean | undefined; fixWeights?: boolean | undefined;
        fixKeys?: boolean | undefined }
    // 動く骨格を取り込む。path は存在しなくてよい (新規作成される)。
    | { t: 'bt.template.apply'; template: string; path: string;
        name?: string | undefined; description?: string | undefined }
    | { t: 'vfx.generateMotionVectors'; texturePath: string; columns: number; rows: number; searchRadius?: number | undefined; loop?: boolean | undefined;
        rowSequences?: boolean | undefined; materialPath?: string | undefined }
    // Sprite の名前は「参照キーを兼ねる別名」。ID は触らないので、名前を付け直しても
    // 保存済みの参照は切れない。slice も重なりで ID を引き継ぐ。
    | { t: 'sprite.rename'; path: string; sprite: string; name: string }
    | { t: 'sprite.slice'; path: string; type?: 'grid' | 'automatic' | undefined;
        columns?: number | undefined; rows?: number | undefined;
        cellWidth?: number | undefined; cellHeight?: number | undefined;
        offsetX?: number | undefined; offsetY?: number | undefined;
        paddingX?: number | undefined; paddingY?: number | undefined;
        pivotX?: number | undefined; pivotY?: number | undefined;
        keepEmptyRects?: boolean | undefined;
        prefix?: string | undefined; mode?: 'smart' | 'safe' | 'replace' | undefined }
    | { t: 'animation.control'; id: string; action: 'play' | 'pause' | 'stop' | 'seek'; clipName?: string | undefined; clipIndex?: number | undefined; state?: string | undefined; time?: number | undefined; frame?: number | undefined }
    | { t: 'animation.setParameter'; id: string; name: string; value?: number | boolean | undefined }
    | { t: 'animation.addTransition'; id: string; layer?: string | undefined; from?: string | undefined; to: string; hasExitTime?: boolean | undefined; exitTime?: number | undefined; fixedDuration?: boolean | undefined; transitionDuration?: number | undefined }
    | { t: 'animation.setCondition'; id: string; layer?: string | undefined; from?: string | undefined; transitionIndex: number; action: 'add' | 'update' | 'remove' | 'clear'; parameter?: string | undefined; op?: 'greater' | 'less' | 'equal' | 'notEqual' | 'true' | 'false' | undefined; threshold?: number | undefined; conditionIndex?: number | undefined }
    | { t: 'animation.addState'; id: string; layer?: string | undefined; name: string; mode?: 'clip' | 'blendTree1D' | 'blendTree2D' | undefined; sourcePath?: string | undefined; clipName?: string | undefined; clipIndex?: number | undefined; speed?: number | undefined; loop?: boolean | undefined; ikWeight?: number | undefined; blendParameter?: string | undefined; blendParameterX?: string | undefined; blendParameterY?: string | undefined; setAsDefault?: boolean | undefined }
    | { t: 'animation.setState'; id: string; layer?: string | undefined; state: string; name?: string | undefined; mode?: 'clip' | 'blendTree1D' | 'blendTree2D' | undefined; sourcePath?: string | undefined; clipName?: string | undefined; clipIndex?: number | undefined; speed?: number | undefined; loop?: boolean | undefined; ikWeight?: number | undefined; blendParameter?: string | undefined; blendParameterX?: string | undefined; blendParameterY?: string | undefined; blend2DType?: 'simpleDirectional' | 'freeformCartesian' | undefined; setAsDefault?: boolean | undefined }
    | { t: 'animation.addMotion'; id: string; layer?: string | undefined; state: string; sourcePath?: string | undefined; clipName?: string | undefined; clipIndex?: number | undefined; threshold?: number | undefined; posX?: number | undefined; posY?: number | undefined; speed?: number | undefined; ikWeight?: number | undefined }
    | { t: 'animation.setMotion'; id: string; layer?: string | undefined; state: string; motionIndex: number; sourcePath?: string | undefined; clipName?: string | undefined; clipIndex?: number | undefined; threshold?: number | undefined; posX?: number | undefined; posY?: number | undefined; speed?: number | undefined; ikWeight?: number | undefined }
    | { t: 'animation.removeState'; id: string; layer?: string | undefined; state: string }
    | { t: 'animation.removeMotion'; id: string; layer?: string | undefined; state: string; motionIndex: number }
    | { t: 'animation.removeTransition'; id: string; layer?: string | undefined; from?: string | undefined; transitionIndex: number }
    | { t: 'animation.addParameter'; id: string; name: string; type?: 'float' | 'int' | 'bool' | 'trigger' | undefined; value?: number | boolean | undefined }
    | { t: 'animation.removeParameter'; id: string; name: string }
    | { t: 'animation.addLayer'; id: string; name: string; weight?: number | undefined; mode?: 'override' | 'additive' | undefined; enabled?: boolean | undefined; maskPath?: string | undefined; stateName?: string | undefined; additiveSourcePath?: string | undefined; additiveClipName?: string | undefined; additiveTime?: number | undefined }
    | { t: 'animation.setLayer'; id: string; layer: string; name?: string | undefined; weight?: number | undefined; mode?: 'override' | 'additive' | undefined; enabled?: boolean | undefined; maskPath?: string | undefined; stateName?: string | undefined; defaultStateName?: string | undefined; additiveSourcePath?: string | undefined; additiveClipName?: string | undefined; additiveTime?: number | undefined }
    | { t: 'animation.removeLayer'; id: string; layer: string }
    | { t: 'avatarMask.write'; path: string; action: 'create' | 'setBone' | 'removeBone' | 'clear' | 'setDefaultInclude'; name?: string | undefined; overwrite?: boolean | undefined; defaultInclude?: boolean | undefined; skeletonSourcePath?: string | undefined; bone?: string | undefined; weight?: number | undefined; includeChildren?: boolean | undefined; blendDepth?: number | undefined }
    | { t: 'animation.playSlot'; id: string; layer: string; sourcePath?: string | undefined; clipName?: string | undefined; fadeIn?: number | undefined; fadeOut?: number | undefined; speed?: number | undefined; loop?: boolean | undefined }
    | { t: 'animation.stopSlot'; id: string; layer: string; fadeOut?: number | undefined }
    | { t: 'input.inject'; kind: 'key' | 'axis' | 'gamepadButton' | 'gamepadAxis' | 'mouseButton' | 'mousePosition' | 'mouseDelta' | 'mouseScroll' | 'clear'; key?: string | undefined; buttonName?: string | undefined; pressed?: boolean | undefined; axis?: string | undefined; button?: number | undefined; value?: number | [number, number] | undefined }
    | { t: 'play.control'; action: 'start' | 'stop' | 'pause' | 'resume' | 'step' }
    | { t: 'viewport.camera'; position?: [number, number, number] | undefined; lookAt?: [number, number, number] | undefined; targetId?: string | undefined }
    | { t: 'editor.undo' }
    | { t: 'editor.redo' }
    // ── ワールドオーサリング ──
    // Add Object プリセットの生成。人がメニューから置いたものと中身が完全に一致する。
    | { t: 'preset.create'; preset: string; parent?: string | undefined;
        name?: string | undefined; position?: [number, number, number] | undefined }
    // シーンの入出力は Undo に載らない (Undo でシーンが閉じる方が事故になる)。
    | { t: 'scene.open'; path: string; discardUnsaved?: boolean | undefined }
    | { t: 'scene.save'; path?: string | undefined }
    // ブラシ 1 ストロークぶん。iterations は「押し続けた回数」に相当する。
    | { t: 'terrain.sculpt'; position: [number, number, number];
        op?: 'raise' | 'lower' | 'smooth' | 'flatten' | 'stamp' | 'noise' | 'thermalErosion' | 'hydraulicErosion' | 'terrace' | undefined;
        radius?: number | undefined; strength?: number | undefined; falloff?: 'linear' | 'smooth' | 'gaussian' | undefined;
        iterations?: number | undefined; targetHeight?: number | undefined; id?: string | undefined;
        noiseScale?: number | undefined; noiseOctaves?: number | undefined; seed?: number | undefined;
        terraceStep?: number | undefined; terraceSharpness?: number | undefined; talus?: number | undefined;
        droplets?: number | undefined }
    // layer の上限は Terrain ごとの層数 (C++ 側で検証する)。
    | { t: 'terrain.paint'; position: [number, number, number]; layer: number;
        radius?: number | undefined; strength?: number | undefined; falloff?: 'linear' | 'smooth' | 'gaussian' | undefined;
        iterations?: number | undefined; id?: string | undefined }
    // layer == 層数 で末尾に追加する。
    | { t: 'terrain.setLayerMaterial'; id: string; layer: number; material: string }
    // 始点から終点へ傾く坂。1 ストローク 1 回で、iterations は無い。
    | { t: 'terrain.ramp'; start: [number, number, number]; end: [number, number, number];
        radius?: number | undefined; strength?: number | undefined; falloff?: 'linear' | 'smooth' | 'gaussian' | undefined;
        id?: string | undefined }
    | { t: 'terrain.hole'; position: [number, number, number]; radius?: number | undefined;
        erase?: boolean | undefined; id?: string | undefined }
    // ベイクは非同期。完了は navmesh.state の bakeState で確認する。
    | { t: 'navmesh.bake'; id?: string | undefined }
    | { t: 'audio.control'; id: string; action: 'play' | 'stop' | 'pause' | 'resume' }
    // スクリプト DLL の再ビルド要求。完了は build.status のポーリングで確認する。
    | { t: 'build.run'; target?: 'script' | undefined }
    // ── 流体 (.fluid) ──
    // preview / bake / createEffect(bake) はジョブ id を返してすぐ戻る。焼いた出力は Undo で消えない。
    | { t: 'fluid.create'; path: string; preset?: string | undefined; overwrite?: boolean | undefined }
    | { t: 'fluid.set'; path: string; fields: Record<string, JsonValue> }
    // 部品 (発生源 / 力 / 障害物) の追加・削除・並べ替え。どれも fluid.set と同じく Undo 可能なファイル書き込み。
    | { t: 'fluid.addOperator'; path: string; list: FluidOperatorList; type?: string | undefined;
        index?: number | undefined; fields?: Record<string, JsonValue> | undefined }
    | { t: 'fluid.removeOperator'; path: string; list: FluidOperatorList; index: number }
    | { t: 'fluid.moveOperator'; path: string; list: FluidOperatorList; from: number; to: number }
    | { t: 'fluid.preview'; path: string; frame?: number | undefined; time?: number | undefined;
        size?: number | undefined; contactSheet?: boolean | undefined; variants?: number | undefined;
        seed?: number | undefined }
    | { t: 'fluid.bake'; path: string; updateMaterial?: boolean | undefined; seed?: number | undefined }
    | { t: 'fluid.cancel'; job: number }
    | { t: 'fluid.createEffect'; name: string; dir?: string | undefined; preset?: string | undefined;
        fields?: Record<string, JsonValue> | undefined; bake?: boolean | undefined }
    // ── 検証ループ ──
    // playtest.run はシナリオを開始してすぐ戻る。完了は playtest.status のポーリングで確認する。
    | { t: 'playtest.run'; path?: string | undefined; scenario?: Record<string, JsonValue> | undefined;
        updateBaselines?: boolean | undefined; skipImages?: boolean | undefined }
    | { t: 'playtest.cancel' }
    | { t: 'input.record'; action: 'start' | 'stop'; path?: string | undefined }
    | { t: 'visual.compare'; baseline: string; view?: 'game' | 'scene' | undefined;
        updateBaseline?: boolean | undefined; pixelThreshold?: number | undefined }
    | { t: 'editor.transaction'; label: string; cmds: EditorCommand[] };
export const EditorCommandSchema: z.ZodType<EditorCommand> = z.lazy(() =>
    z.discriminatedUnion('t', [
        // args の中身は Editor 側の params 宣言に従って検証される。
        // ここで形を固定しないのは、operator が増えてもこのファイルを触らずに済ませるため
        // (Editor に 1 つ足すたび TypeScript も直す、が今回無くしたい重複そのもの)。
        z.object({
            t: z.literal('editor.op.invoke'),
            id: z.string().min(1).max(128),
            args: z.record(z.string(), z.unknown()).optional(),
        }).strict(),
        z.object({ t: z.literal('node.create'), parent: NodeIdSchema.optional(), name: CommandNameSchema.optional() }).strict(),
        z.object({
            t: z.literal('node.duplicate'),
            id: NodeIdSchema,
            parent: NodeIdSchema.optional(),
            name: CommandNameSchema.optional(),
        }).strict(),
        z.object({ t: z.literal('node.delete'), id: NodeIdSchema }).strict(),
        z.object({ t: z.literal('node.reparent'), id: NodeIdSchema, parent: NodeIdSchema, index: z.number().int().nonnegative() }).strict(),
        z.object({ t: z.literal('node.rename'), id: NodeIdSchema, name: CommandNameSchema }).strict(),
        z.object({ t: z.literal('node.setActive'), id: NodeIdSchema, active: z.boolean() }).strict(),
        z.object({ t: z.literal('node.setTag'), id: NodeIdSchema, tag: z.string().min(1).max(128) }).strict(),
        z.object({ t: z.literal('node.setLayer'), id: NodeIdSchema, layer: z.number().int().min(0).max(31) }).strict(),
        z.object({ t: z.literal('selection.set'), ids: z.array(NodeIdSchema).max(256) }).strict(),
        z.object({
            t: z.literal('transform.set'),
            id: NodeIdSchema,
            pos: Vec3Schema.optional(),
            rot: Vec3Schema.optional(),
            scale: Vec3Schema.optional(),
        })
            .strict()
            .refine((value) => value.pos !== undefined || value.rot !== undefined || value.scale !== undefined, {
                message: 'pos、rot、scale のいずれかが必要です',
            }),
        z.object({ t: z.literal('component.add'), id: NodeIdSchema, comp: ComponentNameSchema }).strict(),
        z.object({
            t: z.literal('component.set'),
            id: NodeIdSchema,
            comp: ComponentNameSchema,
            field: z.string().min(1).max(128),
            value: JsonValueSchema,
        }).strict(),
        z.object({ t: z.literal('component.remove'), id: NodeIdSchema, comp: ComponentNameSchema }).strict(),
        z.object({ t: z.literal('asset.import'), src: z.string().min(1), dst: z.string().min(1) }).strict(),
        z.object({
            t: z.literal('prefab.instantiate'),
            path: z.string().min(1),
            parent: NodeIdSchema.optional(),
        }).strict(),
        z.object({
            t: z.literal('prefab.create'),
            path: z.string().min(1),
            ids: z.array(NodeIdSchema).max(256).optional(),
        }).strict(),
        z.object({ t: z.literal('prefab.apply'), id: NodeIdSchema }).strict(),
        z.object({ t: z.literal('prefab.revert'), id: NodeIdSchema }).strict(),
        z.object({ t: z.literal('material.assign'), id: NodeIdSchema, path: z.string().min(1) }).strict(),
        z.object({
            t: z.literal('material.override'),
            id: NodeIdSchema,
            parameter: z.string().min(1).max(128),
            value: z.array(z.number().finite()).min(1).max(4),
        }).strict(),
        z.object({
            t: z.literal('material.asset.setShader'),
            path: z.string().min(1),
            shaderPath: z.string().min(1),
        }).strict(),
        // フリップブックアトラスからモーションベクターアトラスを生成する。
        // MV は外部ツールでしか作れず、AI が flipbook のブレンド品質を上げたくても
        // 手段が無かったため、コマンドとして公開する。
        z.object({ t: z.literal('vfx.generateMotionVectors'), texturePath: z.string().min(1).max(1024),
            columns: z.number().int().min(1).max(64), rows: z.number().int().min(1).max(64),
            searchRadius: z.number().int().min(1).max(64).optional(),
            loop: z.boolean().optional(),
            rowSequences: z.boolean().optional(),
            // 推奨 strength は生成結果でしか決まらないため、渡されたら .mat へ直接書き込む。
            materialPath: z.string().min(1).max(1024).optional() }).strict(),
        // ── Sprite ──
        z.object({ t: z.literal('sprite.rename'), path: z.string().min(1).max(1024),
            sprite: z.string().min(1).max(256), name: z.string().min(1).max(128) }).strict(),
        z.object({ t: z.literal('sprite.slice'), path: z.string().min(1).max(1024),
            type: z.enum(['grid', 'automatic']).optional(),
            columns: z.number().int().min(1).max(256).optional(),
            rows: z.number().int().min(1).max(256).optional(),
            cellWidth: z.number().int().min(1).max(16384).optional(),
            cellHeight: z.number().int().min(1).max(16384).optional(),
            offsetX: z.number().int().min(0).max(16384).optional(),
            offsetY: z.number().int().min(0).max(16384).optional(),
            paddingX: z.number().int().min(0).max(16384).optional(),
            paddingY: z.number().int().min(0).max(16384).optional(),
            pivotX: z.number().min(0).max(1).optional(),
            pivotY: z.number().min(0).max(1).optional(),
            keepEmptyRects: z.boolean().optional(),
            prefix: z.string().min(1).max(64).optional(),
            mode: z.enum(['smart', 'safe', 'replace']).optional() }).strict(),
        // ── Behavior Tree ──
        z.object({ t: z.literal('bt.node.add'), path: z.string().min(1).max(1024),
            nodeType: z.string().min(1).max(64), parentId: z.number().int().min(1).optional(),
            name: z.string().min(1).max(128).optional() }).strict(),
        z.object({ t: z.literal('bt.node.remove'), path: z.string().min(1).max(1024),
            nodeId: z.number().int().min(1) }).strict(),
        z.object({ t: z.literal('bt.node.duplicate'), path: z.string().min(1).max(1024),
            nodeId: z.number().int().min(1),
            parentId: z.number().int().min(1).optional() }).strict(),
        z.object({ t: z.literal('bt.node.setParent'), path: z.string().min(1).max(1024),
            nodeId: z.number().int().min(1), parentId: z.number().int().min(0) }).strict(),
        z.object({ t: z.literal('bt.node.setOrder'), path: z.string().min(1).max(1024),
            nodeId: z.number().int().min(1), order: z.number().int().min(0).max(4096) }).strict(),
        z.object({ t: z.literal('bt.node.setField'), path: z.string().min(1).max(1024),
            nodeId: z.number().int().min(1), field: z.string().min(1).max(64),
            value: JsonValueSchema }).strict(),
        z.object({ t: z.literal('bt.blackboard.add'), path: z.string().min(1).max(1024),
            name: z.string().min(1).max(64),
            type: z.enum(['bool', 'int', 'float', 'vector3', 'entity', 'string']).optional() }).strict(),
        z.object({ t: z.literal('bt.blackboard.remove'), path: z.string().min(1).max(1024),
            name: z.string().min(1).max(64) }).strict(),
        z.object({ t: z.literal('bt.autoLayout'), path: z.string().min(1).max(1024) }).strict(),
        // bt.lint が autoFixable=true と言った code だけを機械的に直す (Undo 可能)。
        z.object({ t: z.literal('bt.repair'), path: z.string().min(1).max(1024),
            fixAborts: z.boolean().optional(), fixDurations: z.boolean().optional(),
            fixWeights: z.boolean().optional(), fixKeys: z.boolean().optional() }).strict(),
        z.object({ t: z.literal('bt.template.apply'), template: z.string().min(1).max(256),
            path: z.string().min(1).max(1024),
            name: z.string().min(1).max(128).optional(),
            description: z.string().max(512).optional() }).strict(),
        z.object({
            t: z.literal('animation.control'),
            id: NodeIdSchema,
            action: z.enum(['play', 'pause', 'stop', 'seek']),
            clipName: z.string().min(1).max(128).optional(),
            clipIndex: z.number().int().nonnegative().optional(),
            state: z.string().min(1).max(128).optional(),
            time: z.number().finite().nonnegative().optional(),
            frame: z.number().finite().nonnegative().optional(),
        }).strict().refine((value) => value.action !== 'seek' || value.time !== undefined || value.frame !== undefined, {
            message: 'seek には time または frame が必要です',
        }),
        z.object({
            t: z.literal('animation.setParameter'),
            id: NodeIdSchema,
            name: z.string().min(1).max(128),
            // float/int は number、bool/trigger は boolean。trigger は value 省略で発火。
            value: z.union([z.number().finite(), z.boolean()]).optional(),
        }).strict(),
        z.object({
            t: z.literal('animation.addTransition'),
            id: NodeIdSchema,
            layer: z.string().min(1).max(128).optional(),
            // from 省略 = Any State 遷移。指定時はそのステートからの遷移。
            from: z.string().min(1).max(128).optional(),
            to: z.string().min(1).max(128),
            hasExitTime: z.boolean().optional(),
            exitTime: z.number().finite().min(0).max(1).optional(),
            fixedDuration: z.boolean().optional(),
            transitionDuration: z.number().finite().min(0).max(60).optional(),
        }).strict(),
        z.object({
            t: z.literal('animation.setCondition'),
            id: NodeIdSchema,
            layer: z.string().min(1).max(128).optional(),
            from: z.string().min(1).max(128).optional(),
            transitionIndex: z.number().int().nonnegative(),
            action: z.enum(['add', 'update', 'remove', 'clear']),
            parameter: z.string().min(1).max(128).optional(),
            op: z.enum(['greater', 'less', 'equal', 'notEqual', 'true', 'false']).optional(),
            threshold: z.number().finite().optional(),
            conditionIndex: z.number().int().nonnegative().optional(),
        }).strict().superRefine((input, context) => {
            // add/update は parameter と op が必須。update/remove は conditionIndex が必須。
            if ((input.action === 'add' || input.action === 'update')
                && (input.parameter === undefined || input.op === undefined)) {
                context.addIssue({ code: 'custom', message: `action=${input.action} には parameter と op が必要です` });
            }
            if ((input.action === 'update' || input.action === 'remove') && input.conditionIndex === undefined) {
                context.addIssue({ code: 'custom', message: `action=${input.action} には conditionIndex が必要です` });
            }
        }),
        z.object({
            t: z.literal('animation.addState'),
            id: NodeIdSchema,
            layer: z.string().min(1).max(128).optional(),
            name: z.string().min(1).max(128),
            mode: z.enum(['clip', 'blendTree1D', 'blendTree2D']).optional(),
            sourcePath: z.string().min(1).max(512).optional(),
            clipName: z.string().min(1).max(128).optional(),
            clipIndex: z.number().int().nonnegative().optional(),
            speed: z.number().finite().optional(),
            loop: z.boolean().optional(),
            ikWeight: z.number().finite().min(0).max(1).optional(),
            blendParameter: z.string().min(1).max(128).optional(),
            blendParameterX: z.string().min(1).max(128).optional(),
            blendParameterY: z.string().min(1).max(128).optional(),
            setAsDefault: z.boolean().optional(),
        }).strict(),
        z.object({
            t: z.literal('animation.setState'),
            id: NodeIdSchema,
            layer: z.string().min(1).max(128).optional(),
            state: z.string().min(1).max(128),
            name: z.string().min(1).max(128).optional(),
            mode: z.enum(['clip', 'blendTree1D', 'blendTree2D']).optional(),
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
        }).strict(),
        z.object({
            t: z.literal('animation.addMotion'),
            id: NodeIdSchema,
            layer: z.string().min(1).max(128).optional(),
            state: z.string().min(1).max(128),
            sourcePath: z.string().min(1).max(512).optional(),
            clipName: z.string().min(1).max(128).optional(),
            clipIndex: z.number().int().nonnegative().optional(),
            threshold: z.number().finite().optional(),
            posX: z.number().finite().optional(),
            posY: z.number().finite().optional(),
            speed: z.number().finite().optional(),
            ikWeight: z.number().finite().min(0).max(1).optional(),
        }).strict(),
        z.object({
            t: z.literal('animation.setMotion'),
            id: NodeIdSchema,
            layer: z.string().min(1).max(128).optional(),
            state: z.string().min(1).max(128),
            motionIndex: z.number().int().nonnegative(),
            sourcePath: z.string().min(1).max(512).optional(),
            clipName: z.string().min(1).max(128).optional(),
            clipIndex: z.number().int().nonnegative().optional(),
            threshold: z.number().finite().optional(),
            posX: z.number().finite().optional(),
            posY: z.number().finite().optional(),
            speed: z.number().finite().optional(),
            ikWeight: z.number().finite().min(0).max(1).optional(),
        }).strict(),
        z.object({ t: z.literal('animation.removeState'), id: NodeIdSchema, layer: z.string().min(1).max(128).optional(), state: z.string().min(1).max(128) }).strict(),
        z.object({
            t: z.literal('animation.removeMotion'),
            id: NodeIdSchema,
            layer: z.string().min(1).max(128).optional(),
            state: z.string().min(1).max(128),
            motionIndex: z.number().int().nonnegative(),
        }).strict(),
        z.object({
            t: z.literal('animation.removeTransition'),
            id: NodeIdSchema,
            layer: z.string().min(1).max(128).optional(),
            from: z.string().min(1).max(128).optional(),
            transitionIndex: z.number().int().nonnegative(),
        }).strict(),
        z.object({
            t: z.literal('animation.addParameter'),
            id: NodeIdSchema,
            name: z.string().min(1).max(128),
            type: z.enum(['float', 'int', 'bool', 'trigger']).optional(),
            value: z.union([z.number().finite(), z.boolean()]).optional(),
        }).strict(),
        z.object({ t: z.literal('animation.removeParameter'), id: NodeIdSchema, name: z.string().min(1).max(128) }).strict(),
        z.object({
            t: z.literal('animation.addLayer'),
            id: NodeIdSchema,
            name: z.string().min(1).max(128),
            weight: z.number().finite().min(0).max(1).optional(),
            mode: z.enum(['override', 'additive']).optional(),
            enabled: z.boolean().optional(),
            maskPath: z.string().min(1).max(512).optional(),
            stateName: z.string().min(1).max(128).optional(),
            additiveSourcePath: z.string().min(1).max(512).optional(),
            additiveClipName: z.string().min(1).max(128).optional(),
            additiveTime: z.number().finite().min(0).optional(),
        }).strict(),
        z.object({
            t: z.literal('animation.setLayer'),
            id: NodeIdSchema,
            layer: z.string().min(1).max(128),
            name: z.string().min(1).max(128).optional(),
            weight: z.number().finite().min(0).max(1).optional(),
            mode: z.enum(['override', 'additive']).optional(),
            enabled: z.boolean().optional(),
            maskPath: z.string().max(512).optional(),
            stateName: z.string().max(128).optional(),
            defaultStateName: z.string().max(128).optional(),
            additiveSourcePath: z.string().max(512).optional(),
            additiveClipName: z.string().max(128).optional(),
            additiveTime: z.number().finite().min(0).optional(),
        }).strict(),
        z.object({
            t: z.literal('animation.removeLayer'),
            id: NodeIdSchema,
            layer: z.string().min(1).max(128),
        }).strict(),
        z.object({
            t: z.literal('avatarMask.write'),
            path: z.string().min(1).max(512),
            action: z.enum(['create', 'setBone', 'removeBone', 'clear', 'setDefaultInclude']),
            name: z.string().min(1).max(128).optional(),
            overwrite: z.boolean().optional(),
            defaultInclude: z.boolean().optional(),
            skeletonSourcePath: z.string().min(1).max(512).optional(),
            bone: z.string().min(1).max(256).optional(),
            weight: z.number().finite().min(0).max(1).optional(),
            includeChildren: z.boolean().optional(),
            blendDepth: z.number().int().min(0).max(8).optional(),
        }).strict(),
        z.object({
            t: z.literal('animation.playSlot'),
            id: NodeIdSchema,
            layer: z.string().min(1).max(128),
            sourcePath: z.string().min(1).max(512).optional(),
            clipName: z.string().min(1).max(128).optional(),
            fadeIn: z.number().finite().min(0).optional(),
            fadeOut: z.number().finite().min(0).optional(),
            speed: z.number().finite().optional(),
            loop: z.boolean().optional(),
        }).strict(),
        z.object({
            t: z.literal('animation.stopSlot'),
            id: NodeIdSchema,
            layer: z.string().min(1).max(128),
            fadeOut: z.number().finite().min(0).optional(),
        }).strict(),
        z.object({
            t: z.literal('input.inject'),
            kind: z.enum(['key', 'axis', 'gamepadButton', 'gamepadAxis', 'mouseButton', 'mousePosition', 'mouseDelta', 'mouseScroll', 'clear']),
            key: z.string().min(1).max(32).optional(),
            buttonName: z.string().min(1).max(64).optional(),
            pressed: z.boolean().optional(),
            axis: z.string().min(1).max(64).optional(),
            button: z.number().int().min(0).max(2).optional(),
            value: z.union([z.number().finite(), z.tuple([z.number().finite(), z.number().finite()])]).optional(),
        }).strict().superRefine((input, context) => {
            const isNumber = typeof input.value === 'number';
            const isVector = Array.isArray(input.value);
            const valid = input.kind === 'clear'
                || (input.kind === 'key' && input.key !== undefined && input.pressed !== undefined)
                || (input.kind === 'axis' && input.axis !== undefined && isNumber)
                || (input.kind === 'gamepadAxis' && input.axis !== undefined && isNumber)
                || (input.kind === 'gamepadButton' && input.buttonName !== undefined && input.pressed !== undefined)
                || (input.kind === 'mouseButton' && input.button !== undefined && input.pressed !== undefined)
                || ((input.kind === 'mousePosition' || input.kind === 'mouseDelta') && isVector)
                || (input.kind === 'mouseScroll' && isNumber);
            if (!valid) context.addIssue({ code: 'custom', message: `kind=${input.kind} に必要な入力が不足しています` });
        }),
        z.object({
            t: z.literal('play.control'),
            action: z.enum(['start', 'stop', 'pause', 'resume', 'step']),
        }).strict(),
        z.object({
            t: z.literal('viewport.camera'),
            position: Vec3Schema.optional(),
            lookAt: Vec3Schema.optional(),
            targetId: NodeIdSchema.optional(),
        })
            .strict()
            .refine((value) => value.position !== undefined || value.lookAt !== undefined || value.targetId !== undefined, {
                message: 'position、lookAt、targetId のいずれかが必要です',
            }),
        z.object({ t: z.literal('editor.undo') }).strict(),
        z.object({ t: z.literal('editor.redo') }).strict(),
        // ── ワールドオーサリング ──
        z.object({
            t: z.literal('preset.create'),
            preset: z.string().min(1).max(64),
            parent: NodeIdSchema.optional(),
            name: CommandNameSchema.optional(),
            position: Vec3Schema.optional(),
        }).strict(),
        z.object({
            t: z.literal('scene.open'),
            path: z.string().min(1),
            discardUnsaved: z.boolean().optional(),
        }).strict(),
        z.object({ t: z.literal('scene.save'), path: z.string().min(1).optional() }).strict(),
        z.object({
            t: z.literal('terrain.sculpt'),
            position: Vec3Schema,
            op: TerrainSculptOpSchema.optional(),
            radius: z.number().finite().gt(0).max(500).optional(),
            strength: z.number().finite().gt(0).max(1).optional(),
            falloff: z.enum(['linear', 'smooth', 'gaussian']).optional(),
            iterations: z.number().int().min(1).max(64).optional(),
            targetHeight: z.number().finite().optional(),
            id: NodeIdSchema.optional(),
            noiseScale: z.number().finite().gt(0).max(1000).optional(),
            noiseOctaves: z.number().int().min(1).max(8).optional(),
            seed: z.number().int().min(0).max(4294967295).optional(),
            terraceStep: z.number().finite().gt(0).max(1000).optional(),
            terraceSharpness: z.number().finite().min(0).max(1).optional(),
            talus: z.number().finite().gt(0).lt(90).optional(),
            droplets: z.number().int().min(1).max(4096).optional(),
        }).strict(),
        z.object({
            t: z.literal('terrain.paint'),
            position: Vec3Schema,
            layer: z.number().int().min(0).max(254),
            radius: z.number().finite().gt(0).max(500).optional(),
            strength: z.number().finite().gt(0).max(1).optional(),
            falloff: z.enum(['linear', 'smooth', 'gaussian']).optional(),
            iterations: z.number().int().min(1).max(64).optional(),
            id: NodeIdSchema.optional(),
        }).strict(),
        z.object({
            t: z.literal('terrain.setLayerMaterial'),
            id: NodeIdSchema,
            layer: z.number().int().min(0).max(254),
            material: z.string().max(512),
        }).strict(),
        z.object({
            t: z.literal('terrain.ramp'),
            start: Vec3Schema,
            end: Vec3Schema,
            radius: z.number().finite().gt(0).max(500).optional(),
            strength: z.number().finite().gt(0).max(1).optional(),
            falloff: z.enum(['linear', 'smooth', 'gaussian']).optional(),
            id: NodeIdSchema.optional(),
        }).strict(),
        z.object({
            t: z.literal('terrain.hole'),
            position: Vec3Schema,
            radius: z.number().finite().gt(0).max(500).optional(),
            erase: z.boolean().optional(),
            id: NodeIdSchema.optional(),
        }).strict(),
        z.object({ t: z.literal('navmesh.bake'), id: NodeIdSchema.optional() }).strict(),
        z.object({
            t: z.literal('audio.control'),
            id: NodeIdSchema,
            action: z.enum(['play', 'stop', 'pause', 'resume']),
        }).strict(),
        z.object({ t: z.literal('build.run'), target: z.literal('script').optional() }).strict(),
        z.object({
            t: z.literal('fluid.create'),
            path: FluidPathSchema,
            preset: FluidPresetSchema.optional(),
            overwrite: z.boolean().optional(),
        }).strict(),
        z.object({ t: z.literal('fluid.set'), path: FluidPathSchema, fields: FluidFieldsSchema }).strict(),
        z.object({
            t: z.literal('fluid.addOperator'),
            path: FluidPathSchema,
            list: FluidOperatorListSchema,
            type: FluidOperatorTypeSchema.optional(),
            index: FluidOperatorIndexSchema.optional(),
            fields: FluidFieldsSchema.optional(),
        }).strict(),
        z.object({
            t: z.literal('fluid.removeOperator'),
            path: FluidPathSchema,
            list: FluidOperatorListSchema,
            index: FluidOperatorIndexSchema,
        }).strict(),
        z.object({
            t: z.literal('fluid.moveOperator'),
            path: FluidPathSchema,
            list: FluidOperatorListSchema,
            from: FluidOperatorIndexSchema,
            to: FluidOperatorIndexSchema,
        }).strict(),
        z.object({
            t: z.literal('fluid.preview'),
            path: FluidPathSchema,
            // frame が第一級 (焼きの第 n コマ)。time は «一番近いコマ» へ吸着させる旧来の指定。
            frame: z.number().int().min(0).max(1023).optional(),
            time: z.number().finite().min(0).max(600).optional(),
            size: z.number().int().min(32).max(2048).optional(),
            contactSheet: z.boolean().optional(),
            variants: z.number().int().min(1).max(16).optional(),
            seed: z.number().int().min(0).max(4294967295).optional(),
        }).strict(),
        z.object({
            t: z.literal('fluid.bake'),
            path: FluidPathSchema,
            updateMaterial: z.boolean().optional(),
            seed: z.number().int().min(0).max(4294967295).optional(),
        }).strict(),
        z.object({ t: z.literal('fluid.cancel'), job: FluidJobIdSchema }).strict(),
        z.object({
            t: z.literal('fluid.createEffect'),
            // ファイル名の素になる。区切り文字を許すと dir の外へ書けてしまう。
            name: FluidEffectNameSchema,
            dir: z.string().min(1).max(1024).optional(),
            preset: FluidPresetSchema.optional(),
            fields: FluidFieldsSchema.optional(),
            bake: z.boolean().optional(),
        }).strict(),
        z.object({
            t: z.literal('playtest.run'),
            path: z.string().min(1).max(1024).optional(),
            scenario: z.record(z.string(), JsonValueSchema).optional(),
            updateBaselines: z.boolean().optional(),
            skipImages: z.boolean().optional(),
        }).strict().refine((value) => value.path !== undefined || value.scenario !== undefined, {
            message: 'path か scenario のどちらかが必要です',
        }),
        z.object({ t: z.literal('playtest.cancel') }).strict(),
        z.object({
            t: z.literal('input.record'),
            action: z.enum(['start', 'stop']),
            path: z.string().min(1).max(1024).optional(),
        }).strict(),
        z.object({
            t: z.literal('visual.compare'),
            // Tests/Golden 相対・拡張子なし。".." を許すと基準画像の置き場の外へ書ける。
            baseline: z.string().min(1).max(128).regex(/^[A-Za-z0-9_\-/]+$/),
            view: z.enum(['game', 'scene']).optional(),
            updateBaseline: z.boolean().optional(),
            pixelThreshold: z.number().min(0).max(1).optional(),
        }).strict(),
        z.object({
            t: z.literal('editor.transaction'),
            label: z.string().min(1).max(128),
            cmds: z.array(EditorCommandSchema).min(1).max(128),
        }).strict(),
    ]),
);

// Named Pipe 上を流れる要求 envelope。source は将来 React クライアントと区別するため常に付与する。
export interface EditorBusRequest {
    protocol: typeof EDITOR_PROTOCOL;
    id: string;
    kind: 'query' | 'command';
    payload: EditorQuery | EditorCommand;
    dryRun: boolean;
    source: 'mcp';
}

export const EditorBusResponseSchema = z.object({
    protocol: z.literal(EDITOR_PROTOCOL),
    id: z.string().uuid(),
    ok: z.boolean(),
    result: z.unknown().optional(),
    error: z.object({
        code: z.string().min(1),
        message: z.string().min(1),
        details: z.unknown().optional(),
    }).optional(),
}).strict();
export type EditorBusResponse = z.infer<typeof EditorBusResponseSchema>;

// viewport.capture の応答。engine が PNG を base64 化して返し、MCP が image content へ変換する。
export const ViewportCaptureResultSchema = z.object({
    mimeType: z.literal('image/png'),
    base64: z.string().min(1),
    width: z.number().int().positive(),
    height: z.number().int().positive(),
    cameraId: NodeIdSchema.optional(),
    view: z.enum(['scene', 'game']).optional(),
}).strict();
export type ViewportCaptureResult = z.infer<typeof ViewportCaptureResultSchema>;

export const SemanticViewportResultSchema = z.object({
    mimeType: z.literal('image/png'),
    base64: z.string().min(1),
    width: z.number().int().positive(),
    height: z.number().int().positive(),
    cameraPosition: Vec3Schema,
    view: z.enum(['scene', 'game']).optional(),
    objects: z.array(z.object({
        id: NodeIdSchema,
        name: z.string(),
        parent: NodeIdSchema.optional(),
        worldPosition: Vec3Schema,
        visible: z.boolean(),
        depth: z.number(),
        pixel: z.tuple([z.number(), z.number()]),
    }).strict()),
}).strict();
export type SemanticViewportResult = z.infer<typeof SemanticViewportResultSchema>;

export const AssetThumbnailResultSchema = z.object({
    mimeType: z.enum(['image/png', 'image/jpeg']),
    base64: z.string().min(1),
    path: z.string().min(1),
}).strict();

// fluid.jobStatus の応答。C++ 側で項目が増えても MCP を壊さないよう未知キーは素通しする。
// image は asset.thumbnail と同じ形で、MCP 側で image content へ移し替える。
export const FluidJobImageSchema = z.looseObject({
    mimeType: z.enum(['image/png', 'image/jpeg']),
    base64: z.string().min(1),
});
export const FluidJobStatusResultSchema = z.looseObject({
    job: z.number().int(),
    state: z.enum(['queued', 'running', 'encoding', 'done', 'failed', 'cancelled']),
    image: FluidJobImageSchema.optional(),
});

// 切り抜き 1 コマ。reference をそのまま component_set へ渡せる形で返す。
export const SpriteThumbnailResultSchema = z.object({
    mimeType: z.literal('image/png'),
    base64: z.string().min(1),
    width: z.number().int(),
    height: z.number().int(),
    sourceWidth: z.number().int(),
    sourceHeight: z.number().int(),
    name: z.string(),
    reference: z.string().min(1),
}).strict();
