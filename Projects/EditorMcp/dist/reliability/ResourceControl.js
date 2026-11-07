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
/// @brief Stable machine-local namespace for one Editor endpoint and project.
/// @note Callers must use the same canonical endpoint/project identity; alternate namespaces do not coordinate.
export function ResourceNamespace(editorEndpoint, projectRoot) {
    const canonicalRoot = resolve(projectRoot);
    return JSON.stringify(process.platform === 'win32'
        ? [editorEndpoint.toLowerCase(), canonicalRoot.toLowerCase()]
        : [editorEndpoint, canonicalRoot]);
}
export class ResourceControlError extends Error {
    code;
    conflicts;
    constructor(code, message, conflicts = []) {
        super(message);
        this.name = 'ResourceControlError';
        this.code = code;
        this.conflicts = conflicts;
    }
}
/// @brief Cooperative, opt-in coordination; legacy calls do not participate or get blocked.
/// @note State uses immutable nonempty generation directories and exclusive directory rename, without expiring filesystem mutexes.
/// @note Only registry claims are atomic. Engine mutations, rollback, OS process exclusion and persistence are not implied.
/// @note Use local storage supporting atomic directory rename. State is not a security boundary against other same-user processes.
/// @note Expired pins remain held while the PID lives; PID reuse or denied liveness checks conservatively retain a claim.
export class ResourceControl {
    directory;
    namespaceHash;
    now;
    maxAttempts;
    processInstance = randomUUID();
    constructor(namespace, stateDirectory = join(tmpdir(), 'fbzz-editor-mcp-coordination'), options = {}) {
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
        const initial = { format: 1, namespaceHash: this.namespaceHash, revision: '0', leases: [] };
        this.Publish(initial);
    }
    /// @brief Atomically claim all targets or fail with structured conflicts, including claims by the same owner label.
    async Acquire(owner, targets, ttlMs) {
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
            const lease = { leaseId, capabilityHash: Hash(capability), owner, targets: normalized, expiresAt: now + ttlMs, pins: [] };
            state.leases.push(lease);
            return { leaseId, capability, owner, targets: [...normalized], expiresAt: lease.expiresAt };
        });
    }
    /// @brief Renew a still-valid lease; an expired capability cannot revive an old claim.
    async Renew(owner, token, ttlMs) {
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
    async Release(owner, token) {
        ValidateOwner(owner);
        ValidateToken(token);
        return this.Update((state, now) => {
            const lease = state.leases.find((candidate) => candidate.leaseId === token.leaseId);
            if (!lease)
                return false;
            Authorize(state, owner, token, now, true);
            if (lease.pins.length !== 0) {
                throw new ResourceControlError('ACTIVE_OPERATION', 'The lease has an active operation and cannot be released.', [View(lease, now)]);
            }
            state.leases = state.leases.filter((candidate) => candidate.leaseId !== token.leaseId);
            return true;
        });
    }
    /// @brief Observe valid ownership at this instant; use RunWithLease to protect an operation from expiry races.
    async Assert(owner, token, targets) {
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
    async Inspect() {
        return this.Update((state, now) => state.leases.map((lease) => View(lease, now)));
    }
    /// @brief Atomically authorize and pin targets before entering the callback; release the pin on success or failure.
    /// @note Pin lifetime follows callback lifetime. A caller must await all actual work before its callback completes.
    async RunWithLease(owner, token, targets, operation, jobName = 'operation') {
        ValidateOwner(owner);
        ValidateToken(token);
        ValidateJobName(jobName);
        const normalized = NormalizeTargets(targets);
        if (typeof operation !== 'function')
            throw new ResourceControlError('INVALID_ARGUMENT', 'An operation callback is required.');
        const jobId = randomUUID();
        const pin = await this.Update((state, now) => {
            const lease = Authorize(state, owner, token, now);
            AssertCovered(lease, normalized);
            if (lease.pins.some((candidate) => Overlaps(candidate.targets, normalized))) {
                throw new ResourceControlError('RESOURCE_BUSY', 'Another operation on this lease overlaps the requested targets.', [View(lease, now)]);
            }
            const created = { jobId, jobName, targets: normalized, pid: process.pid, processInstance: this.processInstance, startedAt: now };
            lease.pins.push(created);
            return PublicPin(created);
        });
        let operationError;
        let failed = false;
        let result;
        try {
            result = await operation(pin);
        }
        catch (error) {
            failed = true;
            operationError = error;
        }
        try {
            await this.EndPin(owner, token, jobId);
        }
        catch (cleanupError) {
            if (failed)
                throw new AggregateError([operationError, cleanupError], 'Operation and resource cleanup both failed.');
            throw cleanupError;
        }
        if (failed)
            throw operationError;
        return result;
    }
    /// @brief Cooperating build and Editor calls share one execution target, regardless of job display name.
    async RunExclusive(owner, jobName, operation, options = {}) {
        ValidateJobName(jobName);
        const targets = NormalizeTargets([EXECUTION_RESOURCE, ...(options.targets ?? [])]);
        const lease = await this.Acquire(owner, targets, options.ttlMs ?? 30000);
        let result;
        let operationError;
        let failed = false;
        try {
            result = await this.RunWithLease(owner, lease, targets, operation, jobName);
        }
        catch (error) {
            failed = true;
            operationError = error;
        }
        try {
            await this.Release(owner, lease);
        }
        catch (cleanupError) {
            if (failed)
                throw new AggregateError([operationError, cleanupError], 'Operation and lease cleanup both failed.');
            throw cleanupError;
        }
        if (failed)
            throw operationError;
        return result;
    }
    async EndPin(owner, token, jobId) {
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
    async Update(change) {
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
            if (JSON.stringify(state) === before)
                return value;
            state.revision = (BigInt(state.revision) + 1n).toString();
            if (this.Publish(state))
                return value;
            await new Promise((complete) => setTimeout(complete, Math.min(attempt + 1, 10)));
        }
        throw new ResourceControlError('RESOURCE_CONTENTION', 'The resource registry remained contended; no ownership was granted.');
    }
    Read() {
        let revision;
        for (const name of readdirSync(this.directory)) {
            const match = GENERATION_PATTERN.exec(name);
            if (match) {
                const candidate = BigInt(match[1]);
                if (revision === undefined || candidate > revision)
                    revision = candidate;
            }
        }
        if (revision === undefined)
            throw new ResourceControlError('RESOURCE_STATE_INVALID', 'The resource registry has no committed generation.');
        const revisionText = revision.toString();
        let parsed;
        try {
            parsed = JSON.parse(readFileSync(join(this.GenerationPath(revisionText), 'state.json'), 'utf8'));
        }
        catch {
            throw new ResourceControlError('RESOURCE_STATE_INVALID', 'The committed resource registry could not be read.');
        }
        if (!ValidState(parsed, this.namespaceHash, revisionText)) {
            throw new ResourceControlError('RESOURCE_STATE_INVALID', 'The committed resource registry failed validation.');
        }
        return parsed;
    }
    /// @note The complete nonempty directory is renamed after closing its file; existing nonempty generations cannot be replaced.
    Publish(state) {
        const destination = this.GenerationPath(state.revision);
        if (existsSync(destination))
            return false;
        const candidate = join(this.directory, `.pending-${process.pid}-${randomUUID()}`);
        const candidateFile = join(candidate, 'state.json');
        let fd;
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
            }
            catch (error) {
                const code = error.code;
                if (existsSync(destination) && ['EEXIST', 'ENOTEMPTY', 'EPERM', 'EACCES'].includes(code ?? ''))
                    return false;
                throw new ResourceControlError('RESOURCE_STATE_UNAVAILABLE', `Atomic resource publication failed (${code ?? 'unknown'}); local directory-rename support and writable state storage are required.`);
            }
        }
        finally {
            if (fd !== undefined)
                closeSync(fd);
            if (existsSync(candidateFile))
                unlinkSync(candidateFile);
            if (existsSync(candidate))
                rmdirSync(candidate);
        }
    }
    GenerationPath(revision) {
        if (!/^\d{1,20}$/.test(revision))
            throw new ResourceControlError('RESOURCE_STATE_INVALID', 'The registry revision is outside the supported range.');
        return join(this.directory, `generation-${revision.padStart(20, '0')}`);
    }
}
function Hash(value) {
    return createHash('sha256').update(value).digest('hex');
}
function ValidateOwner(owner) {
    if (typeof owner !== 'string' || owner.trim().length === 0 || owner.length > 512) {
        throw new ResourceControlError('INVALID_ARGUMENT', 'A nonempty owner label of at most 512 characters is required.');
    }
}
function ValidateJobName(jobName) {
    if (typeof jobName !== 'string' || jobName.trim().length === 0 || jobName.length > 512) {
        throw new ResourceControlError('INVALID_ARGUMENT', 'A nonempty job display name of at most 512 characters is required.');
    }
}
function ValidateToken(token) {
    if (!token || typeof token.leaseId !== 'string' || !/^[0-9a-f-]{36}$/.test(token.leaseId)
        || typeof token.capability !== 'string' || !/^[0-9a-f]{64}$/.test(token.capability)) {
        throw new ResourceControlError('INVALID_ARGUMENT', 'A well-formed opaque resource token is required.');
    }
}
function ValidateTtl(ttlMs) {
    if (!Number.isSafeInteger(ttlMs) || ttlMs < 1 || ttlMs > MAX_TTL_MS) {
        throw new ResourceControlError('INVALID_ARGUMENT', 'Lease TTL must be an integer from 1 millisecond to 24 hours.');
    }
}
/// @note Hierarchical targets are case-sensitive and '/' separated; '*' covers all targets in the namespace.
function NormalizeTargets(targets) {
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
function Covers(scope, target) {
    return scope === '*' || scope === target || target.startsWith(`${scope}/`);
}
function Overlaps(left, right) {
    return left.some((scope) => right.some((target) => Covers(scope, target) || Covers(target, scope)));
}
function AssertCovered(lease, targets) {
    if (targets.some((target) => !lease.targets.some((scope) => Covers(scope, target)))) {
        throw new ResourceControlError('TARGET_NOT_OWNED', 'The lease does not own every requested target.');
    }
}
function Authorize(state, owner, token, now, allowExpired = false) {
    const lease = state.leases.find((candidate) => candidate.leaseId === token.leaseId);
    if (!lease)
        throw new ResourceControlError('LEASE_EXPIRED_OR_MISSING', 'The resource lease has expired or no longer exists.');
    if (lease.owner !== owner || !timingSafeEqual(Buffer.from(lease.capabilityHash, 'hex'), Buffer.from(Hash(token.capability), 'hex'))) {
        throw new ResourceControlError('NOT_OWNER', 'The owner and resource capability do not match this lease.');
    }
    if (!allowExpired && lease.expiresAt <= now)
        throw new ResourceControlError('LEASE_EXPIRED_OR_MISSING', 'The resource lease has expired.');
    return lease;
}
function ProcessAlive(pid) {
    try {
        process.kill(pid, 0);
        return true;
    }
    catch (error) {
        return error.code !== 'ESRCH';
    }
}
function PublicPin(pin) {
    return { jobId: pin.jobId, jobName: pin.jobName, targets: [...pin.targets], pid: pin.pid, startedAt: pin.startedAt };
}
function View(lease, now) {
    return { leaseId: lease.leaseId, owner: lease.owner, targets: [...lease.targets], expiresAt: lease.expiresAt,
        expired: lease.expiresAt <= now, operations: lease.pins.map(PublicPin) };
}
/// @note Corrupt persisted state fails closed instead of silently clearing another process's claims.
function ValidState(value, namespaceHash, revision) {
    if (typeof value !== 'object' || value === null)
        return false;
    const state = value;
    if (state.format !== 1 || state.namespaceHash !== namespaceHash || state.revision !== revision || !Array.isArray(state.leases))
        return false;
    const ids = new Set();
    try {
        for (const lease of state.leases) {
            if (typeof lease !== 'object' || lease === null || !/^[0-9a-f-]{36}$/.test(lease.leaseId)
                || typeof lease.capabilityHash !== 'string' || !/^[0-9a-f]{64}$/.test(lease.capabilityHash)
                || !Number.isSafeInteger(lease.expiresAt) || lease.expiresAt < 0 || !Array.isArray(lease.pins) || ids.has(lease.leaseId))
                return false;
            ValidateOwner(lease.owner);
            NormalizeTargets(lease.targets);
            ids.add(lease.leaseId);
            for (const pin of lease.pins) {
                if (typeof pin !== 'object' || pin === null || typeof pin.jobId !== 'string' || !/^[0-9a-f-]{36}$/.test(pin.jobId)
                    || typeof pin.processInstance !== 'string' || !/^[0-9a-f-]{36}$/.test(pin.processInstance)
                    || !Number.isSafeInteger(pin.pid) || pin.pid < 1 || !Number.isSafeInteger(pin.startedAt) || pin.startedAt < 0)
                    return false;
                ValidateJobName(pin.jobName);
                AssertCovered(lease, NormalizeTargets(pin.targets));
            }
        }
    }
    catch {
        return false;
    }
    return true;
}
//# sourceMappingURL=ResourceControl.js.map