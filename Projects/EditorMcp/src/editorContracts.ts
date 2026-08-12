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

// VFX プレビューの視点。注視点まわりの球面座標で持つ。
// WHY: 自由なカメラ行列を組ませると同じ「斜め上から」を再現できず評価が揺れる。
//      距離 1 本で LOD 検証、yaw 1 本でビルボードのシルエット検証ができる形に絞る。
export const VFXPreviewCameraSchema = z.object({
    preset: z.enum(['front', 'angle', 'side', 'top']).optional()
        .describe('代表視点。yaw/pitch を個別指定すると、そちらが preset を上書きする'),
    target: Vec3Schema.optional().describe('注視点 (既定 [0, 0.5, 0])'),
    distance: z.number().finite().min(0.05).max(500).optional()
        .describe('注視点からの距離 m。ゲーム内距離で読めるかの検証はここを振る (既定 5)'),
    yaw: z.number().finite().min(-360).max(360).optional()
        .describe('度。0 = 正面 / 90 = 真横。ビルボードの破綻は正面からは判らない'),
    pitch: z.number().finite().min(-89).max(89).optional().describe('度。正で見下ろし (既定 10)'),
    fovY: z.number().finite().min(5).max(120).optional().describe('度 (既定 60)'),
}).strict();
export type VFXPreviewCamera = z.infer<typeof VFXPreviewCameraSchema>;

// 副作用のない読み取り要求。Editor の状態を観測するだけで Scene を変更しない。
export type EditorQuery =
    | { t: 'editor.catalog' }
    | { t: 'editor.catalog.search'; query?: string | undefined; category?: string | undefined; limit?: number | undefined }
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
    // detail 既定 summary。transform / editorPosition / group / signalNode の中身は full だけ返る。
    | { t: 'vfx.graph'; path: string; detail?: 'summary' | 'full' | undefined }
    // vfx.node.setField の対になる読み出し。schemaPath 省略時はノードの全 leaf。
    | { t: 'vfx.nodeField'; path: string; nodeId: number; schemaPath?: string | undefined; prefix?: string | undefined }
    | { t: 'vfx.lint'; path: string }
    | { t: 'vfx.diff'; base: string; target: string }
    | { t: 'vfx.params'; path: string; id?: string | undefined }
    | { t: 'vfx.schema' }
    | { t: 'vfx.guide' }
    | { t: 'vfx.templateCatalog'; query?: string | undefined; limit?: number | undefined }
    // ── Behavior Tree ──
    // 木の構造・Blackboard・検証結果。order が優先度そのものなので必ず含まれる。
    | { t: 'bt.tree'; path: string }
    | { t: 'bt.lint'; path: string }
    | { t: 'bt.guide' }
    // 直前の vfx.preview が構築した実行状態。引数は無く、常にその時刻のまま返る。
    | { t: 'vfx.runtime' }
    // Preview World を起動し、プレビュー系を呼べる状態かを返す。preview 系の前提確認。
    | { t: 'vfx.previewEnsure' }
    // 素材テクスチャの観測。中身を知らないまま blendMode 等を決めるのを防ぐ。
    | { t: 'vfx.textureAnalyze'; path: string }
    // .mat とその albedo を併せて見る。Emitter の blendMode は .mat に上書きされるため。
    | { t: 'vfx.materialAnalyze'; path: string }
    // プロジェクトの素材を分類し、狙う層構成に対して何が足りないかを返す。
    // 分類は size + mtime でキャッシュされ、refresh=true でのみ再解析する。
    | { t: 'vfx.assetSurvey'; directory?: string | undefined; limit?: number | undefined;
        detail?: 'summary' | 'full' | undefined; refresh?: boolean | undefined }
    // シェーダーが公開する変数とテクスチャスロットの目録。書ける名前の唯一の正本。
    | { t: 'shader.inspect'; path: string }
    | { t: 'shader.diagnostics' }
    | { t: 'vfx.curvePresets' }
    | { t: 'vfx.preview'; path: string; time: number; w?: number | undefined; h?: number | undefined; view?: 'normal' | 'overdraw' | 'gizmos' | undefined; camera?: VFXPreviewCamera | undefined }
    // 直前に描いたプレビュー画を「絵」ではなく「数値」として読む。動き指標は連続呼び出しで意味を持つ。
    | { t: 'vfx.previewMetrics'; path: string; view?: 'normal' | 'overdraw' | 'gizmos' | undefined }
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
    | { t: 'viewport.capture'; w: number; h: number; cameraId?: string | undefined; view?: 'scene' | 'game' | 'vfx' | undefined }
    | { t: 'viewport.semantic'; w: number; h: number; view?: 'scene' | 'game' | undefined };
