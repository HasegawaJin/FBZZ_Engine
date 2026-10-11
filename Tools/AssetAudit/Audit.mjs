/// @file    Audit.mjs
/// @brief   Read only asset audit CLI and explicit report contract.
/// @author  Hasegawa Jin
/// @date    2026-10-11
import fs from 'node:fs/promises';
import path from 'node:path';
import { createHash, randomUUID } from 'node:crypto';
import { fileURLToPath } from 'node:url';
import { Parse, Category, TEXTURE_EXTENSIONS } from './Parsers.mjs';

const SEVERITIES = { info: 0, warning: 1, error: 2 };
const PROFILES = {
    mobile: { triangles: 50000, vertices: 100000, bones: 64, maxWeightsPerVertex: 4, materials: 8, textureDimension: 2048, textureBytes: 8388608, durationSeconds: 120, keys: 100000 },
    desktop: { triangles: 500000, vertices: 1000000, bones: 256, maxWeightsPerVertex: 4, materials: 32, textureDimension: 8192, textureBytes: 67108864, durationSeconds: 600, keys: 1000000 },
    cinematic: { triangles: 2000000, vertices: 4000000, bones: 512, maxWeightsPerVertex: 8, materials: 128, textureDimension: 16384, textureBytes: 268435456, durationSeconds: 3600, keys: 4000000 }
};
export function Configuration(overrides = {}) {
    const allowed = new Set(['profile','thresholds','workers','maxFileBytes','maxTextBytes','maxHashBytes','maxTotalHashBytes','maxElements','maxInflatedBytes','maxFiles','settleSeconds','failOn','scope','sharedAssets','_file','_brief','_author','_date']);
    for (const key of Object.keys(overrides)) if (!allowed.has(key)) throw new Error(`unknown configuration key ${key}`);
    const profile = overrides.profile ?? 'desktop';
    if (!Object.hasOwn(PROFILES, profile)) throw new Error(`unknown profile ${profile}`);
    const config = { profile, workers: 1, maxFileBytes: 134217728, maxTextBytes: 16777216, maxHashBytes: 536870912, maxTotalHashBytes: 2147483648, maxElements: 20000000, maxInflatedBytes: 268435456, maxFiles: 25000, settleSeconds: 30, failOn: 'error', scope: 'all', sharedAssets: [], ...overrides, thresholds: { ...PROFILES[profile], ...(overrides.thresholds ?? {}) } };
    for (const [key, value] of Object.entries(config.thresholds)) {
        if (!Object.hasOwn(PROFILES.desktop, key) || !Number.isFinite(value) || value < 0) throw new Error(`invalid threshold ${key}`);
    }
    for (const key of ['workers','maxFileBytes','maxTextBytes','maxHashBytes','maxTotalHashBytes','maxElements','maxInflatedBytes','maxFiles']) {
        if (!Number.isSafeInteger(config[key]) || config[key] < 1) throw new Error(`invalid positive integer ${key}`);
    }
    if (config.workers > 2) throw new Error('workers must be 1 or 2');
    if (config.maxFileBytes > 536870912 || config.maxTextBytes > 33554432 || config.maxInflatedBytes > 536870912 || config.maxElements > 100000000 || config.maxFiles > 100000) throw new Error('configured parser limits exceed hard safety caps');
    if (!Number.isFinite(config.settleSeconds) || config.settleSeconds < 0) throw new Error('invalid settleSeconds');
    if (!Object.hasOwn(SEVERITIES, config.failOn)) throw new Error('failOn must be info, warning or error');
    if (!['all','assets'].includes(config.scope)) throw new Error('scope must be all or assets');
    if (!Array.isArray(config.sharedAssets) || config.sharedAssets.some(p => typeof p !== 'string')) throw new Error('sharedAssets must be an array of read only asset directories');
    return config;
}
function Slash(value) { return value.replaceAll('\\', '/'); }
function Key(value) { const normalized = path.resolve(value); return process.platform === 'win32' ? normalized.toLowerCase() : normalized; }
function Inside(child, parent) { const relative = path.relative(Key(parent), Key(child)); return relative === '' || (!relative.startsWith('..' + path.sep) && relative !== '..' && !path.isAbsolute(relative)); }
function Hash(buffer) { return createHash('sha256').update(buffer).digest('hex'); }
function Stamp(stat) { return { size: Number(stat.size), mtimeNs: stat.mtimeNs.toString(), ctimeNs: stat.ctimeNs.toString(), device: stat.dev.toString(), inode: stat.ino.toString() }; }
function SameStamp(a, b) { return Boolean(a && b && Object.keys(a).every(key => a[key] === b[key])); }
export function SameFingerprint(before, after) { return SameStamp(before?.stamp, after?.stamp) && Boolean(before?.sha256) && before.sha256 === after.sha256; }
function Finding(code, assetPath, severity, evidence, suggestion, certainty = 'confirmed', dependencies = []) { return { code, path: assetPath, severity, certainty, evidence, suggestion, dependencies }; }

