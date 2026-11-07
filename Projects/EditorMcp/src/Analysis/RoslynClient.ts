/// @file    RoslynClient.ts
/// @brief   Editor-owned C# source snapshots and guarded semantic rename sessions.
/// @author  Hasegawa Jin
/// @date    2026-11-06
import { spawn } from 'node:child_process';
import { createHash, randomUUID } from 'node:crypto';
import { lstat, readdir, readFile, realpath, rename, unlink, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as z from 'zod/v4';
import type { EditorBus } from '../busClient.js';
import type { PermissionMode } from '../config.js';

type RecordValue = Record<string, unknown>;
const ContextSchema = z.object({ projectRoot: z.string().min(1), pendingRestore: z.boolean().optional(),
    managedBuildBusy: z.boolean().optional(), managed: z.object({ apiPath: z.string().nullable().optional() }).passthrough().optional() }).passthrough();
const ResultSchema = z.object({ ok: z.boolean(), error: z.string().optional() }).passthrough();
const EditSchema = z.object({ file: z.string(), start: z.number().int().nonnegative(), length: z.number().int().positive(), before: z.string(), after: z.string() });
const RenameResultSchema = ResultSchema.extend({ blocked: z.boolean(), blockers: z.array(z.string()), edits: z.array(EditSchema).max(10000) });
const MAX_SOURCE_BYTES = 16 * 1024 * 1024;
const PLAN_LIFETIME_MS = 5 * 60 * 1000;
const Hash = (value: string | Buffer): string => createHash('sha256').update(value).digest('hex');
const AsRecord = (value: unknown): RecordValue => value !== null && typeof value === 'object' && !Array.isArray(value) ? value as RecordValue : {};
const PathKey = (value: string): string => process.platform === 'win32' ? value.toLowerCase() : value;

interface SourceFile { file: string; absolute: string; content: string; bytes: Buffer; hash: string; }
interface Snapshot { root: string; sources: SourceFile[]; references: Array<{ path: string; hash: string }>; fingerprint: string; }
interface RenamePlan { snapshot: Snapshot; referencePaths: string[]; edits: z.infer<typeof EditSchema>[]; expiresAt: number; }
export interface AnalysisOptions { referencePaths?: string[]; limit?: number; filter?: string; file?: string; line?: number; column?: number; newName?: string; }
export type AnalysisRunner = (request: RecordValue) => Promise<RecordValue>;

function IsWithin(root: string, candidate: string): boolean {
    const relative = path.relative(root, candidate);
    return relative !== '' && !path.isAbsolute(relative) && relative !== '..' && !relative.startsWith(`..${path.sep}`);
}

/// @note Every requested path is resolved through realpath and confined to the Editor's current project.
async function ProjectFile(root: string, requested: string): Promise<string> {
    if (path.isAbsolute(requested)) throw new Error('Project-relative paths are required.');
    const candidate = path.resolve(root, requested);
    if (!IsWithin(root, candidate)) throw new Error('Path escapes the active project.');
    const resolved = await realpath(candidate);
    if (!IsWithin(root, resolved) || (await lstat(candidate)).isSymbolicLink()) throw new Error('Linked project paths are unsupported.');
    return resolved;
}

async function Context(bus: EditorBus): Promise<z.infer<typeof ContextSchema>> {
    const response = AsRecord(await bus.Query({ t: 'editor.op.query', id: 'authoring.context' }));
    const context = ContextSchema.parse(response.data);
    if (context.pendingRestore) throw new Error('Editor is restoring its scene; retry after restoration.');
    return context;
}

async function Capture(bus: EditorBus, referencePaths: string[]): Promise<Snapshot> {
    const context = await Context(bus);
    const root = await realpath(context.projectRoot);
    const sourceRoot = await ProjectFile(root, 'Assets/Scripts');
    const sources: SourceFile[] = [];
    let sourceBytes = 0;
    async function Visit(directory: string): Promise<void> {
        const entries = await readdir(directory, { withFileTypes: true });
        entries.sort((left, right) => left.name.localeCompare(right.name, 'en'));
        for (const entry of entries) {
            const absolute = path.join(directory, entry.name);
            if (entry.isSymbolicLink()) throw new Error('Linked script paths are unsupported.');
            if (entry.isDirectory()) { await Visit(absolute); continue; }
            if (!entry.isFile() || path.extname(entry.name).toLowerCase() !== '.cs') continue;
            const resolved = await realpath(absolute);
            if (!IsWithin(root, resolved)) throw new Error('Script path escaped the active project.');
            if (sources.length >= 512) throw new Error('Source snapshot exceeds 512 files; no partial snapshot was analyzed.');
            if ((await lstat(resolved)).size > MAX_SOURCE_BYTES - sourceBytes) throw new Error('Source snapshot exceeds 16 MiB; no partial snapshot was analyzed.');
            const bytes = await readFile(resolved);
            sourceBytes += bytes.length;
            if (sourceBytes > MAX_SOURCE_BYTES) throw new Error('Source snapshot exceeds 16 MiB; no partial snapshot was analyzed.');
            const content = bytes.toString('utf8');
            if (!Buffer.from(content, 'utf8').equals(bytes)) throw new Error('Only valid UTF-8 script sources are supported.');
            sources.push({ file: path.relative(root, absolute).replaceAll('\\', '/'), absolute: resolved, content, bytes, hash: Hash(bytes) });
        }
    }
    await Visit(sourceRoot);
    if (sources.length === 0) throw new Error('Assets/Scripts contains no C# source files.');
    const referenceFiles: string[] = [];
    if (context.managed?.apiPath) referenceFiles.push(await realpath(context.managed.apiPath));
    for (const requested of referencePaths) {
        if (path.extname(requested).toLowerCase() !== '.dll') throw new Error('Metadata references must be DLL files.');
        referenceFiles.push(await ProjectFile(root, requested));
    }
    const references: Snapshot['references'] = [];
    for (const reference of [...new Set(referenceFiles)].sort()) {
        const stat = await lstat(reference);
        if (!stat.isFile() || stat.size > 64 * 1024 * 1024) throw new Error('Metadata reference must be a file no larger than 64 MiB.');
        references.push({ path: reference, hash: Hash(await readFile(reference)) });
    }
    const fingerprint = Hash(JSON.stringify({ root: PathKey(root), sources: sources.map(source => [source.file, source.hash]), references }));
    const after = await Context(bus);
    if (PathKey(await realpath(after.projectRoot)) !== PathKey(root) || after.managed?.apiPath !== context.managed?.apiPath)
        throw new Error('Active project or Script API changed while reading the source snapshot.');
    return { root, sources, references, fingerprint };
}

/// @note Spawn has no shell and never builds or executes the inspected project or its analyzers.
/// @see https://nodejs.org/api/child_process.html#child_processspawncommand-args-options Child process pipes and timeout.
export async function RunRoslyn(request: RecordValue): Promise<RecordValue> {
    const managedRoot = fileURLToPath(new URL('../../Managed/', import.meta.url));
    const candidates = [path.join(managedRoot, 'Fbzz.EditorAnalysis.dll'), ...['Release', 'Development', 'Debug'].map(configuration =>
        path.join(managedRoot, 'bin', configuration, 'net10.0', 'Fbzz.EditorAnalysis.dll'))];
    let helper: string | undefined;
    for (const candidate of candidates) {
        try { if ((await lstat(candidate)).isFile()) { helper = candidate; break; } } catch { }
    }
    if (!helper) throw new Error('Roslyn helper is not built. Run the documented build:analysis task.');
    const input = JSON.stringify(request);
    if (Buffer.byteLength(input) > 32 * 1024 * 1024) throw new Error('Roslyn request exceeds 32 MiB.');
    return new Promise((resolve, reject) => {
        const child = spawn('dotnet', [helper!], { windowsHide: true, shell: false, stdio: ['pipe', 'pipe', 'pipe'], timeout: 60000 });
        let output = '';
        let errors = '';
        let exceeded = false;
        child.stdout.setEncoding('utf8');
        child.stderr.setEncoding('utf8');
        child.stdout.on('data', (chunk: string) => {
            output += chunk;
            if (Buffer.byteLength(output) > 8 * 1024 * 1024) { exceeded = true; child.kill(); }
        });
        child.stderr.on('data', (chunk: string) => { errors = (errors + chunk).slice(-4096); });
        child.once('error', reject);
        child.stdin.once('error', reject);
        child.once('close', code => {
            if (exceeded) { reject(new Error('Roslyn result exceeded 8 MiB.')); return; }
            try {
                const result = ResultSchema.parse(JSON.parse(output));
                if (code !== 0 || !result.ok) { reject(new Error(result.error ?? errors ?? `Roslyn exited with ${code}.`)); return; }
                resolve(result);
            } catch (error) { reject(error); }
        });
        child.stdin.end(input);
    });
}

/// @note Plans belong to one MCP session and retain original bytes; all snapshot files and references are rechecked before apply.
export class CSharpSession {
    private readonly plans = new Map<string, RenamePlan>();
    private applying = false;
    constructor(private readonly bus: EditorBus, private readonly permission: PermissionMode,
        private readonly runner: AnalysisRunner = RunRoslyn, private readonly now: () => number = Date.now) {}

    async Analyze(operation: 'diagnostics' | 'symbols' | 'references', options: AnalysisOptions = {}): Promise<RecordValue> {
        const snapshot = await Capture(this.bus, options.referencePaths ?? []);
        return this.Execute(snapshot, operation, options);
    }

    async Preview(options: AnalysisOptions): Promise<RecordValue> {
        const referencePaths = options.referencePaths ?? [];
        const snapshot = await Capture(this.bus, referencePaths);
        const result = RenameResultSchema.parse(await this.Execute(snapshot, 'rename', options));
        if (result.blocked) return result;
        for (const [id, plan] of this.plans) if (plan.expiresAt <= this.now()) this.plans.delete(id);
        if (this.plans.size >= 8) throw new Error('Eight rename plans are pending; apply a plan or wait for its five-minute expiry.');
        if (result.edits.length === 0) throw new Error('Roslyn returned an empty rename plan.');
        for (const edit of result.edits) {
            const source = snapshot.sources.find(source => source.file === edit.file);
            if (!source || source.content.slice(edit.start, edit.start + edit.length) !== edit.before || edit.after !== options.newName)
                throw new Error('Roslyn returned an invalid source edit.');
        }
        const planId = randomUUID();
        const expiresAt = this.now() + PLAN_LIFETIME_MS;
        this.plans.set(planId, { snapshot, referencePaths, edits: result.edits, expiresAt });
        return { ...result, planId, expiresAt, sourceFingerprint: snapshot.fingerprint, atomic: false };
    }

    private async Execute(snapshot: Snapshot, operation: string, options: AnalysisOptions): Promise<RecordValue> {
        let file = options.file?.replaceAll('\\', '/');
        if (file !== undefined) {
            const resolved = await ProjectFile(snapshot.root, file);
            const source = snapshot.sources.find(source => PathKey(source.absolute) === PathKey(resolved));
            if (!source) throw new Error('Selected file is not in the Assets/Scripts source snapshot.');
            file = source.file;
        }
        const result = ResultSchema.parse(await this.runner({ operation,
            sources: snapshot.sources.map(source => ({ file: source.file, content: source.content })),
            references: snapshot.references.map(reference => reference.path), limit: options.limit ?? 250,
            ...(options.filter === undefined ? {} : { filter: options.filter }),
            ...(file === undefined ? {} : { file }), ...(options.line === undefined ? {} : { line: options.line }),
            ...(options.column === undefined ? {} : { column: options.column }), ...(options.newName === undefined ? {} : { newName: options.newName }) }));
        if (!result.ok) throw new Error(result.error ?? 'Roslyn analysis failed.');
        return { ...result, snapshot: { fingerprint: snapshot.fingerprint, projectRoot: snapshot.root,
            sourceCount: snapshot.sources.length, references: snapshot.references.map(reference => reference.path), scope: 'Assets/Scripts/**/*.cs',
            projectEvaluated: false, analyzersExecuted: false, generatorsExecuted: false } };
    }

    async Apply(planId: string): Promise<RecordValue> {
        if (this.applying) throw new Error('A C# rename apply is already running in this session.');
        this.applying = true;
        try { return await this.ApplyPlan(planId); }
        finally { this.applying = false; }
    }

    private async ApplyPlan(planId: string): Promise<RecordValue> {
        if (this.permission === 'read') throw new Error('Read mode cannot apply C# edits.');
        const plan = this.plans.get(planId);
        if (!plan || plan.expiresAt <= this.now()) { this.plans.delete(planId); throw new Error('Rename plan is absent or expired.'); }
        const snapshot = await Capture(this.bus, plan.referencePaths);
        if (snapshot.fingerprint !== plan.snapshot.fingerprint) throw new Error('Source snapshot, project or metadata reference changed; create a new rename preview.');
        if (this.permission !== 'write') return { status: 'dry-run', planId, edits: plan.edits, sourceFingerprint: snapshot.fingerprint, filesWritten: 0, atomic: false };
        const context = await Context(this.bus);
        if (context.managedBuildBusy) throw new Error('Managed build is active; apply after its completion.');
        this.plans.delete(planId);
        const writes: Array<{ source: SourceFile; bytes: Buffer; temporary: string; applied: boolean }> = [];
        const restoredFiles: string[] = [];
        const recoveryConflicts: string[] = [];
        try {
            for (const source of snapshot.sources) {
                const edits = plan.edits.filter(edit => edit.file === source.file).sort((left, right) => right.start - left.start);
                if (edits.length === 0) continue;
                let content = source.content;
                let boundary = content.length;
                for (const edit of edits) {
                    if (edit.start + edit.length > boundary || content.slice(edit.start, edit.start + edit.length) !== edit.before) throw new Error('Overlapping or stale rename edits.');
                    content = content.slice(0, edit.start) + edit.after + content.slice(edit.start + edit.length);
                    boundary = edit.start;
                }
                const write = { source, bytes: Buffer.from(content, 'utf8'), temporary: `${source.absolute}.roslyn-${randomUUID()}.tmp`, applied: false };
                writes.push(write);
                await writeFile(write.temporary, write.bytes, { flag: 'wx' });
            }
            const beforeCommit = await Capture(this.bus, plan.referencePaths);
            if (beforeCommit.fingerprint !== snapshot.fingerprint) throw new Error('Source snapshot changed while staging the rename.');
            for (const write of writes) {
                if (Hash(await readFile(write.source.absolute)) !== write.source.hash) throw new Error('Source changed during rename publication.');
                await rename(write.temporary, write.source.absolute);
                write.applied = true;
            }
            for (const write of writes) if (Hash(await readFile(write.source.absolute)) !== Hash(write.bytes)) throw new Error('Rename readback failed.');
            return { status: 'success', planId, filesWritten: writes.length, files: writes.map(write => ({ file: write.source.file, beforeHash: write.source.hash, afterHash: Hash(write.bytes) })),
                sourceFingerprint: snapshot.fingerprint, atomic: false, requiresManagedBuild: true, sceneFilesModified: false };
        } catch (error) {
            for (const write of writes.filter(write => write.applied).reverse()) {
                try {
                    if (Hash(await readFile(write.source.absolute)) !== Hash(write.bytes)) { recoveryConflicts.push(write.source.file); continue; }
                    await writeFile(write.temporary, write.source.bytes, { flag: 'wx' });
                    await rename(write.temporary, write.source.absolute);
                    restoredFiles.push(write.source.file);
                } catch { recoveryConflicts.push(write.source.file); }
            }
            return { status: 'failure', planId, atomic: false, error: error instanceof Error ? error.message : String(error),
                appliedFiles: writes.filter(write => write.applied).map(write => write.source.file), restoredFiles, recoveryConflicts };
        } finally {
            for (const write of writes) { try { await unlink(write.temporary); } catch { } }
        }
    }
}