export const EditorQuerySchema: z.ZodType<EditorQuery> = z.discriminatedUnion('t', [
    z.object({ t: z.literal('editor.catalog') }).strict(),
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
    z.object({ t: z.literal('vfx.graph'), path: z.string().min(1),
        detail: z.enum(['summary', 'full']).optional() }).strict(),
    // schemaPath 単体か、prefix で絞った leaf 群の現在値を返す (setField の value と同じ表現)。
    z.object({ t: z.literal('vfx.nodeField'), path: z.string().min(1),
        nodeId: z.number().int().positive(),
        schemaPath: z.string().min(1).max(256).optional(),
        prefix: z.string().min(1).max(256).optional() }).strict(),
    // .vfx を静的診断し、孤立ノード/欠落アセット/budget超過などを構造化して返す。
    z.object({ t: z.literal('vfx.lint'), path: z.string().min(1) }).strict(),
    // 2つの .vfx をノード/リンク/パラメーター単位で比較する。
    z.object({ t: z.literal('vfx.diff'), base: z.string().min(1), target: z.string().min(1) }).strict(),
    z.object({ t: z.literal('vfx.params'), path: z.string().min(1), id: NodeIdSchema.optional() }).strict(),
    z.object({ t: z.literal('vfx.schema') }).strict(),
    // 作る前に読むオーサリング規約。lintCode を持つ規約は vfx.lint が検査する。
    z.object({ t: z.literal('vfx.guide') }).strict(),
    z.object({
        t: z.literal('vfx.templateCatalog'),
        query: z.string().min(1).max(128).optional(),
        limit: z.number().int().min(1).max(256).optional(),
    }).strict(),
    z.object({ t: z.literal('bt.tree'), path: z.string().min(1).max(1024) }).strict(),
    z.object({ t: z.literal('bt.lint'), path: z.string().min(1).max(1024) }).strict(),
    z.object({ t: z.literal('bt.guide') }).strict(),
    z.object({ t: z.literal('vfx.runtime') }).strict(),
    // Preview World の起動と存在確認。preview 系を呼ぶ前の前提チェック。
    z.object({ t: z.literal('vfx.previewEnsure') }).strict(),
    z.object({ t: z.literal('vfx.textureAnalyze'), path: z.string().min(1).max(1024) }).strict(),
    z.object({ t: z.literal('vfx.materialAnalyze'), path: z.string().min(1).max(1024) }).strict(),
    z.object({ t: z.literal('vfx.assetSurvey'),
        directory: z.string().min(1).max(1024).optional(),
        limit: z.number().int().min(1).max(400).optional(),
        detail: z.enum(['summary', 'full']).optional(),
        refresh: z.boolean().optional() }).strict(),
    z.object({ t: z.literal('shader.inspect'), path: z.string().min(1).max(1024) }).strict(),
    z.object({ t: z.literal('shader.diagnostics') }).strict(),
    // 名前付きカーブプリセットの目録。setField の value へ {preset:'Spike'} で渡す。
    z.object({ t: z.literal('vfx.curvePresets') }).strict(),
    z.object({ t: z.literal('vfx.preview'), path: z.string().min(1), time: z.number().finite().nonnegative(),
        w: z.number().int().min(160).max(1920).optional(), h: z.number().int().min(90).max(1080).optional(),
        // 診断ビュー。既定 normal はクリーンな評価画のままで、明示要求時だけ切り替える。
        view: z.enum(['normal', 'overdraw', 'gizmos']).optional(),
        // 視点。省略すると従来どおり Editor の固定視点になる。
        camera: VFXPreviewCameraSchema.optional() }).strict(),
    z.object({ t: z.literal('vfx.previewMetrics'), path: z.string().min(1),
        view: z.enum(['normal', 'overdraw', 'gizmos']).optional() }).strict(),
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
        view: z.enum(['scene', 'game', 'vfx']).optional(),
    }).strict(),
    z.object({
        t: z.literal('viewport.semantic'),
        w: z.number().int().min(160).max(1920),
        h: z.number().int().min(90).max(1080),
        view: z.enum(['scene', 'game']).optional(),
    }).strict(),
]);