/// @note No directory symlink/junction is followed; each root and ancestor is validated before reading or writing.
async function NoLinks(location) {
    let current = path.resolve(location);
    while (true) {
        try { const stat = await fs.lstat(current); if (stat.isSymbolicLink()) throw new Error(`symlink/junction path is excluded: ${current}`); }
        catch (error) { if (error.code !== 'ENOENT') throw error; }
        const parent = path.dirname(current); if (parent === current) break; current = parent;
    }
}
async function Inventory(roots, config) {
    const files = [], directories = [], skipped = [], errors = []; let visited = 0;
    for (const root of roots) {
        const stack = [root.path];
        while (stack.length) {
            const directory = stack.pop(); let entries;
            try { entries = await fs.readdir(directory, { withFileTypes: true }); directories.push(directory); }
            catch (error) { if (error.code === 'ENOENT' && directory === root.path && root.optional) continue; errors.push({ path: directory, reason: error.message }); continue; }
            for (const entry of entries.sort((a,b) => a.name.localeCompare(b.name))) {
                const absolute = path.join(directory, entry.name);
                if (++visited > config.maxFiles) throw new Error(`inventory entry cap ${config.maxFiles} exceeded; narrow the scope or raise maxFiles`);
                let stat; try { stat = await fs.lstat(absolute, { bigint: true }); } catch (error) { errors.push({ path: absolute, reason: error.message }); continue; }
                if (stat.isSymbolicLink()) { skipped.push({ path: absolute, reason: 'symlink/junction' }); continue; }
                if (stat.isDirectory()) stack.push(absolute);
                else if (stat.isFile()) files.push({ absolute, path: root.label + '/' + Slash(path.relative(root.path, absolute)), extension: path.extname(absolute).toLowerCase(), category: Category(path.extname(absolute).toLowerCase()), stamp: Stamp(stat), root: root.label });
                else skipped.push({ path: absolute, reason: 'non-regular input' });
            }
        }
    }
    files.sort((a,b) => a.path.localeCompare(b.path)); return { files, directories, skipped, errors };
}

/// @see https://nodejs.org/api/fs.html FileHandle.read and open read flag; no input handle is opened writable.
async function Fingerprint(file, config) {
    let handle;
    try {
        await NoLinks(file.absolute); handle = await fs.open(file.absolute, 'r');
        const before = Stamp(await handle.stat({ bigint: true }));
        if (before.size > config.maxHashBytes) return { stamp: before, sha256: null, reason: `hash byte cap ${config.maxHashBytes} exceeded` };
        const hash = createHash('sha256'), chunk = Buffer.alloc(262144); let position = 0;
        while (true) {
            const { bytesRead } = await handle.read(chunk, 0, chunk.length, position); if (!bytesRead) break;
            position += bytesRead; if (position > config.maxHashBytes || position > before.size) return { stamp: before, sha256: null, changedDuringRead: true, reason: 'input grew during hashing' };
            hash.update(chunk.subarray(0, bytesRead));
        }
        const after = Stamp(await handle.stat({ bigint: true })), sha256 = hash.digest('hex');
        return { stamp: after, sha256, changedDuringRead: !SameStamp(before, after) || position !== before.size };
    } catch (error) { return { stamp: null, sha256: null, reason: error.message }; }
    finally { await handle?.close(); }
}
async function MapBounded(values, workers, task) {
    const results = new Array(values.length); let next = 0;
    await Promise.all(Array.from({ length: workers }, async () => { while (next < values.length) { const index = next++; results[index] = await task(values[index], index); } }));
    return results;
}
async function ReadBounded(file, cap) {
    await NoLinks(file.absolute); const handle = await fs.open(file.absolute, 'r');
    try {
        const before = Stamp(await handle.stat({ bigint: true })); if (before.size > cap) throw new Error(`parse byte cap ${cap} exceeded`);
        const buffer = Buffer.alloc(before.size); let position = 0;
        while (position < buffer.length) { const { bytesRead } = await handle.read(buffer, position, buffer.length - position, position); if (!bytesRead) break; position += bytesRead; }
        const extra = Buffer.alloc(1), growth = (await handle.read(extra, 0, 1, position)).bytesRead;
        const after = Stamp(await handle.stat({ bigint: true }));
        return { buffer: buffer.subarray(0, position), changed: growth !== 0 || position !== before.size || !SameStamp(before, after) };
    } finally { await handle.close(); }
}

