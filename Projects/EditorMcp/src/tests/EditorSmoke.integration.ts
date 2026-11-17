/// @file    EditorSmoke.integration.ts
/// @brief   MCP stdio から実 Editor の保存・再読込・Playtest を検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-17
import assert from 'node:assert/strict';
import { spawn, spawnSync } from 'node:child_process';
import { once } from 'node:events';
import { cp, mkdir, mkdtemp, open, readFile, realpath, unlink, writeFile } from 'node:fs/promises';
import { dirname, join, resolve } from 'node:path';
import test from 'node:test';
import { setTimeout as delay } from 'node:timers/promises';
import { fileURLToPath } from 'node:url';
import { Client } from '@modelcontextprotocol/sdk/client/index.js';
import { StdioClientTransport } from '@modelcontextprotocol/sdk/client/stdio.js';
import { CallToolResultSchema } from '@modelcontextprotocol/sdk/types.js';

const SCENE_PATH = 'Assets/Scenes/Smoke.scene';
const SCENARIO_PATH = 'Tests/Playtests/Smoke.playtest.json';
const SCENARIO_NAME = 'EditorMcpSmoke';
const CAPTURE_SCENARIO = 'Tests/Playtests/CaptureFreshness.playtest.json';
const OBJECT_ID = '6a4efced-4563-4cbe-9e3d-7c7212cf6d2b';
const CAMERA_ID = 'fa025438-3c92-45ee-95e8-0a91cc3095c0';
const ORIGINAL_NAME = 'McpSmokeBefore';
const SAVED_NAME = 'McpSmokeAfter';
const BUS_TIMEOUT_MS = 20000;

async function Call(client: Client, name: string, args: Record<string, unknown> = {}) {
    const reply = CallToolResultSchema.parse(await client.callTool(
        { name, arguments: args }, CallToolResultSchema, { timeout: BUS_TIMEOUT_MS + 5000 },
    ));
    const body = reply.content.find(item => item.type === 'text')?.text;
    assert.ok(!reply.isError, `${name}: ${body ?? 'no error detail'}`);
    return reply.structuredContent ?? JSON.parse(body ?? '{}');
}

async function ExpectName(client: Client, name: string) {
    const found = await Call(client, 'scene_find', { name });
    assert.equal(found.count, 1, name);
    assert.equal(found.matches[0].id, OBJECT_ID);
    assert.equal(found.matches[0].name, name);
}

async function CaptureGame(client: Client, project: string, name: string, minFrame?: number) {
    const reply = CallToolResultSchema.parse(await client.callTool({
        name: 'viewport_capture_semantic',
        arguments: { view: 'game', w: 320, h: 180, visibleOnly: false, minFrame },
    }, CallToolResultSchema, { timeout: BUS_TIMEOUT_MS + 5000 }));
    assert.ok(!reply.isError, JSON.stringify(reply.content));
    const image = reply.content.find(item => item.type === 'image');
    const result = reply.structuredContent;
    assert.ok(image && result?.provenance, 'PNG must include source provenance');
    assert.equal(image.mimeType, 'image/png');
    const png = Buffer.from(image.data, 'base64');
    assert.equal(png.subarray(0, 8).toString('hex'), '89504e470d0a1a0a');
    await writeFile(join(project, '..', name + '.png'), png);
    return result;
}

async function WaitForScenario(client: Client, path: string, name: string) {
    const launched = await Call(client, 'scenario_run', { path });
    assert.equal(launched.started, true);
    assert.equal(launched.name, name);
    const deadline = performance.now() + 60000;
    let report = await Call(client, 'scenario_status');
    while (report.state === 'running' && performance.now() < deadline) {
        await delay(250);
        report = await Call(client, 'scenario_status');
    }
    assert.equal(report.name, name);
    assert.equal(report.state, 'passed', JSON.stringify(report));
    return report;
}

