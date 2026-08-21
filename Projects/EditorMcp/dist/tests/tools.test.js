// FBZZ Engine
// tools.test.ts | EditorMcp
// MCP 公開面、dryRun 強制、画像応答を in-memory transport で検証する
import assert from 'node:assert/strict';
import test from 'node:test';
import { Client } from '@modelcontextprotocol/sdk/client/index.js';
import { InMemoryTransport } from '@modelcontextprotocol/sdk/inMemory.js';
import { CallToolResultSchema } from '@modelcontextprotocol/sdk/types.js';
import { EditorCommandSchema } from '../editorContracts.js';
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
        if (query.t === 'vfx.lint') {
            return { errors: 0, warnings: 0, issues: [] };
        }
        if (query.t === 'vfx.preview') {
            return { duration: 1 };
        }
        if (query.t === 'vfx.previewMetrics') {
            return {
                exposure: { luminanceMean: 1, clippedRatio: 0 },
                occupancy: { coverage: 0.2 },
                motion: { meanLuminanceDelta: 0.1 },
                issues: [],
            };
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
        assert.equal(names.includes('vfx_inspect_graph'), true);
        assert.equal(names.includes('vfx_get_params'), true);
        assert.equal(names.includes('vfx_get_schema'), true);
        assert.equal(names.includes('vfx_preview'), true);
        // 実行状態の読み出しは診断専用の読み取りツール。read モードでも使えないと、
        // 「なぜ出ないか」の切り分けが write 権限を持つ場面でしかできなくなる。
        assert.equal(names.includes('vfx_runtime_state'), true);
        // 素材解析は読み取りのみ。設定を決める前段なので read モードでも必ず使える。
        assert.equal(names.includes('vfx_analyze_texture'), true);
        assert.equal(names.includes('vfx_analyze_material'), true);
        assert.equal(names.includes('vfx_survey_assets'), true);
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
        assert.equal(names.includes('foliage_inspect'), true);
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
        assert.equal(names.includes('foliage_scatter'), false);
        assert.equal(names.includes('navmesh_bake'), false);
        assert.equal(names.includes('audio_control'), false);
        assert.equal(names.includes('build_run'), false);
        // 親子付けはアセットを書き換えるので read モードには出さない。
        assert.equal(names.includes('vfx_node_set_parent'), false);
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
test('vfx_inspect_graph は構造・event・budget Query へ写像し、既定は summary', async () => {
    // WHY: 既定を summary に固定する。ここが full へ戻ると、lint で問題なしと判った
    //      グラフを一覧するだけで transform と editorPosition が応答の大半を占める。
    const harness = await CreateHarness('read');
    try {
        await harness.client.callTool({
            name: 'vfx_inspect_graph',
            arguments: { path: 'Assets/VFX/Explosion.vfx' },
        });
        await harness.client.callTool({
            name: 'vfx_inspect_graph',
            arguments: { path: 'Assets/VFX/Explosion.vfx', detail: 'full' },
        });
        assert.deepEqual(harness.bus.queries, [
            { t: 'vfx.graph', path: 'Assets/VFX/Explosion.vfx', detail: 'summary' },
            { t: 'vfx.graph', path: 'Assets/VFX/Explosion.vfx', detail: 'full' },
        ]);
    }
    finally {
        await harness.close();
    }
});
test('vfx_node_get_field は setField の対になる読み出し Query へ写像する', async () => {
    // WHY: schemaPath / prefix は「省略 = 全 leaf」なので、省略時にキーごと落ちることを固定する。
    const harness = await CreateHarness('read');
    try {
        const path = 'Assets/VFX/Explosion.vfx';
        await harness.client.callTool({
            name: 'vfx_node_get_field', arguments: { path, nodeId: 2, schemaPath: 'particle.blendMode' },
        });
        await harness.client.callTool({
            name: 'vfx_node_get_field', arguments: { path, nodeId: 2, prefix: 'particle.' },
        });
        await harness.client.callTool({ name: 'vfx_node_get_field', arguments: { path, nodeId: 2 } });
        assert.deepEqual(harness.bus.queries, [
            { t: 'vfx.nodeField', path, nodeId: 2, schemaPath: 'particle.blendMode' },
            { t: 'vfx.nodeField', path, nodeId: 2, prefix: 'particle.' },
            { t: 'vfx.nodeField', path, nodeId: 2 },
        ]);
    }
    finally {
        await harness.close();
    }
});
test('vfx_preview_ensure は Preview World の起動確認 Query へ写像する', async () => {
    // WHY: preview 系は全て Preview World を前提にするが、それを作る手段が無いと
    //      vfx_guide が勧める previewMetrics / previewCurve が原理的に実行できない。
    const harness = await CreateHarness('read');
    try {
        await harness.client.callTool({ name: 'vfx_preview_ensure', arguments: {} });
        assert.deepEqual(harness.bus.queries, [{ t: 'vfx.previewEnsure' }]);
    }
    finally {
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
test('VFX編集ツールは型付きUndo Commandへ写像する', async () => {
    const harness = await CreateHarness('write');
    try {
        const path = 'Assets/VFX/Explosion.vfx';
        const id = '11111111-1111-4111-8111-111111111111';
        await harness.client.callTool({ name: 'vfx_node_add', arguments: { path, nodeType: 'particle', name: 'Smoke', from: 1 } });
        await harness.client.callTool({ name: 'vfx_node_set_enabled', arguments: { path, nodeId: 2, enabled: false } });
        await harness.client.callTool({ name: 'vfx_node_set_field', arguments: { path, nodeId: 2, schemaPath: 'particle.emitRate', value: 80 } });
        await harness.client.callTool({ name: 'vfx_param_declare', arguments: { path, name: 'Intensity', paramType: 'float', defaultValue: 1, minimum: 0, maximum: 4 } });
        await harness.client.callTool({ name: 'vfx_param_bind', arguments: { path, name: 'Intensity', nodeId: 2, schemaPath: 'particle.emitRate' } });
        await harness.client.callTool({ name: 'vfx_instance_set', arguments: { id, name: 'Intensity', value: 2 } });
        await harness.client.callTool({ name: 'vfx_template_apply', arguments: { template: 'explosion', path: 'Assets/VFX/NewExplosion.vfx', name: 'New Explosion' } });
        await harness.client.callTool({ name: 'vfx_optimize_budget', arguments: { path, targetParticles: 5000 } });
        await harness.client.callTool({ name: 'vfx_variant_upsert', arguments: { path, name: 'Large', values: { Intensity: 3 } } });
        assert.deepEqual(harness.bus.commands, [
            { command: { t: 'vfx.node.add', path, nodeType: 'particle', name: 'Smoke', from: 1 }, dryRun: false },
            { command: { t: 'vfx.node.setEnabled', path, nodeId: 2, enabled: false }, dryRun: false },
            { command: { t: 'vfx.node.setField', path, nodeId: 2, schemaPath: 'particle.emitRate', value: 80 }, dryRun: false },
            { command: { t: 'vfx.param.declare', path, name: 'Intensity', paramType: 'float', defaultValue: 1, minimum: 0, maximum: 4 }, dryRun: false },
            { command: { t: 'vfx.param.bind', path, name: 'Intensity', nodeId: 2, schemaPath: 'particle.emitRate' }, dryRun: false },
            { command: { t: 'vfx.instance.set', id, name: 'Intensity', value: 2 }, dryRun: false },
            { command: { t: 'vfx.template.apply', template: 'explosion', path: 'Assets/VFX/NewExplosion.vfx', name: 'New Explosion' }, dryRun: false },
            { command: { t: 'vfx.optimize', path, targetParticles: 5000 }, dryRun: false },
            { command: { t: 'vfx.variant.upsert', path, name: 'Large', values: { Intensity: 3 } }, dryRun: false },
        ]);
    }
    finally {
        await harness.close();
    }
});
test('vfx_node_set_parent は親子付けと解除の両方を写像する', async () => {
    // WHY: parentNodeId は「省略 = 親を外す」意味を持つため、省略時にキーごと落ちて
    //      C++ 側の既定 (-1) に委ねられることを型で固定しておく。
    //      ここが崩れると「親を外したつもりが何も起きない」形で静かに壊れる。
    const harness = await CreateHarness('write');
    try {
        const path = 'Assets/VFX/Explosion.vfx';
        await harness.client.callTool({ name: 'vfx_node_set_parent', arguments: { path, nodeId: 3, parentNodeId: 2 } });
        await harness.client.callTool({ name: 'vfx_node_set_parent', arguments: { path, nodeId: 3 } });
        assert.deepEqual(harness.bus.commands, [
            { command: { t: 'vfx.node.setParent', path, nodeId: 3, parentNodeId: 2 }, dryRun: false },
            { command: { t: 'vfx.node.setParent', path, nodeId: 3 }, dryRun: false },
        ]);
    }
    finally {
        await harness.close();
    }
});
test('VFX Editorの全編集面をAI Commandへ写像する', async () => {
    const harness = await CreateHarness('write');
    try {
        const catalog = await harness.client.listTools();
        assert.equal(catalog.tools.filter((tool) => tool.name.startsWith('vfx_')).length, 51);
        const path = 'Assets/VFX/Explosion.vfx';
        await harness.client.callTool({ name: 'vfx_graph_set', arguments: { path, name: 'Explosion', maxParticles: 8000 } });
        await harness.client.callTool({ name: 'vfx_node_duplicate', arguments: { path, nodeId: 2, name: 'Smoke Copy', editorX: 420, editorY: 180 } });
        await harness.client.callTool({ name: 'vfx_node_set_metadata', arguments: { path, nodeId: 3, name: 'Core', editorX: 120 } });
        await harness.client.callTool({ name: 'vfx_link_update', arguments: { path, index: 0, trigger: 'onDeath', delay: 0.2 } });
        await harness.client.callTool({ name: 'vfx_param_remove', arguments: { path, name: 'Intensity' } });
        await harness.client.callTool({ name: 'vfx_param_unbind', arguments: { path, name: 'Color', nodeId: 2 } });
        await harness.client.callTool({ name: 'vfx_variant_remove', arguments: { path, name: 'Large' } });
        await harness.client.callTool({ name: 'vfx_group_add', arguments: { path, title: 'Smoke Layer', note: 'Tail', x: 10, y: 20 } });
        await harness.client.callTool({ name: 'vfx_group_update', arguments: { path, groupId: 1, width: 500, color: [0.2, 0.3, 0.4, 0.25] } });
        await harness.client.callTool({ name: 'vfx_group_remove', arguments: { path, groupId: 1 } });
        assert.deepEqual(harness.bus.commands, [
            { command: { t: 'vfx.graph.set', path, name: 'Explosion', maxParticles: 8000 }, dryRun: false },
            { command: { t: 'vfx.node.duplicate', path, nodeId: 2, name: 'Smoke Copy', editorX: 420, editorY: 180 }, dryRun: false },
            { command: { t: 'vfx.node.setMetadata', path, nodeId: 3, name: 'Core', editorX: 120 }, dryRun: false },
            { command: { t: 'vfx.link.update', path, index: 0, trigger: 'onDeath', delay: 0.2 }, dryRun: false },
            { command: { t: 'vfx.param.remove', path, name: 'Intensity' }, dryRun: false },
            { command: { t: 'vfx.param.unbind', path, name: 'Color', nodeId: 2 }, dryRun: false },
            { command: { t: 'vfx.variant.remove', path, name: 'Large' }, dryRun: false },
            { command: { t: 'vfx.group.add', path, title: 'Smoke Layer', note: 'Tail', x: 10, y: 20 }, dryRun: false },
            { command: { t: 'vfx.group.update', path, groupId: 1, width: 500, color: [0.2, 0.3, 0.4, 0.25] }, dryRun: false },
            { command: { t: 'vfx.group.remove', path, groupId: 1 }, dryRun: false },
        ]);
    }
    finally {
        await harness.close();
    }
});
test('VFX候補の分岐・採択・知識化はUndo可能なTemplate複製へ写像する', async () => {
    const harness = await CreateHarness('write');
    try {
        await harness.client.callTool({
            name: 'vfx_candidate_fork',
            arguments: { source: 'Impact', candidatePath: 'Assets/VFX/Trials/Impact_A.vfx', name: 'Impact A' },
        });
        await harness.client.callTool({
            name: 'vfx_candidate_accept',
            arguments: { candidatePath: 'Assets/VFX/Trials/Impact_A.vfx', targetPath: 'Assets/VFX/Impact.vfx' },
        });
        await harness.client.callTool({
            name: 'vfx_knowledge_promote',
            arguments: {
                path: 'Assets/VFX/Impact.vfx',
                name: 'impact_fast_readable',
                summary: '高速ピークで遠距離から読める衝撃',
            },
        });
        assert.deepEqual(harness.bus.commands, [
            {
                command: {
                    t: 'vfx.template.apply', template: 'Impact',
                    path: 'Assets/VFX/Trials/Impact_A.vfx', name: 'Impact A',
                },
                dryRun: false,
            },
            {
                command: {
                    t: 'vfx.template.apply', template: 'Assets/VFX/Trials/Impact_A.vfx',
                    path: 'Assets/VFX/Impact.vfx',
                },
                dryRun: false,
            },
            {
                command: {
                    t: 'vfx.template.apply', template: 'Assets/VFX/Impact.vfx',
                    path: 'Assets/VFX/Templates/impact_fast_readable.vfx',
                    // 用途は description。名前へ押し込むとカタログの表示名が説明文になる。
                    name: 'impact_fast_readable',
                    description: '高速ピークで遠距離から読める衝撃',
                },
                dryRun: false,
            },
        ]);
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
test('Template適用は層・接続先・Variantを指定して既存グラフへ追記できる', async () => {
    const harness = await CreateHarness('write');
    try {
        // merge: Explosion の「煙柱の層」だけを、既存グラフのヒットノードの後段へ足す。
        await harness.client.callTool({
            name: 'vfx_template_apply',
            arguments: {
                template: 'Explosion', path: 'Assets/VFX/Hit.vfx', mode: 'merge',
                groups: [2], anchorNodeId: 3, trigger: 'onComplete', delay: 0.05,
                variant: 'Large', raiseBudget: true,
            },
        });
        // subgraph: 複製せず参照として置く (Template を直すと伝播する)。
        await harness.client.callTool({
            name: 'vfx_template_apply',
            arguments: {
                template: 'Smoke', path: 'Assets/VFX/Hit.vfx', mode: 'subgraph', anchorNodeId: 3,
            },
        });
        assert.deepEqual(harness.bus.commands, [
            {
                command: {
                    t: 'vfx.template.apply', template: 'Explosion', path: 'Assets/VFX/Hit.vfx',
                    mode: 'merge', groups: [2], anchorNodeId: 3, trigger: 'onComplete',
                    delay: 0.05, variant: 'Large', raiseBudget: true,
                },
                dryRun: false,
            },
            {
                command: {
                    t: 'vfx.template.apply', template: 'Smoke', path: 'Assets/VFX/Hit.vfx',
                    mode: 'subgraph', anchorNodeId: 3,
                },
                dryRun: false,
            },
        ]);
    }
    finally {
        await harness.close();
    }
});
test('取り込み位置の指定はreplaceでは受け付けない', () => {
    // WHY: replace は複製なので「既存グラフのどこへ入れるか」は意味を持たない。
    //      黙って無視すると、指定が効いたと誤解したまま次の手へ進むことになる。
    assert.throws(() => EditorCommandSchema.parse({
        t: 'vfx.template.apply', template: 'Explosion', path: 'Assets/VFX/New.vfx',
        anchorNodeId: 3,
    }));
    assert.throws(() => EditorCommandSchema.parse({
        t: 'vfx.template.apply', template: 'Explosion', path: 'Assets/VFX/New.vfx',
        mode: 'subgraph', groups: [1],
    }));
    assert.ok(EditorCommandSchema.parse({
        t: 'vfx.template.apply', template: 'Explosion', path: 'Assets/VFX/New.vfx',
        mode: 'merge', groups: [1], anchorNodeId: 3,
    }));
});
test('VFX知識カタログと候補評価を固定契約で実行する', async () => {
    const harness = await CreateHarness('read');
    try {
        await harness.client.callTool({
            name: 'vfx_knowledge_catalog', arguments: { query: 'impact', limit: 8 },
        });
        const evaluated = CallToolResultSchema.parse(await harness.client.callTool({
            name: 'vfx_candidate_evaluate',
            arguments: {
                path: 'Assets/VFX/Trials/Impact_A.vfx',
                samples: 3,
                objective: { peakBefore: 1, maxTailRatio: 1 },
                semanticAssessment: { score: 0.9, rationale: 'シルエットと時間構成が意図に一致' },
            },
        }));
        assert.deepEqual(harness.bus.queries[0], {
            t: 'vfx.templateCatalog', query: 'impact', limit: 8,
        });
        const text = evaluated.content.find((item) => item.type === 'text');
        assert.equal(text?.type, 'text');
        assert.equal(JSON.parse(text?.type === 'text' ? text.text : '{}').decision, 'accept');
    }
    finally {
        await harness.close();
    }
});
test('vfx_repair は指定した修復だけを送り、省略時は既定へ委ねる', async () => {
    // WHY: 修復フラグは全て「省略 = 有効」なので、省略時にキーを送らないことが
    //      既定値の唯一の表現になる。false を明示したときだけ落とせることを固定する。
    const harness = await CreateHarness('write');
    try {
        const path = 'Assets/VFX/Explosion.vfx';
        await harness.client.callTool({ name: 'vfx_repair', arguments: { path } });
        await harness.client.callTool({ name: 'vfx_repair', arguments: { path, fixSprites: false, fixParents: true } });
        assert.deepEqual(harness.bus.commands, [
            { command: { t: 'vfx.repair', path }, dryRun: false },
            { command: { t: 'vfx.repair', path, fixSprites: false, fixParents: true }, dryRun: false },
        ]);
    }
    finally {
        await harness.close();
    }
});
test('vfx_analyze_texture は Query 面へ写像する', async () => {
    const harness = await CreateHarness('read');
    try {
        await harness.client.callTool({
            name: 'vfx_analyze_texture', arguments: { path: 'Assets/Textures/Smoke.png' },
        });
        assert.deepEqual(harness.bus.queries, [
            { t: 'vfx.textureAnalyze', path: 'Assets/Textures/Smoke.png' },
        ]);
    }
    finally {
        await harness.close();
    }
});
test('vfx_analyze_material と vfx_survey_assets は Query 面へ写像する', async () => {
    // WHY: survey の directory / limit / refresh は「省略 = C++ 側の既定 (Assets / 120枚 / 再解析なし)」
    //      なので、省略時にキーごと落ちることを型で固定する。送ってしまうと既定を上書きする。
    //      detail だけは既定を summary に固定したいので、省略時も明示的に送る。
    const harness = await CreateHarness('read');
    try {
        await harness.client.callTool({
            name: 'vfx_analyze_material', arguments: { path: 'Assets/Materials/Fire.mat' },
        });
        await harness.client.callTool({ name: 'vfx_survey_assets', arguments: {} });
        await harness.client.callTool({
            name: 'vfx_survey_assets',
            arguments: { directory: 'Assets/Textures/VFX', limit: 40, detail: 'full', refresh: true },
        });
        await harness.client.callTool({
            name: 'shader_inspect', arguments: { path: 'guid:0c0bb0a5d2c448cd8138e19e9358e5a7' },
        });
        await harness.client.callTool({ name: 'shader_get_compile_diagnostics', arguments: {} });
        assert.deepEqual(harness.bus.queries, [
            { t: 'vfx.materialAnalyze', path: 'Assets/Materials/Fire.mat' },
            { t: 'vfx.assetSurvey', detail: 'summary' },
            { t: 'vfx.assetSurvey', directory: 'Assets/Textures/VFX', limit: 40,
                detail: 'full', refresh: true },
            { t: 'shader.inspect', path: 'guid:0c0bb0a5d2c448cd8138e19e9358e5a7' },
            { t: 'shader.diagnostics' },
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
test('vfx_node_set_field は配列要素の添字付き schemaPath を通す', async () => {
    // WHY: bursts のような配列 leaf は "bursts[0].count" でしか到達できない。
    //      zod で弾かれると、エンジンが対応していても AI からは存在しないのと同じになる。
    const harness = await CreateHarness('write');
    try {
        const path = 'Assets/VFX/Explosion.vfx';
        await harness.client.callTool({
            name: 'vfx_node_set_field',
            arguments: { path, nodeId: 2, schemaPath: 'particle.bursts[0].count', value: 32 },
        });
        assert.deepEqual(harness.bus.commands, [
            { command: { t: 'vfx.node.setField', path, nodeId: 2, schemaPath: 'particle.bursts[0].count', value: 32 }, dryRun: false },
        ]);
    }
    finally {
        await harness.close();
    }
});
test('vfx_guide と vfx_curve_presets は引数なしの Query 面へ写像する', async () => {
    // WHY: どちらも「作る前に読む」ための面。AI が最初に呼ぶ導線なので、
    //      引数スキーマが空であること自体が契約 (余計な必須引数があると呼ばれなくなる)。
    const harness = await CreateHarness('read');
    try {
        await harness.client.callTool({ name: 'vfx_guide', arguments: {} });
        await harness.client.callTool({ name: 'vfx_curve_presets', arguments: {} });
        assert.deepEqual(harness.bus.queries, [
            { t: 'vfx.guide' },
            { t: 'vfx.curvePresets' },
        ]);
    }
    finally {
        await harness.close();
    }
});
test('vfx_preview は診断ビューを指定でき、既定は評価用の normal', async () => {
    // WHY: 既定が normal から動くと「決定論プレビューで同一時刻が同一画になる」前提が崩れる。
    //      診断表示は明示要求のときだけ、という契約をテストで固定する。
    const harness = await CreateHarness('read');
    try {
        const path = 'Assets/VFX/Fire.vfx';
        await harness.client.callTool({ name: 'vfx_preview', arguments: { path, time: 0.5, w: 320, h: 180 } });
        await harness.client.callTool({
            name: 'vfx_preview',
            arguments: { path, time: 0.5, w: 320, h: 180, view: 'gizmos' },
        });
        const previewQueries = harness.bus.queries.filter((q) => q.t === 'vfx.preview');
        assert.deepEqual(previewQueries, [
            { t: 'vfx.preview', path, time: 0.5, w: 320, h: 180, view: 'normal' },
            { t: 'vfx.preview', path, time: 0.5, w: 320, h: 180, view: 'gizmos' },
        ]);
    }
    finally {
        await harness.close();
    }
});
test('vfx_preview は camera 引数をそのまま Query 面へ通す', async () => {
    // WHY: カメラを動かせないと、ゲーム内距離での可読性もビルボードのシルエットも
    //      LOD の切り替わりも一度も検証できない。引数が握り潰されないことを固定する。
    const harness = await CreateHarness('read');
    try {
        const path = 'Assets/VFX/Fire.vfx';
        await harness.client.callTool({
            name: 'vfx_preview',
            arguments: { path, time: 0.2, w: 320, h: 180, camera: { preset: 'side', distance: 12 } },
        });
        assert.deepEqual(harness.bus.queries.filter((q) => q.t === 'vfx.preview'), [
            { t: 'vfx.preview', path, time: 0.2, w: 320, h: 180, view: 'normal',
                camera: { preset: 'side', distance: 12 } },
        ]);
    }
    finally {
        await harness.close();
    }
});
test('vfx_preview_metrics は preview を組んでから同じ view で指標を要求する', async () => {
    // WHY: 指標は「直前に描いた画」を読み戻す。preview を組まずに測ると前の時刻の画を測ってしまう。
    //      順序と view の一致がこのツールの正しさそのもの。
    const harness = await CreateHarness('read');
    try {
        const path = 'Assets/VFX/Fire.vfx';
        await harness.client.callTool({
            name: 'vfx_preview_metrics',
            arguments: { path, time: 0.4, w: 320, h: 180, view: 'overdraw' },
        });
        assert.deepEqual(harness.bus.queries, [
            { t: 'vfx.preview', path, time: 0.4, w: 320, h: 180, view: 'overdraw' },
            { t: 'vfx.previewMetrics', path, view: 'overdraw' },
        ]);
    }
    finally {
        await harness.close();
    }
});
test('vfx_preview_sweep は距離ごとに視点を変えて測り直す', async () => {
    // WHY: 距離を変えられないと LOD の切り替わりを一度も見られない。
    //      distances の各要素が実際に別々の camera.distance として送られることを固定する。
    const harness = await CreateHarness('read');
    try {
        const path = 'Assets/VFX/Fire.vfx';
        await harness.client.callTool({
            name: 'vfx_preview_sweep',
            arguments: { path, time: 0.3, distances: [2, 20], w: 320, h: 180, includeImages: false },
        });
        const distances = harness.bus.queries
            .filter((q) => q.t === 'vfx.preview')
            .map((q) => q.camera?.distance);
        assert.deepEqual(distances, [2, 20]);
    }
    finally {
        await harness.close();
    }
});
test('vfx_preview_curve は明示 times を全て測り、時間の形を返す', async () => {
    // WHY: エフェクトの質は静止画ではなく時間の形で決まる。サンプルが 1 点でも欠けると
    //      ピーク位置がずれ、時間設計の評価そのものが誤る。
    const harness = await CreateHarness('read');
    try {
        const path = 'Assets/VFX/Fire.vfx';
        const rawResult = await harness.client.callTool({
            name: 'vfx_preview_curve',
            arguments: { path, times: [0, 0.25, 0.5], w: 320, h: 180 },
        });
        const result = CallToolResultSchema.parse(rawResult);
        const measured = harness.bus.queries.filter((q) => q.t === 'vfx.previewMetrics');
        assert.equal(measured.length, 3);
        const text = result.content.find((item) => item.type === 'text');
        assert.ok(text?.type === 'text' && text.text.includes('peakNormalized'));
    }
    finally {
        await harness.close();
    }
});
test('vfx_node_set_field は Curve をプリセット名でも生キーでも渡せる', async () => {
    // WHY: Curve/Gradient は長らく JsonToSchemaValue に無く、schema には見えるのに
    //      AI からは一切編集できないフィールドだった。エフェクトの質を最も左右する
    //      時間曲線が触れないままだったので、両方の指定形が通ることを固定する。
    const harness = await CreateHarness('write');
    try {
        const path = 'Assets/VFX/Fire.vfx';
        await harness.client.callTool({
            name: 'vfx_node_set_field',
            arguments: { path, nodeId: 7, schemaPath: 'light.intensityCurve',
                value: { preset: 'Breathe', scale: 1.0 } },
        });
        await harness.client.callTool({
            name: 'vfx_node_set_field',
            arguments: { path, nodeId: 2, schemaPath: 'particle.sizeCurve',
                value: { interp: 2, keys: [[0, 0], [0.3, 1], [1, 0.2]] } },
        });
        assert.deepEqual(harness.bus.commands, [
            { command: { t: 'vfx.node.setField', path, nodeId: 7, schemaPath: 'light.intensityCurve',
                    value: { preset: 'Breathe', scale: 1.0 } }, dryRun: false },
            { command: { t: 'vfx.node.setField', path, nodeId: 2, schemaPath: 'particle.sizeCurve',
                    value: { interp: 2, keys: [[0, 0], [0.3, 1], [1, 0.2]] } }, dryRun: false },
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
        const id = '44444444-4444-4444-8444-444444444444';
        await harness.client.callTool({ name: 'scene_list', arguments: {} });
        await harness.client.callTool({ name: 'preset_catalog', arguments: {} });
        await harness.client.callTool({ name: 'terrain_inspect', arguments: {} });
        // [x, z] の 2 要素で問い合わせられることを固定する。高さを問うのに y は要らないため。
        await harness.client.callTool({ name: 'terrain_sample', arguments: { points: [[10, 20], [1, 2, 3]] } });
        await harness.client.callTool({ name: 'foliage_inspect', arguments: { id } });
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
            { t: 'foliage.inspect', id },
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
test('地形・植生・NavMesh・シーン入出力は Undo 単位の Command へ写像する', async () => {
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
        await harness.client.callTool({
            name: 'foliage_scatter',
            arguments: { id: terrainId, species: 0, position: [5, 0, 5], count: 20, seed: 7 },
        });
        await harness.client.callTool({ name: 'foliage_clear', arguments: { id: terrainId, species: 0 } });
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
            { command: { t: 'foliage.scatter', id: terrainId, species: 0, position: [5, 0, 5], radius: 5, count: 20, maxSlopeDegrees: 40, seed: 7 }, dryRun: false },
            { command: { t: 'foliage.clear', id: terrainId, species: 0 }, dryRun: false },
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