/// @see Projects/Engine/src/Asset/AssetDatabase.cpp DeriveGuid uses two FNV-1a 64-bit offset bases.
export function DeriveGuid(source, subKey) {
    const bytes = Buffer.from(source + '/' + subKey, 'utf8'), mask = (1n << 64n) - 1n;
    const Fold = basis => { let hash = basis; for (const byte of bytes) hash = ((hash ^ BigInt(byte)) * 1099511628211n) & mask; return hash.toString(16).padStart(16, '0'); };
    return Fold(14695981039346656037n) + Fold(1469598103934665603n);
}
function Limits(asset, config) {
    const findings = [], metric = asset.metrics;
    for (const key of ['triangles','vertices','bones','maxWeightsPerVertex','materials','durationSeconds','keys']) {
        if (Number.isFinite(metric[key]) && metric[key] > config.thresholds[key]) findings.push(Finding('BUDGET_' + key.toUpperCase(), asset.path, 'warning', { metric: key, actual: metric[key], limit: config.thresholds[key], profile: config.profile, parser: asset.parser }, 'Compare this asset with its intended usage and adjust the profile or optimize the source manually.'));
    }
    if (asset.category === 'texture') {
        for (const [key, actual] of [['textureDimension',Math.max(metric.width ?? 0, metric.height ?? 0)],['textureBytes',asset.bytes]]) {
            if (actual > config.thresholds[key]) findings.push(Finding('BUDGET_' + key.toUpperCase(), asset.path, 'warning', { actual, limit: config.thresholds[key], profile: config.profile }, 'Review resolution, compression and streaming requirements manually.'));
        }
    }
    for (const clip of metric.clips ?? []) if (clip.durationSeconds > config.thresholds.durationSeconds) findings.push(Finding('BUDGET_CLIP_DURATION', asset.path, 'warning', { clip, limit: config.thresholds.durationSeconds }, 'Review the intended clip range.'));
    return findings;
}
function BaseReference(value) { return value.split('::sprite::')[0].replace(/:\d+$/, ''); }