async function VerifyAssetMoveBatch(client: Client, project: string, context: { diagnostic(message: string): void }) {
    const prefab = 'Assets/Prefabs/Smoke.prefab';
    const material = 'Assets/Materials/Smoke.mat';
    const movedPrefab = 'Assets/Reviewed/Renamed.prefab';
    const movedMaterial = 'Assets/Reviewed/Renamed.mat';
    await mkdir(join(project, 'Assets', 'Prefabs'), { recursive: true });
    await mkdir(join(project, 'Assets', 'Materials'), { recursive: true });
    await mkdir(join(project, 'Assets', 'Reviewed'), { recursive: true });
    await Call(client, 'prefab_create', { path: prefab, ids: [OBJECT_ID] });
    await Call(client, 'editor_op_invoke', { id: 'asset.create', args: { path: material, kind: 'material' } });

    const readPair = async (path: string) => ({
        body: await readFile(join(project, path)),
        meta: await readFile(join(project, path + '.meta')),
    });
    const guidOf = (meta: Buffer) => {
        const guid = meta.toString('utf8').match(/^guid\s*=\s*['"]([\da-f-]+)['"]/m)?.[1];
        assert.ok(guid, 'The normal Editor save must have written a GUID');
        return guid;
    };
    const prefabBefore = await readPair(prefab);
    const materialBefore = await readPair(material);
    const prefabGuid = guidOf(prefabBefore.meta);
    const materialGuid = guidOf(materialBefore.meta);
    const moves = [
        { path: 'guid:' + prefabGuid, destination: movedPrefab },
        { path: material, destination: movedMaterial },
    ];
    const files = [
        { guid: prefabGuid, source: prefab, destination: movedPrefab, sourceMeta: prefab + '.meta', destinationMeta: movedPrefab + '.meta' },
        { guid: materialGuid, source: material, destination: movedMaterial, sourceMeta: material + '.meta', destinationMeta: movedMaterial + '.meta' },
    ];
    const preview = async (items = moves) => (await Call(client, 'editor_op_query', {
        id: 'asset.move_batch.preview', args: { moves: items },
    })).data;
    const apply = async (receipt: string) => Call(client, 'editor_op_invoke', {
        id: 'asset.move_batch.apply', args: { receipt },
    });
    const expectMissingPair = async (path: string) => {
        await assert.rejects(readFile(join(project, path)), { code: 'ENOENT' });
        await assert.rejects(readFile(join(project, path + '.meta')), { code: 'ENOENT' });
    };

    await assert.rejects(preview([moves[0], { ...moves[0], destination: 'Assets/Reviewed/Again.prefab' }]), /repeats a source GUID/i);
    await assert.rejects(preview([{ ...moves[0], destination: 'Assets/../Outside.prefab' }]), /under Assets/i);

    const reviewed = await preview();
    assert.deepEqual(reviewed.moves, files);
    assert.ok(reviewed.receipt);
    assert.deepEqual(await readPair(prefab), prefabBefore);
    assert.deepEqual(await readPair(material), materialBefore);
    await expectMissingPair(movedPrefab);
    await expectMissingPair(movedMaterial);

    /// @note 読み取りは許可し、2件目のmetaを移す段階で Windows の共有違反を起こす。
    /// @see https://learn.microsoft.com/windows/win32/api/fileapi/nf-fileapi-createfilew FILE_SHARE_DELETE
    const lock = spawn('powershell.exe', ['-NoProfile', '-NonInteractive', '-Command', [
        '$stream = [IO.File]::Open($env:FBZZ_SMOKE_LOCK_PATH, "Open", "Read", "ReadWrite")',
        'if ($null -eq $stream) { exit 1 }',
        'try { [Console]::WriteLine("locked"); [Console]::ReadLine() | Out-Null } finally { $stream.Dispose() }',
    ].join('; ')], {
        env: { ...process.env, FBZZ_SMOKE_LOCK_PATH: join(project, material + '.meta') },
        stdio: ['pipe', 'pipe', 'inherit'],
        windowsHide: true,
    });
    const lockStopped = once(lock, 'close');
    try {
        const ready = await Promise.race([
            once(lock.stdout, 'data').then(([data]) => String(data).trim()),
            lockStopped.then(([code]) => `exited (${code})`),
        ]);
        assert.equal(ready, 'locked');
        await assert.rejects(apply(reviewed.receipt), (failure: Error) => {
            assert.match(failure.message, /1 prior pair.*restored.*Smoke\.mat\.meta/i);
            context.diagnostic(failure.message);
            return true;
        });
        assert.deepEqual(await readPair(prefab), prefabBefore);
        assert.deepEqual(await readPair(material), materialBefore);
        await expectMissingPair(movedPrefab);
        await expectMissingPair(movedMaterial);
        await assert.rejects(apply(reviewed.receipt), /review.*again/i);
        context.diagnostic('Asset move batch: the second sidecar rename failed; both original file/meta pairs were restored byte for byte, including their GUIDs.');
    } finally {
        lock.stdin.end();
        await lockStopped;
    }

    const rechecked = await preview();
    assert.deepEqual(rechecked.moves, files);
    await writeFile(join(project, movedMaterial + '.meta'), materialBefore.meta);
    try {
        await assert.rejects(apply(rechecked.receipt), /changed.*review.*again/i);
        assert.deepEqual(await readPair(prefab), prefabBefore);
        assert.deepEqual(await readPair(material), materialBefore);
        assert.deepEqual(await readFile(join(project, movedMaterial + '.meta')), materialBefore.meta);
    } finally {
        await unlink(join(project, movedMaterial + '.meta'));
    }

    const accepted = await preview();
    const applied = await apply(accepted.receipt);
    assert.deepEqual(applied.data.moves, files);
    await expectMissingPair(prefab);
    await expectMissingPair(material);
    assert.deepEqual(await readPair(movedPrefab), prefabBefore);
    assert.deepEqual(await readPair(movedMaterial), materialBefore);
    const reverse = await preview([
        { path: 'guid:' + prefabGuid, destination: prefab },
        { path: 'guid:' + materialGuid, destination: material },
    ]);
    assert.deepEqual(reverse.moves.map((move: { source: string }) => move.source), [movedPrefab, movedMaterial]);
    context.diagnostic('Asset move batch: confirmed the stale review was rejected without writes and both GUIDs resolve to the new paths after success.');
}

/// @brief 起動待ちが終わらない場合に、CI が起動した Editor のスレッド位置を保存する。
/// @see https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/cdb-command-line-options CDB の非侵襲接続。
async function CaptureStartupStacks(pid: number, executable: string, run: string) {
    const programFiles = process.env['ProgramFiles(x86)'];
    if (!programFiles) return;
    const cdb = join(programFiles, 'Windows Kits', '10', 'Debuggers', 'x64', 'cdb.exe');
    const symbols = `${dirname(executable)};${dirname(dirname(executable))}`;
    const result = spawnSync(cdb, ['-pv', '-p', String(pid), '-y', symbols, '-c', '~* kc 24; q'], {
        encoding: 'utf8', timeout: 15000, windowsHide: true,
    });
    await writeFile(join(run, 'Stacks.log'), [result.stdout, result.stderr, result.error?.message].filter(Boolean).join('\n'));
}

/// @note CI で起動したプロセスが開いた作業コピーを照合してから、書き込みを送る。
test('Editor MCP connects, edits, saves, reloads and runs a Playtest', { timeout: 240000 }, async context => {
    assert.ok(process.env.FBZZ_SMOKE_EDITOR, 'FBZZ_SMOKE_EDITOR must point to the built FBZZEditor.exe');
    assert.ok(process.env.FBZZ_SMOKE_OUTPUT, 'FBZZ_SMOKE_OUTPUT must point to the test output directory');
    const executable = await realpath(process.env.FBZZ_SMOKE_EDITOR);
    const output = resolve(process.env.FBZZ_SMOKE_OUTPUT);
    await mkdir(output, { recursive: true });
    const run = await mkdtemp(join(output, 'run-'));
    const fixture = new URL('../../../Tests/Fixtures/EditorMcpSmoke/', import.meta.url);
    const project = join(run, 'Project');
    await cp(fixture, project, { recursive: true });
    context.diagnostic(`Editor log: ${join(run, 'Editor.log')}`);

    const log = await open(join(run, 'Editor.log'), 'w');
    const editor = spawn(executable, ['--project', project, '--hidden', '--warp'], {
        cwd: project,
        env: { ...process.env, FBZZ_JOB_BREAKAWAY_DONE: '1', FBZZ_SDK_ROOT: '' },
        stdio: ['ignore', log.fd, log.fd],
        windowsHide: true,
    });
    const stopped = new Promise<void>(done => editor.once('close', done));
    let startError: Error | undefined;
    editor.on('error', error => { startError = error; });
    const client = new Client({ name: 'fbzz-editor-smoke', version: '0.1.0' });
    const transport = new StdioClientTransport({
        command: process.execPath,
        args: [fileURLToPath(new URL('../stdio.js', import.meta.url))],
        env: {
            FBZZ_MCP_PERMISSION: 'write',
            FBZZ_EDITOR_PIPE: '\\\\.\\pipe\\FBZZEditorCommandBus',
            FBZZ_EDITOR_BUS_TIMEOUT_MS: String(BUS_TIMEOUT_MS),
        },
        stderr: 'inherit',
    });

    try {
        await client.connect(transport, { timeout: 15000 });
        let authoring: Awaited<ReturnType<typeof Call>> | undefined;
        const connectDeadline = performance.now() + 90000;
        while (performance.now() < connectDeadline) {
            if (startError) throw startError;
            assert.equal(editor.exitCode, null, `Editor exited during startup (${editor.exitCode})`);
            try {
                authoring = (await Call(client, 'editor_op_query', { id: 'authoring.context' })).data;
                break;
            } catch (error) {
                /// @note WARP はパイプの起動後も初回描画の準備を続ける。照会だけを再試行する。
                if (!/ENOENT|EBUSY|timeout|timed out|応答が \d+ms を超えました/i.test(String(error))) throw error;
                await delay(250);
            }
        }
        if (!authoring && editor.pid && editor.exitCode === null) await CaptureStartupStacks(editor.pid, executable, run);
        assert.ok(authoring, 'Editor opening the test project did not respond within 90 seconds');
        assert.equal(await realpath(authoring.projectRoot), await realpath(project), 'Connected Editor must own the fixture copy');
        assert.equal(await realpath(authoring.scenePath), await realpath(join(project, SCENE_PATH)));
        assert.equal(authoring.mode, 'edit');
        assert.equal(authoring.sceneDirty, false);
        await ExpectName(client, ORIGINAL_NAME);
        const beforeImage = await CaptureGame(client, project, 'BeforeEdit');
        const beforeSource = beforeImage.provenance as Record<string, unknown>;
        assert.equal(await realpath(String(beforeSource.scenePath)), await realpath(join(project, SCENE_PATH)));
        assert.equal(String(beforeSource.sceneGeneration), authoring.sceneGeneration);
        assert.equal(beforeSource.cameraId, CAMERA_ID);
        assert.equal(beforeSource.view, 'game');
        assert.equal(beforeSource.renderPass, 'ViewportOutput');
        assert.ok((beforeSource.passes as string[]).includes('UIPass'));
        assert.ok(Number.isSafeInteger(beforeSource.frame));
        await Call(client, 'editor_op_invoke', {
            id: 'renderpass.configure', args: { settings: { view: 'game', pass: 'UIPass', output: 'Output / Color 0' } },
        });

        const openOperators = await Call(client, 'editor_op_list', { search: 'scene.open' });
        const dialog = openOperators.operators.find((operator: { id: string }) => operator.id === 'scene.open');
        assert.equal(dialog.dialog, 'required');
        assert.equal(dialog.availability.code, 'DIALOG_REQUIRED');
        const preview = await Call(client, 'editor_op_invoke', { id: 'scene.open', dryRun: true });
        assert.equal(preview.dryRun, true);
        assert.equal(preview.availability.code, 'DIALOG_REQUIRED');
        const emptyUndo = await Call(client, 'editor_op_invoke', { id: 'edit.undo', dryRun: true });
        assert.equal(emptyUndo.availability.code, 'POLL_REJECTED');

        const importOperators = await Call(client, 'editor_op_list', { search: 'asset.model.import' });
        const modelImport = importOperators.operators.find((operator: { id: string }) => operator.id === 'asset.model.import');
        assert.deepEqual(modelImport.requiredArgs, ['path']);
        assert.equal(modelImport.dialog, 'none');
        assert.equal(modelImport.availability.code, 'ARGUMENTS_REQUIRED');

        await Call(client, 'node_rename', { id: OBJECT_ID, name: SAVED_NAME });
        await ExpectName(client, SAVED_NAME);
        const editedState = await Call(client, 'editor_get_state');
        assert.equal(editedState.sceneDirty, true);
        const afterImage = await CaptureGame(client, project, 'AfterEdit', editedState.frameIndex);
        const afterSource = afterImage.provenance as Record<string, unknown>;
        assert.equal(afterSource.sceneGeneration, beforeSource.sceneGeneration);
        assert.equal(afterSource.cameraId, CAMERA_ID);
        assert.ok(Number(afterSource.frame) >= editedState.frameIndex && Number(afterSource.frame) > Number(beforeSource.frame));
        const objects = afterImage.objects as Array<{ id: string; name: string }>;
        assert.equal(objects.find(object => object.id === OBJECT_ID)?.name, SAVED_NAME);
        const passArgs = { id: 'renderpass.capture', args: { name: 'GameUi.png', minFrame: editedState.frameIndex } };
        const pass = (await Call(client, 'editor_op_invoke', passArgs)).data;
        assert.equal(pass.provenance.cameraId, CAMERA_ID);
        assert.equal(pass.provenance.sceneGeneration, beforeSource.sceneGeneration);
        assert.equal(pass.provenance.renderPass, 'UIPass');
        assert.equal(pass.provenance.output, 'Output / Color 0');
        assert.ok(pass.provenance.frame >= editedState.frameIndex);
        await assert.rejects(Call(client, 'editor_op_invoke', {
            id: 'renderpass.capture', args: { name: 'Stale.png', minFrame: Number.MAX_SAFE_INTEGER },
        }), /CAPTURE_NOT_READY/);
        await assert.rejects(readFile(join(project, 'Library', 'Captures', 'Stale.png')), { code: 'ENOENT' });
        context.diagnostic(`UIPass frame ${pass.provenance.frame} was confirmed, while a readback below the requested frame was refused.`);
        context.diagnostic(`Viewport source advanced from frame ${beforeSource.frame} to ${afterSource.frame} after same-scene edit.`);
        assert.equal((await Call(client, 'scene_save')).dirty, false);
        const opened = await Call(client, 'scene_open', { path: SCENE_PATH });
        assert.equal(opened.opened, SCENE_PATH);
        assert.equal(opened.undoCleared, true);
        const reloaded = (await Call(client, 'editor_op_query', { id: 'authoring.context' })).data;
        assert.notEqual(reloaded.sceneGeneration, authoring.sceneGeneration);
        await ExpectName(client, SAVED_NAME);

        const listed = await Call(client, 'scenario_list');
        assert.ok(listed.playtests.some((item: { path: string }) => item.path === SCENARIO_PATH));
        const report = await WaitForScenario(client, SCENARIO_PATH, SCENARIO_NAME);
        const persisted = JSON.parse(await readFile(join(project, 'Library', 'Playtests', SCENARIO_NAME, 'report.json'), 'utf8'));
        assert.deepEqual(persisted, report);
        assert.equal((await Call(client, 'editor_get_state')).playState, 'editor');
        const freshness = await WaitForScenario(client, CAPTURE_SCENARIO, 'EditorMcpCaptureFreshness');
        const [before, renamed, viewport, renderpass] = freshness.steps.slice(1, 5);
        assert.equal(renamed.frame, before.frame);
        assert.equal(viewport.frame, renamed.frame);
        assert.equal(renderpass.frame, renamed.frame);
        assert.equal(viewport.message, 'expected CAPTURE_NOT_READY');
        assert.equal(renderpass.message, 'expected CAPTURE_NOT_READY');
        assert.ok(freshness.steps[6].result.data.provenance.frame > before.result.data.provenance.frame);
        await assert.rejects(readFile(join(project, 'Library', 'Captures', 'RejectedStalePass.png')), { code: 'ENOENT' });
        await ExpectName(client, SAVED_NAME);
        context.diagnostic('Real Editor refused both the viewport and pass PNG in the same frame as a scene edit; a later rendered pass succeeded.');
        await VerifyAssetMoveBatch(client, project, context);
    } finally {
        try {
            await client.close();
        } finally {
            editor.kill();
            await stopped;
            await log.close();
        }
    }
});
