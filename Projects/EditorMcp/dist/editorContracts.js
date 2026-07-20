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
export const EditorQuerySchema = z.discriminatedUnion('t', [
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
    z.object({ t: z.literal('scene.tree') }).strict(),
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
    z.object({ t: z.literal('vfx.graph'), path: z.string().min(1) }).strict(),
    z.object({ t: z.literal('vfx.params'), path: z.string().min(1), id: NodeIdSchema.optional() }).strict(),
    z.object({ t: z.literal('vfx.schema') }).strict(),
    z.object({ t: z.literal('vfx.preview'), path: z.string().min(1), time: z.number().finite().nonnegative(),
        w: z.number().int().min(160).max(1920).optional(), h: z.number().int().min(90).max(1080).optional() }).strict(),
    z.object({ t: z.literal('material.inspect'), id: NodeIdSchema }).strict(),
    z.object({ t: z.literal('animation.state'), id: NodeIdSchema }).strict(),
    z.object({ t: z.literal('animation.graph'), id: NodeIdSchema }).strict(),
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
export const EditorCommandSchema = z.lazy(() => z.discriminatedUnion('t', [
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
    z.object({ t: z.literal('vfx.node.add'), path: z.string().min(1),
        nodeType: z.enum(['particle', 'trail', 'meshTrail', 'light', 'audio', 'decal', 'delay', 'subGraph']),
        name: z.string().min(1).max(128).optional(), from: z.number().int().positive().optional(),
        assetPath: z.string().min(1).optional() }).strict(),
    z.object({ t: z.literal('vfx.node.remove'), path: z.string().min(1), nodeId: z.number().int().positive() }).strict(),
    z.object({ t: z.literal('vfx.node.setField'), path: z.string().min(1), nodeId: z.number().int().positive(),
        schemaPath: z.string().min(1).max(256), value: JsonValueSchema }).strict(),
    z.object({ t: z.literal('vfx.link.add'), path: z.string().min(1), from: z.number().int().positive(),
        to: z.number().int().positive(), trigger: z.enum(['onComplete', 'onStart', 'onCollision', 'onDeath']).optional(),
        delay: z.number().finite().nonnegative().optional() }).strict(),
    z.object({ t: z.literal('vfx.link.remove'), path: z.string().min(1), index: z.number().int().nonnegative() }).strict(),
    z.object({ t: z.literal('vfx.param.declare'), path: z.string().min(1), name: z.string().min(1).max(128),
        paramType: z.enum(['float', 'int', 'bool', 'color', 'vector3', 'asset']), defaultValue: JsonValueSchema,
        minimum: z.number().finite().optional(), maximum: z.number().finite().optional() }).strict(),
    z.object({ t: z.literal('vfx.param.bind'), path: z.string().min(1), name: z.string().min(1).max(128),
        nodeId: z.number().int().positive(), schemaPath: z.string().min(1).max(256) }).strict(),
    z.object({ t: z.literal('vfx.param.setDefault'), path: z.string().min(1), name: z.string().min(1).max(128),
        value: JsonValueSchema }).strict(),
    z.object({ t: z.literal('vfx.instance.set'), id: NodeIdSchema, name: z.string().min(1).max(128),
        value: JsonValueSchema }).strict(),
    z.object({ t: z.literal('vfx.instance.clear'), id: NodeIdSchema, name: z.string().min(1).max(128) }).strict(),
    z.object({ t: z.literal('vfx.template.apply'), template: z.string().min(1).max(260),
        path: z.string().min(1).max(1024), name: z.string().min(1).max(128).optional() }).strict(),
    z.object({ t: z.literal('vfx.optimize'), path: z.string().min(1),
        targetParticles: z.number().int().min(1).max(10000000) }).strict(),
    z.object({ t: z.literal('vfx.variant.upsert'), path: z.string().min(1),
        name: z.string().min(1).max(128), values: z.record(z.string().min(1).max(128), JsonValueSchema) }).strict(),
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
    z.object({ t: z.literal('animation.removeState'), id: NodeIdSchema, state: z.string().min(1).max(128) }).strict(),
    z.object({
        t: z.literal('animation.removeMotion'),
        id: NodeIdSchema,
        state: z.string().min(1).max(128),
        motionIndex: z.number().int().nonnegative(),
    }).strict(),
    z.object({
        t: z.literal('animation.removeTransition'),
        id: NodeIdSchema,
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
    view: z.enum(['scene', 'game', 'vfx']).optional(),
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
//# sourceMappingURL=editorContracts.js.map