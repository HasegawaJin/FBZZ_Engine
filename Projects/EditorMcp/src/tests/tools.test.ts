// FBZZ Engine
// tools.test.ts | EditorMcp
// MCP 公開面、dryRun 強制、画像応答を in-memory transport で検証する
import assert from 'node:assert/strict';
import test from 'node:test';
import { Client } from '@modelcontextprotocol/sdk/client/index.js';
import { InMemoryTransport } from '@modelcontextprotocol/sdk/inMemory.js';
import { CallToolResultSchema } from '@modelcontextprotocol/sdk/types.js';
import type { EditorBus } from '../busClient.js';
import type { EditorCommand, EditorQuery } from '../editorContracts.js';
import { CreateEditorMcpServer } from '../server.js';
import type { PermissionMode } from '../config.js';

// engine を模した bus。query/command を記録し、viewport.capture だけ画像応答を返す。
class FakeEditorBus implements EditorBus {
    queries: EditorQuery[] = [];
    commands: Array<{ command: EditorCommand; dryRun: boolean }> = [];

    async Query(query: EditorQuery): Promise<unknown> {
        this.queries.push(query);
        if (query.t === 'viewport.capture') {
            return {
                mimeType: 'image/png',
                base64: 'iVBORw0KGgo=',
                width: query.w,
                height: query.h,
                view: query.view ?? 'scene',
            };
        }
        if (query.t === 'viewport.semantic') {
            return {
                mimeType: 'image/png',
                base64: 'iVBORw0KGgo=',
                width: query.w,
                height: query.h,
                view: query.view ?? 'scene',
                cameraPosition: [0, 2, -8],
                objects: [{
                    id: '11111111-1111-4111-8111-111111111111',
                    name: 'Cube',
                    worldPosition: [0, 0, 0],
                    visible: true,
                    depth: 0.5,
                    pixel: [query.w / 2, query.h / 2],
                }],
            };
        }
        if (query.t === 'editor.state') {
            return {
                playState: 'editor', restorePending: false, scriptReloadBusy: false,
                scene: 'Assets/Scenes/Test.scene', frameIndex: 10,
            };
        }
        if (query.t === 'console.logs') {
            return { entries: [], count: 0, cursor: 42, oldestSequence: 1, dropped: false };
        }
        if (query.t === 'profiler.snapshot') {
            return { fps: 60, frameMs: 16.67, drawCalls: 10, triangles: 1000 };
        }
        if (query.t === 'physics.events') {
            return { enter: [], stay: [], exit: [] };
        }
        return { query: query.t };
    }

    async Command(command: EditorCommand, dryRun: boolean): Promise<unknown> {
        this.commands.push({ command, dryRun });
        return { accepted: true, dryRun };
    }

    Close(): void {}
}

// 実 transport と同じ JSON-RPC 経路をメモリ内で通し、登録だけのテストにしない。
async function CreateHarness(permission: PermissionMode) {
    const bus = new FakeEditorBus();
    const server = CreateEditorMcpServer(bus, permission);
    const client = new Client({ name: 'fbzz-editor-mcp-test', version: '0.1.0' });
    const [clientTransport, serverTransport] = InMemoryTransport.createLinkedPair();
    await server.connect(serverTransport);
    await client.connect(clientTransport);
    return {
        bus,
        client,
        close: async (): Promise<void> => {
            await client.close();
            await server.close();
        },
    };
}

