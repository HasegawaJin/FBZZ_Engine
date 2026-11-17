/// @file    EditorSmoke.integration.ts
/// @brief   MCP stdio から実 Editor の保存・再読込・Playtest を検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-17
import assert from 'node:assert/strict';
import { spawn, spawnSync } from 'node:child_process';
import { cp, mkdir, mkdtemp, open, readFile, realpath, writeFile } from 'node:fs/promises';
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
const OBJECT_ID = '6a4efced-4563-4cbe-9e3d-7c7212cf6d2b';
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
    const editor = spawn(executable, ['--project', project, '--hidden', '--warp', '--no-viewports'], {
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
        assert.equal((await Call(client, 'editor_get_state')).sceneDirty, true);
        assert.equal((await Call(client, 'scene_save')).dirty, false);
        const opened = await Call(client, 'scene_open', { path: SCENE_PATH });
        assert.equal(opened.opened, SCENE_PATH);
        assert.equal(opened.undoCleared, true);
        const reloaded = (await Call(client, 'editor_op_query', { id: 'authoring.context' })).data;
        assert.notEqual(reloaded.sceneGeneration, authoring.sceneGeneration);
        await ExpectName(client, SAVED_NAME);

        const listed = await Call(client, 'scenario_list');
        assert.ok(listed.playtests.some((item: { path: string }) => item.path === SCENARIO_PATH));
        const launched = await Call(client, 'scenario_run', { path: SCENARIO_PATH });
        assert.equal(launched.started, true);
        assert.equal(launched.name, SCENARIO_NAME);
        const playDeadline = performance.now() + 60000;
        let report = await Call(client, 'scenario_status');
        while (report.state === 'running' && performance.now() < playDeadline) {
            await delay(250);
            report = await Call(client, 'scenario_status');
        }
        assert.equal(report.name, SCENARIO_NAME);
        assert.equal(report.state, 'passed', JSON.stringify(report));
        const persisted = JSON.parse(await readFile(join(project, 'Library', 'Playtests', SCENARIO_NAME, 'report.json'), 'utf8'));
        assert.deepEqual(persisted, report);
        assert.equal((await Call(client, 'editor_get_state')).playState, 'editor');
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
