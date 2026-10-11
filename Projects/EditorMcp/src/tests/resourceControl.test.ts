/// @file    resourceControl.test.ts
/// @brief   Isolated ownership, expiry, execution and cross-process regression tests.
/// @author  Hasegawa Jin
/// @date    2026-10-11
import assert from 'node:assert/strict';
import { spawn, type ChildProcess } from 'node:child_process';
import { once } from 'node:events';
import { mkdtempSync, readdirSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import test, { type TestContext } from 'node:test';
import { fileURLToPath } from 'node:url';
import { EXECUTION_RESOURCE, ResourceControl, ResourceControlError, ResourceNamespace, type ResourceOperation } from '../reliability/ResourceControl.js';

function Fixture(t: TestContext): string {
    const directory = mkdtempSync(join(tmpdir(), 'fbzz-resource-fixture-'));
    t.after(() => {
        const checked = resolve(directory);
        assert.ok(checked.startsWith(`${resolve(tmpdir())}${process.platform === 'win32' ? '\\' : '/'}fbzz-resource-fixture-`));
        rmSync(checked, { recursive: true, force: true });
    });
    return directory;
}

function Code(code: string): (error: unknown) => boolean {
    return (error: unknown) => error instanceof ResourceControlError && error.code === code;
}

function Deferred<T>(): { promise: Promise<T>; resolve: (value: T | PromiseLike<T>) => void } {
    let complete!: (value: T | PromiseLike<T>) => void;
    const promise = new Promise<T>((resolvePromise) => { complete = resolvePromise; });
    return { promise, resolve: complete };
}

test('ownership requires capability; hierarchical targets conflict and namespace scopes remain independent', async (t) => {
    const directory = Fixture(t);
    const first = new ResourceControl('editor-A/project-A', directory);
    const second = new ResourceControl('editor-A/project-A', directory);
    const otherProject = new ResourceControl('editor-A/project-B', directory);
    const lease = await first.Acquire('AI-A', ['scene/entity/123', 'scene/entity/123'], 30000);
    assert.deepEqual(lease.targets, ['scene/entity/123']);
    assert.match(lease.capability, /^[0-9a-f]{64}$/);
    await assert.rejects(second.Acquire('AI-A', ['scene/entity/123/Transform'], 30000), Code('RESOURCE_BUSY'));
    await assert.rejects(second.Acquire('AI-B', ['scene'], 30000), Code('RESOURCE_BUSY'));
    await assert.rejects(second.Acquire('AI-B', ['*'], 30000), Code('RESOURCE_BUSY'));
    const otherEntity = await second.Acquire('AI-B', ['scene/entity/1234'], 30000);
    const isolated = await otherProject.Acquire('AI-B', ['scene/entity/123'], 30000);
    await assert.rejects(first.Assert('AI-B', lease, ['scene/entity/123']), Code('NOT_OWNER'));
    await assert.rejects(first.Renew('AI-B', lease, 30000), Code('NOT_OWNER'));
    await assert.rejects(first.Release('AI-B', lease), Code('NOT_OWNER'));
    const forged = { ...lease, capability: '0'.repeat(64) };
    await assert.rejects(second.Assert('AI-A', forged, ['scene/entity/123']), Code('NOT_OWNER'));
    await assert.rejects(second.Renew('AI-A', forged, 30000), Code('NOT_OWNER'));
    await assert.rejects(second.Release('AI-A', forged), Code('NOT_OWNER'));
    await assert.rejects(first.Assert('AI-A', lease, ['scene/entity/999']), Code('TARGET_NOT_OWNED'));
    const observations = await second.Inspect();
    assert.equal(observations.length, 2);
    assert.ok(!JSON.stringify(observations).includes(lease.capability));
    assert.ok(!JSON.stringify(observations).includes('capabilityHash'));
    assert.equal(await second.Release('AI-A', lease), true);
    assert.equal(await second.Release('AI-A', lease), false);
    await second.Release('AI-B', otherEntity);
    await otherProject.Release('AI-B', isolated);
});

test('expiry and renewal use exact boundaries and never revive an expired lease', async (t) => {
    let now = 100000;
    const control = new ResourceControl('expiry', Fixture(t), { now: () => now });
    const lease = await control.Acquire('AI-A', ['scene'], 100);
    now += 50;
    const renewed = await control.Renew('AI-A', lease, 200);
    assert.equal(renewed.expiresAt, now + 200);
    now = renewed.expiresAt;
    await assert.rejects(control.Assert('AI-A', lease, ['scene']), Code('LEASE_EXPIRED_OR_MISSING'));
    await assert.rejects(control.Renew('AI-A', lease, 100), Code('LEASE_EXPIRED_OR_MISSING'));
    const replacement = await control.Acquire('AI-B', ['scene'], 100);
    assert.notEqual(replacement.leaseId, lease.leaseId);
    assert.equal(await control.Release('AI-A', lease), false);
    assert.equal((await control.Inspect())[0].owner, 'AI-B');
    for (const ttl of [0, -1, 0.5, NaN, Infinity, 86400001]) {
        await assert.rejects(control.Acquire('AI-C', ['asset'], ttl), Code('INVALID_ARGUMENT'));
    }
    for (const targets of [[], [''], ['scene/'], ['/scene'], ['scene//entity'], ['scene\\entity'], [' scene'], ['scene*']]) {
        await assert.rejects(control.Acquire('AI-C', targets, 10), Code('INVALID_ARGUMENT'));
    }
});

test('live operation pins survive expiry, reject early release and same-token overlap, and release after failure', async (t) => {
    let now = 100000;
    const directory = Fixture(t);
    const first = new ResourceControl('pin', directory, { now: () => now });
    const second = new ResourceControl('pin', directory, { now: () => now });
    const lease = await first.Acquire('AI-A', ['scene'], 10);
    let called = false;
    await assert.rejects(first.RunWithLease('AI-A', lease, ['assets'], () => { called = true; }), Code('TARGET_NOT_OWNED'));
    assert.equal(called, false);
    const entered = Deferred<ResourceOperation>();
    const finish = Deferred<void>();
    const failure = new Error('fixture mutation failed');
    const running = first.RunWithLease('AI-A', lease, ['scene/entity/1'], async (context) => {
        entered.resolve(context);
        await finish.promise;
        throw failure;
    }, 'same-display-name');
    const operation = await entered.promise;
    assert.equal(operation.jobName, 'same-display-name');
    await assert.rejects(second.RunWithLease('AI-A', lease, ['scene/entity/1/Transform'], () => undefined), Code('RESOURCE_BUSY'));
    await assert.rejects(second.Release('AI-A', lease), Code('ACTIVE_OPERATION'));
    now += 11;
    await assert.rejects(second.Acquire('AI-B', ['scene/entity/1'], 100), Code('RESOURCE_BUSY'));
    await assert.rejects(second.Renew('AI-A', lease, 100), Code('LEASE_EXPIRED_OR_MISSING'));
    await assert.rejects(second.Release('AI-A', lease), Code('ACTIVE_OPERATION'));
    assert.equal((await second.Inspect())[0].expired, true);
    const rejection = assert.rejects(running, (error) => error === failure);
    finish.resolve();
    await rejection;
    assert.deepEqual(await second.Inspect(), []);
    const replacement = await second.Acquire('AI-B', ['scene'], 100);
    await second.Release('AI-B', replacement);
});

test('disjoint jobs sharing one lease have unique IDs even with identical display names', async (t) => {
    const control = new ResourceControl('job-identities', Fixture(t));
    const lease = await control.Acquire('AI', ['scene'], 30000);
    const firstEntered = Deferred<ResourceOperation>();
    const secondEntered = Deferred<ResourceOperation>();
    const finish = Deferred<void>();
    const first = control.RunWithLease('AI', lease, ['scene/entity/1'], async (context) => {
        firstEntered.resolve(context);
        await finish.promise;
    }, 'same-name');
    await firstEntered.promise;
    const second = control.RunWithLease('AI', lease, ['scene/entity/2'], async (context) => {
        secondEntered.resolve(context);
        await finish.promise;
    }, 'same-name');
    const one = await firstEntered.promise;
    const two = await secondEntered.promise;
    assert.notEqual(one.jobId, two.jobId);
    assert.equal((await control.Inspect())[0].operations.length, 2);
    finish.resolve();
    await Promise.all([first, second]);
    assert.equal((await control.Inspect())[0].operations.length, 0);
    await control.Release('AI', lease);
});

test('build and Editor executions share one target and failure cleanup permits the next job', async (t) => {
    const directory = Fixture(t);
    const build = new ResourceControl('shared-endpoint-project', directory);
    const editor = new ResourceControl('shared-endpoint-project', directory);
    const entered = Deferred<ResourceOperation>();
    const finish = Deferred<void>();
    const buildFailure = new Error('fixture build failed');
    const running = build.RunExclusive('builder', 'native-build', async (context) => {
        entered.resolve(context);
        await finish.promise;
        throw buildFailure;
    });
    assert.deepEqual((await entered.promise).targets, [EXECUTION_RESOURCE]);
    await assert.rejects(editor.RunExclusive('editor', 'editor-render', () => undefined, { targets: ['scene'] }), Code('RESOURCE_BUSY'));
    await assert.rejects(editor.RunExclusive('builder', 'native-build', () => undefined), Code('RESOURCE_BUSY'));
    const rejected = assert.rejects(running, (error) => error === buildFailure);
    finish.resolve();
    await rejected;
    assert.equal(await editor.RunExclusive('editor', 'editor-render', () => 'rendered'), 'rendered');
    assert.deepEqual(await editor.Inspect(), []);
});

test('concurrent acquisition from independent control instances grants exactly one claim', async (t) => {
    const directory = Fixture(t);
    const outcomes = await Promise.all(Array.from({ length: 20 }, (_, index) => {
        const control = new ResourceControl('same-process-contention', directory);
        return control.Acquire(`AI-${index}`, ['scene'], 30000).then(() => 'acquired', (error: unknown) => {
            assert.ok(Code('RESOURCE_BUSY')(error));
            return 'busy';
        });
    }));
    assert.equal(outcomes.filter((result) => result === 'acquired').length, 1);
});

interface WorkerMessage {
    type: string;
    ok?: boolean;
    code?: string;
    jobId?: string;
    owner?: string;
}

function Worker(t: TestContext, script: string, directory: string, namespace: string): ChildProcess {
    const moduleUrl = new URL('../reliability/ResourceControl.js', import.meta.url).href;
    const child = spawn(process.execPath, ['--input-type=module', '-e', script, moduleUrl, directory, namespace], { stdio: ['ignore', 'pipe', 'pipe', 'ipc'] });
    let stderr = '';
    child.stderr?.setEncoding('utf8');
    child.stderr?.on('data', (chunk: string) => { stderr += chunk; });
    child.on('error', (error) => { stderr += error.message; });
    t.after(async () => {
        if (child.exitCode === null && child.signalCode === null) {
            const exited = once(child, 'exit');
            child.kill();
            await exited;
        }
        assert.equal(stderr, '', stderr);
    });
    return child;
}

function Message(child: ChildProcess, type: string): Promise<WorkerMessage> {
    return new Promise<WorkerMessage>((complete, reject) => {
        const timer = setTimeout(() => { cleanup(); reject(new Error(`worker message timed out: ${type}`)); }, 15000);
        const received = (value: WorkerMessage) => {
            if (value.type === type) { cleanup(); complete(value); }
        };
        const exited = () => { cleanup(); reject(new Error(`worker exited before ${type}`)); };
        function cleanup(): void {
            clearTimeout(timer);
            child.off('message', received);
            child.off('exit', exited);
        }
        child.on('message', received);
        child.on('exit', exited);
    });
}

test('separate processes racing through atomic generations produce one winner without check-then-acquire', { timeout: 25000 }, async (t) => {
    const directory = Fixture(t);
    const script = `
        const { ResourceControl } = await import(process.argv[1]);
        const control = new ResourceControl(process.argv[3], process.argv[2]);
        process.send({type:'ready'});
        process.once('message', async () => {
            try {
                await control.Acquire('worker-' + process.pid, ['execution'], 30000);
                process.send({type:'result', ok:true}, () => process.exit());
            } catch(error) {
                process.send({type:'result', ok:false, code:error.code}, () => process.exit());
            }
        });
    `;
    const children = Array.from({ length: 8 }, () => Worker(t, script, directory, 'process-contention'));
    await Promise.all(children.map((child) => Message(child, 'ready')));
    const resultPromises = children.map((child) => Message(child, 'result'));
    for (const child of children) child.send('start');
    const results = await Promise.all(resultPromises);
    assert.equal(results.filter((result) => result.ok).length, 1);
    assert.ok(results.filter((result) => !result.ok).every((result) => result.code === 'RESOURCE_BUSY'));
    const state = await new ResourceControl('process-contention', directory).Inspect();
    assert.equal(state.length, 1);
});

test('live subprocess pin cannot be reclaimed after expiry; crashed subprocess pin can', { timeout: 25000 }, async (t) => {
    const directory = Fixture(t);
    const script = `
        const { ResourceControl } = await import(process.argv[1]);
        const control = new ResourceControl(process.argv[3], process.argv[2], {now:() => 100000});
        process.on('message', () => {});
        const lease = await control.Acquire('crash-worker', ['execution'], 10);
        await control.RunWithLease('crash-worker', lease, ['execution'], async(context) => {
            process.send({type:'pinned', jobId:context.jobId});
            await new Promise(() => {});
        }, 'crash-fixture');
    `;
    const child = Worker(t, script, directory, 'crash-pin');
    await Message(child, 'pinned');
    const observer = new ResourceControl('crash-pin', directory, { now: () => 100011 });
    await assert.rejects(observer.Acquire('replacement', ['execution'], 10), Code('RESOURCE_BUSY'));
    assert.equal((await observer.Inspect())[0].expired, true);
    const exited = once(child, 'exit');
    assert.equal(child.kill(), true);
    await exited;
    const replacement = await observer.Acquire('replacement', ['execution'], 10);
    assert.equal((await observer.Inspect())[0].owner, 'replacement');
    await observer.Release('replacement', replacement);
});

test('separate processes claiming disjoint targets preserve every concurrent claim', { timeout: 25000 }, async (t) => {
    const directory = Fixture(t);
    const script = `
        const { ResourceControl } = await import(process.argv[1]);
        const control = new ResourceControl(process.argv[3], process.argv[2]);
        process.send({type:'ready'});
        process.once('message', async () => {
            try {
                await control.Acquire('worker-' + process.pid, ['scene/node/' + process.pid], 30000);
                process.send({type:'result', ok:true}, () => process.exit());
            } catch(error) {
                process.send({type:'result', ok:false, code:error.code}, () => process.exit());
            }
        });
    `;
    const children = Array.from({ length: 8 }, () => Worker(t, script, directory, 'disjoint-process-contention'));
    await Promise.all(children.map((child) => Message(child, 'ready')));
    const resultPromises = children.map((child) => Message(child, 'result'));
    for (const child of children) child.send('start');
    const results = await Promise.all(resultPromises);
    assert.ok(results.every((result) => result.ok), JSON.stringify(results));
    const state = await new ResourceControl('disjoint-process-contention', directory).Inspect();
    assert.equal(state.length, children.length);
    assert.equal(new Set(state.map((claim) => claim.leaseId)).size, children.length);
});

test('corrupt state fails closed and abandoned unpublished candidates never become claims', async (t) => {
    const directory = Fixture(t);
    const control = new ResourceControl('corruption', directory);
    await control.Acquire('AI', ['scene'], 30000);
    const namespaceDirectory = join(directory, readdirSync(directory)[0]);
    writeFileSync(join(namespaceDirectory, '.pending-orphan.json'), 'incomplete');
    assert.equal((await control.Inspect()).length, 1);
    const generations = readdirSync(namespaceDirectory).filter((name) => name.startsWith('generation-')).sort();
    writeFileSync(join(namespaceDirectory, generations[generations.length - 1], 'state.json'), '{partial');
    await assert.rejects(control.Acquire('AI-B', ['assets'], 30000), Code('RESOURCE_STATE_INVALID'));
});

test('namespace canonicalization is stable and project-specific', () => {
    const root = fileURLToPath(new URL('../../', import.meta.url));
    assert.equal(ResourceNamespace('pipe-a', root), ResourceNamespace('pipe-a', join(root, '.')));
    assert.notEqual(ResourceNamespace('pipe-a', root), ResourceNamespace('pipe-b', root));
    assert.notEqual(ResourceNamespace('pipe-a', root), ResourceNamespace('pipe-a', join(root, 'another-project')));
    if (process.platform === 'win32') {
        assert.equal(ResourceNamespace('PIPE-A', root.toUpperCase()), ResourceNamespace('pipe-a', root.toLowerCase()));
    }
});