test('read mode は Query と capture だけを公開する', async () => {
    const harness = await CreateHarness('read');
    try {
        const tools = await harness.client.listTools();
        const names = tools.tools.map((tool) => tool.name);
        assert.equal(names.includes('editor_catalog'), true);
        assert.equal(names.includes('editor_get_state'), true);
        assert.equal(names.includes('editor_get_undo_history'), true);
        assert.equal(names.includes('console_get_logs'), true);
        assert.equal(names.includes('scene_find'), true);
        assert.equal(names.includes('scene_get_tree'), true);
        assert.equal(names.includes('viewport_capture'), true);
        assert.equal(names.includes('editor_perceive'), true);
        assert.equal(names.includes('physics_raycast'), true);
        assert.equal(names.includes('physics_overlap_sphere'), true);
        assert.equal(names.includes('viewport_capture_semantic'), true);
        assert.equal(names.includes('scene_snapshot'), true);
        assert.equal(names.includes('scene_diff'), true);
        assert.equal(names.includes('scene_validate'), true);
        assert.equal(names.includes('asset_inspect'), true);
        assert.equal(names.includes('vfx_inspect_graph'), true);
        assert.equal(names.includes('vfx_get_params'), true);
        assert.equal(names.includes('vfx_get_schema'), true);
        assert.equal(names.includes('vfx_preview'), true);
        assert.equal(names.includes('material_inspect'), true);
        assert.equal(names.includes('animation_get_state'), true);
        assert.equal(names.includes('animation_get_graph'), true);
        assert.equal(names.includes('animation_get_blend_tree'), true);
        assert.equal(names.includes('animation_get_pose'), true);
        assert.equal(names.includes('profiler_get_snapshot'), true);
        assert.equal(names.includes('editor_wait'), true);
        assert.equal(names.includes('node_create'), false);
        assert.equal(names.includes('node_set_active'), false);
        assert.equal(names.includes('node_set_tag'), false);
        assert.equal(names.includes('node_set_layer'), false);
        assert.equal(names.includes('animation_set_parameter'), false);
        assert.equal(names.includes('animation_add_transition'), false);
        assert.equal(names.includes('animation_set_condition'), false);
        assert.equal(names.includes('animation_add_state'), false);
        assert.equal(names.includes('animation_set_state'), false);
        assert.equal(names.includes('animation_add_motion'), false);
        assert.equal(names.includes('animation_set_motion'), false);
        assert.equal(names.includes('animation_remove_state'), false);
        assert.equal(names.includes('animation_remove_transition'), false);
        assert.equal(names.includes('animation_remove_motion'), false);
        assert.equal(names.includes('animation_add_parameter'), false);
        assert.equal(names.includes('animation_remove_parameter'), false);
        assert.equal(names.includes('run_transaction'), false);
        assert.equal(names.includes('playtest_run'), false);
    } finally {
        await harness.close();
    }
});

test('editor_catalog と scene_find は推測なしの発見Queryへ写像する', async () => {
    const harness = await CreateHarness('read');
    try {
        await harness.client.callTool({ name: 'editor_catalog', arguments: {} });
        await harness.client.callTool({
            name: 'scene_find',
            arguments: {
                name: 'player',
                components: ['CameraComponent'],
                properties: [{ comp: 'CameraComponent', field: 'fov', op: 'greater', value: 45 }],
                limit: 25,
            },
        });
        assert.deepEqual(harness.bus.queries, [
            { t: 'editor.catalog' },
            {
                t: 'scene.find',
                name: 'player',
                components: ['CameraComponent'],
                properties: [{ comp: 'CameraComponent', field: 'fov', op: 'greater', value: 45 }],
                limit: 25,
            },
        ]);
    } finally {
        await harness.close();
    }
});

test('vfx_inspect_graph は構造・event・budget Query へ写像する', async () => {
    const harness = await CreateHarness('read');
    try {
        await harness.client.callTool({
            name: 'vfx_inspect_graph',
            arguments: { path: 'Assets/VFX/Explosion.vfx' },
        });
        assert.deepEqual(harness.bus.queries, [
            { t: 'vfx.graph', path: 'Assets/VFX/Explosion.vfx' },
        ]);
    } finally {
        await harness.close();
    }
});

test('VFX parameter とauthoring schemaは共通Query面へ写像する', async () => {
    const harness = await CreateHarness('read');
    try {
        const id = '11111111-1111-4111-8111-111111111111';
        await harness.client.callTool({ name: 'vfx_get_params', arguments: { path: 'Assets/VFX/Explosion.vfx', id } });
        await harness.client.callTool({ name: 'vfx_get_schema', arguments: {} });
        assert.deepEqual(harness.bus.queries, [
            { t: 'vfx.params', path: 'Assets/VFX/Explosion.vfx', id },
            { t: 'vfx.schema' },
        ]);
    } finally {
        await harness.close();
    }
});