const CommandNameSchema = z.string().min(1).max(128);
const ComponentNameSchema = z.string().min(1).max(128);

// Scene を変更しうる要求。engine 側で Undo 可能な Command Bus へ転送される。
export type EditorCommand =
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
    | { t: 'vfx.graph.set'; path: string; name?: string | undefined; maxParticles?: number | undefined; maxLights?: number | undefined; maxAudioVoices?: number | undefined }
    | { t: 'vfx.node.add'; path: string; nodeType: 'particle' | 'trail' | 'meshTrail' | 'light' | 'audio' | 'decal' | 'delay' | 'subGraph' | 'forceField' | 'mesh' | 'screenEffect' | 'cameraShake' | 'timeScale' | 'wind' | 'reroute'; name?: string | undefined; from?: number | undefined; assetPath?: string | undefined }
    | { t: 'vfx.node.duplicate'; path: string; nodeId: number; name?: string | undefined; editorX?: number | undefined; editorY?: number | undefined }
    | { t: 'vfx.node.remove'; path: string; nodeId: number }
    | { t: 'vfx.node.setEnabled'; path: string; nodeId: number; enabled: boolean }
    | { t: 'vfx.node.setMetadata'; path: string; nodeId: number; name?: string | undefined; editorX?: number | undefined; editorY?: number | undefined }
    // 空間の親子。parentNodeId 省略 = 親を外す (owner 直下へ戻す)。link とは別軸。
    | { t: 'vfx.node.setParent'; path: string; nodeId: number; parentNodeId?: number | undefined }
    | { t: 'vfx.node.setField'; path: string; nodeId: number; schemaPath: string; value: JsonValue }
    | { t: 'vfx.link.add'; path: string; from: number; to: number; trigger?: 'onComplete' | 'onStart' | 'onCollision' | 'onDeath' | undefined; delay?: number | undefined }
    | { t: 'vfx.link.update'; path: string; index: number; from?: number | undefined; to?: number | undefined; trigger?: 'onComplete' | 'onStart' | 'onCollision' | 'onDeath' | undefined; delay?: number | undefined }
    | { t: 'vfx.link.remove'; path: string; index: number }
    | { t: 'vfx.param.declare'; path: string; name: string; paramType: 'float' | 'int' | 'bool' | 'color' | 'vector3' | 'asset'; defaultValue: JsonValue; minimum?: number | undefined; maximum?: number | undefined }
    | { t: 'vfx.param.remove'; path: string; name: string }
    | { t: 'vfx.param.bind'; path: string; name: string; nodeId: number; schemaPath: string }
    | { t: 'vfx.param.unbind'; path: string; name: string; nodeId?: number | undefined; schemaPath?: string | undefined }
    | { t: 'vfx.param.setDefault'; path: string; name: string; value: JsonValue }
    | { t: 'vfx.instance.set'; id: string; name: string; value: JsonValue }
    | { t: 'vfx.instance.clear'; id: string; name: string }
    // mode 省略 = replace (Template を複製して新規アセット化。従来の挙動)。
    //   merge    — 既存 .vfx へ層を追記する。groups で Template の層だけを選べる。
    //   subgraph — 複製せず Sub Graph ノードとして参照する。Template の更新が伝播する。
    | { t: 'vfx.template.apply'; template: 'explosion' | 'fire' | 'smoke' | 'impact' | 'magic' | string; path: string;
        name?: string | undefined; description?: string | undefined; tags?: string[] | undefined;
        mode?: 'replace' | 'merge' | 'subgraph' | undefined;
        groups?: number[] | undefined; anchorNodeId?: number | undefined;
        trigger?: 'onComplete' | 'onStart' | 'onCollision' | 'onDeath' | undefined;
        delay?: number | undefined; parentNodeId?: number | undefined;
        variant?: string | undefined; raiseBudget?: boolean | undefined }
    // strategy 省略時は従来どおり粒子数だけを削る。fill rate が原因のときは 'fillRate' / 'both'。
    | { t: 'vfx.optimize'; path: string; targetParticles?: number | undefined; strategy?: 'particles' | 'fillRate' | 'both' | undefined }
    // 修復フラグは全て「省略 = 有効」。個別に false を渡したときだけその修復を止める。
    | { t: 'vfx.repair'; path: string; connectOrphans?: boolean | undefined; fixAssets?: boolean | undefined;
        fixSprites?: boolean | undefined; fixLighting?: boolean | undefined; fixSorting?: boolean | undefined;
        fixMeshFade?: boolean | undefined; fixParents?: boolean | undefined }
    // ── Behavior Tree ──
    // parentId 省略は「ルートとして作る」。既にルートがあれば拒否される。
    | { t: 'bt.node.add'; path: string; nodeType: string; parentId?: number | undefined; name?: string | undefined }
    | { t: 'bt.node.remove'; path: string; nodeId: number }
    | { t: 'bt.node.setParent'; path: string; nodeId: number; parentId: number }
    // order = 同じ親の中での優先度。小さいほど先に評価される。
    | { t: 'bt.node.setOrder'; path: string; nodeId: number; order: number }
    | { t: 'bt.node.setField'; path: string; nodeId: number; field: string; value: JsonValue }
    | { t: 'bt.blackboard.add'; path: string; name: string; type?: 'bool' | 'int' | 'float' | 'vector3' | 'entity' | 'string' | undefined }
    | { t: 'bt.blackboard.remove'; path: string; name: string }
    | { t: 'bt.autoLayout'; path: string }
    | { t: 'vfx.variant.upsert'; path: string; name: string; values: { [key: string]: JsonValue } }
    | { t: 'vfx.variant.remove'; path: string; name: string }
    | { t: 'vfx.group.add'; path: string; title?: string | undefined; note?: string | undefined; x?: number | undefined; y?: number | undefined; width?: number | undefined; height?: number | undefined; color?: [number, number, number, number] | undefined }
    | { t: 'vfx.group.update'; path: string; groupId: number; title?: string | undefined; note?: string | undefined; x?: number | undefined; y?: number | undefined; width?: number | undefined; height?: number | undefined; color?: [number, number, number, number] | undefined }
    | { t: 'vfx.group.remove'; path: string; groupId: number }
    | { t: 'vfx.generateMotionVectors'; texturePath: string; columns: number; rows: number; searchRadius?: number | undefined; loop?: boolean | undefined }
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
    | { t: 'editor.transaction'; label: string; cmds: EditorCommand[] };
