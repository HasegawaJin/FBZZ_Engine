/// @file    contracts.test.ts
/// @brief   ローカル接続制約と Editor Command / Query の入力検証を確認する。
/// @author  Hasegawa Jin
/// @date    2026-07-20
import assert from 'node:assert/strict';
import test from 'node:test';
import { IsEditorPipeEndpoint, LoadConfig } from '../config.js';
import { EditorCommandSchema, EditorQuerySchema } from '../editorContracts.js';
test('Material の行列・配列と uint32 の最大値を丸めず受け付ける', () => {
    const base = { t: 'material.override', id: '11111111-1111-4111-8111-111111111111', parameter: 'values' };
    for (const value of [Array(16).fill(1), [4294967295], Array(64).fill(0)]) {
        const parsed = EditorCommandSchema.safeParse({ ...base, value });
        assert.equal(parsed.success, true);
        if (parsed.success && parsed.data.t === 'material.override')
            assert.deepEqual(parsed.data.value, value);
    }
    assert.equal(EditorCommandSchema.safeParse({ ...base, value: [] }).success, false);
    assert.equal(EditorCommandSchema.safeParse({ ...base, value: [Infinity] }).success, false);
    assert.equal(EditorCommandSchema.safeParse({ ...base, value: Array(16385).fill(0) }).success, false);
});
test('Command Bus は FBZZ 専用 Named Pipe だけを許可する', () => {
    assert.equal(IsEditorPipeEndpoint('\\\\.\\pipe\\FBZZEditorCommandBus'), true);
    assert.equal(IsEditorPipeEndpoint('\\\\.\\pipe\\FBZZEditorTest'), true);
    assert.equal(IsEditorPipeEndpoint('\\\\server\\pipe\\FBZZEditorCommandBus'), false);
    assert.equal(IsEditorPipeEndpoint('C:\\temp\\FBZZEditorCommandBus'), false);
});
test('不正な環境変数は read と loopback の安全な既定値へ戻る', () => {
    const config = LoadConfig({
        FBZZ_EDITOR_PIPE: '\\\\server\\pipe\\Editor',
        FBZZ_MCP_PERMISSION: 'admin',
        FBZZ_EDITOR_BUS_TIMEOUT_MS: '999999',
    });
    assert.equal(config.busEndpoint, '\\\\.\\pipe\\FBZZEditorCommandBus');
    assert.equal(config.permission, 'read');
    assert.equal(config.requestTimeoutMs, 60000);
});
test('空の transform.set は拒否し、有効な transaction は受理する', () => {
    const nodeId = '11111111-1111-4111-8111-111111111111';
    assert.equal(EditorCommandSchema.safeParse({ t: 'transform.set', id: nodeId }).success, false);
    assert.equal(EditorCommandSchema.safeParse({
        t: 'editor.transaction',
        label: 'Create and place object',
        cmds: [
            { t: 'node.create', name: 'Cube' },
            { t: 'transform.set', id: nodeId, pos: [1, 2, 3] },
        ],
    }).success, true);
});
test('Operator ゲートウェイの契約を検証する', () => {
    /// @note 目録は絞り込み条件なしでも引ける (全操作の一覧が出発点になる)。
    assert.equal(EditorQuerySchema.safeParse({ t: 'editor.op.list' }).success, true);
    assert.equal(EditorQuerySchema.safeParse({
        t: 'editor.op.list',
        search: 'gizmo',
        category: 'Gizmo',
        includeUnavailable: false,
    }).success, true);
    /// @note strict なので綴り違いは受理しない。
    assert.equal(EditorQuerySchema.safeParse({ t: 'editor.op.list', query: 'gizmo' }).success, false);
    /// @note 引数なしの操作は id だけで呼べる。
    assert.equal(EditorCommandSchema.safeParse({ t: 'editor.op.invoke', id: 'scene.save' }).success, true);
    assert.equal(EditorCommandSchema.safeParse({
        t: 'editor.op.invoke',
        id: 'node.set_tag',
        args: { id: '11111111-1111-4111-8111-111111111111', tag: 'Enemy' },
    }).success, true);
    /// @note id は必須。args の中身は Editor 側の params 宣言で検証されるため、ここでは形だけを見る。
    assert.equal(EditorCommandSchema.safeParse({ t: 'editor.op.invoke' }).success, false);
    assert.equal(EditorCommandSchema.safeParse({ t: 'editor.op.invoke', id: '' }).success, false);
});
test('fluid.* の Query / Command 契約を検証する', () => {
    const path = 'Assets/VFX/Fluid/Smoke.fluid';
    assert.equal(EditorQuerySchema.safeParse({ t: 'fluid.schema' }).success, true);
    assert.equal(EditorQuerySchema.safeParse({ t: 'fluid.get', path }).success, true);
    /// @note .fluid 以外の拡張子は C++ まで届けない。
    assert.equal(EditorQuerySchema.safeParse({ t: 'fluid.get', path: 'Assets/VFX/Fluid/Smoke.mat' }).success, false);
    assert.equal(EditorQuerySchema.safeParse({ t: 'fluid.jobStatus', job: 1 }).success, true);
    assert.equal(EditorQuerySchema.safeParse({ t: 'fluid.jobStatus', job: 1, includeImage: false }).success, true);
    /// @note job 0 は FluidBakeService が「受け付けなかった」に使う値。
    assert.equal(EditorQuerySchema.safeParse({ t: 'fluid.jobStatus', job: 0 }).success, false);
    assert.equal(EditorQuerySchema.safeParse({ t: 'fluid.jobStatus', job: 1.5 }).success, false);
    assert.equal(EditorCommandSchema.safeParse({ t: 'fluid.create', path }).success, true);
    assert.equal(EditorCommandSchema.safeParse({ t: 'fluid.create', path, preset: 'LavaBlob', overwrite: true }).success, true);
    assert.equal(EditorCommandSchema.safeParse({ t: 'fluid.set', path, fields: { gas: { buoyancy: 2 } } }).success, true);
    assert.equal(EditorCommandSchema.safeParse({
        t: 'fluid.set', path, fields: { source: [{ shape: 'cone' }, { motion: { key: [{ time: 0, offset: [0, 0, 0] }] } }] },
    }).success, true);
    /// @note 部品の追加・削除・並べ替え。list は 3 種だけで、type と index は省略できる。
    assert.equal(EditorCommandSchema.safeParse({ t: 'fluid.addOperator', path, list: 'source' }).success, true);
    assert.equal(EditorCommandSchema.safeParse({
        t: 'fluid.addOperator', path, list: 'collider', type: 'plane', fields: { direction: [1, 1, 0], friction: 0.5 },
    }).success, true);
    assert.equal(EditorCommandSchema.safeParse({
        t: 'fluid.addOperator', path, list: 'source', type: 'texture', fields: { texture: 'Assets/Textures/Logo.png' },
    }).success, true);
    assert.equal(EditorCommandSchema.safeParse({ t: 'fluid.removeOperator', path, list: 'collider', index: 0 }).success, true);
    assert.equal(EditorCommandSchema.safeParse({ t: 'fluid.moveOperator', path, list: 'collider', from: 1, to: 0 }).success, true);
    assert.equal(EditorCommandSchema.safeParse({ t: 'fluid.addOperator', path, list: 'obstacle' }).success, false);
    assert.equal(EditorCommandSchema.safeParse({
        t: 'fluid.addOperator', path, list: 'force', type: 'vortex', index: 0, fields: { strength: 4 },
    }).success, true);
    assert.equal(EditorCommandSchema.safeParse({ t: 'fluid.addOperator', path, list: 'emitter' }).success, false);
    assert.equal(EditorCommandSchema.safeParse({ t: 'fluid.addOperator', path, list: 'source', index: -1 }).success, false);
    assert.equal(EditorCommandSchema.safeParse({ t: 'fluid.removeOperator', path, list: 'force', index: 2 }).success, true);
    assert.equal(EditorCommandSchema.safeParse({ t: 'fluid.removeOperator', path, list: 'force' }).success, false);
    assert.equal(EditorCommandSchema.safeParse({ t: 'fluid.moveOperator', path, list: 'source', from: 0, to: 3 }).success, true);
    assert.equal(EditorCommandSchema.safeParse({ t: 'fluid.moveOperator', path, list: 'source', from: 0 }).success, false);
    /// @note 空の部分レシピは「何も変えない」要求なので受けない。
    assert.equal(EditorCommandSchema.safeParse({ t: 'fluid.set', path, fields: {} }).success, false);
    assert.equal(EditorCommandSchema.safeParse({ t: 'fluid.preview', path, time: 2, size: 256 }).success, true);
    assert.equal(EditorCommandSchema.safeParse({ t: 'fluid.preview', path, size: 4096 }).success, false);
    assert.equal(EditorCommandSchema.safeParse({ t: 'fluid.bake', path, updateMaterial: false }).success, true);
    assert.equal(EditorCommandSchema.safeParse({ t: 'fluid.cancel', job: 12 }).success, true);
    assert.equal(EditorCommandSchema.safeParse({ t: 'fluid.cancel' }).success, false);
    assert.equal(EditorCommandSchema.safeParse({ t: 'fluid.createEffect', name: 'Campfire' }).success, true);
    assert.equal(EditorCommandSchema.safeParse({
        t: 'fluid.createEffect', name: 'Campfire', dir: 'Assets/VFX/Fluid', preset: 'Fire',
        fields: { bake: { mode: '3d' } }, bake: false,
    }).success, true);
    /// @note name はファイル名 1 つ分。区切り文字で dir の外へ出られない。
    assert.equal(EditorCommandSchema.safeParse({ t: 'fluid.createEffect', name: '../Escape' }).success, false);
    assert.equal(EditorCommandSchema.safeParse({ t: 'fluid.createEffect', name: 'a/b' }).success, false);
});
test('viewport.capture は既定上限を超える要求を拒否する', () => {
    assert.equal(EditorQuerySchema.safeParse({ t: 'viewport.capture', w: 960, h: 540 }).success, true);
    assert.equal(EditorQuerySchema.safeParse({ t: 'viewport.capture', w: 960, h: 540, view: 'game' }).success, true);
    /// @note .vfx はプレファブになり、専用のプレビュー面が無くなった。
    assert.equal(EditorQuerySchema.safeParse({ t: 'viewport.capture', w: 960, h: 540, view: 'vfx' }).success, false);
    assert.equal(EditorQuerySchema.safeParse({ t: 'viewport.capture', w: 3840, h: 2160 }).success, false);
});
test('catalog・scene.find・node.duplicate の契約を検証する', () => {
    const nodeId = '11111111-1111-4111-8111-111111111111';
    assert.equal(EditorQuerySchema.safeParse({ t: 'editor.catalog' }).success, true);
    assert.equal(EditorQuerySchema.safeParse({
        t: 'scene.find',
        name: 'player',
        comp: 'CameraComponent',
        limit: 100,
    }).success, true);
    assert.equal(EditorQuerySchema.safeParse({ t: 'scene.find' }).success, false);
    assert.equal(EditorCommandSchema.safeParse({ t: 'node.duplicate', id: nodeId }).success, true);
    /// @note GameObject プロパティ設定。
    assert.equal(EditorCommandSchema.safeParse({ t: 'node.setActive', id: nodeId, active: false }).success, true);
    assert.equal(EditorCommandSchema.safeParse({ t: 'node.setActive', id: nodeId }).success, false);
    assert.equal(EditorCommandSchema.safeParse({ t: 'node.setTag', id: nodeId, tag: 'Enemy' }).success, true);
    assert.equal(EditorCommandSchema.safeParse({ t: 'node.setLayer', id: nodeId, layer: 8 }).success, true);
    assert.equal(EditorCommandSchema.safeParse({ t: 'node.setLayer', id: nodeId, layer: 99 }).success, false);
});
test('実行制御・物理Query・Prefab・検索フィルタの契約を検証する', () => {
    const nodeId = '11111111-1111-4111-8111-111111111111';
    assert.equal(EditorQuerySchema.safeParse({ t: 'editor.state' }).success, true);
    assert.equal(EditorQuerySchema.safeParse({ t: 'editor.undoHistory' }).success, true);
    assert.equal(EditorQuerySchema.safeParse({ t: 'editor.undoHistory', limit: 20 }).success, true);
    assert.equal(EditorQuerySchema.safeParse({ t: 'editor.undoHistory', limit: 999 }).success, false);
    assert.equal(EditorQuerySchema.safeParse({
        t: 'console.logs', minLevel: 'warning', limit: 50, afterSequence: 123,
    }).success, true);
    assert.equal(EditorQuerySchema.safeParse({ t: 'console.logs', afterSequence: -1 }).success, false);
    assert.equal(EditorQuerySchema.safeParse({
        t: 'physics.raycast', origin: [0, 10, 0], direction: [0, -1, 0], maxDistance: 100,
    }).success, true);
    assert.equal(EditorQuerySchema.safeParse({
        t: 'scene.find',
        components: ['RigidBodyComponent', 'BoxColliderComponent'],
        properties: [{ comp: 'RigidBodyComponent', field: 'mass', op: 'greater', value: 10 }],
    }).success, true);
    assert.equal(EditorCommandSchema.safeParse({ t: 'play.control', action: 'step' }).success, true);
    assert.equal(EditorCommandSchema.safeParse({
        t: 'prefab.instantiate', path: 'Assets/Prefabs/Enemy.prefab', parent: nodeId,
    }).success, true);
    assert.equal(EditorCommandSchema.safeParse({ t: 'viewport.camera' }).success, false);
    assert.equal(EditorCommandSchema.safeParse({ t: 'viewport.camera', targetId: nodeId }).success, true);
});
test('意味付き知覚・診断Queryと入力・マテリアル・アニメーションCommandを検証する', () => {
    const nodeId = '11111111-1111-4111-8111-111111111111';
    assert.equal(EditorQuerySchema.safeParse({ t: 'viewport.semantic', w: 960, h: 540 }).success, true);
    assert.equal(EditorQuerySchema.safeParse({ t: 'editor.catalog.search', query: 'camera', limit: 10 }).success, true);
    assert.equal(EditorQuerySchema.safeParse({ t: 'editor.catalog.search' }).success, false);
    assert.equal(EditorQuerySchema.safeParse({ t: 'scene.snapshot' }).success, true);
    assert.equal(EditorQuerySchema.safeParse({ t: 'scene.validate' }).success, true);
    assert.equal(EditorQuerySchema.safeParse({ t: 'asset.inspect', path: 'Assets/Test.mat' }).success, true);
    assert.equal(EditorQuerySchema.safeParse({ t: 'profiler.snapshot', limit: 20 }).success, true);
    assert.equal(EditorQuerySchema.safeParse({ t: 'animation.pose', id: nodeId, limit: 128 }).success, true);
    assert.equal(EditorQuerySchema.safeParse({ t: 'animation.graph', id: nodeId }).success, true);
    assert.equal(EditorQuerySchema.safeParse({ t: 'animation.blendTree', id: nodeId, state: 'Locomotion' }).success, true);
    assert.equal(EditorQuerySchema.safeParse({ t: 'animation.blendTree', id: nodeId }).success, false);
    assert.equal(EditorCommandSchema.safeParse({
        t: 'material.override', id: nodeId, parameter: 'BaseColor', value: [1, 0.5, 0, 1],
    }).success, true);
    assert.equal(EditorCommandSchema.safeParse({
        t: 'animation.control', id: nodeId, action: 'seek', frame: 12,
    }).success, true);
    /// @note float/int は数値、bool/trigger は真偽、trigger は value 省略で発火を許可する。
    assert.equal(EditorCommandSchema.safeParse({
        t: 'animation.setParameter', id: nodeId, name: 'Speed', value: 1.5,
    }).success, true);
    assert.equal(EditorCommandSchema.safeParse({
        t: 'animation.setParameter', id: nodeId, name: 'Grounded', value: true,
    }).success, true);
    assert.equal(EditorCommandSchema.safeParse({
        t: 'animation.setParameter', id: nodeId, name: 'Jump',
    }).success, true);
    assert.equal(EditorCommandSchema.safeParse({
        t: 'animation.setParameter', id: nodeId, name: 'Speed', value: 'fast',
    }).success, false);
    /// @note 構造編集: 遷移追加 (from 省略 = Any State) と条件編集。
    assert.equal(EditorCommandSchema.safeParse({
        t: 'animation.addTransition', id: nodeId, from: 'Idle', to: 'Run', transitionDuration: 0.2,
    }).success, true);
    assert.equal(EditorCommandSchema.safeParse({
        t: 'animation.addTransition', id: nodeId, to: 'Hit',
    }).success, true);
    assert.equal(EditorCommandSchema.safeParse({
        t: 'animation.setCondition', id: nodeId, from: 'Idle', transitionIndex: 0, action: 'add', parameter: 'Speed', op: 'greater', threshold: 0.1,
    }).success, true);
    assert.equal(EditorCommandSchema.safeParse({
        t: 'animation.setCondition', id: nodeId, transitionIndex: 0, action: 'clear',
    }).success, true);
    /// @note add には parameter と op が必要、remove には conditionIndex が必要。
    assert.equal(EditorCommandSchema.safeParse({
        t: 'animation.setCondition', id: nodeId, transitionIndex: 0, action: 'add', parameter: 'Speed',
    }).success, false);
    assert.equal(EditorCommandSchema.safeParse({
        t: 'animation.setCondition', id: nodeId, transitionIndex: 0, action: 'remove',
    }).success, false);
    /// @note ステート CRUD と BlendTree Motion。
    assert.equal(EditorCommandSchema.safeParse({
        t: 'animation.addState', id: nodeId, name: 'Jump', mode: 'clip', sourcePath: 'Assets/Anim/Jump.fbx', setAsDefault: true,
    }).success, true);
    assert.equal(EditorCommandSchema.safeParse({ t: 'animation.addState', id: nodeId }).success, false);
    assert.equal(EditorCommandSchema.safeParse({
        t: 'animation.setState', id: nodeId, state: 'Idle', name: 'Idle2', speed: 1.5, blend2DType: 'freeformCartesian',
    }).success, true);
    assert.equal(EditorCommandSchema.safeParse({ t: 'animation.setState', id: nodeId }).success, false);
    assert.equal(EditorCommandSchema.safeParse({
        t: 'animation.addMotion', id: nodeId, state: 'Locomotion', clipName: 'Run', threshold: 3.0,
    }).success, true);
    assert.equal(EditorCommandSchema.safeParse({
        t: 'animation.setMotion', id: nodeId, state: 'Locomotion', motionIndex: 1, posX: 0.5, posY: -0.5,
    }).success, true);
    assert.equal(EditorCommandSchema.safeParse({
        t: 'animation.setMotion', id: nodeId, state: 'Locomotion',
    }).success, false);
    /// @note 削除系とパラメーター CRUD。
    assert.equal(EditorCommandSchema.safeParse({ t: 'animation.removeState', id: nodeId, state: 'Run' }).success, true);
    assert.equal(EditorCommandSchema.safeParse({ t: 'animation.removeMotion', id: nodeId, state: 'Locomotion', motionIndex: 2 }).success, true);
    assert.equal(EditorCommandSchema.safeParse({ t: 'animation.removeTransition', id: nodeId, transitionIndex: 1 }).success, true);
    assert.equal(EditorCommandSchema.safeParse({ t: 'animation.removeTransition', id: nodeId, from: 'Idle', transitionIndex: 0 }).success, true);
    assert.equal(EditorCommandSchema.safeParse({ t: 'animation.removeTransition', id: nodeId }).success, false);
    assert.equal(EditorCommandSchema.safeParse({
        t: 'animation.addParameter', id: nodeId, name: 'Speed', type: 'float', value: 0.5,
    }).success, true);
    assert.equal(EditorCommandSchema.safeParse({ t: 'animation.addParameter', id: nodeId, name: 'Jump', type: 'trigger' }).success, true);
    assert.equal(EditorCommandSchema.safeParse({ t: 'animation.addParameter', id: nodeId, name: 'Bad', type: 'vector' }).success, false);
    assert.equal(EditorCommandSchema.safeParse({ t: 'animation.removeParameter', id: nodeId, name: 'Speed' }).success, true);
    assert.equal(EditorCommandSchema.safeParse({ t: 'animation.removeParameter', id: nodeId }).success, false);
    assert.equal(EditorCommandSchema.safeParse({
        t: 'input.inject', kind: 'key', key: 'space', pressed: true,
    }).success, true);
    assert.equal(EditorCommandSchema.safeParse({
        t: 'input.inject', kind: 'gamepadButton', buttonName: 'South', pressed: true,
    }).success, true);
    assert.equal(EditorCommandSchema.safeParse({ t: 'input.inject', kind: 'gamepadButton' }).success, false);
});
//# sourceMappingURL=contracts.test.js.map