test('editor_perceive は Scene 状態と viewport image を1応答へまとめる', async () => {
    const harness = await CreateHarness('read');
    try {
        const rawResult = await harness.client.callTool({
            name: 'editor_perceive',
            arguments: { w: 640, h: 360 },
        });
        const result = CallToolResultSchema.parse(rawResult);
        assert.equal(result.isError, undefined);
        assert.equal(result.content.some((item) => item.type === 'text'), true);
        assert.equal(result.content.some((item) => item.type === 'image'), true);
        assert.deepEqual(harness.bus.queries, [
            { t: 'editor.state' },
            { t: 'scene.tree' },
            { t: 'scene.selection' },
            { t: 'console.logs', minLevel: 'warning', limit: 50 },
            { t: 'viewport.capture', w: 640, h: 360 },
        ]);
    } finally {
        await harness.close();
    }
});

test('dry-run mode は Command を公開し dryRun=true を強制する', async () => {
    const harness = await CreateHarness('dry-run');
    try {
        const result = await harness.client.callTool({
            name: 'node_create',
            arguments: { name: 'AI Cube' },
        });
        assert.equal(result.isError, undefined);
        assert.equal(harness.bus.commands.length, 1);
        assert.deepEqual(harness.bus.commands[0], {
            command: { t: 'node.create', name: 'AI Cube' },
            dryRun: true,
        });
    } finally {
        await harness.close();
    }
});

test('VFX編集ツールは型付きUndo Commandへ写像する', async () => {
    const harness = await CreateHarness('write');
    try {
        const path = 'Assets/VFX/Explosion.vfx';
        const id = '11111111-1111-4111-8111-111111111111';
        await harness.client.callTool({ name: 'vfx_node_add', arguments: { path, nodeType: 'particle', name: 'Smoke', from: 1 } });
        await harness.client.callTool({ name: 'vfx_node_set_field', arguments: { path, nodeId: 2, schemaPath: 'particle.emitRate', value: 80 } });
        await harness.client.callTool({ name: 'vfx_param_declare', arguments: { path, name: 'Intensity', paramType: 'float', defaultValue: 1, minimum: 0, maximum: 4 } });
        await harness.client.callTool({ name: 'vfx_param_bind', arguments: { path, name: 'Intensity', nodeId: 2, schemaPath: 'particle.emitRate' } });
        await harness.client.callTool({ name: 'vfx_instance_set', arguments: { id, name: 'Intensity', value: 2 } });
        await harness.client.callTool({ name: 'vfx_template_apply', arguments: { template: 'explosion', path: 'Assets/VFX/NewExplosion.vfx', name: 'New Explosion' } });
        await harness.client.callTool({ name: 'vfx_optimize_budget', arguments: { path, targetParticles: 5000 } });
        await harness.client.callTool({ name: 'vfx_variant_upsert', arguments: { path, name: 'Large', values: { Intensity: 3 } } });
        assert.deepEqual(harness.bus.commands, [
            { command: { t: 'vfx.node.add', path, nodeType: 'particle', name: 'Smoke', from: 1 }, dryRun: false },
            { command: { t: 'vfx.node.setField', path, nodeId: 2, schemaPath: 'particle.emitRate', value: 80 }, dryRun: false },
            { command: { t: 'vfx.param.declare', path, name: 'Intensity', paramType: 'float', defaultValue: 1, minimum: 0, maximum: 4 }, dryRun: false },
            { command: { t: 'vfx.param.bind', path, name: 'Intensity', nodeId: 2, schemaPath: 'particle.emitRate' }, dryRun: false },
            { command: { t: 'vfx.instance.set', id, name: 'Intensity', value: 2 }, dryRun: false },
            { command: { t: 'vfx.template.apply', template: 'explosion', path: 'Assets/VFX/NewExplosion.vfx', name: 'New Explosion' }, dryRun: false },
            { command: { t: 'vfx.optimize', path, targetParticles: 5000 }, dryRun: false },
            { command: { t: 'vfx.variant.upsert', path, name: 'Large', values: { Intensity: 3 } }, dryRun: false },
        ]);
    } finally {
        await harness.close();
    }
});

