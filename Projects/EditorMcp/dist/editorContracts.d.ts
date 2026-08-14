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
export declare const VFXPreviewCameraSchema: z.ZodObject<{
    preset: z.ZodOptional<z.ZodEnum<{
        angle: "angle";
        front: "front";
        side: "side";
        top: "top";
    }>>;
    target: z.ZodOptional<z.ZodTuple<[z.ZodNumber, z.ZodNumber, z.ZodNumber], null>>;
    distance: z.ZodOptional<z.ZodNumber>;
    yaw: z.ZodOptional<z.ZodNumber>;
    pitch: z.ZodOptional<z.ZodNumber>;
    fovY: z.ZodOptional<z.ZodNumber>;
}, z.core.$strict>;
export type VFXPreviewCamera = z.infer<typeof VFXPreviewCameraSchema>;
export type EditorQuery = {
    t: 'editor.catalog';
} | {
    t: 'editor.catalog.search';
    query?: string | undefined;
    category?: string | undefined;
    limit?: number | undefined;
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
    t: 'vfx.graph';
    path: string;
    detail?: 'summary' | 'full' | undefined;
} | {
    t: 'vfx.nodeField';
    path: string;
    nodeId: number;
    schemaPath?: string | undefined;
    prefix?: string | undefined;
} | {
    t: 'vfx.lint';
    path: string;
} | {
    t: 'vfx.diff';
    base: string;
    target: string;
} | {
    t: 'vfx.params';
    path: string;
    id?: string | undefined;
} | {
    t: 'vfx.schema';
} | {
    t: 'vfx.guide';
} | {
    t: 'vfx.templateCatalog';
    query?: string | undefined;
    limit?: number | undefined;
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
    t: 'vfx.runtime';
} | {
    t: 'vfx.previewEnsure';
} | {
    t: 'vfx.textureAnalyze';
    path: string;
} | {
    t: 'vfx.materialAnalyze';
    path: string;
} | {
    t: 'vfx.assetSurvey';
    directory?: string | undefined;
    limit?: number | undefined;
    detail?: 'summary' | 'full' | undefined;
    refresh?: boolean | undefined;
} | {
    t: 'shader.inspect';
    path: string;
} | {
    t: 'shader.diagnostics';
} | {
    t: 'vfx.curvePresets';
} | {
    t: 'vfx.preview';
    path: string;
    time: number;
    w?: number | undefined;
    h?: number | undefined;
    view?: 'normal' | 'overdraw' | 'gizmos' | undefined;
    camera?: VFXPreviewCamera | undefined;
} | {
    t: 'vfx.previewMetrics';
    path: string;
    view?: 'normal' | 'overdraw' | 'gizmos' | undefined;
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
    view?: 'scene' | 'game' | 'vfx' | undefined;
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
    t: 'foliage.inspect';
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
};
export declare const EditorQuerySchema: z.ZodType<EditorQuery>;
export type EditorCommand = {
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
    t: 'vfx.graph.set';
    path: string;
    name?: string | undefined;
    maxParticles?: number | undefined;
    maxLights?: number | undefined;
    maxAudioVoices?: number | undefined;
} | {
    t: 'vfx.node.add';
    path: string;
    nodeType: 'particle' | 'trail' | 'meshTrail' | 'light' | 'audio' | 'decal' | 'delay' | 'subGraph' | 'forceField' | 'mesh' | 'screenEffect' | 'cameraShake' | 'timeScale' | 'wind' | 'reroute';
    name?: string | undefined;
    from?: number | undefined;
    assetPath?: string | undefined;
} | {
    t: 'vfx.node.duplicate';
    path: string;
    nodeId: number;
    name?: string | undefined;
    editorX?: number | undefined;
    editorY?: number | undefined;
} | {
    t: 'vfx.node.remove';
    path: string;
    nodeId: number;
} | {
    t: 'vfx.node.setEnabled';
    path: string;
    nodeId: number;
    enabled: boolean;
} | {
    t: 'vfx.node.setMetadata';
    path: string;
    nodeId: number;
    name?: string | undefined;
    editorX?: number | undefined;
    editorY?: number | undefined;
} | {
    t: 'vfx.node.setParent';
    path: string;
    nodeId: number;
    parentNodeId?: number | undefined;
} | {
    t: 'vfx.node.setField';
    path: string;
    nodeId: number;
    schemaPath: string;
    value: JsonValue;
} | {
    t: 'vfx.link.add';
    path: string;
    from: number;
    to: number;
    trigger?: 'onComplete' | 'onStart' | 'onCollision' | 'onDeath' | undefined;
    delay?: number | undefined;
} | {
    t: 'vfx.link.update';
    path: string;
    index: number;
    from?: number | undefined;
    to?: number | undefined;
    trigger?: 'onComplete' | 'onStart' | 'onCollision' | 'onDeath' | undefined;
    delay?: number | undefined;
} | {
    t: 'vfx.link.remove';
    path: string;
    index: number;
} | {
    t: 'vfx.param.declare';
    path: string;
    name: string;
    paramType: 'float' | 'int' | 'bool' | 'color' | 'vector3' | 'asset';
    defaultValue: JsonValue;
    minimum?: number | undefined;
    maximum?: number | undefined;
} | {
    t: 'vfx.param.remove';
    path: string;
    name: string;
} | {
    t: 'vfx.param.bind';
    path: string;
    name: string;
    nodeId: number;
    schemaPath: string;
} | {
    t: 'vfx.param.unbind';
    path: string;
    name: string;
    nodeId?: number | undefined;
    schemaPath?: string | undefined;
} | {
    t: 'vfx.param.setDefault';
    path: string;
    name: string;
    value: JsonValue;
} | {
    t: 'vfx.instance.set';
    id: string;
    name: string;
    value: JsonValue;
} | {
    t: 'vfx.instance.clear';
    id: string;
    name: string;
} | {
    t: 'vfx.template.apply';
    template: 'explosion' | 'fire' | 'smoke' | 'impact' | 'magic' | string;
    path: string;
    name?: string | undefined;
    description?: string | undefined;
    tags?: string[] | undefined;
    mode?: 'replace' | 'merge' | 'subgraph' | undefined;
    groups?: number[] | undefined;
    anchorNodeId?: number | undefined;
    trigger?: 'onComplete' | 'onStart' | 'onCollision' | 'onDeath' | undefined;
    delay?: number | undefined;
    parentNodeId?: number | undefined;
    variant?: string | undefined;
    raiseBudget?: boolean | undefined;
} | {
    t: 'vfx.optimize';
    path: string;
    targetParticles?: number | undefined;
    strategy?: 'particles' | 'fillRate' | 'both' | undefined;
} | {
    t: 'vfx.repair';
    path: string;
    connectOrphans?: boolean | undefined;
    fixAssets?: boolean | undefined;
    fixSprites?: boolean | undefined;
    fixLighting?: boolean | undefined;
    fixSorting?: boolean | undefined;
    fixMeshFade?: boolean | undefined;
    fixParents?: boolean | undefined;
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
    t: 'vfx.variant.upsert';
    path: string;
    name: string;
    values: {
        [key: string]: JsonValue;
    };
} | {
    t: 'vfx.variant.remove';
    path: string;
    name: string;
} | {
    t: 'vfx.group.add';
    path: string;
    title?: string | undefined;
    note?: string | undefined;
    x?: number | undefined;
    y?: number | undefined;
    width?: number | undefined;
    height?: number | undefined;
    color?: [number, number, number, number] | undefined;
} | {
    t: 'vfx.group.update';
    path: string;
    groupId: number;
    title?: string | undefined;
    note?: string | undefined;
    x?: number | undefined;
    y?: number | undefined;
    width?: number | undefined;
    height?: number | undefined;
    color?: [number, number, number, number] | undefined;
} | {
    t: 'vfx.group.remove';
    path: string;
    groupId: number;
} | {
    t: 'vfx.generateMotionVectors';
    texturePath: string;
    columns: number;
    rows: number;
    searchRadius?: number | undefined;
    loop?: boolean | undefined;
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
    t: 'foliage.scatter';
    id: string;
    species: number;
    position: [number, number, number];
    radius?: number | undefined;
    count?: number | undefined;
    maxSlopeDegrees?: number | undefined;
    seed?: number | undefined;
} | {
    t: 'foliage.clear';
    id: string;
    species: number;
    position?: [number, number, number] | undefined;
    radius?: number | undefined;
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
        vfx: "vfx";
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
