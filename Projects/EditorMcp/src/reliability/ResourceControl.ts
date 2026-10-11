/// @file    ResourceControl.ts
/// @brief   Opt-in local ownership leases and pinned execution for cooperating MCP processes.
/// @author  Hasegawa Jin
/// @date    2026-10-11
import { createHash, randomBytes, randomUUID, timingSafeEqual } from 'node:crypto';
import { closeSync, existsSync, fsyncSync, mkdirSync, openSync, readFileSync, readdirSync, renameSync, rmdirSync, unlinkSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';

/// @brief Both build and Editor execution use this target within the same namespace.
export const EXECUTION_RESOURCE = 'execution';
const MAX_TTL_MS = 24 * 60 * 60 * 1000;
const GENERATION_PATTERN = /^generation-(\d{20})$/;

/// @brief An opaque capability; names and owner labels alone never authorize operations.
export interface ResourceToken {
    leaseId: string;
    capability: string;
}

/// @brief The acquisition result; this object can also be supplied wherever a token is required.
export interface ResourceLease extends ResourceToken {
    owner: string;
    targets: string[];
    expiresAt: number;
}

/// @brief No capabilities or capability hashes are included in public state observations.
export interface ResourceClaim {
    leaseId: string;
    owner: string;
    targets: string[];
    expiresAt: number;
    expired: boolean;
    operations: ResourceOperation[];
}

export interface ResourceOperation {
    jobId: string;
    jobName: string;
    targets: string[];
    pid: number;
    startedAt: number;
}

export interface ResourceControlOptions {
    /// @note A clock override is for deterministic fixtures; production should use the default clock.
    now?: () => number;
    /// @note Bound contention retries; a failure never grants ownership optimistically.
    maxAttempts?: number;
}

export interface ExclusiveOptions {
    ttlMs?: number;
    targets?: readonly string[];
}

/// @brief Stable machine-local namespace for one Editor endpoint and project.
/// @note Callers must use the same canonical endpoint/project identity; alternate namespaces do not coordinate.
export function ResourceNamespace(editorEndpoint: string, projectRoot: string): string {
    const canonicalRoot = resolve(projectRoot);
    return JSON.stringify(process.platform === 'win32'
        ? [editorEndpoint.toLowerCase(), canonicalRoot.toLowerCase()]
        : [editorEndpoint, canonicalRoot]);
}

export class ResourceControlError extends Error {
    readonly code: string;
    readonly conflicts: ResourceClaim[];

    constructor(code: string, message: string, conflicts: ResourceClaim[] = []) {
        super(message);
        this.name = 'ResourceControlError';
        this.code = code;
        this.conflicts = conflicts;
    }
}

interface StoredPin extends ResourceOperation {
    processInstance: string;
}

interface StoredLease {
    leaseId: string;
    capabilityHash: string;
    owner: string;
    targets: string[];
    expiresAt: number;
    pins: StoredPin[];
}

interface RegistryState {
    format: 1;
    namespaceHash: string;
    revision: string;
    leases: StoredLease[];
}

/// @brief Cooperative, opt-in coordination; legacy calls do not participate or get blocked.
/// @note State uses immutable nonempty generation directories and exclusive directory rename, without expiring filesystem mutexes.
/// @note Only registry claims are atomic. Engine mutations, rollback, OS process exclusion and persistence are not implied.
/// @note Use local storage supporting atomic directory rename. State is not a security boundary against other same-user processes.
/// @note Expired pins remain held while the PID lives; PID reuse or denied liveness checks conservatively retain a claim.
export class ResourceControl {
    private readonly directory: string;
    private readonly namespaceHash: string;
    private readonly now: () => number;
    private readonly maxAttempts: number;
    private readonly processInstance = randomUUID();

    constructor(namespace: string, stateDirectory = join(tmpdir(), 'fbzz-editor-mcp-coordination'), options: ResourceControlOptions = {}) {
        if (typeof namespace !== 'string' || namespace.trim().length === 0 || namespace.length > 4096) {
            throw new ResourceControlError('INVALID_ARGUMENT', 'A nonempty stable namespace is required.');
        }
        this.namespaceHash = Hash(namespace);
        this.directory = join(resolve(stateDirectory), this.namespaceHash);
        this.now = options.now ?? Date.now;
        this.maxAttempts = options.maxAttempts ?? 256;
        if (!Number.isInteger(this.maxAttempts) || this.maxAttempts < 1 || this.maxAttempts > 10000) {
            throw new ResourceControlError('INVALID_ARGUMENT', 'maxAttempts must be an integer from 1 to 10000.');
        }
        mkdirSync(this.directory, { recursive: true, mode: 0o700 });
        const initial: RegistryState = { format: 1, namespaceHash: this.namespaceHash, revision: '0', leases: [] };
        this.Publish(initial);
    }

    /// @brief Atomically claim all targets or fail with structured conflicts, including claims by the same owner label.
    async Acquire(owner: string, targets: readonly string[], ttlMs: number): Promise<ResourceLease> {
        ValidateOwner(owner);
        const normalized = NormalizeTargets(targets);
        ValidateTtl(ttlMs);
        const leaseId = randomUUID();
        const capability = randomBytes(32).toString('hex');
        return this.Update((state, now) => {
            const conflicts = state.leases.filter((lease) => Overlaps(lease.targets, normalized));
            if (conflicts.length !== 0) {
                throw new ResourceControlError('RESOURCE_BUSY', 'One or more targets are already owned.', conflicts.map((lease) => View(lease, now)));
            }
            const lease: StoredLease = { leaseId, capabilityHash: Hash(capability), owner, targets: normalized, expiresAt: now + ttlMs, pins: [] };
            state.leases.push(lease);
            return { leaseId, capability, owner, targets: [...normalized], expiresAt: lease.expiresAt };
        });
    }

    /// @brief Renew a still-valid lease; an expired capability cannot revive an old claim.
    async Renew(owner: string, token: ResourceToken, ttlMs: number): Promise<ResourceLease> {
        ValidateOwner(owner);
        ValidateToken(token);
        ValidateTtl(ttlMs);
        return this.Update((state, now) => {
            const lease = Authorize(state, owner, token, now);
            lease.expiresAt = now + ttlMs;
            return { leaseId: lease.leaseId, capability: token.capability, owner, targets: [...lease.targets], expiresAt: lease.expiresAt };
        });
    }

    /// @brief Release an idle owned lease; never remove an active operation, even after lease expiry.
    /// @return False when the claim has already disappeared; active claims require the exact owner and capability.
    async Release(owner: string, token: ResourceToken): Promise<boolean> {
        ValidateOwner(owner);
        ValidateToken(token);
        return this.Update((state, now) => {
            const lease = state.leases.find((candidate) => candidate.leaseId === token.leaseId);
            if (!lease) return false;
            Authorize(state, owner, token, now, true);
            if (lease.pins.length !== 0) {
                throw new ResourceControlError('ACTIVE_OPERATION', 'The lease has an active operation and cannot be released.', [View(lease, now)]);
            }
            state.leases = state.leases.filter((candidate) => candidate.leaseId !== token.leaseId);
            return true;
        });
    }

    /// @brief Observe valid ownership at this instant; use RunWithLease to protect an operation from expiry races.
    async Assert(owner: string, token: ResourceToken, targets: readonly string[]): Promise<ResourceClaim> {
        ValidateOwner(owner);
        ValidateToken(token);
        const normalized = NormalizeTargets(targets);
        return this.Update((state, now) => {
            const lease = Authorize(state, owner, token, now);
            AssertCovered(lease, normalized);
            return View(lease, now);
        });
    }

    /// @brief Observe participating claims and jobs, excluding secret capabilities.
    async Inspect(): Promise<ResourceClaim[]> {
        return this.Update((state, now) => state.leases.map((lease) => View(lease, now)));
    }

    /// @brief Atomically authorize and pin targets before entering the callback; release the pin on success or failure.
    /// @note Pin lifetime follows callback lifetime. A caller must await all actual work before its callback completes.
    async RunWithLease<T>(owner: string, token: ResourceToken, targets: readonly string[], operation: (context: ResourceOperation) => Promise<T> | T, jobName = 'operation'): Promise<T> {
        ValidateOwner(owner);
        ValidateToken(token);
        ValidateJobName(jobName);
        const normalized = NormalizeTargets(targets);
        if (typeof operation !== 'function') throw new ResourceControlError('INVALID_ARGUMENT', 'An operation callback is required.');
        const jobId = randomUUID();
        const pin = await this.Update((state, now) => {
            const lease = Authorize(state, owner, token, now);
            AssertCovered(lease, normalized);
            if (lease.pins.some((candidate) => Overlaps(candidate.targets, normalized))) {
                throw new ResourceControlError('RESOURCE_BUSY', 'Another operation on this lease overlaps the requested targets.', [View(lease, now)]);
            }
            const created: StoredPin = { jobId, jobName, targets: normalized, pid: process.pid, processInstance: this.processInstance, startedAt: now };
            lease.pins.push(created);
            return PublicPin(created);
        });
        let operationError: unknown;
        let failed = false;
        let result: T | undefined;
        try {
            result = await operation(pin);
        } catch (error) {
            failed = true;
            operationError = error;
        }
        try {
            await this.EndPin(owner, token, jobId);
        } catch (cleanupError) {
            if (failed) throw new AggregateError([operationError, cleanupError], 'Operation and resource cleanup both failed.');
            throw cleanupError;
        }
        if (failed) throw operationError;
        return result as T;
    }

    /// @brief Cooperating build and Editor calls share one execution target, regardless of job display name.
    async RunExclusive<T>(owner: string, jobName: string, operation: (context: ResourceOperation) => Promise<T> | T, options: ExclusiveOptions = {}): Promise<T> {
        ValidateJobName(jobName);
        const targets = NormalizeTargets([EXECUTION_RESOURCE, ...(options.targets ?? [])]);
        const lease = await this.Acquire(owner, targets, options.ttlMs ?? 30000);
        let result: T | undefined;
        let operationError: unknown;
        let failed = false;
        try {
            result = await this.RunWithLease(owner, lease, targets, operation, jobName);
        } catch (error) {
            failed = true;
            operationError = error;
        }
        try {
            await this.Release(owner, lease);
        } catch (cleanupError) {
            if (failed) throw new AggregateError([operationError, cleanupError], 'Operation and lease cleanup both failed.');
            throw cleanupError;
        }
        if (failed) throw operationError;
        return result as T;
    }

    private async EndPin(owner: string, token: ResourceToken, jobId: string): Promise<void> {
        await this.Update((state, now) => {
            const lease = Authorize(state, owner, token, now, true);
            const pin = lease.pins.find((candidate) => candidate.jobId === jobId);
            if (!pin || pin.pid !== process.pid || pin.processInstance !== this.processInstance) {
                throw new ResourceControlError('NOT_OWNER', 'The operation pin no longer belongs to this process instance.');
            }
            lease.pins = lease.pins.filter((candidate) => candidate.jobId !== jobId);
            if (lease.expiresAt <= now && lease.pins.length === 0) {
                state.leases = state.leases.filter((candidate) => candidate.leaseId !== token.leaseId);
            }
        });
    }

    /// @note Each retry recomputes checks from the current generation; only a successful exclusive publication grants a claim.
    private async Update<T>(change: (state: RegistryState, now: number) => T): Promise<T> {
        for (let attempt = 0; attempt < this.maxAttempts; ++attempt) {
            const state = this.Read();
            const now = this.now();
            if (!Number.isSafeInteger(now) || now < 0 || now > Number.MAX_SAFE_INTEGER - MAX_TTL_MS) {
                throw new ResourceControlError('INVALID_CLOCK', 'The coordination clock is outside the supported range.');
            }
            state.leases = state.leases.filter((lease) => {
                lease.pins = lease.pins.filter((pin) => ProcessAlive(pin.pid));
                return lease.expiresAt > now || lease.pins.length !== 0;
            });
            const before = JSON.stringify(state);
            const value = change(state, now);
            if (JSON.stringify(state) === before) return value;
            state.revision = (BigInt(state.revision) + 1n).toString();
            if (this.Publish(state)) return value;
            await new Promise<void>((complete) => setTimeout(complete, Math.min(attempt + 1, 10)));
        }
        throw new ResourceControlError('RESOURCE_CONTENTION', 'The resource registry remained contended; no ownership was granted.');
    }

    private Read(): RegistryState {
        let revision: bigint | undefined;
        for (const name of readdirSync(this.directory)) {
            const match = GENERATION_PATTERN.exec(name);
            if (match) {
                const candidate = BigInt(match[1]);
                if (revision === undefined || candidate > revision) revision = candidate;
            }
        }
        if (revision === undefined) throw new ResourceControlError('RESOURCE_STATE_INVALID', 'The resource registry has no committed generation.');
        const revisionText = revision.toString();
        let parsed: unknown;
        try {
            parsed = JSON.parse(readFileSync(join(this.GenerationPath(revisionText), 'state.json'), 'utf8'));
        } catch {
            throw new ResourceControlError('RESOURCE_STATE_INVALID', 'The committed resource registry could not be read.');
        }
        if (!ValidState(parsed, this.namespaceHash, revisionText)) {
            throw new ResourceControlError('RESOURCE_STATE_INVALID', 'The committed resource registry failed validation.');
        }
        return parsed;
    }

    /// @note The complete nonempty directory is renamed after closing its file; existing nonempty generations cannot be replaced.
    private Publish(state: RegistryState): boolean {
        const destination = this.GenerationPath(state.revision);
        if (existsSync(destination)) return false;
        const candidate = join(this.directory, `.pending-${process.pid}-${randomUUID()}`);
        const candidateFile = join(candidate, 'state.json');
        let fd: number | undefined;
        try {
            mkdirSync(candidate, { mode: 0o700 });
            fd = openSync(candidateFile, 'wx', 0o600);
            writeFileSync(fd, JSON.stringify(state), 'utf8');
            fsyncSync(fd);
            closeSync(fd);
            fd = undefined;
            try {
                renameSync(candidate, destination);
                return true;
            } catch (error) {
                const code = (error as NodeJS.ErrnoException).code;
                if (existsSync(destination) && ['EEXIST', 'ENOTEMPTY', 'EPERM', 'EACCES'].includes(code ?? '')) return false;
                throw new ResourceControlError('RESOURCE_STATE_UNAVAILABLE', `Atomic resource publication failed (${code ?? 'unknown'}); local directory-rename support and writable state storage are required.`);
            }
        } finally {
            if (fd !== undefined) closeSync(fd);
            if (existsSync(candidateFile)) unlinkSync(candidateFile);
            if (existsSync(candidate)) rmdirSync(candidate);
        }
    }

    private GenerationPath(revision: string): string {
        if (!/^\d{1,20}$/.test(revision)) throw new ResourceControlError('RESOURCE_STATE_INVALID', 'The registry revision is outside the supported range.');
        return join(this.directory, `generation-${revision.padStart(20, '0')}`);
    }
}

function Hash(value: string): string {
    return createHash('sha256').update(value).digest('hex');
}

function ValidateOwner(owner: string): void {
    if (typeof owner !== 'string' || owner.trim().length === 0 || owner.length > 512) {
        throw new ResourceControlError('INVALID_ARGUMENT', 'A nonempty owner label of at most 512 characters is required.');
    }
}

function ValidateJobName(jobName: string): void {
    if (typeof jobName !== 'string' || jobName.trim().length === 0 || jobName.length > 512) {
        throw new ResourceControlError('INVALID_ARGUMENT', 'A nonempty job display name of at most 512 characters is required.');
    }
}

function ValidateToken(token: ResourceToken): void {
    if (!token || typeof token.leaseId !== 'string' || !/^[0-9a-f-]{36}$/.test(token.leaseId)
        || typeof token.capability !== 'string' || !/^[0-9a-f]{64}$/.test(token.capability)) {
        throw new ResourceControlError('INVALID_ARGUMENT', 'A well-formed opaque resource token is required.');
    }
}

function ValidateTtl(ttlMs: number): void {
    if (!Number.isSafeInteger(ttlMs) || ttlMs < 1 || ttlMs > MAX_TTL_MS) {
        throw new ResourceControlError('INVALID_ARGUMENT', 'Lease TTL must be an integer from 1 millisecond to 24 hours.');
    }
}

/// @note Hierarchical targets are case-sensitive and '/' separated; '*' covers all targets in the namespace.
function NormalizeTargets(targets: readonly string[]): string[] {
    if (!Array.isArray(targets) || targets.length === 0 || targets.length > 256) {
        throw new ResourceControlError('INVALID_ARGUMENT', 'From 1 to 256 resource targets are required.');
    }
    for (const target of targets) {
        if (typeof target !== 'string' || target.length === 0 || target.length > 1024 || target.trim() !== target
            || /[\u0000-\u001f\\]/.test(target) || target.startsWith('/') || target.endsWith('/') || target.includes('//')
            || (target.includes('*') && target !== '*')) {
            throw new ResourceControlError('INVALID_ARGUMENT', 'Targets must be canonical nonempty slash-separated paths or the wildcard.');
        }
    }
    return [...new Set(targets)].sort();
}

function Covers(scope: string, target: string): boolean {
    return scope === '*' || scope === target || target.startsWith(`${scope}/`);
}

function Overlaps(left: readonly string[], right: readonly string[]): boolean {
    return left.some((scope) => right.some((target) => Covers(scope, target) || Covers(target, scope)));
}

function AssertCovered(lease: StoredLease, targets: readonly string[]): void {
    if (targets.some((target) => !lease.targets.some((scope) => Covers(scope, target)))) {
        throw new ResourceControlError('TARGET_NOT_OWNED', 'The lease does not own every requested target.');
    }
}

function Authorize(state: RegistryState, owner: string, token: ResourceToken, now: number, allowExpired = false): StoredLease {
    const lease = state.leases.find((candidate) => candidate.leaseId === token.leaseId);
    if (!lease) throw new ResourceControlError('LEASE_EXPIRED_OR_MISSING', 'The resource lease has expired or no longer exists.');
    if (lease.owner !== owner || !timingSafeEqual(Buffer.from(lease.capabilityHash, 'hex'), Buffer.from(Hash(token.capability), 'hex'))) {
        throw new ResourceControlError('NOT_OWNER', 'The owner and resource capability do not match this lease.');
    }
    if (!allowExpired && lease.expiresAt <= now) throw new ResourceControlError('LEASE_EXPIRED_OR_MISSING', 'The resource lease has expired.');
    return lease;
}

function ProcessAlive(pid: number): boolean {
    try {
        process.kill(pid, 0);
        return true;
    } catch (error) {
        return (error as NodeJS.ErrnoException).code !== 'ESRCH';
    }
}

function PublicPin(pin: StoredPin): ResourceOperation {
    return { jobId: pin.jobId, jobName: pin.jobName, targets: [...pin.targets], pid: pin.pid, startedAt: pin.startedAt };
}

function View(lease: StoredLease, now: number): ResourceClaim {
    return { leaseId: lease.leaseId, owner: lease.owner, targets: [...lease.targets], expiresAt: lease.expiresAt,
        expired: lease.expiresAt <= now, operations: lease.pins.map(PublicPin) };
}

/// @note Corrupt persisted state fails closed instead of silently clearing another process's claims.
function ValidState(value: unknown, namespaceHash: string, revision: string): value is RegistryState {
    if (typeof value !== 'object' || value === null) return false;
    const state = value as Partial<RegistryState>;
    if (state.format !== 1 || state.namespaceHash !== namespaceHash || state.revision !== revision || !Array.isArray(state.leases)) return false;
    const ids = new Set<string>();
    try {
        for (const lease of state.leases) {
            if (typeof lease !== 'object' || lease === null || !/^[0-9a-f-]{36}$/.test(lease.leaseId)
                || typeof lease.capabilityHash !== 'string' || !/^[0-9a-f]{64}$/.test(lease.capabilityHash)
                || !Number.isSafeInteger(lease.expiresAt) || lease.expiresAt < 0 || !Array.isArray(lease.pins) || ids.has(lease.leaseId)) return false;
            ValidateOwner(lease.owner);
            NormalizeTargets(lease.targets);
            ids.add(lease.leaseId);
            for (const pin of lease.pins) {
                if (typeof pin !== 'object' || pin === null || typeof pin.jobId !== 'string' || !/^[0-9a-f-]{36}$/.test(pin.jobId)
                    || typeof pin.processInstance !== 'string' || !/^[0-9a-f-]{36}$/.test(pin.processInstance)
                    || !Number.isSafeInteger(pin.pid) || pin.pid < 1 || !Number.isSafeInteger(pin.startedAt) || pin.startedAt < 0) return false;
                ValidateJobName(pin.jobName);
                AssertCovered(lease, NormalizeTargets(pin.targets));
            }
        }
    } catch {
        return false;
    }
    return true;
}