test('node_duplicate は親・名前を保ったUndo対応Commandへ写像する', async () => {
    const harness = await CreateHarness('write');
    try {
        const id = '11111111-1111-4111-8111-111111111111';
        const parent = '22222222-2222-4222-8222-222222222222';
        await harness.client.callTool({
            name: 'node_duplicate',
            arguments: { id, parent, name: 'Player Copy' },
        });
        assert.deepEqual(harness.bus.commands[0], {
            command: { t: 'node.duplicate', id, parent, name: 'Player Copy' },
            dryRun: false,
        });
    } finally {
        await harness.close();
    }
});

test('GameObjectプロパティ設定 (active/tag/layer) は専用 Editor Command へ写像する', async () => {
    const harness = await CreateHarness('write');
    try {
        const id = '11111111-1111-4111-8111-111111111111';
        await harness.client.callTool({ name: 'node_set_active', arguments: { id, active: false } });
        await harness.client.callTool({ name: 'node_set_tag', arguments: { id, tag: 'Enemy' } });
        await harness.client.callTool({ name: 'node_set_layer', arguments: { id, layer: 8 } });
        assert.deepEqual(harness.bus.commands, [
            { command: { t: 'node.setActive', id, active: false }, dryRun: false },
            { command: { t: 'node.setTag', id, tag: 'Enemy' }, dryRun: false },
            { command: { t: 'node.setLayer', id, layer: 8 }, dryRun: false },
        ]);
    } finally {
        await harness.close();
    }
});

test('Play・Prefab・カメラ操作は専用 Editor Command へ写像する', async () => {
    const harness = await CreateHarness('write');
    try {
        const parent = '22222222-2222-4222-8222-222222222222';
        const targetId = '33333333-3333-4333-8333-333333333333';
        await harness.client.callTool({ name: 'play_control', arguments: { action: 'start' } });
        await harness.client.callTool({
            name: 'prefab_instantiate',
            arguments: { path: 'Assets/Prefabs/Enemy.prefab', parent },
        });
        await harness.client.callTool({
            name: 'viewport_camera_set',
            arguments: { position: [1, 2, 3], targetId },
        });
        assert.deepEqual(harness.bus.commands, [
            { command: { t: 'play.control', action: 'start' }, dryRun: false },
            { command: { t: 'prefab.instantiate', path: 'Assets/Prefabs/Enemy.prefab', parent }, dryRun: false },
            { command: { t: 'viewport.camera', position: [1, 2, 3], targetId }, dryRun: false },
        ]);
    } finally {
        await harness.close();
    }
});

test('playtest_run は今回のログ・性能・意味付き画像を評価し、入力状態を必ず解除する', async () => {
    const harness = await CreateHarness('write');
    try {
        const visibleId = '11111111-1111-4111-8111-111111111111';
        const rawResult = await harness.client.callTool({
            name: 'playtest_run',
            arguments: {
                startPlay: false,
                settleMs: 0,
                steps: [{ delayMs: 0, input: { kind: 'key', key: 'space', pressed: true } }],
                assertions: { noErrors: true, minFps: 30, visibleNodeIds: [visibleId] },
                w: 640,
                h: 360,
            },
        });
        const result = CallToolResultSchema.parse(rawResult);
        assert.equal(result.isError, undefined);
        assert.equal(result.content.some((item) => item.type === 'image'), true);
        assert.equal((result.structuredContent as Record<string, unknown>).passed, true);
        assert.deepEqual(harness.bus.commands, [
            { command: { t: 'input.inject', kind: 'key', key: 'space', pressed: true }, dryRun: false },
            { command: { t: 'input.inject', kind: 'clear' }, dryRun: false },
        ]);
        assert.equal(harness.bus.queries.some((query) => query.t === 'console.logs'
            && query.afterSequence === 42), true);
        assert.equal(harness.bus.queries.some((query) => query.t === 'viewport.semantic'
            && query.view === 'game'), true);
    } finally {
        await harness.close();
    }
});

