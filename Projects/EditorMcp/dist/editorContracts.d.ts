import * as z from 'zod/v4';
export declare const EDITOR_PROTOCOL = "fbzz.editor.v1";
export declare const NodeIdSchema: z.ZodString;
export declare const Vec3Schema: z.ZodTuple<[z.ZodNumber, z.ZodNumber, z.ZodNumber], null>;
export type JsonValue = null | boolean | number | string | JsonValue[] | {
    [key: string]: JsonValue;
};
export declare const JsonValueSchema: z.ZodType<JsonValue>;
export declare const ScenePropertyFilterSchema: z.ZodObject<{
    comp: z.ZodString;
    field: z.ZodString;
    op: z.ZodDefault<z.ZodEnum<{
        contains: "contains";
        equals: "equals";
        greater: "greater";
        less: "less";
        notEquals: "notEquals";
    }>>;
    value: z.ZodType<JsonValue, unknown, z.core.$ZodTypeInternals<JsonValue, unknown>>;
}, z.core.$strict>;
export type ScenePropertyFilter = z.infer<typeof ScenePropertyFilterSchema>;
export declare const FluidPathSchema: z.ZodString;
export declare const FluidJobIdSchema: z.ZodNumber;
export declare const FluidPresetSchema: z.ZodString;
export declare const FluidFieldsSchema: z.ZodRecord<z.ZodString, z.ZodType<JsonValue, unknown, z.core.$ZodTypeInternals<JsonValue, unknown>>>;
export declare const FluidEffectNameSchema: z.ZodString;
export declare const FluidOperatorListSchema: z.ZodEnum<{
    collider: "collider";
    force: "force";
    source: "source";
}>;
export type FluidOperatorList = z.infer<typeof FluidOperatorListSchema>;
export declare const FluidOperatorIndexSchema: z.ZodNumber;
export declare const FluidOperatorTypeSchema: z.ZodString;
export type EditorQuery = {
    t: 'editor.catalog';
} | {
    t: 'editor.catalog.search';
    query?: string | undefined;
    category?: string | undefined;
    limit?: number | undefined;
} | {
    t: 'editor.op.list';
    search?: string | undefined;
    category?: string | undefined;
    includeUnavailable?: boolean | undefined;
} | {
    t: 'editor.op.query';
    id: string;
    args?: Record<string, unknown> | undefined;
} | {
    t: 'editor.state';
} | {
    t: 'editor.undoHistory';
    limit?: number | undefined;
} | {
    t: 'console.logs';
    minLevel?: 'debug' | 'info' | 'warning' | 'error' | undefined;
    contains?: string | undefined;
    limit?: number | undefined;
    afterSequence?: number | undefined;
} | {
    t: 'scene.tree';
    includeGenerated?: boolean | undefined;
} | {
    t: 'scene.snapshot';
} | {
    t: 'scene.validate';
} | {
    t: 'scene.find';
    name?: string | undefined;
    tag?: string | undefined;
    active?: boolean | undefined;
    comp?: string | undefined;
    components?: string[] | undefined;
    properties?: ScenePropertyFilter[] | undefined;
    limit?: number | undefined;
} | {
    t: 'scene.selection';
} | {
    t: 'node.components';
    id: string;
} | {
    t: 'asset.list';
    dir?: string | undefined;
} | {
    t: 'asset.inspect';
    path: string;
} | {
    t: 'asset.findUnused';
    limit?: number | undefined;
} | {
    t: 'asset.thumbnail';
    path: string;
} | {
    t: 'sprite.list';
    path: string;
} | {
    t: 'sprite.thumbnail';
    path: string;
    sprite: string;
} | {
    t: 'bt.tree';
    path: string;
} | {
    t: 'bt.lint';
    path: string;
} | {
    t: 'bt.guide';
} | {
    t: 'bt.schema';
    nodeType?: string | undefined;
} | {
    t: 'bt.nodeField';
    path: string;
    nodeId: number;
    field?: string | undefined;
} | {
    t: 'bt.runtime';
    path?: string | undefined;
    id?: string | undefined;
} | {
    t: 'bt.diff';
    base: string;
    target: string;
} | {
    t: 'bt.templateCatalog';
} | {
    t: 'shader.inspect';
    path: string;
} | {
    t: 'shader.diagnostics';
} | {
    t: 'material.inspect';
    id: string;
} | {
    t: 'animation.state';
    id: string;
} | {
    t: 'animation.graph';
    id: string;
    layer?: string | undefined;
} | {
    t: 'avatarMask.get';
    path: string;
} | {
    t: 'animation.blendTree';
    id: string;
    state: string;
} | {
    t: 'animation.pose';
    id: string;
    limit?: number | undefined;
} | {
    t: 'profiler.snapshot';
    limit?: number | undefined;
} | {
    t: 'physics.raycast';
    origin: [number, number, number];
    direction: [number, number, number];
    maxDistance: number;
    sphereRadius?: number | undefined;
} | {
    t: 'physics.overlapSphere';
    center: [number, number, number];
    radius: number;
} | {
    t: 'physics.events';
} | {
    t: 'viewport.capture';
    w: number;
    h: number;
    cameraId?: string | undefined;
    view?: 'scene' | 'game' | undefined;
} | {
    t: 'viewport.semantic';
    w: number;
    h: number;
    view?: 'scene' | 'game' | undefined;
} | {
    t: 'scene.list';
} | {
    t: 'preset.catalog';
    category?: string | undefined;
} | {
    t: 'terrain.inspect';
    id?: string | undefined;
} | {
    t: 'terrain.sample';
    points: Array<number[]>;
    id?: string | undefined;
} | {
    t: 'navmesh.state';
    id?: string | undefined;
} | {
    t: 'navmesh.path';
    from?: [number, number, number] | undefined;
    to?: [number, number, number] | undefined;
    fromId?: string | undefined;
    toId?: string | undefined;
    surfaceId?: string | undefined;
    agentTypeId?: number | undefined;
    areaMask?: number | undefined;
} | {
    t: 'navmesh.sample';
    points: Array<number[]>;
    surfaceId?: string | undefined;
    agentTypeId?: number | undefined;
} | {
    t: 'environment.inspect';
} | {
    t: 'audio.inspect';
    id?: string | undefined;
} | {
    t: 'ui.inspect';
    id?: string | undefined;
} | {
    t: 'build.status';
    limit?: number | undefined;
} | {
    t: 'fluid.schema';
} | {
    t: 'fluid.get';
    path: string;
} | {
    t: 'fluid.jobStatus';
    job: number;
    includeImage?: boolean | undefined;
} | {
    t: 'playtest.status';
} | {
    t: 'playtest.list';
} | {
    t: 'editor.bus.list';
};
export declare const EditorQuerySchema: z.ZodType<EditorQuery>;
export type EditorCommand = {
    t: 'editor.op.invoke';
    id: string;
    args?: Record<string, unknown> | undefined;
} | {
    t: 'node.create';
    parent?: string | undefined;
    name?: string | undefined;
} | {
    t: 'node.duplicate';
    id: string;
    parent?: string | undefined;
    name?: string | undefined;
} | {
    t: 'node.delete';
    id: string;
} | {
    t: 'node.reparent';
    id: string;
    parent: string;
    index: number;
} | {
    t: 'node.rename';
    id: string;
    name: string;
} | {
    t: 'node.setActive';
    id: string;
    active: boolean;
} | {
    t: 'node.setTag';
    id: string;
    tag: string;
} | {
    t: 'node.setLayer';
    id: string;
    layer: number;
} | {
    t: 'selection.set';
    ids: string[];
} | {
    t: 'transform.set';
    id: string;
    pos?: [number, number, number] | undefined;
    rot?: [number, number, number] | undefined;
    scale?: [number, number, number] | undefined;
} | {
    t: 'component.add';
    id: string;
    comp: string;
} | {
    t: 'component.set';
    id: string;
    comp: string;
    field: string;
    value: JsonValue;
} | {
    t: 'component.remove';
    id: string;
    comp: string;
} | {
    t: 'asset.import';
    src: string;
    dst: string;
} | {
    t: 'prefab.instantiate';
    path: string;
    parent?: string | undefined;
} | {
    t: 'prefab.create';
    path: string;
    ids?: string[] | undefined;
} | {
    t: 'prefab.apply';
    id: string;
} | {
    t: 'prefab.revert';
    id: string;
} | {
    t: 'material.assign';
    id: string;
    path: string;
} | {
    t: 'material.override';
    id: string;
    parameter: string;
    value: number[];
} | {
    t: 'material.asset.setShader';
    path: string;
    shaderPath: string;
} | {
    t: 'bt.node.add';
    path: string;
    nodeType: string;
    parentId?: number | undefined;
    name?: string | undefined;
} | {
    t: 'bt.node.remove';
    path: string;
    nodeId: number;
} | {
    t: 'bt.node.duplicate';
    path: string;
    nodeId: number;
    parentId?: number | undefined;
} | {
    t: 'bt.node.setParent';
    path: string;
    nodeId: number;
    parentId: number;
} | {
    t: 'bt.node.setOrder';
    path: string;
    nodeId: number;
    order: number;
} | {
    t: 'bt.node.setField';
    path: string;
    nodeId: number;
    field: string;
    value: JsonValue;
} | {
    t: 'bt.blackboard.add';
    path: string;
    name: string;
    type?: 'bool' | 'int' | 'float' | 'vector3' | 'entity' | 'string' | undefined;
} | {
    t: 'bt.blackboard.remove';
    path: string;
    name: string;
} | {
    t: 'bt.autoLayout';
    path: string;
} | {
    t: 'bt.repair';
    path: string;
    fixAborts?: boolean | undefined;
    fixDurations?: boolean | undefined;
    fixWeights?: boolean | undefined;
    fixKeys?: boolean | undefined;
} | {
    t: 'bt.template.apply';
    template: string;
    path: string;
    name?: string | undefined;
    description?: string | undefined;
} | {
    t: 'vfx.generateMotionVectors';
    texturePath: string;
    columns: number;
    rows: number;
    searchRadius?: number | undefined;
    loop?: boolean | undefined;
    rowSequences?: boolean | undefined;
    materialPath?: string | undefined;
} | {
    t: 'sprite.rename';
    path: string;
    sprite: string;
    name: string;
} | {
    t: 'sprite.slice';
    path: string;
    type?: 'grid' | 'automatic' | undefined;
    columns?: number | undefined;
    rows?: number | undefined;
    cellWidth?: number | undefined;
    cellHeight?: number | undefined;
    offsetX?: number | undefined;
    offsetY?: number | undefined;
    paddingX?: number | undefined;
    paddingY?: number | undefined;
    pivotX?: number | undefined;
    pivotY?: number | undefined;
    keepEmptyRects?: boolean | undefined;
    prefix?: string | undefined;
    mode?: 'smart' | 'safe' | 'replace' | undefined;
} | {
    t: 'animation.control';
    id: string;
    action: 'play' | 'pause' | 'stop' | 'seek';
    clipName?: string | undefined;
    clipIndex?: number | undefined;
    state?: string | undefined;
    time?: number | undefined;
    frame?: number | undefined;
} | {
    t: 'animation.setParameter';
    id: string;
    name: string;
    value?: number | boolean | undefined;
} | {
    t: 'animation.addTransition';
    id: string;
    layer?: string | undefined;
    from?: string | undefined;
    to: string;
    hasExitTime?: boolean | undefined;
    exitTime?: number | undefined;
    fixedDuration?: boolean | undefined;
    transitionDuration?: number | undefined;
} | {
    t: 'animation.setCondition';
    id: string;
    layer?: string | undefined;
    from?: string | undefined;
    transitionIndex: number;
    action: 'add' | 'update' | 'remove' | 'clear';
    parameter?: string | undefined;
    op?: 'greater' | 'less' | 'equal' | 'notEqual' | 'true' | 'false' | undefined;
    threshold?: number | undefined;
    conditionIndex?: number | undefined;
} | {
    t: 'animation.addState';
    id: string;
    layer?: string | undefined;
    name: string;
    mode?: 'clip' | 'blendTree1D' | 'blendTree2D' | undefined;
    sourcePath?: string | undefined;
    clipName?: string | undefined;
    clipIndex?: number | undefined;
    speed?: number | undefined;
    loop?: boolean | undefined;
    ikWeight?: number | undefined;
    blendParameter?: string | undefined;
    blendParameterX?: string | undefined;
    blendParameterY?: string | undefined;
    setAsDefault?: boolean | undefined;
} | {
    t: 'animation.setState';
    id: string;
    layer?: string | undefined;
    state: string;
    name?: string | undefined;
    mode?: 'clip' | 'blendTree1D' | 'blendTree2D' | undefined;
    sourcePath?: string | undefined;
    clipName?: string | undefined;
    clipIndex?: number | undefined;
    speed?: number | undefined;
    loop?: boolean | undefined;
    ikWeight?: number | undefined;
    blendParameter?: string | undefined;
    blendParameterX?: string | undefined;
    blendParameterY?: string | undefined;
    blend2DType?: 'simpleDirectional' | 'freeformCartesian' | undefined;
    setAsDefault?: boolean | undefined;
} | {
    t: 'animation.addMotion';
    id: string;
    layer?: string | undefined;
    state: string;
    sourcePath?: string | undefined;
    clipName?: string | undefined;
    clipIndex?: number | undefined;
    threshold?: number | undefined;
    posX?: number | undefined;
    posY?: number | undefined;
    speed?: number | undefined;
    ikWeight?: number | undefined;
} | {
    t: 'animation.setMotion';
    id: string;
    layer?: string | undefined;
    state: string;
    motionIndex: number;
    sourcePath?: string | undefined;
    clipName?: string | undefined;
    clipIndex?: number | undefined;
    threshold?: number | undefined;
    posX?: number | undefined;
    posY?: number | undefined;
    speed?: number | undefined;
    ikWeight?: number | undefined;
} | {
    t: 'animation.removeState';
    id: string;
    layer?: string | undefined;
    state: string;
} | {
    t: 'animation.removeMotion';
    id: string;
    layer?: string | undefined;
    state: string;
    motionIndex: number;
} | {
    t: 'animation.removeTransition';
    id: string;
    layer?: string | undefined;
    from?: string | undefined;
    transitionIndex: number;
} | {
    t: 'animation.addParameter';
    id: string;
    name: string;
    type?: 'float' | 'int' | 'bool' | 'trigger' | undefined;
    value?: number | boolean | undefined;
} | {
    t: 'animation.removeParameter';
    id: string;
    name: string;
} | {
    t: 'animation.addLayer';
    id: string;
    name: string;
    weight?: number | undefined;
    mode?: 'override' | 'additive' | undefined;
    enabled?: boolean | undefined;
    maskPath?: string | undefined;
    stateName?: string | undefined;
    additiveSourcePath?: string | undefined;
    additiveClipName?: string | undefined;
    additiveTime?: number | undefined;
} | {
    t: 'animation.setLayer';
    id: string;
    layer: string;
    name?: string | undefined;
    weight?: number | undefined;
    mode?: 'override' | 'additive' | undefined;
    enabled?: boolean | undefined;
    maskPath?: string | undefined;
    stateName?: string | undefined;
    defaultStateName?: string | undefined;
    additiveSourcePath?: string | undefined;
    additiveClipName?: string | undefined;
    additiveTime?: number | undefined;
} | {
    t: 'animation.removeLayer';
    id: string;
    layer: string;
} | {
    t: 'avatarMask.write';
    path: string;
    action: 'create' | 'setBone' | 'removeBone' | 'clear' | 'setDefaultInclude';
    name?: string | undefined;
    overwrite?: boolean | undefined;
    defaultInclude?: boolean | undefined;
    skeletonSourcePath?: string | undefined;
    bone?: string | undefined;
    weight?: number | undefined;
    includeChildren?: boolean | undefined;
    blendDepth?: number | undefined;
} | {
    t: 'animation.playSlot';
    id: string;
    layer: string;
    sourcePath?: string | undefined;
    clipName?: string | undefined;
    fadeIn?: number | undefined;
    fadeOut?: number | undefined;
    speed?: number | undefined;
    loop?: boolean | undefined;
} | {
    t: 'animation.stopSlot';
    id: string;
    layer: string;
    fadeOut?: number | undefined;
} | {
    t: 'input.inject';
    kind: 'key' | 'axis' | 'gamepadButton' | 'gamepadAxis' | 'mouseButton' | 'mousePosition' | 'mouseDelta' | 'mouseScroll' | 'clear';
    key?: string | undefined;
    buttonName?: string | undefined;
    pressed?: boolean | undefined;
    axis?: string | undefined;
    button?: number | undefined;
    value?: number | [number, number] | undefined;
} | {
    t: 'play.control';
    action: 'start' | 'stop' | 'pause' | 'resume' | 'step';
} | {
    t: 'viewport.camera';
    position?: [number, number, number] | undefined;
    lookAt?: [number, number, number] | undefined;
    targetId?: string | undefined;
} | {
    t: 'editor.undo';
} | {
    t: 'editor.redo';
} | {
    t: 'preset.create';
    preset: string;
    parent?: string | undefined;
    name?: string | undefined;
    position?: [number, number, number] | undefined;
} | {
    t: 'scene.open';
    path: string;
    discardUnsaved?: boolean | undefined;
} | {
    t: 'scene.save';
    path?: string | undefined;
} | {
    t: 'terrain.sculpt';
    position: [number, number, number];
    op?: 'raise' | 'lower' | 'smooth' | 'flatten' | 'stamp' | undefined;
    radius?: number | undefined;
    strength?: number | undefined;
    falloff?: 'linear' | 'smooth' | 'gaussian' | undefined;
    iterations?: number | undefined;
    targetHeight?: number | undefined;
    id?: string | undefined;
} | {
    t: 'terrain.paint';
    position: [number, number, number];
    layer: number;
    radius?: number | undefined;
    strength?: number | undefined;
    falloff?: 'linear' | 'smooth' | 'gaussian' | undefined;
    iterations?: number | undefined;
    id?: string | undefined;
} | {
    t: 'terrain.setLayerMaterial';
    id: string;
    layer: number;
    material: string;
} | {
    t: 'navmesh.bake';
    id?: string | undefined;
} | {
    t: 'audio.control';
    id: string;
    action: 'play' | 'stop' | 'pause' | 'resume';
} | {
    t: 'build.run';
    target?: 'script' | undefined;
} | {
    t: 'fluid.create';
    path: string;
    preset?: string | undefined;
    overwrite?: boolean | undefined;
} | {
    t: 'fluid.set';
    path: string;
    fields: Record<string, JsonValue>;
} | {
    t: 'fluid.addOperator';
    path: string;
    list: FluidOperatorList;
    type?: string | undefined;
    index?: number | undefined;
    fields?: Record<string, JsonValue> | undefined;
} | {
    t: 'fluid.removeOperator';
    path: string;
    list: FluidOperatorList;
    index: number;
} | {
    t: 'fluid.moveOperator';
    path: string;
    list: FluidOperatorList;
    from: number;
    to: number;
} | {
    t: 'fluid.preview';
    path: string;
    frame?: number | undefined;
    time?: number | undefined;
    size?: number | undefined;
    contactSheet?: boolean | undefined;
    variants?: number | undefined;
    seed?: number | undefined;
} | {
    t: 'fluid.bake';
    path: string;
    updateMaterial?: boolean | undefined;
    seed?: number | undefined;
} | {
    t: 'fluid.cancel';
    job: number;
} | {
    t: 'fluid.createEffect';
    name: string;
    dir?: string | undefined;
    preset?: string | undefined;
    fields?: Record<string, JsonValue> | undefined;
    bake?: boolean | undefined;
} | {
    t: 'playtest.run';
    path?: string | undefined;
    scenario?: Record<string, JsonValue> | undefined;
    updateBaselines?: boolean | undefined;
    skipImages?: boolean | undefined;
} | {
    t: 'playtest.cancel';
} | {
    t: 'input.record';
    action: 'start' | 'stop';
    path?: string | undefined;
} | {
    t: 'visual.compare';
    baseline: string;
    view?: 'game' | 'scene' | undefined;
    updateBaseline?: boolean | undefined;
    pixelThreshold?: number | undefined;
} | {
    t: 'editor.transaction';
    label: string;
    cmds: EditorCommand[];
};
export declare const EditorCommandSchema: z.ZodType<EditorCommand>;
export interface EditorBusRequest {
    protocol: typeof EDITOR_PROTOCOL;
    id: string;
    kind: 'query' | 'command';
    payload: EditorQuery | EditorCommand;
    dryRun: boolean;
    source: 'mcp';
}
export declare const EditorBusResponseSchema: z.ZodObject<{
    protocol: z.ZodLiteral<"fbzz.editor.v1">;
    id: z.ZodString;
    ok: z.ZodBoolean;
    result: z.ZodOptional<z.ZodUnknown>;
    error: z.ZodOptional<z.ZodObject<{
        code: z.ZodString;
        message: z.ZodString;
        details: z.ZodOptional<z.ZodUnknown>;
    }, z.core.$strip>>;
}, z.core.$strict>;
export type EditorBusResponse = z.infer<typeof EditorBusResponseSchema>;
export declare const ViewportCaptureResultSchema: z.ZodObject<{
    mimeType: z.ZodLiteral<"image/png">;
    base64: z.ZodString;
    width: z.ZodNumber;
    height: z.ZodNumber;
    cameraId: z.ZodOptional<z.ZodString>;
    view: z.ZodOptional<z.ZodEnum<{
        game: "game";
        scene: "scene";
    }>>;
}, z.core.$strict>;
export type ViewportCaptureResult = z.infer<typeof ViewportCaptureResultSchema>;
export declare const SemanticViewportResultSchema: z.ZodObject<{
    mimeType: z.ZodLiteral<"image/png">;
    base64: z.ZodString;
    width: z.ZodNumber;
    height: z.ZodNumber;
    cameraPosition: z.ZodTuple<[z.ZodNumber, z.ZodNumber, z.ZodNumber], null>;
    view: z.ZodOptional<z.ZodEnum<{
        game: "game";
        scene: "scene";
    }>>;
    objects: z.ZodArray<z.ZodObject<{
        id: z.ZodString;
        name: z.ZodString;
        parent: z.ZodOptional<z.ZodString>;
        worldPosition: z.ZodTuple<[z.ZodNumber, z.ZodNumber, z.ZodNumber], null>;
        visible: z.ZodBoolean;
        depth: z.ZodNumber;
        pixel: z.ZodTuple<[z.ZodNumber, z.ZodNumber], null>;
    }, z.core.$strict>>;
}, z.core.$strict>;
export type SemanticViewportResult = z.infer<typeof SemanticViewportResultSchema>;
export declare const AssetThumbnailResultSchema: z.ZodObject<{
    mimeType: z.ZodEnum<{
        "image/jpeg": "image/jpeg";
        "image/png": "image/png";
    }>;
    base64: z.ZodString;
    path: z.ZodString;
}, z.core.$strict>;
export declare const FluidJobImageSchema: z.ZodObject<{
    mimeType: z.ZodEnum<{
        "image/jpeg": "image/jpeg";
        "image/png": "image/png";
    }>;
    base64: z.ZodString;
}, z.core.$loose>;
export declare const FluidJobStatusResultSchema: z.ZodObject<{
    job: z.ZodNumber;
    state: z.ZodEnum<{
        cancelled: "cancelled";
        done: "done";
        encoding: "encoding";
        failed: "failed";
        queued: "queued";
        running: "running";
    }>;
    image: z.ZodOptional<z.ZodObject<{
        mimeType: z.ZodEnum<{
            "image/jpeg": "image/jpeg";
            "image/png": "image/png";
        }>;
        base64: z.ZodString;
    }, z.core.$loose>>;
}, z.core.$loose>;
export declare const SpriteThumbnailResultSchema: z.ZodObject<{
    mimeType: z.ZodLiteral<"image/png">;
    base64: z.ZodString;
    width: z.ZodNumber;
    height: z.ZodNumber;
    sourceWidth: z.ZodNumber;
    sourceHeight: z.ZodNumber;
    name: z.ZodString;
    reference: z.ZodString;
}, z.core.$strict>;
