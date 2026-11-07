/// @file    ReliabilityTools.ts
/// @brief   Opt-in planning, author-safe verification and cooperative execution over existing Editor APIs.
/// @author  Hasegawa Jin
/// @date    2026-10-11
/// @see     Docs/design/ai-reliability.md
import { createHash, randomUUID } from 'node:crypto';
import * as z from 'zod/v4';
import { EditorBusError } from '../busClient.js';
import { EditorCommandSchema, SemanticViewportResultSchema } from '../editorContracts.js';
import { ResourceControl, ResourceControlError } from './ResourceControl.js';
const AsRecord = (value) => value !== null && typeof value === 'object' && !Array.isArray(value) ? value : {};
const Check = (status, reason) => ({ status, reason });
const TokenSchema = z.object({ leaseId: z.string().min(1).max(128), capability: z.string().min(1).max(256) }).strict();
const Wait = (ms) => new Promise(resolve => setTimeout(resolve, ms));
/// @note Canonical equality ignores object key order but preserves array order and numeric values.
function Canonical(value) {
    if (Array.isArray(value))
        return `[${value.map(Canonical).join(',')}]`;
    if (value !== null && typeof value === 'object')
        return `{${Object.entries(value).sort(([a], [b]) => a.localeCompare(b)).map(([key, entry]) => `${JSON.stringify(key)}:${Canonical(entry)}`).join(',')}}`;
    return JSON.stringify(value) ?? 'null';
}
const Fingerprint = (value) => createHash('sha256').update(Canonical(value)).digest('hex');
/// @note Owners are unique to each MCP session; labels and job names never authorize a release or rollback.
export class ReliabilitySession {
    bus;
    permission;
    coordination;
    now;
    owner = randomUUID();
    plans = new Map();
    receipts = new Map();
    jobs = new Map();
    constructor(bus, permission, coordination, now = Date.now) {
        this.bus = bus;
        this.permission = permission;
        this.coordination = coordination;
        this.now = now;
    }
    /// @note Legacy tools and observations never create a coordination registry during server registration.
    get control() { return this.coordination ??= new ResourceControl('fbzz-ai-reliability-v1'); }
    async Observe(options = {}) {
        try {
            const observation = AsRecord(await this.bus.Query({ t: 'editor.observation', ...options }));
            if (observation.schemaVersion !== 1 || observation.consistency !== 'main-thread')
                throw new EditorBusError('BAD_OBSERVATION', 'Native observation contract was not returned.');
            return { ...observation, owner: this.owner };
        }
        catch (error) {
            if (!(error instanceof EditorBusError) || !['UNKNOWN_QUERY', 'UNSUPPORTED', 'BAD_OBSERVATION'].includes(error.code))
                throw error;
            const result = { schemaVersion: 1, consistency: 'multi-call-unverified', owner: this.owner };
            const checks = {};
            const queries = [
                ['state', { t: 'editor.state' }], ['selection', { t: 'scene.selection' }], ['snapshot', { t: 'scene.snapshot' }],
                ['validation', { t: 'scene.validate' }], ['build', { t: 'build.status', ...(options.managedRequestId ? { managedRequestId: options.managedRequestId } : {}), ...(options.packageRequestId ? { packageRequestId: options.packageRequestId } : {}) }],
                ['logs', { t: 'console.logs', minLevel: 'warning', limit: 512, ...(options.afterSequence === undefined ? {} : { afterSequence: options.afterSequence }) }],
                ['undoHistory', { t: 'editor.undoHistory' }],
            ];
            for (const [key, query] of queries) {
                try {
                    result[key] = await this.bus.Query(query);
                    checks[key] = Check('success', 'Query completed in a separate Editor call.');
                }
                catch (failure) {
                    checks[key] = Check('unverified', failure instanceof Error ? failure.message : String(failure));
                }
            }
            checks.guard = Check('unverified', 'Running Editor lacks the native reliability API. Existing tools work; checked edits require a coordinated Editor update.');
            result.checks = checks;
            return result;
        }
    }
    SceneTarget(guard) {
        const value = AsRecord(guard);
        if (typeof value.project !== 'string' || typeof value.scene !== 'string' || typeof value.revision !== 'string' || !AsRecord(value.snapshot).nodes)
            throw new Error('Native guard is unavailable; update Editor through the coordinated build workflow.');
        return `scene/${Fingerprint([value.project.replaceAll('\\', '/').toLowerCase(), value.scene.replaceAll('\\', '/').toLowerCase()])}`;
    }
    async Claim(ids, ttlMs, mode) {
        if (this.permission === 'read')
            throw new Error('Read mode cannot acquire ownership.');
        const observation = await this.Observe();
        const scene = this.SceneTarget(observation.guard);
        const targets = ids.length ? ids.map(id => `${scene}/node/${id.toLowerCase()}`) : [scene];
        if (mode === 'trial' || this.permission !== 'write')
            return { status: 'unverified', mode: 'trial', owner: this.owner, targets, state: await this.control.Inspect(), reason: 'No claim was acquired. Explicit enforced mode in write permission opts into cooperative ownership.' };
        const lease = await this.control.Acquire(this.owner, targets, ttlMs);
        return { status: 'success', mode: 'enforced', cooperative: true, lease };
    }
    async Renew(token, ttlMs) { return this.control.Renew(this.owner, token, ttlMs); }
    async Release(token) { return this.control.Release(this.owner, token); }
    async Resources() { return this.control.Inspect(); }
    async Plan(cmds, label) {
        if (this.permission === 'read')
            throw new Error('Read mode cannot submit Command dry-runs.');
        const observation = await this.Observe();
        const scene = this.SceneTarget(observation.guard);
        const guard = observation.guard;
        const receipt = randomUUID();
        const command = EditorCommandSchema.parse({ t: 'editor.checked', owner: this.owner, receipt, guard, label, cmds });
        if (command.t !== 'editor.checked')
            throw new Error('Invalid checked plan.');
        const preview = AsRecord(await this.bus.Command(structuredClone(command), true));
        if (AsRecord(preview.preflight).status !== 'success' || preview.dryRun !== true)
            throw new Error('Native checked preflight was not confirmed; no plan was created.');
        const targets = [...new Set(command.cmds.map(cmd => `${scene}/node/${String(AsRecord(cmd).id).toLowerCase()}`))];
        const afterSequence = AsRecord(observation.logs).cursor;
        const plan = { owner: this.owner, receipt, label, cmds: command.cmds, guard: command.guard, expiresAt: this.now() + 60000, targets,
            ...(typeof afterSequence === 'number' ? { afterSequence } : {}) };
        for (const [id, older] of this.plans)
            if (older.expiresAt <= this.now())
                this.plans.delete(id);
        if (this.plans.size >= 32)
            this.plans.delete(this.plans.keys().next().value);
        const planId = randomUUID();
        this.plans.set(planId, plan);
        return { status: 'unverified', preflight: Check('success', 'Native builders and requested field diff validated without applying.'), planId,
            receipt, owner: this.owner, expiresAt: plan.expiresAt, commandsFingerprint: Fingerprint(plan.cmds), targets, changes: preview.changes, atomic: false,
            reason: 'Review changes, acquire an enforced lease for the targets, then apply this single-use plan.' };
    }
    async Apply(planId, token) {
        if (this.permission !== 'write')
            return { status: 'unverified', reason: 'Write permission is required; no mutation was sent.' };
        const plan = this.plans.get(planId);
        if (!plan || plan.expiresAt <= this.now())
            throw new Error('Plan expired or was consumed; observe and plan again.');
        let release = false;
        try {
            return await this.control.RunWithLease(this.owner, token, plan.targets, async () => this.control.RunExclusive(this.owner, plan.label, async () => {
                /// @note Consume before dispatch; ambiguous transport failures must never trigger a mutation replay.
                this.plans.delete(planId);
                try {
                    const result = AsRecord(await this.bus.Command({ t: 'editor.checked', owner: this.owner, receipt: plan.receipt, guard: plan.guard, label: plan.label, cmds: plan.cmds }, false));
                    if (result.status === 'success' && result.guard) {
                        if (this.receipts.size >= 8)
                            this.receipts.delete(this.receipts.keys().next().value);
                        this.receipts.set(plan.receipt, { guard: result.guard, targets: plan.targets, persisted: false,
                            ...(plan.afterSequence === undefined ? {} : { afterSequence: plan.afterSequence }) });
                    }
                    else
                        release = true;
                    let observation;
                    let observationCheck = Check('unverified', 'Post-edit observation unavailable.');
                    try {
                        observation = await this.Observe(plan.afterSequence === undefined ? {} : { afterSequence: plan.afterSequence });
                        observationCheck = Check('success', 'Post-edit observation returned; individual sections retain availability.');
                    }
                    catch (failure) {
                        observationCheck = Check('unverified', String(failure));
                    }
                    return { ...result, status: result.status === 'failure' ? 'failure' : result.status === 'success' ? 'success' : 'unverified',
                        receipt: plan.receipt, observation, observationCheck, persistence: Check('unverified', 'Save and reload not yet requested.'),
                        render: Check('unverified', 'Rendering not yet verified.'), scope: 'native-field-readback', atomic: false };
                }
                catch (error) {
                    release = true;
                    throw error;
                }
            }));
        }
        catch (error) {
            release = true;
            throw error;
        }
        finally {
            if (release)
                await this.control.Release(this.owner, token).catch(() => undefined);
        }
    }
    async Rollback(receipt, token) {
        if (this.permission !== 'write')
            return { status: 'unverified', reason: 'Write permission is required; no rollback was sent.' };
        const known = this.receipts.get(receipt);
        if (!known)
            throw new Error('Receipt is not owned by this MCP session.');
        const result = await this.control.RunWithLease(this.owner, token, known.targets, async () => this.control.RunExclusive(this.owner, 'rollback', async () => AsRecord(await this.bus.Command({ t: 'editor.rollback', owner: this.owner, receipt }, false))));
        let ownershipRelease = Check('unverified', 'Rollback did not complete; ownership retained until explicit release or expiry.');
        if (result.status === 'success')
            this.receipts.delete(receipt);
        if (result.status === 'success') {
            try {
                const released = await this.control.Release(this.owner, token);
                ownershipRelease = Check('success', released ? 'Target lease released.' : 'Target lease was already absent after expiry or release.');
            }
            catch (failure) {
                ownershipRelease = Check('unverified', failure instanceof Error ? failure.message : String(failure));
            }
        }
        return { ...result, ownershipRelease, scope: 'memory-only', persisted: known.persisted, disk: Check('unverified', 'Rollback does not restore previously saved files.'), atomic: false };
    }
    async Verify(receipt, token, options) {
        const known = this.receipts.get(receipt);
        if (!known)
            throw new Error('Receipt is not owned by this MCP session.');
        let release = false;
        try {
            return await this.control.RunWithLease(this.owner, token, known.targets, async () => this.control.RunExclusive(this.owner, 'verify', async () => {
                const checks = { save: Check('unverified', 'Not requested.'), reload: Check('unverified', 'Not requested.'),
                    render: Check('unverified', 'Current render frame provenance is unavailable.'), capture: Check('unverified', 'Not requested.'),
                    validation: Check('unverified', 'No validation received.'), errors: Check('unverified', 'No error stream received.') };
                let finalized = {};
                if (options.save || options.reload) {
                    finalized = AsRecord(await this.bus.Command({ t: 'editor.finalize', guard: known.guard, save: options.save, reload: options.reload }, this.permission !== 'write'));
                    checks.save = finalized.save;
                    checks.reload = finalized.reload;
                    if (AsRecord(finalized.save).status === 'success')
                        known.persisted = true;
                }
                const observation = await this.Observe(known.afterSequence === undefined ? {} : { afterSequence: known.afterSequence });
                const validation = AsRecord(observation.validation), logs = AsRecord(observation.logs);
                if (typeof validation.errors === 'number')
                    checks.validation = Check(validation.errors > 0 ? 'failure' : 'success', 'Existing scene.validate coverage; all native/managed reference kinds are not covered.');
                const entries = Array.isArray(logs.entries) ? logs.entries.map(AsRecord) : undefined;
                if (entries && known.afterSequence !== undefined) {
                    if (entries.some(entry => entry.level === 'error'))
                        checks.errors = Check('failure', 'New Editor error entries were observed.');
                    else if (logs.dropped === true || logs.truncated === true || typeof logs.count !== 'number' || logs.count > entries.length)
                        checks.errors = Check('unverified', 'Error stream was dropped or truncated.');
                    else
                        checks.errors = Check('success', 'No new errors in the complete Editor log cursor range.');
                }
                if (options.render) {
                    try {
                        const capture = SemanticViewportResultSchema.parse(await this.bus.Query({ t: 'viewport.semantic', w: 960, h: 540, view: 'scene' }));
                        checks.capture = Check('success', 'PNG and semantic objects returned. Capture freshness and visual correctness need a playtest baseline.');
                        finalized.capture = capture;
                    }
                    catch (failure) {
                        checks.capture = Check('failure', String(failure));
                    }
                }
                const observationGuard = observation.guard;
                /// @note Save/reload may intentionally move history; use finalize's returned guard only after it completed.
                const expectedGuard = AsRecord(finalized.observation).guard ?? known.guard;
                checks.state = observationGuard && Fingerprint(observationGuard) === Fingerprint(expectedGuard)
                    ? Check('success', 'Native guard still matches this operation.') : Check('failure', 'Scene or history changed; receipt verification is conflicted.');
                let status = Object.values(checks).some(check => AsRecord(check).status === 'failure') ? 'failure' : 'unverified';
                if (options.save && options.reload && !options.render && ['save', 'reload', 'validation', 'errors', 'state'].every(key => AsRecord(checks[key]).status === 'success'))
                    status = 'success';
                let rollback = Check('unverified', 'Not required or not requested.');
                if (status === 'failure' && options.rollbackOnFailure && !known.persisted && !options.reload && this.permission === 'write') {
                    try {
                        const undone = AsRecord(await this.bus.Command({ t: 'editor.rollback', owner: this.owner, receipt }, false));
                        rollback = Check(undone.status === 'success' ? 'success' : 'failure', 'Native ownership and exact state guard decided rollback.');
                        if (undone.status === 'success')
                            this.receipts.delete(receipt);
                    }
                    catch (failure) {
                        rollback = Check('failure', String(failure));
                    }
                }
                if (options.reload && AsRecord(checks.reload).status === 'success') {
                    this.receipts.delete(receipt);
                    release = true;
                }
                if (status === 'failure')
                    release = true;
                return { status, checks, rollback, observation, finalized, atomic: false, scope: options.render ? 'render-freshness-unverified' : 'requested-persistence-and-log-checks' };
            }));
        }
        catch (error) {
            release = true;
            throw error;
        }
        finally {
            if (release)
                await this.control.Release(this.owner, token).catch(() => undefined);
        }
    }
    /// @note Pins are held until the correlated backend job is terminal; user-facing status calls never release a running job.
    async StartJob(name, command) {
        if (command.t !== 'build.run' || command.target !== 'managed')
            throw new Error('Only existing managed build.run is supported; use established CPU build coordination for native builds.');
        if (this.permission !== 'write')
            return { status: 'unverified', dryRun: true, preview: await this.bus.Command(command, true) };
        const job = { jobId: randomUUID(), name, state: 'starting', status: 'unverified' };
        if (this.jobs.size >= 32) {
            const completed = [...this.jobs.values()].find(value => value.state === 'complete');
            if (completed)
                this.jobs.delete(completed.jobId);
            else
                throw new Error('Too many active jobs.');
        }
        this.jobs.set(job.jobId, job);
        let started;
        let rejected;
        const admission = new Promise((resolve, reject) => { started = resolve; rejected = reject; });
        void this.control.RunExclusive(this.owner, name, async () => {
            const state = AsRecord(await this.bus.Query({ t: 'editor.state' }));
            const builds = AsRecord(await this.bus.Query({ t: 'build.status' }));
            if (state.playState !== 'editor' || state.restorePending === true || state.managedBuildBusy === true || builds.building === true || AsRecord(builds.managed).busy === true)
                throw new Error('Editor or build backend is busy; wait for its current owner.');
            let requestId;
            try {
                const result = AsRecord(await this.bus.Command(command, false));
                requestId = typeof result.managedRequestId === 'string' ? result.managedRequestId : undefined;
                job.result = result;
                job.state = 'running';
                started();
            }
            catch (error) {
                if (!(error instanceof EditorBusError) || !error.code.startsWith('BUS_'))
                    throw error;
                job.error = error.message;
                job.state = 'running';
                started();
            }
            for (;;) {
                try {
                    const status = AsRecord(await this.bus.Query({ t: 'build.status', ...(requestId ? { managedRequestId: requestId } : {}) }));
                    const managed = AsRecord(status.managed), request = AsRecord(managed.request);
                    if (requestId && request.managedRequestId === requestId && request.terminal === true) {
                        job.result = request;
                        job.status = request.state === 'applied' ? 'success' : 'failure';
                        break;
                    }
                    if (!requestId && status.building === false && managed.busy === false && status.managedBuildBusy === false) {
                        job.result = status;
                        job.status = 'unverified';
                        break;
                    }
                }
                catch (error) {
                    job.error = error instanceof Error ? error.message : String(error);
                }
                await Wait(500);
            }
            job.state = 'complete';
        }).catch(error => { job.error = error instanceof Error ? error.message : String(error); job.state = 'complete'; job.status = 'failure'; rejected(error); });
        await admission;
        return { ...job, cooperative: true, reason: 'Execution pin remains held until the correlated build request is terminal.' };
    }
    Job(jobId) { const job = this.jobs.get(jobId); if (!job)
        throw new Error('Job ID not found in this MCP session.'); return { ...job }; }
}
function Result(value) {
    const record = AsRecord(value);
    const finalized = AsRecord(record.finalized), capture = AsRecord(finalized.capture);
    const content = [];
    let report = record;
    if (typeof capture.base64 === 'string' && typeof capture.mimeType === 'string') {
        const { base64, ...metadata } = capture;
        report = { ...record, finalized: { ...finalized, capture: metadata } };
        content.push({ type: 'image', data: base64, mimeType: capture.mimeType });
    }
    content.unshift({ type: 'text', text: JSON.stringify(report, null, 2) });
    return { content, structuredContent: report,
        ...(record.status === 'failure' ? { isError: true } : {}) };
}
async function Safe(operation) {
    try {
        return Result(await operation());
    }
    catch (error) {
        const uncertain = error instanceof EditorBusError && error.code.startsWith('BUS_');
        return Result({ status: uncertain ? 'unverified' : 'failure', code: error instanceof EditorBusError || error instanceof ResourceControlError ? error.code : 'RELIABILITY_REJECTED',
            ...(error instanceof ResourceControlError ? { conflicts: error.conflicts } : {}),
            reason: error instanceof Error ? error.message : String(error), automaticReplay: false });
    }
}
export function RegisterReliabilityTools(server, bus, permission) {
    const session = new ReliabilitySession(bus, permission);
    const read = { readOnlyHint: true, openWorldHint: false };
    const write = { readOnlyHint: false, destructiveHint: false, openWorldHint: false };
    server.registerTool('editor_observe_structured', { description: '現在状態・選択・native公開コンポーネント・参照検証・build・ログを同一Editor呼出しで取得。旧Editorはmulti-call未検証として返します。',
        inputSchema: { ids: z.array(z.string().uuid()).max(128).optional(), afterSequence: z.number().int().nonnegative().max(Number.MAX_SAFE_INTEGER).optional(), managedRequestId: z.string().regex(/^managed:[1-9][0-9]{0,19}$/).optional(), packageRequestId: z.string().regex(/^package:[1-9][0-9]{0,19}$/).optional() }, annotations: read }, options => Safe(() => session.Observe(options)));
    server.registerTool('editor_resource_status', { description: 'opt-in所有と実行pinを読み取ります。capability秘密値は含みません。', inputSchema: {}, annotations: read }, () => Safe(() => session.Resources()));
    server.registerTool('editor_exclusive_job_status', { description: '同名表示でもopaque IDで区別する、このMCPセッションの排他job状態。', inputSchema: { jobId: z.string().uuid() }, annotations: read }, ({ jobId }) => Safe(async () => session.Job(jobId)));
    if (permission === 'read')
        return;
    server.registerTool('editor_resource_claim', { description: 'Sceneまたはnodeの協調所有。既定trialは所有しません。write権限かつmode=enforcedで明示opt-inします。旧ツールは維持。',
        inputSchema: { ids: z.array(z.string().uuid()).max(128).default([]), ttlMs: z.number().int().min(1000).max(300000).default(60000), mode: z.enum(['trial', 'enforced']).default('trial') }, annotations: write }, ({ ids, ttlMs, mode }) => Safe(() => session.Claim(ids, ttlMs, mode)));
    server.registerTool('editor_resource_renew', { description: '自分のcapability付きleaseだけを更新します。', inputSchema: { token: TokenSchema, ttlMs: z.number().int().min(1000).max(300000).default(60000) }, annotations: write }, ({ token, ttlMs }) => Safe(() => session.Renew(token, ttlMs)));
    server.registerTool('editor_resource_release', { description: '自分のcapability付きleaseだけを解放します。', inputSchema: { token: TokenSchema }, annotations: write }, ({ token }) => Safe(() => session.Release(token)));
    server.registerTool('editor_plan_checked', { description: 'nativeメモリ内field編集をdry-runし、変更前/予定値の差分と60秒のsingle-use planを作ります。ファイル/managed設定は既存専用APIを使用。',
        inputSchema: { label: z.string().min(1).max(128), cmds: z.array(EditorCommandSchema).min(1).max(128) }, annotations: write }, ({ label, cmds }) => Safe(() => session.Plan(cmds, label)));
    server.registerTool('editor_apply_checked', { description: '自分のenforced leaseとsingle-use planで実行。nativeが変更直前のScene/履歴を比較しfield値を読み返します。atomic保証はありません。',
        inputSchema: { planId: z.string().uuid(), token: TokenSchema }, annotations: write }, ({ planId, token }) => Safe(() => session.Apply(planId, token)));
    server.registerTool('editor_rollback_checked', { description: '自分の変更だけを、nativeの履歴/Scene一致条件付きでUndo。後続作者の変更があれば拒否。保存済ファイルは戻しません。',
        inputSchema: { receipt: z.string().uuid(), token: TokenSchema }, annotations: write }, ({ receipt, token }) => Safe(() => session.Rollback(receipt, token)));
    server.registerTool('editor_verify_checked', { description: '保存/現在Scene再読込/参照/今回ログ/画像取得を個別に成功・失敗・未検証で返します。再読込はsave=trueが必須。画像のframe由来が無い場合は描画未検証。',
        inputSchema: { receipt: z.string().uuid(), token: TokenSchema, save: z.boolean().default(false), reload: z.boolean().default(false), render: z.boolean().default(true), rollbackOnFailure: z.boolean().default(true) }, annotations: write }, ({ receipt, token, ...options }) => Safe(() => session.Verify(receipt, token, options)));
    server.registerTool('editor_build_managed_exclusive', { description: '既存managed build.runを協調排他で起動し、要求IDのterminalまで実行pinを保持。nativeビルドは既存CPU枠/AgentBuild調整を使用。',
        inputSchema: { name: z.string().min(1).max(128).default('managed-build') }, annotations: write }, ({ name }) => Safe(() => session.StartJob(name, { t: 'build.run', target: 'managed' })));
}
//# sourceMappingURL=ReliabilityTools.js.map