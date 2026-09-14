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
export const JsonValueSchema = z.lazy(() => z.union([
    z.null(),
    z.boolean(),
    z.number().finite(),
    z.string(),
    z.array(JsonValueSchema),
    z.record(z.string(), JsonValueSchema),
]));
export const ScenePropertyFilterSchema = z.object({
    comp: z.string().min(1).max(128),
    field: z.string().min(1).max(128),
    op: z.enum(['equals', 'notEquals', 'contains', 'greater', 'less']).default('equals'),
    value: JsonValueSchema,
}).strict();
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
export const FluidOperatorIndexSchema = z.number().int().min(0).max(15);
// 形 (source: sphere / box / cone / ring / texture、collider: sphere / box / plane) か
// 力の種類 (wind / attract / ...) のラベル。一覧は fluid.schema が正本。
export const FluidOperatorTypeSchema = z.string().min(1).max(32);
export const EditorQuerySchema = z.discriminatedUnion('t', [
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
]);
const CommandNameSchema = z.string().min(1).max(128);
const ComponentNameSchema = z.string().min(1).max(128);
export const EditorCommandSchema = z.lazy(() => z.discriminatedUnion('t', [
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
        if (!valid)
            context.addIssue({ code: 'custom', message: `kind=${input.kind} に必要な入力が不足しています` });
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
        op: z.enum(['raise', 'lower', 'smooth', 'flatten', 'stamp']).optional(),
        radius: z.number().finite().gt(0).max(500).optional(),
        strength: z.number().finite().gt(0).max(1).optional(),
        falloff: z.enum(['linear', 'smooth', 'gaussian']).optional(),
        iterations: z.number().int().min(1).max(64).optional(),
        targetHeight: z.number().finite().optional(),
        id: NodeIdSchema.optional(),
    }).strict(),
    z.object({
        t: z.literal('terrain.paint'),
        position: Vec3Schema,
        layer: z.number().int().min(0).max(3),
        radius: z.number().finite().gt(0).max(500).optional(),
        strength: z.number().finite().gt(0).max(1).optional(),
        falloff: z.enum(['linear', 'smooth', 'gaussian']).optional(),
        iterations: z.number().int().min(1).max(64).optional(),
        id: NodeIdSchema.optional(),
    }).strict(),
    z.object({
        t: z.literal('terrain.setLayerMaterial'),
        id: NodeIdSchema,
        layer: z.number().int().min(0).max(3),
        material: z.string().max(512),
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
        t: z.literal('editor.transaction'),
        label: z.string().min(1).max(128),
        cmds: z.array(EditorCommandSchema).min(1).max(128),
    }).strict(),
]));
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
// viewport.capture の応答。engine が PNG を base64 化して返し、MCP が image content へ変換する。
export const ViewportCaptureResultSchema = z.object({
    mimeType: z.literal('image/png'),
    base64: z.string().min(1),
    width: z.number().int().positive(),
    height: z.number().int().positive(),
    cameraId: NodeIdSchema.optional(),
    view: z.enum(['scene', 'game']).optional(),
}).strict();
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
//# sourceMappingURL=editorContracts.js.map