/// @note Reports differentiate missing GUID authority, usable fallback, and references outside the observed scope.
function References(assets, inventory, project, config, inventoryChanged) {
    const findings = [], guidMap = new Map(), fileMap = new Map(assets.map(a => [Key(a.absolute),a])), directoryKeys = new Set(inventory.directories.map(Key));
    const Register = (guid, absolute, dependency) => { if (!guidMap.has(guid)) guidMap.set(guid, []); guidMap.get(guid).push({ absolute, dependency }); };
    for (const asset of assets) {
        if (asset.metrics.guid) Register(asset.metrics.guid, asset.absolute.slice(0,-5), asset.path);
        const baked = /^Library\/Baked\/([0-9a-f]{32})\/(.+)$/i.exec(asset.path);
        if (baked && (TEXTURE_EXTENSIONS.has(asset.extension) || ['.anim','.mat'].includes(asset.extension))) Register(DeriveGuid(baked[1].toLowerCase(), baked[2]), asset.absolute, asset.path);
    }
    for (const [guid, targets] of guidMap) {
        if (targets.length < 2) continue;
        const contents = targets.map(t => fileMap.get(Key(t.absolute))?.before?.sha256);
        const identical = contents.every(Boolean) && contents.every(x => x === contents[0]);
        const relativeKeys = targets.map(t=> { const asset = fileMap.get(Key(t.absolute)); return asset?.path.replace(/^(?:Assets|Shared\[\d+\])\//,'').toLowerCase(); });
        const sharedCopy = targets.some(t=>fileMap.get(Key(t.absolute))?.root.startsWith('Shared[')) && relativeKeys.every(Boolean) && relativeKeys.every(x=>x === relativeKeys[0]);
        const dependencies = targets.flatMap(t=>[t.dependency, fileMap.get(Key(t.absolute))?.path].filter(Boolean));
        const certainty = dependencies.some(dep=>assets.find(a=>a.path === dep)?.stability !== 'stable') ? 'pending' : 'confirmed';
        findings.push(Finding(sharedCopy ? 'GUID_SHARED_COPY' : identical ? 'GUID_ALIAS_IDENTICAL' : 'GUID_CONFLICT', targets[0].dependency, sharedCopy || identical ? 'info' : 'error', { guid, targets: targets.map(t=>Slash(t.absolute)), identicalBytes: identical, sharedCopy }, 'Review identities and references manually; matching relative shared/project paths are allowed copies by the engine.', certainty, dependencies));
    }
    const uncertainTree = inventoryChanged || inventory.errors.length > 0 || inventory.skipped.length > 0;
    const uncertainIndex = uncertainTree || assets.some(a => a.extension === '.meta' && (a.status === 'unknown' || a.stability !== 'stable'));
    function Resolve(value, source, kind) {
        const base = BaseReference(Slash(value)); let absolute;
        if (/^(?:Assets\/|Library\/Baked\/)/i.test(base)) absolute = path.resolve(project, base);
        else if (kind === 'relative') absolute = path.resolve(path.dirname(source.absolute), base);
        else if (path.isAbsolute(base) || /^[a-z]:\//i.test(base)) absolute = path.resolve(base);
        else return { external: true };
        if (!Inside(absolute, project) && !config.sharedAssets.some(root => Inside(absolute, root))) return { external: true, absolute };
        const target = fileMap.get(Key(absolute)), directory = directoryKeys.has(Key(absolute));
        if (target || directory) return { found: true, absolute, dependencies: target ? [target.path] : [] };
        if (kind === 'project' && /^Assets\//i.test(base)) {
            for (const shared of config.sharedAssets) {
                const sharedAbsolute = path.resolve(shared, base.slice(7)), item = fileMap.get(Key(sharedAbsolute));
                if (item) return { found: true, absolute: sharedAbsolute, dependencies: [item.path] };
            }
        }
        const inScope = /^Assets\//i.test(base) || /^Library\/Baked\//i.test(base) && config.scope === 'all' || kind === 'relative' || config.sharedAssets.some(root=>Inside(absolute,root));
        return { found: false, absolute, external: !inScope };
    }
    for (const asset of assets) {
        if (asset.extension === '.meta' && asset.metrics.guid) {
            const target = asset.absolute.slice(0,-5);
            if (!fileMap.has(Key(target)) && !directoryKeys.has(Key(target))) findings.push(Finding('ORPHAN_META', asset.path, 'warning', { target: Slash(target) }, 'Confirm whether the original is still being produced or intentionally absent; keep its GUID until reviewed.', uncertainIndex ? 'pending' : 'confirmed'));
        }
        for (const reference of asset.references) {
            let code, severity = 'error', certainty = 'confirmed', evidence = { reference: reference.value, location: reference.location }, dependencies = [];
            if (reference.embedded) continue;
            if (/[<>*?\r\n]/.test(reference.value) || /^guid:/i.test(reference.value) && /\s/.test(reference.value) || asset.extension === '.json' && /(?:Destination|referenceForms|compilePlacement)(?:\.|$)/i.test(reference.location)) {
                findings.push(Finding('REFERENCE_TEMPLATE_UNCHECKED',asset.path,'info',evidence,'Review the containing document; a placeholder or explanatory text is not an actionable reference.','unknown')); continue;
            }
            if (/^guid:/i.test(reference.value)) {
                const match = /^guid:([0-9a-f]{32})([^|]*)(?:\|(.*))?$/i.exec(reference.value);
                if (!match) { code = 'INVALID_GUID_REFERENCE'; }
                else {
                    const targets = guidMap.get(match[1].toLowerCase()) ?? [], hint = match[3] ? Resolve(match[3], asset, 'project') : null;
                    const liveTargets = targets.filter(t => fileMap.has(Key(t.absolute)) || directoryKeys.has(Key(t.absolute)));
                    if (liveTargets.length) {
                        dependencies = liveTargets.flatMap(t=>[t.dependency, fileMap.get(Key(t.absolute))?.path].filter(Boolean));
                        if (match[2]) { code = 'SUBASSET_UNCHECKED'; severity = 'info'; certainty = 'unknown'; evidence.suffix = match[2]; }
                        else if (hint?.found && !liveTargets.some(t=>Key(t.absolute) === Key(hint.absolute))) { code = 'STALE_PATH_HINT'; severity = 'info'; evidence.authority = liveTargets.map(t=>Slash(t.absolute)); }
                        else continue;
                    } else if (hint?.found) { code = 'GUID_FALLBACK_ONLY'; severity = 'warning'; dependencies = hint.dependencies; evidence.guid = match[1]; evidence.fallback = Slash(hint.absolute); }
                    else { code = 'MISSING_GUID'; evidence.guid = match[1]; certainty = uncertainIndex || hint?.external ? 'pending' : 'confirmed'; }
                }
            } else {
                const target = Resolve(reference.value, asset, reference.kind);
                if (target.found) continue;
                code = target.external ? 'EXTERNAL_REFERENCE_UNCHECKED' : 'MISSING_PATH';
                certainty = target.external ? 'unknown' : uncertainTree ? 'pending' : 'confirmed';
                if (target.external) severity = 'info'; evidence.resolved = target.absolute ? Slash(target.absolute) : null;
                if (asset.extension === '.fbx' && reference.kind === 'relative' && !target.external) {
                    const alternative = reference.alternative ? Resolve(reference.alternative,asset,'external') : null;
                    code = alternative?.found ? 'FBX_TEXTURE_FALLBACK_ONLY' : 'FBX_TEXTURE_PATH_UNRESOLVED'; severity = 'warning';
                    evidence.alternative = reference.alternative ?? null; evidence.alternativeObserved = alternative?.found ?? false;
                    dependencies = alternative?.dependencies ?? []; certainty = uncertainTree ? 'pending' : 'candidate';
                }
            }
            if (asset.stability !== 'stable' || dependencies.some(dep => assets.find(a=>a.path === dep)?.stability !== 'stable')) certainty = 'pending';
            findings.push(Finding(code, asset.path, severity, evidence, 'Verify GUID authority and fallback paths against the active project; rerun after production settles before changing references.', certainty, dependencies));
        }
    }
    return findings;
}
function Duplicates(assets) {
    const byteGroups = new Map(), similarGroups = new Map(), findings = [];
    for (const asset of assets) {
        if (asset.stability !== 'stable' || !['model','texture','animation','material'].includes(asset.category)) continue;
        if (asset.before.sha256 && asset.bytes > 0) { const key = asset.bytes + ':' + asset.before.sha256; if (!byteGroups.has(key)) byteGroups.set(key, []); byteGroups.get(key).push(asset.path); }
        let signature;
        if (asset.category === 'material' && asset.metrics.normalizedTextHashInput) signature = 'material:' + Hash(Buffer.from(asset.metrics.normalizedTextHashInput));
        if (asset.category === 'texture' && asset.metrics.width) signature = `texture:${asset.extension}:${asset.metrics.width}x${asset.metrics.height}:${Math.floor(Math.log2(Math.max(1,asset.bytes)))}`;
        if (signature) { if (!similarGroups.has(signature)) similarGroups.set(signature, []); similarGroups.get(signature).push(asset.path); }
    }
    const exact = [...byteGroups.values()].filter(group=>group.length>1).map(paths=>({ kind: 'byte_identical_sha256', paths }));
    const candidates = [...similarGroups.entries()].filter(([,group])=>group.length>1).map(([signature,paths])=>({ kind: 'similarity_candidate', method: signature.startsWith('material:') ? 'lexical material values' : 'same format/dimensions and size bucket; not visual similarity', paths }));
    for (const group of exact) findings.push(Finding('BYTE_DUPLICATE', group.paths[0], 'info', group, 'Check usage and references before consolidating; identical bytes can have distinct intended identities.', 'confirmed', group.paths));
    for (const group of candidates) findings.push(Finding('SIMILARITY_CANDIDATE', group.paths[0], 'info', group, 'Compare manually; this is a coarse candidate and does not prove equivalent content.', 'candidate', group.paths));
    return { exact, candidates, findings };
}
export function ExitCode(report, failOn = 'error') {
    if (report.findings.some(f=>f.certainty === 'confirmed' && SEVERITIES[f.severity] >= SEVERITIES[failOn])) return 1;
    return report.incomplete ? 2 : 0;
}

/// @note The phase callback reports progress; a full second hash pass verifies every observed regular input.
export async function Audit(projectPath, config = Configuration(), progress = () => {}) {
    const project = path.resolve(projectPath); await NoLinks(project);
    const roots = [{ path: path.join(project,'Assets'), label: 'Assets' }];
    if (config.scope === 'all') roots.push({ path: path.join(project,'Library','Baked'), label: 'Library/Baked', optional: true });
    for (const [i, root] of config.sharedAssets.entries()) { await NoLinks(root); roots.push({ path: path.resolve(root), label: `Shared[${i}]` }); }
    for (let i = 0; i < roots.length; i++) for (let j = i + 1; j < roots.length; j++) if (Inside(roots[i].path, roots[j].path) || Inside(roots[j].path, roots[i].path)) throw new Error('scan roots must not overlap');
    const startedAt = new Date().toISOString(), inventory = await Inventory(roots, config);
    if (!inventory.directories.includes(roots[0].path)) throw new Error(`project Assets directory cannot be read: ${roots[0].path}`);
    let remainingBytes = config.maxTotalHashBytes;
    const selected = inventory.files.map(file => { const allowed = file.stamp.size <= remainingBytes; if (allowed) remainingBytes -= file.stamp.size; return allowed; });
    await progress('snapshot-before', { files: inventory.files.length, bytes: config.maxTotalHashBytes - remainingBytes, workers: config.workers });
    const before = await MapBounded(inventory.files, config.workers, async (file,i) => {
        const result = selected[i] ? await Fingerprint(file, config) : { stamp: file.stamp, sha256: null, reason: 'total hashing byte budget exceeded' };
        if ((i + 1) % 500 === 0) await progress('snapshot-before-progress',{ processed: i + 1, total: inventory.files.length });
        return result;
    });
    await progress('analyzing', { files: inventory.files.length });
    const scanTime = Date.parse(startedAt), assets = await MapBounded(inventory.files, config.workers, async (file,i) => {
        const asset = { ...file, bytes: file.stamp.size, before: before[i], after: null, stability: 'stable', verifiedUnchanged: true, recent: false, status: 'not_applicable', parser: 'inventory only', metrics: {}, references: [], findings: [], limitations: [] };
        if (!before[i].sha256 || before[i].changedDuringRead || !SameStamp(file.stamp,before[i].stamp)) { asset.stability = 'pending'; asset.verifiedUnchanged = false; }
        if (config.settleSeconds > 0 && scanTime - Number(file.stamp.mtimeNs) / 1000000 < config.settleSeconds * 1000) { asset.stability = 'pending'; asset.recent = true; }
        if (asset.category !== 'other') {
            const parseCap = ['metadata','material','references'].includes(asset.category) || asset.extension === '.obj' ? Math.min(config.maxFileBytes,config.maxTextBytes) : config.maxFileBytes;
            if (file.stamp.size > parseCap || !before[i].sha256) { asset.status = 'unknown'; asset.limitations.push(before[i].reason ?? `parse byte cap ${parseCap} exceeded`); }
            else {
                try {
                    const { buffer, changed } = await ReadBounded(file, parseCap);
                    if (changed || Hash(buffer) !== before[i].sha256) { asset.stability = 'pending'; asset.verifiedUnchanged = false; }
                    Object.assign(asset, Parse(buffer, file.extension, { ...config, metadataGuidExpected: !/^Library\/Baked\//.test(file.path) }));
                } catch (error) { asset.status = 'unknown'; asset.stability = 'pending'; asset.verifiedUnchanged = false; asset.limitations.push(error.message); }
            }
        }
        return asset;
    });
    await progress('snapshot-after', { files: inventory.files.length });
    const after = await MapBounded(inventory.files, config.workers, async(file,i) => {
        const result = selected[i] ? await Fingerprint(file,config) : { stamp: file.stamp, sha256: null, reason: 'total hashing byte budget exceeded' };
        if ((i + 1) % 500 === 0) await progress('snapshot-after-progress',{ processed: i + 1, total: inventory.files.length });
        return result;
    });
    const finalInventory = await Inventory(roots, config), originalKeys = new Set(inventory.files.map(f=>Key(f.absolute))), finalKeys = new Set(finalInventory.files.map(f=>Key(f.absolute)));
    const added = finalInventory.files.filter(f=>!originalKeys.has(Key(f.absolute))).map(f=>f.path), removed = inventory.files.filter(f=>!finalKeys.has(Key(f.absolute))).map(f=>f.path);
    const originalDirectories = new Set(inventory.directories.map(Key)), finalDirectories = new Set(finalInventory.directories.map(Key));
    const directoriesAdded = finalInventory.directories.filter(d=>!originalDirectories.has(Key(d))).map(Slash), directoriesRemoved = inventory.directories.filter(d=>!finalDirectories.has(Key(d))).map(Slash);
    const inventoryChanged = added.length > 0 || removed.length > 0 || directoriesAdded.length > 0 || directoriesRemoved.length > 0 || finalInventory.errors.length > 0 || finalInventory.skipped.length > 0;
    const findings = [];
    for (const [i, asset] of assets.entries()) {
        asset.after = after[i];
        const finalStamp = finalInventory.files.find(f=>Key(f.absolute) === Key(asset.absolute))?.stamp;
        if (!SameFingerprint(asset.before, asset.after) || asset.after.changedDuringRead || !SameStamp(asset.after.stamp, finalStamp)) { asset.stability = 'pending'; asset.verifiedUnchanged = false; }
        if (asset.stability === 'pending') findings.push(Finding('INPUT_UNSETTLED', asset.path, 'info', { before: asset.before, after: asset.after, settleSeconds: config.settleSeconds }, 'Wait for production to settle and rerun the audit.', 'pending'));
        findings.push(...asset.findings.map(f=>({ ...f, path: asset.path, certainty: asset.stability === 'stable' ? 'confirmed' : 'pending', dependencies: [] })), ...Limits(asset,config).map(f=>({ ...f, certainty: asset.stability === 'stable' ? 'confirmed' : 'pending' })));
    }
    if (inventoryChanged) findings.push(Finding('INVENTORY_CHANGED', '', 'info', { added, removed, directoriesAdded, directoriesRemoved, errors: finalInventory.errors }, 'Rerun after the asset tree stops changing.', 'pending'));
    for (const entry of [...inventory.errors,...inventory.skipped]) findings.push(Finding('INPUT_UNOBSERVED', Slash(entry.path), 'info', entry, 'Review permissions or copy this input into the observed tree without links.', 'unknown'));
    findings.push(...References(assets,inventory,project,config,inventoryChanged));
    const duplicates = Duplicates(assets); findings.push(...duplicates.findings);
    const coverage = {};
    for (const asset of assets) {
        coverage[asset.category] ??= { total: 0, supported: 0, partial: 0, unknown: 0, not_applicable: 0, pending: 0 };
        coverage[asset.category].total++; coverage[asset.category][asset.status]++; if (asset.stability !== 'stable') coverage[asset.category].pending++;
    }
    for (const count of Object.values(coverage)) { count.readablePercent = count.total ? 100 * (count.supported + count.partial) / count.total : 0; count.completePercent = count.total ? 100 * count.supported / count.total : 0; }
    const scan = {
        files: assets.length, bytes: assets.reduce((sum,a)=>sum+a.bytes,0), added, removed,
        unknown: assets.filter(a=>a.status === 'unknown').length, partial: assets.filter(a=>a.status === 'partial').length,
        pending: assets.filter(a=>a.stability !== 'stable').length, recentInputs: assets.filter(a=>a.recent).length,
        hashChanges: assets.filter(a=>a.before.sha256 && a.after.sha256 && a.before.sha256 !== a.after.sha256).length,
        unverifiedInputs: assets.filter(a=>!a.verifiedUnchanged).length,
        inputUnchanged: !inventoryChanged && assets.every(a=>a.verifiedUnchanged) && !inventory.errors.length && !inventory.skipped.length,
        verification: 'Every selected regular file: inventory stat + pre-SHA256 + parsed bytes hash + post-SHA256 + final stat; timestamps/ctime/inode/size compared. Recent timestamps are pending even when hashes agree. No atomic filesystem snapshot is claimed.'
    };
    const report = { schemaVersion: 1, tool: 'FBZZ AssetAudit', runId: randomUUID(), startedAt, finishedAt: new Date().toISOString(), project: Slash(project), config, inputRoots: roots.map(r=>Slash(r.path)), scan, coverage, findings, duplicates: { exact: duplicates.exact, candidates: duplicates.candidates }, assets };
    report.incomplete = report.scan.unknown > 0 || report.scan.partial > 0 || report.scan.pending > 0 || !report.scan.inputUnchanged || findings.some(f=>['pending','unknown'].includes(f.certainty));
    report.exitCode = ExitCode(report,config.failOn); report.outcome = report.exitCode === 1 ? 'confirmed_findings' : report.exitCode === 2 ? 'incomplete' : 'complete';
    for (const asset of assets) { delete asset.absolute; delete asset.findings; delete asset.metrics.normalizedTextHashInput; }
    return report;
}
function Escape(value) { return String(value ?? '').replaceAll('&','&amp;').replaceAll('<','&lt;').replaceAll('>','&gt;').replaceAll('"','&quot;').replaceAll("'",'&#39;'); }
export function Html(report) {
    const rows = report.findings.map(f=>`<tr><td>${Escape(f.path)}</td><td>${Escape(f.severity)} / ${Escape(f.certainty)}</td><td>${Escape(f.code)}</td><td><pre>${Escape(JSON.stringify(f.evidence,null,2))}</pre></td><td>${Escape(f.suggestion)}</td></tr>`).join('');
    const coverage = Object.entries(report.coverage).map(([category,c])=>`<tr><td>${Escape(category)}</td><td>${c.total}</td><td>${c.supported}</td><td>${c.partial}</td><td>${c.unknown}</td><td>${c.pending}</td><td>${c.readablePercent.toFixed(1)}%</td><td>${c.completePercent.toFixed(1)}%</td></tr>`).join('');
    const assets = report.assets.filter(a=>a.category !== 'other').map(a=>`<details><summary>${Escape(a.path)} — ${Escape(a.status)} / ${Escape(a.stability)}</summary><pre>${Escape(JSON.stringify({ parser:a.parser, metrics:a.metrics, limitations:a.limitations },null,2))}</pre></details>`).join('');
    return `<!doctype html><html lang="ja"><meta charset="utf-8"><meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline'"><title>FBZZ AssetAudit</title><style>body{font:15px system-ui;margin:30px;color:#17283b}table{border-collapse:collapse;width:100%;margin:20px 0}td,th{border:1px solid #bac7d5;padding:9px;text-align:left;vertical-align:top}th{background:#e8eef5}pre{white-space:pre-wrap;overflow-wrap:anywhere;margin:0}summary{padding:8px;cursor:pointer}h1{color:#16476b}.status{padding:16px;background:#fff2cd}</style><h1>FBZZ AssetAudit</h1><p>${Escape(report.project)} — ${Escape(report.finishedAt)}</p><p class="status">Outcome: ${Escape(report.outcome)} / exit ${report.exitCode}. Incomplete: ${report.incomplete}. Supported means implemented checks; partial and unknown require review. No auto fixes or deletion.</p><p>Files ${report.scan.files}, bytes ${report.scan.bytes}, pending ${report.scan.pending}, unknown ${report.scan.unknown}. Input unchanged: ${report.scan.inputUnchanged}. Profile ${Escape(report.config.profile)}, workers ${report.config.workers}.</p><h2>Coverage</h2><table><tr><th>Category</th><th>Total</th><th>Supported</th><th>Partial</th><th>Unknown</th><th>Pending</th><th>Readable</th><th>Complete</th></tr>${coverage}</table><h2>Findings</h2><table><tr><th>Path</th><th>Severity / certainty</th><th>Code</th><th>Evidence</th><th>Suggested review</th></tr>${rows}</table><h2>Asset metrics and limits</h2>${assets}<h2>Input verification</h2><p>${Escape(report.scan.verification)}</p></html>`;
}
export async function WriteReport(report, outputPath) {
    const output = path.resolve(outputPath);
    if (Inside(output,report.project) || report.inputRoots.some(root=>Inside(output,root))) throw new Error('reports must be outside the project and all observed input roots');
    await NoLinks(output); await fs.mkdir(output,{ recursive: true }); await NoLinks(output);
    const jsonPath = path.join(output,'Audit.json'), htmlPath = path.join(output,'Audit.html');
    await NoLinks(jsonPath); await NoLinks(htmlPath);
    const existing = await Promise.all([jsonPath,htmlPath].map(async p=> { try { await fs.lstat(p); return true; } catch(error) { if (error.code === 'ENOENT') return false; throw error; } }));
    if (existing.some(Boolean)) throw new Error('report files already exist; choose a new output directory');
    const jsonHandle = await fs.open(jsonPath,'wx');
    try { await jsonHandle.writeFile(JSON.stringify(report,null,2)); } finally { await jsonHandle.close(); }
    const htmlHandle = await fs.open(htmlPath,'wx');
    try { await htmlHandle.writeFile(Html(report)); } finally { await htmlHandle.close(); }
    return { json: jsonPath, html: htmlPath };
}

const HELP = `FBZZ AssetAudit (Node.js >=22, no dependencies)\nUsage: node Audit.mjs --project <project directory> --output <new report directory>\nOptions: --profile mobile|desktop|cinematic --config <JSON> --workers 1|2\n         --scope all|assets --shared-assets <directory> (repeatable)\n         --settle-seconds <seconds> --fail-on info|warning|error\nInputs: Assets and, by default, Library/Baked. Nothing is imported or written there.\nOutput: exclusive Audit.json and escaped static Audit.html, outside all input roots.\nExit: 0=complete with no configured findings; 1=confirmed findings;\n      2=incomplete/unknown/partial/unsettled with no finding at fail-on; 3=usage/fatal.\nConfirmed finding exit 1 takes priority over incomplete exit 2; inspect incomplete.\n`;
async function Main(args) {
    if (!args.length || args.includes('--help') || args.includes('-h')) { console.log(HELP); return; }
    const options = {}, shared = [], map = { '--profile':'profile','--workers':'workers','--scope':'scope','--settle-seconds':'settleSeconds','--fail-on':'failOn' }; let project, output, configFile;
    for (let i = 0; i < args.length; i++) {
        const flag = args[i], value = args[++i]; if (!value || value.startsWith('--')) throw new Error(`missing value for ${flag}`);
        if (flag === '--project') project = value; else if (flag === '--output') output = value; else if (flag === '--config') configFile = value; else if (flag === '--shared-assets') shared.push(path.resolve(value));
        else if (map[flag]) options[map[flag]] = ['--workers','--settle-seconds'].includes(flag) ? Number(value) : value;
        else throw new Error(`unknown option ${flag}`);
    }
    if (!project || !output) throw new Error('--project and --output are required');
    const fromFile = configFile ? JSON.parse(await fs.readFile(configFile,'utf8')) : {};
    if (shared.length) options.sharedAssets = shared;
    const config = Configuration({ ...fromFile,...options });
    if (Inside(output, project) || config.sharedAssets.some(root=>Inside(output,root))) throw new Error('output must be outside the project and shared inputs');
    const report = await Audit(project,config,(phase,details)=>console.error(JSON.stringify({ phase,...details })));
    const outputs = await WriteReport(report,output);
    console.log(JSON.stringify({ outcome:report.outcome, exitCode:report.exitCode, incomplete:report.incomplete, scan:report.scan, outputs })); process.exitCode = report.exitCode;
}
if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) Main(process.argv.slice(2)).catch(error=> { console.error(JSON.stringify({ outcome:'fatal', exitCode:3, error:error.message })); process.exitCode = 3; });