export const EditorCommandSchema: z.ZodType<EditorCommand> = z.lazy(() =>
    z.discriminatedUnion('t', [
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
        z.object({ t: z.literal('vfx.graph.set'), path: z.string().min(1),
            name: z.string().min(1).max(128).optional(),
            maxParticles: z.number().int().min(1).max(10000000).optional(),
            maxLights: z.number().int().min(0).max(1024).optional(),
            maxAudioVoices: z.number().int().min(0).max(1024).optional() }).strict(),
        z.object({ t: z.literal('vfx.node.add'), path: z.string().min(1),
            // C++ 側 ParseVFXNodeType と同じ集合を保つこと。ここが欠けると
            // エンジンに実装済みのノードでも zod で弾かれ、AI からは存在しないのと同じになる。
            nodeType: z.enum(['particle', 'trail', 'meshTrail', 'light', 'audio', 'decal', 'delay',
                'subGraph', 'forceField', 'mesh', 'screenEffect', 'cameraShake', 'timeScale', 'wind',
                'reroute']),
            name: z.string().min(1).max(128).optional(), from: z.number().int().positive().optional(),
            assetPath: z.string().min(1).optional() }).strict(),
        z.object({ t: z.literal('vfx.node.duplicate'), path: z.string().min(1),
            nodeId: z.number().int().positive(), name: z.string().min(1).max(128).optional(),
            editorX: z.number().finite().optional(), editorY: z.number().finite().optional() }).strict(),
        z.object({ t: z.literal('vfx.node.remove'), path: z.string().min(1), nodeId: z.number().int().positive() }).strict(),
        z.object({ t: z.literal('vfx.node.setEnabled'), path: z.string().min(1), nodeId: z.number().int().positive(),
            enabled: z.boolean() }).strict(),
        z.object({ t: z.literal('vfx.node.setMetadata'), path: z.string().min(1), nodeId: z.number().int().positive(),
            name: z.string().min(1).max(128).optional(), editorX: z.number().finite().optional(),
            editorY: z.number().finite().optional() }).strict(),
        // parentNodeId は -1 (親を外す) を許すため positive ではなく int で受ける。
        z.object({ t: z.literal('vfx.node.setParent'), path: z.string().min(1), nodeId: z.number().int().positive(),
            parentNodeId: z.number().int().optional() }).strict(),
        z.object({ t: z.literal('vfx.node.setField'), path: z.string().min(1), nodeId: z.number().int().positive(),
            schemaPath: z.string().min(1).max(256), value: JsonValueSchema }).strict(),
        z.object({ t: z.literal('vfx.link.add'), path: z.string().min(1), from: z.number().int().positive(),
            to: z.number().int().positive(), trigger: z.enum(['onComplete', 'onStart', 'onCollision', 'onDeath']).optional(),
            delay: z.number().finite().nonnegative().optional() }).strict(),
        z.object({ t: z.literal('vfx.link.update'), path: z.string().min(1), index: z.number().int().nonnegative(),
            from: z.number().int().positive().optional(), to: z.number().int().positive().optional(),
            trigger: z.enum(['onComplete', 'onStart', 'onCollision', 'onDeath']).optional(),
            delay: z.number().finite().nonnegative().optional() }).strict(),
        z.object({ t: z.literal('vfx.link.remove'), path: z.string().min(1), index: z.number().int().nonnegative() }).strict(),
        z.object({ t: z.literal('vfx.param.declare'), path: z.string().min(1), name: z.string().min(1).max(128),
            paramType: z.enum(['float', 'int', 'bool', 'color', 'vector3', 'asset']), defaultValue: JsonValueSchema,
            minimum: z.number().finite().optional(), maximum: z.number().finite().optional() }).strict(),
        z.object({ t: z.literal('vfx.param.remove'), path: z.string().min(1),
            name: z.string().min(1).max(128) }).strict(),
        z.object({ t: z.literal('vfx.param.bind'), path: z.string().min(1), name: z.string().min(1).max(128),
            nodeId: z.number().int().positive(), schemaPath: z.string().min(1).max(256) }).strict(),
        z.object({ t: z.literal('vfx.param.unbind'), path: z.string().min(1), name: z.string().min(1).max(128),
            nodeId: z.number().int().positive().optional(), schemaPath: z.string().min(1).max(256).optional() }).strict(),
        z.object({ t: z.literal('vfx.param.setDefault'), path: z.string().min(1), name: z.string().min(1).max(128),
            value: JsonValueSchema }).strict(),
        z.object({ t: z.literal('vfx.instance.set'), id: NodeIdSchema, name: z.string().min(1).max(128),
            value: JsonValueSchema }).strict(),
        z.object({ t: z.literal('vfx.instance.clear'), id: NodeIdSchema, name: z.string().min(1).max(128) }).strict(),
        z.object({ t: z.literal('vfx.template.apply'), template: z.string().min(1).max(260),
            path: z.string().min(1).max(1024), name: z.string().min(1).max(128).optional(),
            description: z.string().max(512).optional(),
            tags: z.array(z.string().min(1).max(48)).max(16).optional(),
            mode: z.enum(['replace', 'merge', 'subgraph']).optional(),
            groups: z.array(z.number().int().min(1)).max(64).optional(),
            anchorNodeId: z.number().int().min(1).optional(),
            trigger: z.enum(['onComplete', 'onStart', 'onCollision', 'onDeath']).optional(),
            delay: z.number().min(0).max(600).optional(),
            parentNodeId: z.number().int().min(1).optional(),
            variant: z.string().min(1).max(128).optional(),
            raiseBudget: z.boolean().optional() }).strict()
            // groups / anchorNodeId / parent は「既存グラフのどこへ入れるか」の指定なので、
            // 複製 (replace) では意味を持たない。黙って無視すると効いたと誤解される。
            .refine((value) => value.mode === 'merge' || value.mode === 'subgraph'
                || (value.groups === undefined && value.anchorNodeId === undefined
                    && value.parentNodeId === undefined && value.trigger === undefined), {
                message: 'groups / anchorNodeId / parentNodeId / trigger は mode=merge または subgraph でのみ使えます',
            })
            .refine((value) => value.mode !== 'subgraph' || value.groups === undefined, {
                message: 'subgraph は Template 全体を参照するため groups を指定できません',
            }),
        z.object({ t: z.literal('vfx.optimize'), path: z.string().min(1),
            targetParticles: z.number().int().min(1).max(10000000).optional(),
            strategy: z.enum(['particles', 'fillRate', 'both']).optional() }).strict()
            .refine((value) => value.targetParticles !== undefined || value.strategy === 'fillRate', {
                message: 'targetParticles は strategy=fillRate のときだけ省略できます',
            }),
        // フリップブックアトラスからモーションベクターアトラスを生成する。
        // MV は外部ツールでしか作れず、AI が flipbook のブレンド品質を上げたくても
        // 手段が無かったため、コマンドとして公開する。
        z.object({ t: z.literal('vfx.generateMotionVectors'), texturePath: z.string().min(1).max(1024),
            columns: z.number().int().min(1).max(64), rows: z.number().int().min(1).max(64),
            searchRadius: z.number().int().min(1).max(64).optional(),
            loop: z.boolean().optional() }).strict(),
        // vfx.lint が指摘する機械的な不備を自動修正する (Undo 可能)。
        z.object({ t: z.literal('vfx.repair'), path: z.string().min(1),
            connectOrphans: z.boolean().optional(), fixAssets: z.boolean().optional(),
            fixSprites: z.boolean().optional(), fixLighting: z.boolean().optional(),
            fixSorting: z.boolean().optional(), fixMeshFade: z.boolean().optional(),
            fixParents: z.boolean().optional() }).strict(),
        // ── Behavior Tree ──
        z.object({ t: z.literal('bt.node.add'), path: z.string().min(1).max(1024),
            nodeType: z.string().min(1).max(64), parentId: z.number().int().min(1).optional(),
            name: z.string().min(1).max(128).optional() }).strict(),
        z.object({ t: z.literal('bt.node.remove'), path: z.string().min(1).max(1024),
            nodeId: z.number().int().min(1) }).strict(),
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
        z.object({ t: z.literal('vfx.variant.upsert'), path: z.string().min(1),
            name: z.string().min(1).max(128), values: z.record(z.string().min(1).max(128), JsonValueSchema) }).strict(),
        z.object({ t: z.literal('vfx.variant.remove'), path: z.string().min(1),
            name: z.string().min(1).max(128) }).strict(),
        z.object({ t: z.literal('vfx.group.add'), path: z.string().min(1),
            title: z.string().min(1).max(128).optional(), note: z.string().max(2048).optional(),
            x: z.number().finite().optional(), y: z.number().finite().optional(),
            width: z.number().finite().min(80).max(10000).optional(),
            height: z.number().finite().min(60).max(10000).optional(),
            color: z.tuple([z.number().finite(), z.number().finite(), z.number().finite(), z.number().finite()]).optional() }).strict(),
        z.object({ t: z.literal('vfx.group.update'), path: z.string().min(1), groupId: z.number().int().positive(),
            title: z.string().min(1).max(128).optional(), note: z.string().max(2048).optional(),
            x: z.number().finite().optional(), y: z.number().finite().optional(),
            width: z.number().finite().min(80).max(10000).optional(),
            height: z.number().finite().min(60).max(10000).optional(),
            color: z.tuple([z.number().finite(), z.number().finite(), z.number().finite(), z.number().finite()]).optional() }).strict(),
        z.object({ t: z.literal('vfx.group.remove'), path: z.string().min(1),
            groupId: z.number().int().positive() }).strict(),
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
    view: z.enum(['scene', 'game', 'vfx']).optional(),
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