test('playtest_run dry-run は待機せず全入力と復元Commandを検証する', async () => {
    const harness = await CreateHarness('dry-run');
    try {
        const result = await harness.client.callTool({
            name: 'playtest_run',
            arguments: {
                steps: [
                    { delayMs: 5000, input: { kind: 'axis', axis: 'Horizontal', value: 1 } },
                ],
                settleMs: 5000,
            },
        });
        assert.equal(result.isError, undefined);
        assert.deepEqual(harness.bus.commands, [
            { command: { t: 'play.control', action: 'start' }, dryRun: true },
            { command: { t: 'input.inject', kind: 'axis', axis: 'Horizontal', value: 1 }, dryRun: true },
            { command: { t: 'input.inject', kind: 'clear' }, dryRun: true },
            { command: { t: 'play.control', action: 'stop' }, dryRun: true },
        ]);
    } finally {
        await harness.close();
    }
});

test('viewport_capture は engine の PNG を MCP image content に変換する', async () => {
    const harness = await CreateHarness('read');
    try {
        const rawResult = await harness.client.callTool({
            name: 'viewport_capture',
            arguments: {},
        });
        const result = CallToolResultSchema.parse(rawResult);
        assert.equal(result.isError, undefined);
        assert.equal(result.content[0]?.type, 'image');
        if (result.content[0]?.type === 'image') {
            assert.equal(result.content[0].mimeType, 'image/png');
            assert.equal(result.content[0].data, 'iVBORw0KGgo=');
        }
        assert.deepEqual(harness.bus.queries[0], { t: 'viewport.capture', w: 960, h: 540, view: 'scene' });
    } finally {
        await harness.close();
    }
});

test('viewport_capture は装飾なし VFX Preview RT を選択できる', async () => {
    const harness = await CreateHarness('read');
    try {
        const rawResult = await harness.client.callTool({
            name: 'viewport_capture',
            arguments: { view: 'vfx', w: 640, h: 360 },
        });
        const result = CallToolResultSchema.parse(rawResult);
        assert.equal(result.isError, undefined);
        assert.deepEqual(harness.bus.queries[0], {
            t: 'viewport.capture', w: 640, h: 360, view: 'vfx',
        });
    } finally {
        await harness.close();
    }
});

test('viewport_capture_semantic は画像とNodeId付き投影座標を同時に返す', async () => {
    const harness = await CreateHarness('read');
    try {
        const rawResult = await harness.client.callTool({
            name: 'viewport_capture_semantic',
            arguments: { w: 640, h: 360, visibleOnly: true },
        });
        const result = CallToolResultSchema.parse(rawResult);
        assert.equal(result.isError, undefined);
        assert.equal(result.content.some((item) => item.type === 'image'), true);
        assert.equal(result.content.some((item) => item.type === 'text'), true);
        assert.deepEqual(harness.bus.queries[0], { t: 'viewport.semantic', w: 640, h: 360, view: 'scene' });
    } finally {
        await harness.close();
    }
});

