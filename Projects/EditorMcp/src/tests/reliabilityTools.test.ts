/// @file    reliabilityTools.test.ts
/// @brief   Isolated MCP and session regressions; fake native responses do not verify an engine build.
/// @author  Hasegawa Jin
/// @date    2026-10-11
import assert from 'node:assert/strict';
import { existsSync, mkdtempSync, realpathSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { basename, dirname, join, resolve } from 'node:path';
import test, { type TestContext } from 'node:test';
import { Client } from '@modelcontextprotocol/sdk/client/index.js';
import { InMemoryTransport } from '@modelcontextprotocol/sdk/inMemory.js';
import { EditorBusError, type EditorBus } from '../busClient.js';
import type { PermissionMode } from '../config.js';
import type { EditorCommand, EditorQuery, JsonValue } from '../editorContracts.js';
import { EXECUTION_RESOURCE, ResourceControl, ResourceControlError, type ResourceLease } from '../reliability/ResourceControl.js';
import { ReliabilitySession } from '../reliability/ReliabilityTools.js';
import { CreateEditorMcpServer } from '../server.js';

type RecordValue = Record<string, unknown>;
const Record = (value: unknown): RecordValue => value as RecordValue;
const NODE = '11111111-1111-4111-8111-111111111111';
const OTHER_NODE = '22222222-2222-4222-8222-222222222222';
const Clone = <T>(value: T): T => structuredClone(value);
const Rename = (): EditorCommand[] => [{ t: 'node.rename', id: NODE, name: 'Reviewed name' }];
const CheckStatus = (checks: unknown, key: string): unknown => Record(Record(checks)[key]).status;
const Code = (code: string) => (error: unknown): boolean => error instanceof ResourceControlError && error.code === code;

function Fixture(t: TestContext): string {
    const parent = realpathSync(tmpdir());
    const directory = resolve(mkdtempSync(join(parent, 'fbzz-reliability-fixture-')));
    t.after(() => {
        assert.equal(resolve(directory), directory);
        assert.equal(dirname(realpathSync(directory)), parent);
        assert.ok(basename(directory).startsWith('fbzz-reliability-fixture-'));
        rmSync(directory, { recursive: true, force: true });
    });
    return directory;
}

function Deferred<T>(): { promise: Promise<T>; resolve: (value: T) => void } {
    let complete!: (value: T) => void;
    const promise = new Promise<T>(resolvePromise => { complete = resolvePromise; });
    return { promise, resolve: complete };
}

async function Eventually(check: () => boolean | Promise<boolean>, message: string): Promise<void> {
    const deadline = Date.now() + 4000;
    while (!(await check())) {
        assert.ok(Date.now() < deadline, message);
        await new Promise<void>(complete => setTimeout(complete, 10));
    }
}

/// @note This model supplies contract fixtures only; it never opens a pipe, scene, process, or build.
class NativeFixtureBus implements EditorBus {
    readonly queries: EditorQuery[] = [];
    readonly commands: Array<{ command: EditorCommand; dryRun: boolean }> = [];
    guard: RecordValue = {
        project: 'C:/isolated/project', scene: 'Scenes/fixture.scene', revision: '1',
        snapshot: { nodes: [{ id: NODE, name: 'Original', components: [{ type: 'Transform', fields: { position: [0, 0, 0] } }] }] },
        undo: { revision: '1', index: 0 },
    };
    validation: RecordValue = { errors: 0, warnings: 0, issues: [], coverage: 'existing-reference-kinds' };
    logs: RecordValue = { cursor: 20, count: 0, entries: [], dropped: false };
    native = true;
    failObservation = false;
    failCapture = false;
    failValidation = false;
    checkedFailure: RecordValue | undefined;
    checkedError: Error | undefined;
    rollbackCompleted: (() => void) | undefined;
    preflightMutator: ((command: Extract<EditorCommand, { t: 'editor.checked' }>) => void) | undefined;
    queryHook: ((query: EditorQuery) => Promise<unknown>) | undefined;
    commandHook: ((command: EditorCommand, dryRun: boolean) => Promise<unknown>) | undefined;
    saved: RecordValue | undefined;
    private top: { owner: string; receipt: string; guard: RecordValue; before: RecordValue } | undefined;

    Observation(): RecordValue {
        return {
            schemaVersion: 1, consistency: 'main-thread', guard: Clone(this.guard),
            state: { playState: 'editor', restorePending: false, sceneDirty: true },
            selection: { ids: [NODE], active: NODE }, snapshot: Clone(this.guard.snapshot),
            components: [{ id: NODE, type: 'Transform', fields: { position: [0, 0, 0] } }],
            validation: Clone(this.validation), logs: Clone(this.logs),
            build: { building: false, managed: { busy: false, request: { managedRequestId: 'managed:7', state: 'applied', terminal: true } } },
            undoHistory: { index: 1, entries: [{ label: 'fixture edit' }] },
            checks: { nativeComponents: { status: 'success' }, managedFields: { status: 'unverified', reason: 'Use existing script observation API.' } },
        };
    }

    async Query(query: EditorQuery): Promise<unknown> {
        this.queries.push(Clone(query));
        if (this.queryHook) return this.queryHook(query);
        switch (query.t) {
            case 'editor.observation':
                if (!this.native) throw new EditorBusError('UNKNOWN_QUERY', 'Older fixture Editor');
                if (this.failObservation) throw new EditorBusError('BUS_TIMEOUT', 'Fixture post-observation unavailable');
                return this.Observation();
            case 'editor.state': return { playState: 'editor', restorePending: false, managedBuildBusy: false };
            case 'scene.selection': return { ids: [NODE] };
            case 'scene.snapshot': return Clone(this.guard.snapshot);
            case 'scene.validate':
                if (this.failValidation) throw new EditorBusError('UNSUPPORTED', 'Fixture validation unavailable');
                return Clone(this.validation);
            case 'console.logs': return Clone(this.logs);
            case 'editor.undoHistory': return { entries: [] };
            case 'build.status': return { building: false, managedBuildBusy: false, managed: { busy: false } };
            case 'viewport.semantic':
                if (this.failCapture) throw new Error('Fixture capture failed');
                return { mimeType: 'image/png', base64: 'AA==', width: 1, height: 1, cameraPosition: [0, 0, 0], objects: [] };
            default: return { fixture: query.t };
        }
    }

    async Command(command: EditorCommand, dryRun: boolean): Promise<unknown> {
        this.commands.push({ command: Clone(command), dryRun });
        if (this.commandHook) return this.commandHook(command, dryRun);
        if (command.t === 'editor.checked') {
            if (dryRun) {
                const changes = Clone(command.cmds);
                this.preflightMutator?.(command);
                return { dryRun: true, preflight: { status: 'success' }, changes, atomic: false };
            }
            if (this.checkedError) throw this.checkedError;
            if (this.checkedFailure) return Clone(this.checkedFailure);
            if (JSON.stringify(command.guard) !== JSON.stringify(this.guard)) return { status: 'failure', code: 'STALE_GUARD', applied: false };
            const before = Clone(this.guard);
            this.guard.revision = String(Number(this.guard.revision) + 1);
            this.guard.snapshot = { nodes: [{ id: NODE, commands: Clone(command.cmds) }] };
            this.top = { owner: command.owner, receipt: command.receipt, guard: Clone(this.guard), before };
            return { status: 'success', guard: Clone(this.guard), readback: Clone(command.cmds), atomic: false };
        }
        if (command.t === 'editor.rollback') {
            if (!this.top || command.owner !== this.top.owner || command.receipt !== this.top.receipt || JSON.stringify(this.guard) !== JSON.stringify(this.top.guard))
                return { status: 'failure', code: 'ROLLBACK_CONFLICT', undone: false };
            this.guard = Clone(this.top.before);
            this.top = undefined;
            this.rollbackCompleted?.();
            return { status: 'success', undone: true };
        }
        if (command.t === 'editor.finalize') {
            if (dryRun) return { save: { status: 'unverified' }, reload: { status: 'unverified' } };
            if (command.reload && !command.save) return { save: { status: 'unverified' }, reload: { status: 'failure', reason: 'Save must be explicitly requested.' } };
            if (JSON.stringify(command.guard) !== JSON.stringify(this.guard)) return { save: { status: 'failure' }, reload: { status: 'failure' } };
            if (command.save) this.saved = Clone(this.guard);
            if (command.reload) this.guard.revision = String(Number(this.guard.revision) + 1);
            return {
                save: { status: command.save ? 'success' : 'unverified' }, reload: { status: command.reload ? 'success' : 'unverified' },
                observation: { guard: Clone(this.guard) },
            };
        }
        if (command.t === 'build.run') return { managedRequestId: 'managed:8', accepted: true };
        throw new Error(`Unexpected fixture command ${command.t}`);
    }

    Close(): void {}
}

function SessionFixture(t: TestContext, permission: PermissionMode = 'write', now: () => number = Date.now) {
    const directory = Fixture(t);
    const control = new ResourceControl('isolated-reliability-test', directory, { now });
    const bus = new NativeFixtureBus();
    const session = new ReliabilitySession(bus, permission, control, now);
    return { directory, control, bus, session };
}

async function Applied(t: TestContext, configure?: (bus: NativeFixtureBus) => void) {
    const fixture = SessionFixture(t);
    configure?.(fixture.bus);
    const plan = await fixture.session.Plan(Rename(), 'fixture edit');
    const claim = await fixture.session.Claim([NODE], 60000, 'enforced');
    const lease = claim.lease as ResourceLease;
    const result = await fixture.session.Apply(String(plan.planId), lease);
    assert.equal(result.status, 'success');
    return { ...fixture, lease, receipt: String(result.receipt), result };
}

async function Connected(t: TestContext, bus: EditorBus, permission: PermissionMode) {
    const server = CreateEditorMcpServer(bus, permission);
    const client = new Client({ name: 'reliability-fixture', version: '1.0.0' });
    const [clientTransport, serverTransport] = InMemoryTransport.createLinkedPair();
    await server.connect(serverTransport);
    await client.connect(clientTransport);
    t.after(async () => { await client.close(); await server.close(); });
    return client;
}

test('read MCP registration and legacy queries remain registry-free and expose no reliability mutation tools', async t => {
    const directory = Fixture(t);
    const previous = { TEMP: process.env.TEMP, TMP: process.env.TMP, TMPDIR: process.env.TMPDIR };
    try {
        process.env.TEMP = directory; process.env.TMP = directory; process.env.TMPDIR = directory;
        assert.equal(resolve(tmpdir()), directory);
        const bus = new NativeFixtureBus();
        const client = await Connected(t, bus, 'read');
        const { tools } = await client.listTools();
        for (const name of ['editor_observe_structured', 'editor_resource_status', 'editor_exclusive_job_status'])
            assert.equal(tools.find(tool => tool.name === name)?.annotations?.readOnlyHint, true);
        for (const name of ['editor_resource_claim', 'editor_resource_renew', 'editor_resource_release', 'editor_plan_checked', 'editor_apply_checked', 'editor_rollback_checked', 'editor_verify_checked', 'editor_build_managed_exclusive'])
            assert.equal(tools.some(tool => tool.name === name), false, name);
        for (const name of ['editor_catalog', 'scene_get_tree', 'editor_observe_structured'])
            assert.notEqual((await client.callTool({ name, arguments: {} })).isError, true);
        assert.equal(bus.commands.length, 0);
        assert.equal(existsSync(join(directory, 'fbzz-editor-mcp-coordination')), false);
    } finally {
        for (const key of ['TEMP', 'TMP', 'TMPDIR'] as const) {
            if (previous[key] === undefined) delete process.env[key]; else process.env[key] = previous[key];
        }
    }
});

test('native structured observation preserves section availability, selections, components, references, build and cursors', async t => {
    const { bus, session, control } = SessionFixture(t, 'read');
    const expected = bus.Observation();
    const observation = await session.Observe({ ids: [NODE], afterSequence: 10, managedRequestId: 'managed:7' });
    const { owner, ...body } = observation;
    assert.equal(owner, session.owner);
    assert.deepEqual(body, expected);
    assert.deepEqual(bus.queries, [{ t: 'editor.observation', ids: [NODE], afterSequence: 10, managedRequestId: 'managed:7' }]);
    assert.deepEqual(await control.Inspect(), []);
});

test('old Editor observation fallback is multi-call unverified and cannot make a checked plan', async t => {
    const { bus, session, control } = SessionFixture(t);
    bus.native = false;
    bus.failValidation = true;
    const observation = await session.Observe({ afterSequence: 9, managedRequestId: 'managed:7' });
    assert.equal(observation.consistency, 'multi-call-unverified');
    assert.equal(observation.guard, undefined);
    assert.equal(CheckStatus(observation.checks, 'guard'), 'unverified');
    assert.equal(CheckStatus(observation.checks, 'validation'), 'unverified');
    assert.ok(bus.queries.some(query => query.t === 'console.logs' && query.afterSequence === 9));
    assert.ok(bus.queries.some(query => query.t === 'build.status' && query.managedRequestId === 'managed:7'));
    await assert.rejects(session.Plan(Rename(), 'unsupported fixture'), /Native guard is unavailable/);
    assert.equal(bus.commands.length, 0);
    assert.deepEqual(await control.Inspect(), []);
});

test('MCP transport errors report unverified without automatic replay', async t => {
    const bus = new NativeFixtureBus();
    bus.failObservation = true;
    const client = await Connected(t, bus, 'write');
    const response = await client.callTool({ name: 'editor_observe_structured', arguments: {} });
    const result = Record(response.structuredContent);
    assert.equal(result.status, 'unverified');
    assert.equal(result.code, 'BUS_TIMEOUT');
    assert.equal(result.automaticReplay, false);
    assert.notEqual(response.isError, true);
    assert.equal(bus.queries.length, 1);
    assert.equal(bus.commands.length, 0);
});

test('dry-run plans and trial ownership never mutate or acquire a lease', async t => {
    const { bus, session, control } = SessionFixture(t, 'dry-run');
    const plan = await session.Plan(Rename(), 'review only');
    assert.equal(plan.status, 'unverified');
    assert.equal(plan.atomic, false);
    assert.equal(CheckStatus({ preflight: plan.preflight }, 'preflight'), 'success');
    assert.equal(bus.commands.length, 1);
    assert.equal(bus.commands[0].dryRun, true);
    const claim = await session.Claim([NODE], 60000, 'enforced');
    assert.equal(claim.mode, 'trial');
    assert.equal(claim.lease, undefined);
    const result = await session.Apply(String(plan.planId), { leaseId: 'unclaimed', capability: 'unclaimed' });
    assert.equal(result.status, 'unverified');
    assert.equal(bus.commands.every(call => call.dryRun), true);
    assert.deepEqual(await control.Inspect(), []);
});

test('dry-run MCP plans expose reviewed differences while checked apply sends no mutation', async t => {
    const bus = new NativeFixtureBus();
    const client = await Connected(t, bus, 'dry-run');
    const preview = await client.callTool({ name: 'editor_plan_checked', arguments: { label: 'MCP review fixture', cmds: Rename() } });
    const plan = Record(preview.structuredContent);
    assert.notEqual(preview.isError, true);
    assert.equal(plan.status, 'unverified');
    assert.equal(plan.atomic, false);
    assert.deepEqual(plan.changes, Rename());
    const apply = await client.callTool({ name: 'editor_apply_checked', arguments: {
        planId: plan.planId, token: { leaseId: '33333333-3333-4333-8333-333333333333', capability: '0'.repeat(64) },
    } });
    assert.equal(Record(apply.structuredContent).status, 'unverified');
    assert.notEqual(apply.isError, true);
    assert.equal(bus.commands.length, 1);
    assert.equal(bus.commands[0].dryRun, true);
});

test('checked preflight requires both native success and dry-run acknowledgement before a plan exists', async t => {
    for (const preview of [
        { dryRun: true, preflight: { status: 'failure' } },
        { dryRun: false, preflight: { status: 'success' } },
        { dryRun: true, preflight: { status: 'unverified' } },
    ]) await t.test(JSON.stringify(preview), async child => {
        const { bus, session, control } = SessionFixture(child);
        bus.commandHook = async (_command, dryRun) => { assert.equal(dryRun, true); return preview; };
        await assert.rejects(session.Plan(Rename(), 'rejected preflight fixture'), /no plan was created/);
        assert.equal(bus.commands.length, 1);
        assert.equal(bus.commands[0].dryRun, true);
        assert.deepEqual(await control.Inspect(), []);
    });
});

test('plans bind immutable commands and guard despite caller and dry-run bus mutation; tokens are single-use', async t => {
    const { bus, session, control } = SessionFixture(t);
    const cmds: EditorCommand[] = [{ t: 'component.set', id: NODE, comp: 'Transform', field: 'position', value: [1, 2, 3] }];
    bus.preflightMutator = command => {
        const field = command.cmds[0];
        if (field.t === 'component.set') field.value = [99, 99, 99];
        Record(command.guard).revision = 'corrupted preflight';
    };
    const beforeGuard = Clone(bus.guard);
    const plan = await session.Plan(cmds, 'immutable fixture');
    const changed = cmds[0];
    if (changed.t === 'component.set') (changed.value as JsonValue[])[0] = 90;
    cmds.push({ t: 'node.rename', id: OTHER_NODE, name: 'Unreviewed' });
    const lease = (await session.Claim([NODE], 60000, 'enforced')).lease as ResourceLease;
    const result = await session.Apply(String(plan.planId), lease);
    assert.equal(result.status, 'success');
    const applied = bus.commands.find(call => !call.dryRun)!.command;
    assert.equal(applied.t, 'editor.checked');
    if (applied.t !== 'editor.checked') assert.fail();
    assert.deepEqual(applied.cmds, [{ t: 'component.set', id: NODE, comp: 'Transform', field: 'position', value: [1, 2, 3] }]);
    assert.deepEqual(applied.guard, beforeGuard);
    await assert.rejects(session.Apply(String(plan.planId), lease), /expired or was consumed/);
    assert.equal(bus.commands.filter(call => !call.dryRun).length, 1);
    await session.Release(lease);
    assert.deepEqual(await control.Inspect(), []);
});

test('plan expiry uses its exact fixture clock and sends no mutation at the deadline', async t => {
    let now = 100000;
    const { bus, session } = SessionFixture(t, 'write', () => now);
    const plan = await session.Plan(Rename(), 'expiring fixture');
    const lease = (await session.Claim([NODE], 120000, 'enforced')).lease as ResourceLease;
    now = Number(plan.expiresAt);
    await assert.rejects(session.Apply(String(plan.planId), lease), /expired or was consumed/);
    assert.equal(bus.commands.filter(call => !call.dryRun).length, 0);
    await session.Release(lease);
});

test('apply requires the correct enforced capability, owner and target coverage before native dispatch', async t => {
    const { bus, session, control } = SessionFixture(t);
    const other = new ReliabilitySession(bus, 'write', control);
    const plan = await session.Plan(Rename(), 'ownership fixture');
    const trial = await session.Claim([NODE], 60000, 'trial');
    assert.equal(trial.lease, undefined);
    const foreign = await control.Acquire(other.owner, plan.targets as string[], 60000);
    await assert.rejects(session.Apply(String(plan.planId), foreign), Code('NOT_OWNER'));
    assert.equal((await control.Inspect())[0].owner, other.owner);
    await other.Release(foreign);
    const wrongTarget = await control.Acquire(session.owner, ['asset/fixture'], 60000);
    await assert.rejects(session.Apply(String(plan.planId), wrongTarget), Code('TARGET_NOT_OWNED'));
    const lease = (await session.Claim([NODE], 60000, 'enforced')).lease as ResourceLease;
    await assert.rejects(session.Apply(String(plan.planId), { ...lease, capability: '0'.repeat(64) }), Code('NOT_OWNER'));
    assert.equal(bus.commands.filter(call => !call.dryRun).length, 0);
    assert.equal((await session.Apply(String(plan.planId), lease)).status, 'success');
    await session.Release(lease);
});

test('node UUID case aliases conflict across distinct authors and retain one canonical owner', async t => {
    const id = 'aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee';
    for (const firstId of [id, id.toUpperCase()]) await t.test(`first claim ${firstId}`, async child => {
        const { bus, session, control } = SessionFixture(child);
        bus.guard.snapshot = { nodes: [{ id, name: 'Case fixture node' }] };
        const other = new ReliabilitySession(bus, 'write', control);
        assert.notEqual(other.owner, session.owner);
        const alias = firstId === id ? id.toUpperCase() : id;
        const lease = (await session.Claim([firstId], 60000, 'enforced')).lease as ResourceLease;
        try {
            assert.equal(lease.targets.length, 1);
            assert.ok(lease.targets[0].endsWith(`/node/${id}`));
            await assert.rejects(other.Claim([alias], 60000, 'enforced'), Code('RESOURCE_BUSY'));
            const claims = await control.Inspect();
            assert.equal(claims.length, 1);
            assert.equal(claims[0].owner, session.owner);
            assert.deepEqual(claims[0].targets, lease.targets);
            assert.equal(bus.commands.length, 0);
        } finally {
            await session.Release(lease);
        }
        assert.deepEqual(await control.Inspect(), []);
    });
});

test('node UUID case aliases share checked plan ownership in both spelling directions', async t => {
    const id = 'aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee';
    for (const claimId of [id.toUpperCase(), id]) await t.test(`claim ${claimId}`, async child => {
        const { bus, session, control } = SessionFixture(child);
        bus.guard.snapshot = { nodes: [{ id, name: 'Case fixture node' }] };
        const planId = claimId === id ? id.toUpperCase() : id;
        const lease = (await session.Claim([claimId], 60000, 'enforced')).lease as ResourceLease;
        try {
            const plan = await session.Plan([{ t: 'node.rename', id: planId, name: 'Reviewed case fixture' }], 'case alias fixture');
            assert.deepEqual(plan.targets, lease.targets);
            assert.ok((plan.targets as string[])[0].endsWith(`/node/${id}`));
            const result = await session.Apply(String(plan.planId), lease);
            assert.equal(result.status, 'success');
            assert.equal(bus.commands.filter(call => !call.dryRun).length, 1);
            const claims = await control.Inspect();
            assert.equal(claims.length, 1);
            assert.deepEqual(claims[0].operations, []);
        } finally {
            await session.Release(lease);
        }
        assert.deepEqual(await control.Inspect(), []);
    });
});

test('native stale guard rejects apply, consumes the plan and releases target and execution leases', async t => {
    const { bus, session, control } = SessionFixture(t);
    const plan = await session.Plan(Rename(), 'stale fixture');
    const lease = (await session.Claim([NODE], 60000, 'enforced')).lease as ResourceLease;
    bus.guard.revision = 'other author changed scene';
    const result = await session.Apply(String(plan.planId), lease);
    assert.equal(result.status, 'failure');
    assert.equal(result.code, 'STALE_GUARD');
    assert.equal(result.atomic, false);
    assert.deepEqual(await control.Inspect(), []);
    await assert.rejects(session.Apply(String(plan.planId), lease), /expired or was consumed/);
    assert.equal(bus.commands.filter(call => !call.dryRun).length, 1);
});

test('failed checked dispatch releases pins and ownership without retrying an ambiguous mutation', async t => {
    const { bus, session, control } = SessionFixture(t);
    const plan = await session.Plan(Rename(), 'failed fixture');
    const lease = (await session.Claim([NODE], 60000, 'enforced')).lease as ResourceLease;
    bus.checkedError = new EditorBusError('BUS_TIMEOUT', 'Fixture mutation acknowledgement timed out');
    await assert.rejects(session.Apply(String(plan.planId), lease), error => error instanceof EditorBusError && error.code === 'BUS_TIMEOUT');
    assert.deepEqual(await control.Inspect(), []);
    await assert.rejects(session.Apply(String(plan.planId), lease), /expired or was consumed/);
    assert.equal(bus.commands.filter(call => !call.dryRun).length, 1);
});

test('post-observation timeout retains a completed mutation success with unverified postconditions', async t => {
    const { bus, session, control } = SessionFixture(t);
    const plan = await session.Plan(Rename(), 'post-observation fixture');
    const lease = (await session.Claim([NODE], 60000, 'enforced')).lease as ResourceLease;
    bus.failObservation = true;
    const result = await session.Apply(String(plan.planId), lease);
    assert.equal(result.status, 'success');
    assert.equal(CheckStatus({ observation: result.observationCheck }, 'observation'), 'unverified');
    assert.equal(CheckStatus({ persistence: result.persistence }, 'persistence'), 'unverified');
    assert.equal(CheckStatus({ render: result.render }, 'render'), 'unverified');
    assert.equal(bus.commands.filter(call => !call.dryRun).length, 1);
    const claims = await control.Inspect();
    assert.equal(claims.length, 1);
    assert.deepEqual(claims[0].operations, []);
    await session.Release(lease);
});

test('receipt handles are session-owned and another author change cannot be rolled back', async t => {
    const { bus, session, control, lease, receipt } = await Applied(t);
    const other = new ReliabilitySession(bus, 'write', control);
    await assert.rejects(other.Rollback(receipt, lease), /not owned by this MCP session/);
    await assert.rejects(other.Verify(receipt, lease, { save: false, reload: false, render: false, rollbackOnFailure: true }), /not owned by this MCP session/);
    assert.equal(bus.commands.filter(call => call.command.t === 'editor.rollback').length, 0);
    bus.guard.revision = 'newer human change';
    const newer = Clone(bus.guard);
    const result = await session.Rollback(receipt, lease);
    assert.equal(result.status, 'failure');
    assert.equal(result.code, 'ROLLBACK_CONFLICT');
    assert.deepEqual(bus.guard, newer);
    const rollback = bus.commands.find(call => call.command.t === 'editor.rollback')!.command;
    assert.equal(rollback.t, 'editor.rollback');
    if (rollback.t === 'editor.rollback') assert.equal(rollback.owner, session.owner);
    await session.Release(lease);
});

test('successful author rollback restores only its memory and releases target ownership', async t => {
    const { bus, session, control, lease, receipt } = await Applied(t);
    const result = await session.Rollback(receipt, lease);
    assert.equal(result.status, 'success');
    assert.equal(CheckStatus({ ownershipRelease: result.ownershipRelease }, 'ownershipRelease'), 'success');
    assert.equal(result.scope, 'memory-only');
    assert.equal(CheckStatus({ disk: result.disk }, 'disk'), 'unverified');
    assert.equal(bus.guard.revision, '1');
    assert.deepEqual(await control.Inspect(), []);
    await assert.rejects(session.Rollback(receipt, lease), /not owned by this MCP session/);
});

test('successful author rollback keeps native Undo success when its lease expires before ownership release', async t => {
    let now = 100000;
    const { bus, session, control } = SessionFixture(t, 'write', () => now);
    const plan = await session.Plan(Rename(), 'expiry during rollback fixture');
    const lease = (await session.Claim([NODE], 1000, 'enforced')).lease as ResourceLease;
    const applied = await session.Apply(String(plan.planId), lease);
    assert.equal(applied.status, 'success');
    assert.equal((await control.Inspect()).length, 1);
    bus.rollbackCompleted = () => { now = lease.expiresAt; };
    const result = await session.Rollback(String(applied.receipt), lease);
    assert.equal(result.status, 'success');
    assert.equal(result.undone, true);
    assert.equal(CheckStatus({ ownershipRelease: result.ownershipRelease }, 'ownershipRelease'), 'success');
    assert.match(String(Record(result.ownershipRelease).reason), /already absent/);
    assert.equal(result.scope, 'memory-only');
    assert.equal(bus.guard.revision, '1');
    assert.equal(bus.commands.filter(call => call.command.t === 'editor.rollback').length, 1);
    assert.deepEqual(await control.Inspect(), []);
    await assert.rejects(control.Assert(session.owner, lease, plan.targets as string[]), Code('LEASE_EXPIRED_OR_MISSING'));
    await assert.rejects(session.Rollback(String(applied.receipt), lease), /not owned by this MCP session/);
});

test('successful author rollback keeps native Undo success when target ownership release throws', async t => {
    const { bus, session, control, lease, receipt } = await Applied(t);
    const originalRelease = control.Release.bind(control);
    control.Release = async (owner, token) => {
        if (token.leaseId === lease.leaseId) throw new ResourceControlError('RESOURCE_STATE_UNAVAILABLE', 'Fixture target release storage unavailable');
        return originalRelease(owner, token);
    };
    try {
        const result = await session.Rollback(receipt, lease);
        assert.equal(result.status, 'success');
        assert.equal(result.undone, true);
        assert.equal(CheckStatus({ ownershipRelease: result.ownershipRelease }, 'ownershipRelease'), 'unverified');
        assert.match(String(Record(result.ownershipRelease).reason), /Fixture target release storage unavailable/);
        assert.equal(bus.guard.revision, '1');
        assert.equal(bus.commands.filter(call => call.command.t === 'editor.rollback').length, 1);
        const claims = await control.Inspect();
        assert.equal(claims.length, 1);
        assert.equal(claims[0].leaseId, lease.leaseId);
        assert.deepEqual(claims[0].operations, []);
        await assert.rejects(session.Rollback(receipt, lease), /not owned by this MCP session/);
    } finally {
        control.Release = originalRelease;
        await session.Release(lease);
    }
    assert.deepEqual(await control.Inspect(), []);
});

test('verification distinguishes complete errors, dropped and truncated streams and never asserts image freshness', async t => {
    const cases = [
        { name: 'complete', logs: { cursor: 21, count: 0, entries: [], dropped: false }, errors: 'success', status: 'unverified' },
        { name: 'new error', logs: { cursor: 21, count: 1, entries: [{ level: 'error', sequence: 21 }], dropped: false }, errors: 'failure', status: 'failure' },
        { name: 'dropped', logs: { cursor: 21, count: 0, entries: [], dropped: true }, errors: 'unverified', status: 'unverified' },
        { name: 'truncated', logs: { cursor: 21, count: 2, entries: [{ level: 'warning', sequence: 21 }], dropped: false }, errors: 'unverified', status: 'unverified' },
        { name: 'explicit truncation with matching count', logs: { cursor: 21, count: 1, entries: [{ level: 'warning', sequence: 21 }], dropped: false, truncated: true }, errors: 'unverified', status: 'unverified' },
        { name: 'missing count', logs: { cursor: 21, entries: [], dropped: false }, errors: 'unverified', status: 'unverified' },
    ];
    for (const item of cases) await t.test(item.name, async child => {
        const { bus, session, control, lease, receipt } = await Applied(child);
        bus.logs = item.logs;
        const result = await session.Verify(receipt, lease, { save: false, reload: false, render: true, rollbackOnFailure: false });
        assert.equal(result.status, item.status);
        assert.equal(CheckStatus(result.checks, 'errors'), item.errors);
        assert.equal(CheckStatus(result.checks, 'capture'), 'success');
        assert.equal(CheckStatus(result.checks, 'render'), 'unverified');
        assert.equal(result.scope, 'render-freshness-unverified');
        assert.equal(Record(Record(result.finalized).capture).base64, 'AA==');
        assert.equal(bus.commands.some(call => call.command.t === 'editor.finalize'), false);
        if (item.status === 'failure') assert.deepEqual(await control.Inspect(), []);
        else await session.Release(lease);
    });
});

test('MCP verification emits image content once and leaves only capture metadata in its reports', async t => {
    const directory = Fixture(t);
    const previous = { TEMP: process.env.TEMP, TMP: process.env.TMP, TMPDIR: process.env.TMPDIR };
    try {
        process.env.TEMP = directory; process.env.TMP = directory; process.env.TMPDIR = directory;
        assert.equal(resolve(tmpdir()), directory);
        const bus = new NativeFixtureBus();
        const client = await Connected(t, bus, 'write');
        const preview = await client.callTool({ name: 'editor_plan_checked', arguments: { label: 'image fixture', cmds: Rename() } });
        assert.notEqual(preview.isError, true);
        const plan = Record(preview.structuredContent);
        const claim = await client.callTool({ name: 'editor_resource_claim', arguments: { ids: [NODE], mode: 'enforced' } });
        assert.equal(Record(claim.structuredContent).status, 'success');
        const lease = Record(Record(claim.structuredContent).lease);
        const token = { leaseId: lease.leaseId, capability: lease.capability };
        try {
            const applied = await client.callTool({ name: 'editor_apply_checked', arguments: { planId: plan.planId, token } });
            assert.equal(Record(applied.structuredContent).status, 'success');
            const response = await client.callTool({ name: 'editor_verify_checked', arguments: {
                receipt: plan.receipt, token, save: false, reload: false, render: true, rollbackOnFailure: false,
            } });
            assert.notEqual(response.isError, true);
            const report = Record(response.structuredContent);
            assert.equal(report.status, 'unverified');
            assert.equal(CheckStatus(report.checks, 'capture'), 'success');
            assert.equal(CheckStatus(report.checks, 'render'), 'unverified');
            const content = response.content as RecordValue[];
            const images = content.filter(item => item.type === 'image');
            assert.equal(images.length, 1);
            assert.equal(images[0].data, 'AA==');
            assert.equal(images[0].mimeType, 'image/png');
            const text = content.filter(item => item.type === 'text');
            assert.equal(text.length, 1);
            assert.deepEqual(JSON.parse(String(text[0].text)), report);
            const metadata = Record(Record(report.finalized).capture);
            assert.equal(metadata.base64, undefined);
            assert.equal(metadata.mimeType, 'image/png');
            assert.equal(metadata.width, 1);
            assert.equal(metadata.height, 1);
            assert.deepEqual(metadata.objects, []);
            assert.equal(String(text[0].text).includes('AA=='), false);
            assert.equal(String(text[0].text).includes('"base64"'), false);
            assert.equal(JSON.stringify(report).includes('AA=='), false);
            assert.equal(JSON.stringify(report).includes('"base64"'), false);
        } finally {
            const released = await client.callTool({ name: 'editor_resource_release', arguments: { token } });
            assert.notEqual(released.isError, true);
        }
    } finally {
        for (const key of ['TEMP', 'TMP', 'TMPDIR'] as const) {
            if (previous[key] === undefined) delete process.env[key]; else process.env[key] = previous[key];
        }
    }
});

test('validation and capture failures remain failures; automatic rollback uses exact author and releases pins', async t => {
    for (const failure of ['validation', 'capture'] as const) await t.test(failure, async child => {
        const { bus, session, control, lease, receipt } = await Applied(child);
        if (failure === 'validation') bus.validation.errors = 1; else bus.failCapture = true;
        const result = await session.Verify(receipt, lease, { save: false, reload: false, render: true, rollbackOnFailure: true });
        assert.equal(result.status, 'failure');
        assert.equal(CheckStatus(result.checks, failure), 'failure');
        assert.equal(CheckStatus({ rollback: result.rollback }, 'rollback'), 'success');
        const rollback = bus.commands.find(call => call.command.t === 'editor.rollback')!.command;
        if (rollback.t !== 'editor.rollback') assert.fail();
        assert.equal(rollback.owner, session.owner);
        assert.equal(rollback.receipt, receipt);
        assert.equal(bus.guard.revision, '1');
        assert.deepEqual(await control.Inspect(), []);
        await assert.rejects(session.Rollback(receipt, lease), /not owned by this MCP session/);
    });
});

test('conflicted verification cannot undo a newer author change and still releases ownership', async t => {
    const { bus, session, control, lease, receipt } = await Applied(t);
    bus.guard.revision = 'newer author revision';
    const newer = Clone(bus.guard);
    const result = await session.Verify(receipt, lease, { save: false, reload: false, render: false, rollbackOnFailure: true });
    assert.equal(result.status, 'failure');
    assert.equal(CheckStatus(result.checks, 'state'), 'failure');
    assert.equal(CheckStatus({ rollback: result.rollback }, 'rollback'), 'failure');
    assert.deepEqual(bus.guard, newer);
    assert.deepEqual(await control.Inspect(), []);
});

test('a missing pre-edit log cursor cannot verify absence of new errors', async t => {
    const { session, lease, receipt } = await Applied(t, bus => { delete bus.logs.cursor; });
    const result = await session.Verify(receipt, lease, { save: true, reload: true, render: false, rollbackOnFailure: false });
    assert.equal(result.status, 'unverified');
    assert.equal(CheckStatus(result.checks, 'errors'), 'unverified');
    assert.equal(CheckStatus(result.checks, 'save'), 'success');
    assert.equal(CheckStatus(result.checks, 'reload'), 'success');
});

test('save and reload require explicit flags; verified persistence consumes receipt and releases ownership', async t => {
    const { bus, session, control, lease, receipt } = await Applied(t);
    const result = await session.Verify(receipt, lease, { save: true, reload: true, render: false, rollbackOnFailure: true });
    assert.equal(result.status, 'success');
    assert.equal(result.atomic, false);
    for (const key of ['save', 'reload', 'validation', 'errors', 'state']) assert.equal(CheckStatus(result.checks, key), 'success', key);
    const finalize = bus.commands.find(call => call.command.t === 'editor.finalize')!;
    assert.equal(finalize.dryRun, false);
    if (finalize.command.t !== 'editor.finalize') assert.fail();
    assert.equal(finalize.command.save, true);
    assert.equal(finalize.command.reload, true);
    assert.ok(bus.saved);
    assert.equal(bus.commands.some(call => call.command.t === 'editor.rollback'), false);
    assert.deepEqual(await control.Inspect(), []);
    await assert.rejects(session.Rollback(receipt, lease), /not owned by this MCP session/);
});

test('reload without save fails explicitly and persisted verification failure never undoes saved disk', async t => {
    await t.test('reload requires save', async child => {
        const { bus, session, control, lease, receipt } = await Applied(child);
        const result = await session.Verify(receipt, lease, { save: false, reload: true, render: false, rollbackOnFailure: true });
        assert.equal(result.status, 'failure');
        assert.equal(CheckStatus(result.checks, 'reload'), 'failure');
        assert.equal(bus.saved, undefined);
        assert.equal(bus.commands.some(call => call.command.t === 'editor.rollback'), false);
        assert.deepEqual(await control.Inspect(), []);
    });
    await t.test('saved failure', async child => {
        const { bus, session, control, lease, receipt } = await Applied(child);
        bus.validation.errors = 1;
        const result = await session.Verify(receipt, lease, { save: true, reload: false, render: false, rollbackOnFailure: true });
        assert.equal(result.status, 'failure');
        assert.equal(CheckStatus(result.checks, 'save'), 'success');
        assert.equal(CheckStatus({ rollback: result.rollback }, 'rollback'), 'unverified');
        assert.equal(bus.commands.some(call => call.command.t === 'editor.rollback'), false);
        const saved = Clone(bus.saved);
        const replacement = (await session.Claim([NODE], 60000, 'enforced')).lease as ResourceLease;
        const undone = await session.Rollback(receipt, replacement);
        assert.equal(undone.status, 'success');
        assert.equal(undone.persisted, true);
        assert.equal(CheckStatus({ disk: undone.disk }, 'disk'), 'unverified');
        assert.deepEqual(bus.saved, saved);
        assert.deepEqual(await control.Inspect(), []);
    });
});

test('verification transport failure releases execution pins and target lease', async t => {
    const { bus, session, control, lease, receipt } = await Applied(t);
    bus.failObservation = true;
    await assert.rejects(session.Verify(receipt, lease, { save: false, reload: false, render: false, rollbackOnFailure: false }),
        error => error instanceof EditorBusError && error.code === 'BUS_TIMEOUT');
    assert.deepEqual(await control.Inspect(), []);
    assert.equal(bus.commands.some(call => call.command.t === 'editor.rollback'), false);
});

test('managed asynchronous execution stays pinned until its exact request is terminal and uses opaque jobs', async t => {
    const { bus, session, control } = SessionFixture(t);
    const other = new ReliabilitySession(bus, 'write', control);
    const terminal = Deferred<unknown>();
    let correlatedPolls = 0;
    bus.queryHook = async query => {
        if (query.t === 'editor.state') return { playState: 'editor', restorePending: false };
        if (query.t === 'build.status') {
            if (!query.managedRequestId) return { building: false, managed: { busy: false } };
            assert.equal(query.managedRequestId, 'managed:8');
            ++correlatedPolls;
            if (correlatedPolls === 1) return { building: false, managed: { busy: false, request: { managedRequestId: 'managed:999', terminal: true, state: 'applied' } } };
            return terminal.promise;
        }
        throw new Error(`Unexpected build query ${query.t}`);
    };
    try {
        const first = await session.StartJob('same visible name', { t: 'build.run', target: 'managed' });
        assert.match(String(first.jobId), /^[0-9a-f-]{36}$/);
        await Eventually(() => correlatedPolls === 2, 'Correlated build polling did not resume after an unrelated terminal request');
        const claims = await control.Inspect();
        assert.equal(claims.length, 1);
        assert.deepEqual(claims[0].targets, [EXECUTION_RESOURCE]);
        assert.equal(claims[0].operations.length, 1);
        assert.equal(session.Job(String(first.jobId)).state, 'running');
        assert.equal(session.Job(String(first.jobId)).status, 'unverified');
        await assert.rejects(other.StartJob('same visible name', { t: 'build.run', target: 'managed' }), Code('RESOURCE_BUSY'));
        assert.throws(() => other.Job(String(first.jobId)), /not found in this MCP session/);
        assert.equal(bus.commands.filter(call => call.command.t === 'build.run' && !call.dryRun).length, 1);
        terminal.resolve({ building: false, managed: { busy: false, request: { managedRequestId: 'managed:8', terminal: true, state: 'applied' } } });
        await Eventually(async () => session.Job(String(first.jobId)).state === 'complete' && (await control.Inspect()).length === 0, 'Terminal build did not release execution');
        assert.equal(session.Job(String(first.jobId)).status, 'success');
        const second = await session.StartJob('same visible name', { t: 'build.run', target: 'managed' });
        assert.notEqual(second.jobId, first.jobId);
        await Eventually(async () => session.Job(String(second.jobId)).state === 'complete' && (await control.Inspect()).length === 0, 'Second opaque job did not complete');
    } finally {
        terminal.resolve({ managed: { request: { managedRequestId: 'managed:8', terminal: true, state: 'failed' } } });
        await Eventually(async () => (await control.Inspect()).length === 0, 'Fixture build cleanup did not release execution');
    }
});

test('managed build startup timeout never resends and remains unverified until backend idle releases pin', async t => {
    const { bus, session, control } = SessionFixture(t);
    const idle = Deferred<unknown>();
    let dispatches = 0;
    bus.commandHook = async command => {
        assert.equal(command.t, 'build.run');
        ++dispatches;
        throw new EditorBusError('BUS_TIMEOUT', 'Fixture build start acknowledgement timed out');
    };
    bus.queryHook = async query => {
        if (query.t === 'editor.state') return { playState: 'editor', restorePending: false };
        if (query.t === 'build.status') {
            if (dispatches === 0) return { building: false, managed: { busy: false } };
            assert.equal(query.managedRequestId, undefined);
            return idle.promise;
        }
        throw new Error(`Unexpected timeout query ${query.t}`);
    };
    try {
        const job = await session.StartJob('timeout fixture', { t: 'build.run', target: 'managed' });
        assert.equal(job.status, 'unverified');
        assert.equal(session.Job(String(job.jobId)).state, 'running');
        assert.equal((await control.Inspect())[0].operations.length, 1);
        idle.resolve({ building: false, managedBuildBusy: false, managed: { busy: false, request: { managedRequestId: 'managed:999', terminal: true, state: 'applied' } } });
        await Eventually(async () => session.Job(String(job.jobId)).state === 'complete' && (await control.Inspect()).length === 0, 'Uncorrelated idle build did not release execution');
        assert.equal(session.Job(String(job.jobId)).status, 'unverified');
        assert.equal(dispatches, 1);
    } finally {
        idle.resolve({ building: false, managedBuildBusy: false, managed: { busy: false } });
        await Eventually(async () => (await control.Inspect()).length === 0, 'Timeout build fixture failed cleanup');
    }
});

test('managed terminal failure and busy admission release execution and do not claim success', async t => {
    await t.test('terminal failure', async child => {
        const { bus, session, control } = SessionFixture(child);
        bus.queryHook = async query => query.t === 'editor.state' ? { playState: 'editor' } : {
            building: false, managed: { busy: false, ...(query.t === 'build.status' && query.managedRequestId
                ? { request: { managedRequestId: query.managedRequestId, terminal: true, state: 'failed' } } : {}) },
        };
        const job = await session.StartJob('failed build fixture', { t: 'build.run', target: 'managed' });
        await Eventually(async () => session.Job(String(job.jobId)).state === 'complete' && (await control.Inspect()).length === 0, 'Failed build did not release execution');
        assert.equal(session.Job(String(job.jobId)).status, 'failure');
    });
    await t.test('busy Editor admission', async child => {
        const { bus, session, control } = SessionFixture(child);
        bus.queryHook = async query => query.t === 'editor.state' ? { playState: 'playing' } : { building: false, managed: { busy: false } };
        await assert.rejects(session.StartJob('busy fixture', { t: 'build.run', target: 'managed' }), /backend is busy/);
        assert.equal(bus.commands.length, 0);
        assert.deepEqual(await control.Inspect(), []);
    });
});
