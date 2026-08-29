// FBZZ Engine
// tools.test.ts | EditorMcp
// MCP 公開面、dryRun 強制、画像応答を in-memory transport で検証する
import assert from 'node:assert/strict';
import test from 'node:test';
import { Client } from '@modelcontextprotocol/sdk/client/index.js';
import { InMemoryTransport } from '@modelcontextprotocol/sdk/inMemory.js';
import { CallToolResultSchema } from '@modelcontextprotocol/sdk/types.js';
import { CreateEditorMcpServer } from '../server.js';
// engine を模した bus。query/command を記録し、viewport.capture だけ画像応答を返す。
class FakeEditorBus {
    queries = [];
    commands = [];
    async Query(query) {
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
    async Command(command, dryRun) {
        this.commands.push({ command, dryRun });
        return { accepted: true, dryRun };
    }
    Close() { }
}
// 実 transport と同じ JSON-RPC 経路をメモリ内で通し、登録だけのテストにしない。
async function CreateHarness(permission) {
    const bus = new FakeEditorBus();
    const server = CreateEditorMcpServer(bus, permission);
    const client = new Client({ name: 'fbzz-editor-mcp-test', version: '0.1.0' });
    const [clientTransport, serverTransport] = InMemoryTransport.createLinkedPair();
    await server.connect(serverTransport);
    await client.connect(clientTransport);
    return {
        bus,
        client,
        close: async () => {
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
        // 実行状態の読み出しは診断専用の読み取りツール。read モードでも使えないと、
        // 「なぜ出ないか」の切り分けが write 権限を持つ場面でしかできなくなる。
        // 素材解析は読み取りのみ。設定を決める前段なので read モードでも必ず使える。
        assert.equal(names.includes('shader_inspect'), true);
        assert.equal(names.includes('shader_get_compile_diagnostics'), true);
        assert.equal(names.includes('material_inspect'), true);
        assert.equal(names.includes('animation_get_state'), true);
        assert.equal(names.includes('animation_get_graph'), true);
        assert.equal(names.includes('animation_get_blend_tree'), true);
        assert.equal(names.includes('animation_get_pose'), true);
        assert.equal(names.includes('profiler_get_snapshot'), true);
        assert.equal(names.includes('editor_wait'), true);
        // ワールドオーサリングの照会も read で使える。地形・植生・NavMesh・環境・UI・ビルド結果は
        // 「壊れている理由」を探す側の情報なので、write 権限が無いと診断できない状態にはしない。
        assert.equal(names.includes('scene_list'), true);
        assert.equal(names.includes('preset_catalog'), true);
        assert.equal(names.includes('terrain_inspect'), true);
        assert.equal(names.includes('terrain_sample'), true);
        assert.equal(names.includes('navmesh_get_state'), true);
        assert.equal(names.includes('navmesh_find_path'), true);
        assert.equal(names.includes('navmesh_sample'), true);
        assert.equal(names.includes('environment_inspect'), true);
        assert.equal(names.includes('audio_inspect'), true);
        assert.equal(names.includes('ui_inspect'), true);
        assert.equal(names.includes('build_get_status'), true);
        // Operator の目録は読み取りのみ。「何ができるか」を知る手段が write 権限に
        // 縛られていると、read で疎通確認している段階では操作面が一切見えない。
        assert.equal(names.includes('editor_op_list'), true);
        // kind=query の Operator を読むゲートウェイも read で見える。
        // WHY: これが write 側にあると、read 権限では「目録には出るのに
        //      1 つも呼べない Query」を見ることになる。
        assert.equal(names.includes('editor_op_query'), true);
        // 実行はシーンを変えうるので read では発見不能。
        assert.equal(names.includes('editor_op_invoke'), false);
        assert.equal(names.includes('node_create'), false);
        // ワールドを書き換える側は read では発見不能。
        assert.equal(names.includes('preset_create'), false);
        assert.equal(names.includes('scene_open'), false);
        assert.equal(names.includes('scene_save'), false);
        assert.equal(names.includes('terrain_sculpt'), false);
        assert.equal(names.includes('terrain_paint'), false);
        assert.equal(names.includes('navmesh_bake'), false);
        assert.equal(names.includes('audio_control'), false);
        assert.equal(names.includes('build_run'), false);
        // 親子付けはアセットを書き換えるので read モードには出さない。
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
    }
    finally {
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
    }
    finally {
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
    }
    finally {
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
    }
    finally {
        await harness.close();
    }
});
test('Operator ゲートウェイは目録 Query と実行 Command へ写像する', async () => {
    const harness = await CreateHarness('write');
    try {
        await harness.client.callTool({
            name: 'editor_op_list',
            arguments: { category: 'Gizmo', includeUnavailable: false },
        });
        assert.deepEqual(harness.bus.queries.at(-1), {
            t: 'editor.op.list', category: 'Gizmo', includeUnavailable: false,
        });
        // 引数なしの操作は id だけで実行できる。
        await harness.client.callTool({ name: 'editor_op_invoke', arguments: { id: 'scene.save' } });
        assert.deepEqual(harness.bus.commands.at(-1), {
            command: { t: 'editor.op.invoke', id: 'scene.save' },
            dryRun: false,
        });
        // args は素通しする。中身の検証は Editor 側の params 宣言が持つので、
        // 操作が増えても MCP 側のスキーマを直す必要がない。
        await harness.client.callTool({
            name: 'editor_op_invoke',
            arguments: { id: 'node.set_tag', args: { id: '11111111-1111-4111-8111-111111111111', tag: 'Enemy' } },
        });
        assert.deepEqual(harness.bus.commands.at(-1), {
            command: {
                t: 'editor.op.invoke',
                id: 'node.set_tag',
                args: { id: '11111111-1111-4111-8111-111111111111', tag: 'Enemy' },
            },
            dryRun: false,
        });
        // Query 側は Command ではなく Query として流れる (read 権限でも通る経路)。
        await harness.client.callTool({
            name: 'editor_op_query',
            arguments: { id: 'panel.list' },
        });
        assert.deepEqual(harness.bus.queries.at(-1), { t: 'editor.op.query', id: 'panel.list' });
    }
    finally {
        await harness.close();
    }
});
test('Behavior Tree の照会・規約・検証は引数の少ない Query 面へ写像する', async () => {
    const harness = await CreateHarness('read');
    try {
        const path = 'Assets/AI/Guard.behaviortree';
        await harness.client.callTool({ name: 'bt_inspect_tree', arguments: { path } });
        await harness.client.callTool({ name: 'bt_lint', arguments: { path } });
        await harness.client.callTool({ name: 'bt_guide', arguments: {} });
        assert.deepEqual(harness.bus.queries, [
            { t: 'bt.tree', path },
            { t: 'bt.lint', path },
            { t: 'bt.guide' },
        ]);
    }
    finally {
        await harness.close();
    }
});
test('Behavior Tree の編集はUndo可能なCommandへ写像する', async () => {
    const harness = await CreateHarness('write');
    try {
        const path = 'Assets/AI/Guard.behaviortree';
        await harness.client.callTool({ name: 'bt_node_add', arguments: { path, nodeType: 'Selector' } });
        await harness.client.callTool({ name: 'bt_node_add', arguments: { path, nodeType: 'Sequence', parentId: 1, name: 'Combat' } });
        await harness.client.callTool({ name: 'bt_node_set_order', arguments: { path, nodeId: 2, order: 0 } });
        await harness.client.callTool({ name: 'bt_blackboard_add', arguments: { path, name: 'AlertLevel', type: 'int' } });
        await harness.client.callTool({ name: 'bt_node_set_field', arguments: { path, nodeId: 2, field: 'abortMode', value: 'lowerPriority' } });
        await harness.client.callTool({ name: 'bt_node_set_parent', arguments: { path, nodeId: 2, parentId: 1 } });
        await harness.client.callTool({ name: 'bt_auto_layout', arguments: { path } });
        await harness.client.callTool({ name: 'bt_node_remove', arguments: { path, nodeId: 2 } });
        assert.deepEqual(harness.bus.commands, [
            { command: { t: 'bt.node.add', path, nodeType: 'Selector' }, dryRun: false },
            { command: { t: 'bt.node.add', path, nodeType: 'Sequence', parentId: 1, name: 'Combat' }, dryRun: false },
            { command: { t: 'bt.node.setOrder', path, nodeId: 2, order: 0 }, dryRun: false },
            { command: { t: 'bt.blackboard.add', path, name: 'AlertLevel', type: 'int' }, dryRun: false },
            { command: { t: 'bt.node.setField', path, nodeId: 2, field: 'abortMode', value: 'lowerPriority' }, dryRun: false },
            { command: { t: 'bt.node.setParent', path, nodeId: 2, parentId: 1 }, dryRun: false },
            { command: { t: 'bt.autoLayout', path }, dryRun: false },
            { command: { t: 'bt.node.remove', path, nodeId: 2 }, dryRun: false },
        ]);
    }
    finally {
        await harness.close();
    }
});
test('Behavior Tree のスキーマ・現在値・実行状態・差分は読み取り面へ写像する', async () => {
    const harness = await CreateHarness('read');
    try {
        const path = 'Assets/AI/Guard.behaviortree';
        await harness.client.callTool({ name: 'bt_schema', arguments: {} });
        await harness.client.callTool({ name: 'bt_schema', arguments: { nodeType: 'MoveTo' } });
        // field 省略はその種別が読むフィールドを全部返す読み方。
        await harness.client.callTool({ name: 'bt_node_get_field', arguments: { path, nodeId: 4 } });
        await harness.client.callTool({
            name: 'bt_node_get_field', arguments: { path, nodeId: 4, field: 'moveTargetKey' },
        });
        await harness.client.callTool({ name: 'bt_runtime_state', arguments: {} });
        await harness.client.callTool({ name: 'bt_runtime_state', arguments: { path } });
        await harness.client.callTool({
            name: 'bt_diff', arguments: { base: path, target: 'Assets/AI/Guard.alt.behaviortree' },
        });
        await harness.client.callTool({ name: 'bt_template_catalog', arguments: {} });
        assert.deepEqual(harness.bus.queries, [
            { t: 'bt.schema' },
            { t: 'bt.schema', nodeType: 'MoveTo' },
            { t: 'bt.nodeField', path, nodeId: 4 },
            { t: 'bt.nodeField', path, nodeId: 4, field: 'moveTargetKey' },
            { t: 'bt.runtime' },
            { t: 'bt.runtime', path },
            { t: 'bt.diff', base: path, target: 'Assets/AI/Guard.alt.behaviortree' },
            { t: 'bt.templateCatalog' },
        ]);
    }
    finally {
        await harness.close();
    }
});
test('Behavior Tree の複製・修復・Template適用はUndo可能なCommandへ写像する', async () => {
    const harness = await CreateHarness('write');
    try {
        const path = 'Assets/AI/Guard.behaviortree';
        await harness.client.callTool({ name: 'bt_node_duplicate', arguments: { path, nodeId: 2 } });
        await harness.client.callTool({
            name: 'bt_node_duplicate', arguments: { path, nodeId: 2, parentId: 1 },
        });
        // フラグ省略は「全部直す」。個別に false を渡したときだけその修復を止める。
        await harness.client.callTool({ name: 'bt_repair', arguments: { path } });
        await harness.client.callTool({ name: 'bt_repair', arguments: { path, fixKeys: false } });
        await harness.client.callTool({
            name: 'bt_template_apply',
            arguments: { template: 'GuardPatrol', path: 'Assets/AI/Sentry.behaviortree', name: 'Sentry' },
        });
        assert.deepEqual(harness.bus.commands, [
            { command: { t: 'bt.node.duplicate', path, nodeId: 2 }, dryRun: false },
            { command: { t: 'bt.node.duplicate', path, nodeId: 2, parentId: 1 }, dryRun: false },
            { command: { t: 'bt.repair', path }, dryRun: false },
            { command: { t: 'bt.repair', path, fixKeys: false }, dryRun: false },
            { command: {
                    t: 'bt.template.apply', template: 'GuardPatrol',
                    path: 'Assets/AI/Sentry.behaviortree', name: 'Sentry',
                }, dryRun: false },
        ]);
    }
    finally {
        await harness.close();
    }
});
test('scene_get_tree は既定で生成物を要求しない', async () => {
    // WHY: 既定で includeGenerated を送ってしまうと、C++ 側の既定 (除外) を上書きして
    //      AI の context へ実行時生成物が流れ込む。省略されることを固定する。
    const harness = await CreateHarness('read');
    try {
        await harness.client.callTool({ name: 'scene_get_tree', arguments: {} });
        await harness.client.callTool({ name: 'scene_get_tree', arguments: { includeGenerated: true } });
        assert.deepEqual(harness.bus.queries, [
            { t: 'scene.tree' },
            { t: 'scene.tree', includeGenerated: true },
        ]);
    }
    finally {
        await harness.close();
    }
});
test('vfx_generate_motion_vectors は任意引数を省略した形で写像する', async () => {
    const harness = await CreateHarness('write');
    try {
        const texturePath = 'Assets/Textures/Smoke_8x8.png';
        await harness.client.callTool({
            name: 'vfx_generate_motion_vectors',
            arguments: { texturePath, columns: 8, rows: 8 },
        });
        await harness.client.callTool({
            name: 'vfx_generate_motion_vectors',
            arguments: { texturePath, columns: 8, rows: 8, searchRadius: 12, loop: false },
        });
        assert.deepEqual(harness.bus.commands, [
            { command: { t: 'vfx.generateMotionVectors', texturePath, columns: 8, rows: 8 }, dryRun: false },
            { command: { t: 'vfx.generateMotionVectors', texturePath, columns: 8, rows: 8, searchRadius: 12, loop: false }, dryRun: false },
        ]);
    }
    finally {
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
    }
    finally {
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
    }
    finally {
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
    }
    finally {
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
        assert.equal(result.structuredContent.passed, true);
        assert.deepEqual(harness.bus.commands, [
            { command: { t: 'input.inject', kind: 'key', key: 'space', pressed: true }, dryRun: false },
            { command: { t: 'input.inject', kind: 'clear' }, dryRun: false },
        ]);
        assert.equal(harness.bus.queries.some((query) => query.t === 'console.logs'
            && query.afterSequence === 42), true);
        assert.equal(harness.bus.queries.some((query) => query.t === 'viewport.semantic'
            && query.view === 'game'), true);
    }
    finally {
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
    }
    finally {
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
    }
    finally {
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
    }
    finally {
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
    }
    finally {
        await harness.close();
    }
});
test('ワールドオーサリングの照会は引数の少ない Query 面へ写像する', async () => {
    const harness = await CreateHarness('read');
    try {
        await harness.client.callTool({ name: 'scene_list', arguments: {} });
        await harness.client.callTool({ name: 'preset_catalog', arguments: {} });
        await harness.client.callTool({ name: 'terrain_inspect', arguments: {} });
        // [x, z] の 2 要素で問い合わせられることを固定する。高さを問うのに y は要らないため。
        await harness.client.callTool({ name: 'terrain_sample', arguments: { points: [[10, 20], [1, 2, 3]] } });
        await harness.client.callTool({ name: 'navmesh_get_state', arguments: {} });
        await harness.client.callTool({ name: 'navmesh_find_path', arguments: { from: [0, 0, 0], to: [10, 0, 10] } });
        await harness.client.callTool({ name: 'navmesh_sample', arguments: { points: [[1, 2, 3]] } });
        await harness.client.callTool({ name: 'environment_inspect', arguments: {} });
        await harness.client.callTool({ name: 'audio_inspect', arguments: {} });
        await harness.client.callTool({ name: 'ui_inspect', arguments: {} });
        await harness.client.callTool({ name: 'build_get_status', arguments: {} });
        assert.deepEqual(harness.bus.queries, [
            { t: 'scene.list' },
            { t: 'preset.catalog' },
            { t: 'terrain.inspect' },
            { t: 'terrain.sample', points: [[10, 20], [1, 2, 3]] },
            { t: 'navmesh.state' },
            { t: 'navmesh.path', from: [0, 0, 0], to: [10, 0, 10] },
            { t: 'navmesh.sample', points: [[1, 2, 3]] },
            { t: 'environment.inspect' },
            { t: 'audio.inspect' },
            { t: 'ui.inspect' },
            { t: 'build.status', limit: 5 },
        ]);
    }
    finally {
        await harness.close();
    }
});
test('地形・NavMesh・シーン入出力は Undo 単位の Command へ写像する', async () => {
    const harness = await CreateHarness('write');
    try {
        const terrainId = '55555555-5555-4555-8555-555555555555';
        // 省略した任意引数は payload へ載せない (C++ 側の「未指定」判定と一致させる)。
        await harness.client.callTool({ name: 'preset_create', arguments: { preset: '3d.cube' } });
        await harness.client.callTool({
            name: 'preset_create',
            arguments: { preset: 'nav.aiAgent', parent: terrainId, name: 'Enemy', position: [1, 0, 2] },
        });
        await harness.client.callTool({
            name: 'terrain_sculpt',
            // WHY iterations を明示するか: smooth / flatten は 1 回では収束しない。
            //     既定 1 のまま送られると AI は同じ要求を何十回も投げ、Undo 履歴がそのぶん汚れる。
            arguments: { position: [5, 0, 5], op: 'flatten', radius: 8, iterations: 20, targetHeight: 2 },
        });
        await harness.client.callTool({
            name: 'terrain_paint',
            arguments: { position: [5, 0, 5], layer: 1, radius: 4 },
        });
        await harness.client.callTool({
            name: 'terrain_set_layer_material',
            arguments: { id: terrainId, layer: 1, material: 'Assets/Materials/Grass.mat' },
        });
        await harness.client.callTool({ name: 'navmesh_bake', arguments: {} });
        await harness.client.callTool({ name: 'audio_control', arguments: { id: terrainId, action: 'play' } });
        await harness.client.callTool({ name: 'scene_save', arguments: {} });
        await harness.client.callTool({
            name: 'scene_open',
            arguments: { path: 'Assets/Scenes/Game.scene', discardUnsaved: true },
        });
        await harness.client.callTool({ name: 'build_run', arguments: {} });
        assert.deepEqual(harness.bus.commands, [
            { command: { t: 'preset.create', preset: '3d.cube' }, dryRun: false },
            { command: { t: 'preset.create', preset: 'nav.aiAgent', parent: terrainId, name: 'Enemy', position: [1, 0, 2] }, dryRun: false },
            { command: { t: 'terrain.sculpt', position: [5, 0, 5], op: 'flatten', radius: 8, strength: 0.05, falloff: 'smooth', iterations: 20, targetHeight: 2 }, dryRun: false },
            { command: { t: 'terrain.paint', position: [5, 0, 5], layer: 1, radius: 4, strength: 0.5, falloff: 'smooth', iterations: 1 }, dryRun: false },
            { command: { t: 'terrain.setLayerMaterial', id: terrainId, layer: 1, material: 'Assets/Materials/Grass.mat' }, dryRun: false },
            { command: { t: 'navmesh.bake' }, dryRun: false },
            { command: { t: 'audio.control', id: terrainId, action: 'play' }, dryRun: false },
            { command: { t: 'scene.save' }, dryRun: false },
            { command: { t: 'scene.open', path: 'Assets/Scenes/Game.scene', discardUnsaved: true }, dryRun: false },
            { command: { t: 'build.run', target: 'script' }, dryRun: false },
        ]);
    }
    finally {
        await harness.close();
    }
});
//# sourceMappingURL=tools.test.js.map