test('入力・マテリアル・アニメーション操作は専用Commandへ写像する', async () => {
    const harness = await CreateHarness('write');
    try {
        const id = '11111111-1111-4111-8111-111111111111';
        await harness.client.callTool({
            name: 'input_inject', arguments: { kind: 'key', key: 'space', pressed: true },
        });
        await harness.client.callTool({
            name: 'material_set_parameter', arguments: { id, parameter: 'Roughness', value: [0.25] },
        });
        await harness.client.callTool({
            name: 'animation_control', arguments: { id, action: 'seek', frame: 24 },
        });
        await harness.client.callTool({
            name: 'animation_set_parameter', arguments: { id, name: 'Speed', value: 1.5 },
        });
        await harness.client.callTool({
            name: 'animation_set_parameter', arguments: { id, name: 'Jump' },
        });
        await harness.client.callTool({
            name: 'animation_add_transition', arguments: { id, from: 'Idle', to: 'Run', transitionDuration: 0.2 },
        });
        await harness.client.callTool({
            name: 'animation_set_condition',
            arguments: { id, from: 'Idle', transitionIndex: 0, action: 'add', parameter: 'Speed', op: 'greater', threshold: 0.1 },
        });
        await harness.client.callTool({
            name: 'animation_add_state', arguments: { id, name: 'Run', mode: 'blendTree1D', blendParameter: 'Speed', setAsDefault: false },
        });
        await harness.client.callTool({
            name: 'animation_set_state', arguments: { id, state: 'Run', name: 'Locomotion', speed: 1.2 },
        });
        await harness.client.callTool({
            name: 'animation_add_motion', arguments: { id, state: 'Locomotion', clipName: 'Walk', threshold: 0 },
        });
        await harness.client.callTool({
            name: 'animation_set_motion', arguments: { id, state: 'Locomotion', motionIndex: 0, threshold: 2.5 },
        });
        await harness.client.callTool({
            name: 'animation_add_parameter', arguments: { id, name: 'Grounded', type: 'bool', value: true },
        });
        await harness.client.callTool({
            name: 'animation_remove_motion', arguments: { id, state: 'Locomotion', motionIndex: 0 },
        });
        await harness.client.callTool({
            name: 'animation_remove_transition', arguments: { id, from: 'Idle', transitionIndex: 0 },
        });
        await harness.client.callTool({
            name: 'animation_remove_state', arguments: { id, state: 'Locomotion' },
        });
        await harness.client.callTool({
            name: 'animation_remove_parameter', arguments: { id, name: 'Grounded' },
        });
        assert.deepEqual(harness.bus.commands, [
            { command: { t: 'input.inject', kind: 'key', key: 'space', pressed: true }, dryRun: false },
            { command: { t: 'material.override', id, parameter: 'Roughness', value: [0.25] }, dryRun: false },
            { command: { t: 'animation.control', id, action: 'seek', frame: 24 }, dryRun: false },
            { command: { t: 'animation.setParameter', id, name: 'Speed', value: 1.5 }, dryRun: false },
            { command: { t: 'animation.setParameter', id, name: 'Jump' }, dryRun: false },
            { command: { t: 'animation.addTransition', id, to: 'Run', from: 'Idle', transitionDuration: 0.2 }, dryRun: false },
            { command: { t: 'animation.setCondition', id, transitionIndex: 0, action: 'add', from: 'Idle', parameter: 'Speed', op: 'greater', threshold: 0.1 }, dryRun: false },
            { command: { t: 'animation.addState', id, name: 'Run', mode: 'blendTree1D', blendParameter: 'Speed', setAsDefault: false }, dryRun: false },
            { command: { t: 'animation.setState', id, state: 'Run', name: 'Locomotion', speed: 1.2 }, dryRun: false },
            { command: { t: 'animation.addMotion', id, state: 'Locomotion', clipName: 'Walk', threshold: 0 }, dryRun: false },
            { command: { t: 'animation.setMotion', id, state: 'Locomotion', motionIndex: 0, threshold: 2.5 }, dryRun: false },
            { command: { t: 'animation.addParameter', id, name: 'Grounded', type: 'bool', value: true }, dryRun: false },
            { command: { t: 'animation.removeMotion', id, state: 'Locomotion', motionIndex: 0 }, dryRun: false },
            { command: { t: 'animation.removeTransition', id, transitionIndex: 0, from: 'Idle' }, dryRun: false },
            { command: { t: 'animation.removeState', id, state: 'Locomotion' }, dryRun: false },
            { command: { t: 'animation.removeParameter', id, name: 'Grounded' }, dryRun: false },
        ]);
    } finally {
        await harness.close();
    }
});
