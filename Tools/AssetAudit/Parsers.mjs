/// @file    Parsers.mjs
/// @brief   Bounded, side effect free asset decoders.
/// @author  Hasegawa Jin
/// @date    2026-10-11
import { inflateSync } from 'node:zlib';

export class UnknownFormat extends Error {}
export class InvalidFormat extends Error {}

class Reader {
    constructor(buffer, limit) { this.buffer = buffer; this.offset = 0; this.limit = limit; this.operations = 0; }
    Take(size) {
        if (!Number.isSafeInteger(size) || size < 0 || size > this.buffer.length - this.offset) throw new InvalidFormat(`truncated payload at byte ${this.offset}, need ${size}`);
        const start = this.offset; this.offset += size; return start;
    }
    Count(count, stride = 1) {
        if (!Number.isSafeInteger(count) || count < 0 || count * stride > this.buffer.length - this.offset) throw new InvalidFormat(`invalid or oversized count ${count} at byte ${this.offset}`);
        if (count > this.limit) throw new UnknownFormat(`element budget exceeded by count ${count}`);
        this.operations += count;
        if (this.operations > this.limit) throw new UnknownFormat('parser operation budget exceeded');
        return count;
    }
    U32() { return this.buffer.readUInt32LE(this.Take(4)); }
    I32() { return this.buffer.readInt32LE(this.Take(4)); }
    F32() { return this.buffer.readFloatLE(this.Take(4)); }
    F64() { return this.buffer.readDoubleLE(this.Take(8)); }
    Text(size) { return this.buffer.subarray(this.Take(size), this.offset).toString('utf8').split('\0')[0]; }
    Floats(count) { this.Count(count, 4); return Array.from({ length: count }, () => this.F32()); }
    Done() { if (this.offset !== this.buffer.length) throw new UnknownFormat(`unparsed trailing bytes: ${this.buffer.length - this.offset}`); }
}

function Result(parser, status = 'supported') { return { parser, status, metrics: {}, references: [], findings: [], limitations: [] }; }
function Issue(result, code, evidence, severity = 'error', suggestion = 'Review the source asset and exporter settings; make any corrections manually.') {
    if (result.findings.length < 100) result.findings.push({ code, severity, evidence, suggestion });
    else result.omittedFindings = (result.omittedFindings ?? 0) + 1;
}
function Finite(result, values, location) {
    if (values.some(value => !Number.isFinite(value))) Issue(result, 'NON_FINITE', { location, values: values.filter(v => !Number.isFinite(v)).map(String) });
}
function Hierarchy(result, parents, children, root, label) {
    const count = parents.length;
    if (root < -1 || root >= count) Issue(result, 'INVALID_ROOT', { label, root, count });
    const states = new Uint8Array(count);
    for (let i = 0; i < count; i++) {
        const parent = parents[i];
        if (parent < -1 || parent >= count || parent === i) Issue(result, 'INVALID_PARENT', { label, node: i, parent, count });
        for (const child of children[i]) {
            if (child < 0 || child >= count || parents[child] !== i) Issue(result, 'INVALID_CHILD', { label, node: i, child, count });
        }
        const chain = []; let node = i;
        while (node >= 0 && node < count && states[node] === 0) { states[node] = 1; chain.push(node); node = parents[node]; }
        if (node >= 0 && node < count && states[node] === 1) Issue(result, 'HIERARCHY_CYCLE', { label, node });
        for (const entry of chain) states[entry] = 2;
    }
}

/// @see Projects/Engine/include/Engine/Format/FzAssetFormat.hpp FZSK layout.
function Skeleton(reader, result) {
    if (reader.Text(4) !== 'FZSK') throw new InvalidFormat('expected FZSK magic');
    const version = reader.U32();
    if (version !== 1) throw new UnknownFormat(`unsupported skeleton version ${version}`);
    const root = reader.I32(), nodeCount = reader.U32(), boneCount = reader.U32();
    Finite(result, reader.Floats(16), 'rootInverse'); reader.Count(nodeCount, 180);
    const parents = [], children = [];
    for (let i = 0; i < nodeCount; i++) {
        reader.Text(64); parents.push(reader.I32()); const bone = reader.I32();
        if (bone < -1 || bone >= boneCount) Issue(result, 'INVALID_BONE_INDEX', { node: i, bone, boneCount });
        Finite(result, reader.Floats(26), `node[${i}]`);
        const childCount = reader.U32(); reader.Count(childCount, 4);
        children.push(Array.from({ length: childCount }, () => reader.I32()));
    }
    reader.Count(boneCount, 136);
    for (let i = 0; i < boneCount; i++) {
        reader.Text(64); const node = reader.I32();
        if (node < 0 || node >= nodeCount) Issue(result, 'INVALID_BONE_NODE', { bone: i, node, nodeCount });
        Finite(result, reader.Floats(16), `bone[${i}]`); reader.Take(4);
    }
    Hierarchy(result, parents, children, root, 'skeleton');
    result.metrics.bones = boneCount; result.metrics.skeletonNodes = nodeCount;
}

/// @see Projects/Graphics/include/Graphics/Renderer/Mesh.hpp Vertex/SkinnedVertex.
function MeshPayload(reader, result, vertices, indices, format, version, label) {
    if (format !== 0 && format !== 1) throw new UnknownFormat(`unsupported vertex format ${format}`);
    const stride = format === 1 ? 76 : version >= 5 ? 60 : 44;
    reader.Count(vertices, stride); const boneIndices = new Set(); let maxInfluences = 0, badWeights = 0;
    for (let i = 0; i < vertices; i++) {
        Finite(result, reader.Floats(11), `${label}.vertex[${i}]`);
        if (format === 1) {
            const ids = Array.from({ length: 4 }, () => reader.U32()), weights = reader.Floats(4);
            let sum = 0, influences = 0;
            weights.forEach((weight, j) => { sum += weight; if (weight > 0) { influences++; boneIndices.add(ids[j]); } });
            if (weights.some(w => !Number.isFinite(w) || w < 0 || w > 1) || Math.abs(sum - 1) > 0.02) badWeights++;
            maxInfluences = Math.max(maxInfluences, influences);
        } else if (stride === 60) Finite(result, reader.Floats(4), `${label}.color[${i}]`);
    }
    if (badWeights) Issue(result, 'INVALID_WEIGHTS', { label, affectedVertices: badWeights, tolerance: 0.02 });
    reader.Count(indices, 4); let invalidIndices = 0;
    for (let i = 0; i < indices; i++) if (reader.U32() >= vertices) invalidIndices++;
    if (invalidIndices) Issue(result, 'INVALID_VERTEX_INDEX', { label, invalidIndices, vertices });
    if (indices % 3) Issue(result, 'NON_TRIANGLE_INDEX_COUNT', { label, indices });
    result.metrics.maxWeightsPerVertex = Math.max(result.metrics.maxWeightsPerVertex ?? 0, maxInfluences);
    return { vertices, indices, triangles: Math.floor(indices / 3), usedBoneIndices: [...boneIndices] };
}

/// @see Projects/Engine/include/Engine/Asset/FzModelFormat.hpp v4/v5 layout.
function Fz(buffer, extension, limits) {
    const reader = new Reader(buffer, limits.maxElements), result = Result('FBZZ binary');
    if (extension === '.skel') { Skeleton(reader, result); reader.Done(); return result; }
    const magic = reader.Text(4), version = reader.U32(); result.metrics.version = version;
    if (extension === '.mesh') {
        if (magic !== 'FZMH') throw new InvalidFormat('expected FZMH magic');
        if (![1, 2].includes(version)) throw new UnknownFormat(`unsupported mesh version ${version}`);
        const flags = reader.U32(), vertices = reader.U32(), indices = reader.U32();
        if (flags & ~1) throw new UnknownFormat(`unknown mesh flags ${flags}`);
        Finite(result, reader.Floats(4), 'bounds');
        Object.assign(result.metrics, MeshPayload(reader, result, vertices, indices, flags & 1, version === 2 ? 5 : 4, 'mesh'));
        delete result.metrics.usedBoneIndices;
        if (flags & 1) { result.status = 'partial'; result.limitations.push('Mesh has no embedded skeleton; bone index range cannot be validated.'); }
    } else {
        if (magic !== 'FZMD') throw new InvalidFormat('expected FZMD magic');
        if (![4, 5].includes(version)) throw new UnknownFormat(`unsupported model version ${version}; current engine accepts v4/v5`);
        const flags = reader.U32(), lodCount = reader.U32(), materials = reader.U32();
        if (flags & ~15) throw new UnknownFormat(`unknown model flags ${flags}`);
        if (!lodCount) throw new InvalidFormat('model has zero LODs');
        Finite(result, reader.Floats(4), 'bounds'); reader.Take(12); reader.Count(materials, 64);
        const materialNames = Array.from({ length: materials }, () => reader.Text(64));
        result.metrics.materials = materials; result.metrics.materialNames = materialNames; result.metrics.lods = [];
        reader.Count(lodCount, 8); const usedBones = new Set();
        for (let li = 0; li < lodCount; li++) {
            const threshold = reader.F32(), submeshCount = reader.U32(); Finite(result, [threshold], `lod[${li}].threshold`);
            reader.Count(submeshCount, 164); const lod = { threshold, submeshes: [], vertices: 0, triangles: 0 };
            for (let si = 0; si < submeshCount; si++) {
                const slot = reader.U32(), format = reader.U32(), vertices = reader.U32(), indices = reader.U32();
                Finite(result, reader.Floats(4), `lod[${li}].mesh[${si}].bounds`);
                if (slot >= materials) Issue(result, 'INVALID_MATERIAL_SLOT', { lod: li, submesh: si, slot, materials });
                const morphs = reader.U32(), name = reader.Text(128);
                const mesh = MeshPayload(reader, result, vertices, indices, format, version, `lod[${li}].mesh[${si}]`);
                mesh.usedBoneIndices.forEach(id => usedBones.add(id)); delete mesh.usedBoneIndices;
                Object.assign(mesh, { name, materialSlot: slot, morphs }); lod.submeshes.push(mesh); lod.vertices += vertices; lod.triangles += mesh.triangles;
                reader.Count(morphs, 132);
                for (let mi = 0; mi < morphs; mi++) {
                    reader.Text(128); const count = reader.U32();
                    if (count !== vertices) throw new InvalidFormat('morph vertex count differs from submesh');
                    reader.Count(count, 36);
                    for (let vi = 0; vi < count; vi++) Finite(result, reader.Floats(9), `morph[${mi}].vertex[${vi}]`);
                }
            }
            result.metrics.lods.push(lod);
        }
        result.metrics.vertices = result.metrics.lods[0].vertices; result.metrics.triangles = result.metrics.lods[0].triangles;
        if (flags & 1) {
            Skeleton(reader, result);
            const invalid = [...usedBones].filter(id => id >= result.metrics.bones);
            if (invalid.length) Issue(result, 'INVALID_BONE_INDEX', { invalid, bones: result.metrics.bones });
        } else result.metrics.bones = 0;
        if (flags & 4) {
            if (reader.Text(4) !== 'FZND') throw new InvalidFormat('expected FZND magic');
            const count = reader.U32(), root = reader.I32(); reader.Take(4); reader.Count(count, 116);
            const parents = [], children = [];
            for (let ni = 0; ni < count; ni++) {
                reader.Text(64); parents.push(reader.I32()); Finite(result, reader.Floats(10), `modelNode[${ni}]`);
                const meshes = reader.U32(), childCount = reader.U32(); reader.Count(meshes, 4);
                for (let j = 0; j < meshes; j++) {
                    const index = reader.U32(); if (index >= result.metrics.lods[0].submeshes.length) Issue(result, 'INVALID_MESH_INDEX', { node: ni, index });
                }
                reader.Count(childCount, 4); children.push(Array.from({ length: childCount }, () => reader.I32()));
            }
            Hierarchy(result, parents, children, root, 'model'); result.metrics.nodes = count;
        }
    }
    reader.Done(); return result;
}

/// @see Projects/Engine/src/Asset/AnimationImporter.cpp LoadV3 and its fixed layouts.
function Animation(buffer, limits) {
    const reader = new Reader(buffer, limits.maxElements), result = Result('FBZZ animation v3');
    if (reader.Text(4) !== 'FZAN') throw new InvalidFormat('expected FZAN magic');
    const version = reader.U32(); result.metrics.version = version;
    if (version !== 3) throw new UnknownFormat(`unsupported animation version ${version}; current engine accepts v3`);
    const name = reader.Text(128), durationTicks = reader.F64(), ticksPerSecond = reader.F64(), tracks = reader.U32(); reader.Take(4);
    const durationSeconds = reader.F64(), frameRate = reader.F32(); reader.Take(8);
    const rootMotionTrackIndex = reader.U32(), events = reader.U32(), propertyTracks = reader.U32(); Finite(result, reader.Floats(3), 'optimization'); reader.Take(4);
    Object.assign(result.metrics, { name, durationTicks, ticksPerSecond, durationSeconds, frameRate, tracks, propertyTracks, events, keys: 0 });
    Finite(result, [durationTicks, ticksPerSecond, durationSeconds, frameRate], 'animation header');
    if (durationTicks < 0 || durationSeconds < 0 || ticksPerSecond <= 0 || frameRate <= 0) Issue(result, 'INVALID_CLIP_TIMING', { durationTicks, ticksPerSecond, durationSeconds, frameRate });
    if (Number.isFinite(durationSeconds) && ticksPerSecond > 0 && Math.abs(durationTicks / ticksPerSecond - durationSeconds) > 0.02) Issue(result, 'CLIP_DURATION_MISMATCH', { durationTicks, ticksPerSecond, durationSeconds }, 'warning');
    if (rootMotionTrackIndex !== 0xffffffff && rootMotionTrackIndex >= tracks) Issue(result, 'INVALID_ROOT_MOTION_TRACK', { rootMotionTrackIndex, tracks });
    reader.Count(events, 80);
    for (let i = 0; i < events; i++) {
        const time = reader.F64(); reader.Text(64); reader.I32(); const value = reader.F32(); Finite(result, [time, value], `event[${i}]`);
        if (time < 0 || time > durationSeconds + 1e-6) Issue(result, 'EVENT_OUT_OF_RANGE', { event: i, time, durationSeconds });
    }
    function Keys(count, stride, floatCount, label, kind = '') {
        reader.Count(count, stride); result.metrics.keys += count; let previous = -Infinity, badTimes = 0, badQuaternions = 0;
        for (let i = 0; i < count; i++) {
            const time = reader.F64(), values = reader.Floats(floatCount); Finite(result, [time, ...values], `${label}.key[${i}]`);
            if (time < previous || time < 0 || time > durationTicks + 1e-6) badTimes++;
            previous = time;
            if (kind === 'quaternion' && Math.abs(values.reduce((sum, v) => sum + v * v, 0) - 1) > 0.02) badQuaternions++;
            reader.Take(stride - 8 - floatCount * 4);
        }
        if (badTimes) Issue(result, 'INVALID_KEY_TIMES', { label, affectedKeys: badTimes, durationTicks });
        if (badQuaternions) Issue(result, 'INVALID_QUATERNIONS', { label, affectedKeys: badQuaternions });
    }
    reader.Count(tracks, 400);
    for (let ti = 0; ti < tracks; ti++) {
        reader.Text(256); reader.Text(128); const interp = reader.buffer[reader.Take(1)]; reader.Take(3);
        if (interp > 2) Issue(result, 'INVALID_INTERPOLATION', { track: ti, interp });
        const positions = reader.U32(), rotations = reader.U32(), scales = reader.U32();
        Keys(positions, 24, 3, `track[${ti}].position`); Keys(rotations, 24, 4, `track[${ti}].rotation`, 'quaternion'); Keys(scales, 24, 3, `track[${ti}].scale`);
    }
    reader.Count(propertyTracks, 484);
    for (let ti = 0; ti < propertyTracks; ti++) {
        reader.Text(256); reader.Text(64); reader.Text(128);
        const start = reader.Take(4), target = buffer[start], type = buffer[start + 1], interp = buffer[start + 2]; reader.I32(); reader.I32();
        if (target > 2 || type > 6 || interp > 2) Issue(result, 'INVALID_PROPERTY_TRACK_TYPE', { track: ti, target, type, interp });
        const counts = Array.from({ length: 6 }, () => reader.U32());
        for (const [i, stride, floats] of [[0,24,3],[1,16,2],[2,24,3],[3,24,4],[4,16,0],[5,16,0]]) Keys(counts[i], stride, floats, `property[${ti}].type[${i}]`);
    }
    reader.Done(); return result;
}

/// @see https://raw.githubusercontent.com/blender/blender-addons/main/io_scene_fbx/parse_fbx.py Binary types and 7500 header widths; independent implementation, no code copied.
/// @see https://nodejs.org/api/zlib.html#class-options maxOutputLength bounds compressed arrays.
function Fbx(buffer, limits) {
    if (!buffer.subarray(0, 23).equals(Buffer.from('Kaydara FBX Binary  \0\x1a\0', 'binary'))) throw new UnknownFormat('ASCII FBX or non-FBX data; binary FBX only');
    const reader = new Reader(buffer, limits.maxElements); reader.Take(23); const version = reader.U32();
    if (version < 7000 || version > 7700) throw new UnknownFormat(`unsupported FBX version ${version}`);
    const wide = version >= 7500, sentinel = wide ? 25 : 13; let inflatedBytes = 0; const invalidFloatArrays = [];
    function SafeU64() { const value = buffer.readBigUInt64LE(reader.Take(8)); if (value > BigInt(Number.MAX_SAFE_INTEGER)) throw new InvalidFormat('FBX offset exceeds safe integer'); return Number(value); }
    function Property() {
        const type = String.fromCharCode(buffer[reader.Take(1)]);
        if (type === 'Y') return buffer.readInt16LE(reader.Take(2));
        if ('BCZ'.includes(type)) return buffer[reader.Take(1)];
        if (type === 'I') return reader.I32(); if (type === 'F') return reader.F32(); if (type === 'D') return reader.F64();
        if (type === 'L') return buffer.readBigInt64LE(reader.Take(8)).toString();
        if ('SR'.includes(type)) { const length = reader.U32(), start = reader.Take(length); return type === 'S' ? buffer.subarray(start, reader.offset).toString('utf8') : length ? { embeddedBytes: length } : null; }
        const strides = { f: 4, d: 8, l: 8, i: 4, b: 1, c: 1 }, stride = strides[type];
        if (!stride) throw new UnknownFormat(`unknown FBX property type ${type}`);
        const count = reader.U32(), encoding = reader.U32(), compressedLength = reader.U32();
        if (count > limits.maxElements) throw new UnknownFormat('FBX array element budget exceeded');
        reader.operations += count; if (reader.operations > limits.maxElements) throw new UnknownFormat('FBX total element budget exceeded');
        const start = reader.Take(compressedLength), expected = count * stride; inflatedBytes += expected;
        if (inflatedBytes > limits.maxInflatedBytes) throw new UnknownFormat('FBX decompression budget exceeded');
        let data = buffer.subarray(start, reader.offset);
        if (encoding === 1) {
            try { data = inflateSync(data, { maxOutputLength: Math.max(1, expected) }); } catch (error) { throw new InvalidFormat(`invalid FBX compressed array: ${error.message}`); }
        } else if (encoding !== 0) throw new UnknownFormat(`unknown FBX array encoding ${encoding}`);
        if (data.length !== expected) throw new InvalidFormat(`FBX array size ${data.length}, expected ${expected}`);
        if (type === 'f' || type === 'd') {
            for (let i = 0; i < count; i++) if (!Number.isFinite(type === 'f' ? data.readFloatLE(i*4) : data.readDoubleLE(i*8))) { invalidFloatArrays.push({ offset: start, type, scalar: i }); break; }
        }
        return { count, Get: i => type === 'd' ? data.readDoubleLE(i*8) : type === 'f' ? data.readFloatLE(i*4) : type === 'i' ? data.readInt32LE(i*4) : type === 'l' ? data.readBigInt64LE(i*8) : data[i] };
    }
    function Node(depth, boundary) {
        if (depth > 64) throw new UnknownFormat('FBX nesting exceeds 64');
        reader.Count(1, sentinel);
        const end = wide ? SafeU64() : reader.U32(), count = wide ? SafeU64() : reader.U32(), bytes = wide ? SafeU64() : reader.U32(), nameLength = buffer[reader.Take(1)];
        if (!end) { if (count || bytes || nameLength) throw new InvalidFormat('invalid FBX null record'); return null; }
        if (end <= reader.offset || end > boundary) throw new InvalidFormat(`invalid FBX node end ${end}`);
        const name = reader.Text(nameLength), propertiesEnd = reader.offset + bytes;
        if (propertiesEnd > end || count > limits.maxElements) throw new InvalidFormat('invalid FBX property list');
        const properties = Array.from({ length: count }, () => Property());
        if (reader.offset !== propertiesEnd) throw new InvalidFormat('FBX property length mismatch');
        const children = [];
        if (reader.offset < end) {
            while (reader.offset < end - sentinel) { const child = Node(depth + 1, end - sentinel); if (!child) throw new InvalidFormat('premature FBX null record'); children.push(child); }
            const start = reader.Take(sentinel);
            if (buffer.subarray(start, reader.offset).some(v => v)) throw new InvalidFormat('invalid FBX child sentinel');
        }
        if (reader.offset !== end) throw new InvalidFormat('FBX node end mismatch');
        return { name, properties, children };
    }
    const roots = [];
    while (reader.offset + sentinel <= buffer.length) { const node = Node(0, buffer.length); if (!node) break; roots.push(node); }
    const result = Result('FBX binary statistics', 'partial');
    for (const array of invalidFloatArrays) Issue(result, 'NON_FINITE', { location: 'FBX floating array', ...array });
    result.limitations.push('Source control points and fan triangle estimate; engine import splits, exact triangulation, runtime skin palette, embedded media decoding and deformation are not evaluated.');
    const objects = roots.find(n => n.name === 'Objects')?.children;
    if (!objects) throw new InvalidFormat('FBX Objects block absent');
    const child = (n, name) => n.children.find(c => c.name === name)?.properties[0];
    let vertices = 0, triangles = 0, polygons = 0, keys = 0, bones = 0, maxInfluences = 0;
    const geometryInfo = [], clusterInfo = new Map(), curves = [], clips = [];
    for (const object of objects) {
        if (object.name === 'Geometry' && object.properties[2] === 'Mesh') {
            const points = child(object, 'Vertices'), indices = child(object, 'PolygonVertexIndex');
            if (!points?.Get || !indices?.Get || points.count % 3) throw new InvalidFormat('invalid FBX mesh arrays');
            const count = points.count / 3; let corners = 0, localTriangles = 0, invalidIndices = 0, badPolygons = 0;
            for (let i = 0; i < points.count; i++) if (!Number.isFinite(points.Get(i))) { Issue(result, 'NON_FINITE', { location: 'FBX Vertices', scalar: i }); break; }
            for (let i = 0; i < indices.count; i++) {
                const index = indices.Get(i), vertex = index < 0 ? -index - 1 : index; corners++;
                if (vertex >= count) invalidIndices++;
                if (index < 0) { if (corners < 3) badPolygons++; localTriangles += Math.max(0, corners - 2); polygons++; corners = 0; }
            }
            if (corners || badPolygons || invalidIndices) Issue(result, 'INVALID_FBX_POLYGONS', { danglingCorners: corners, badPolygons, invalidIndices });
            vertices += count; triangles += localTriangles; geometryInfo.push({ id: String(object.properties[0]), name: object.properties[1], vertices: count, triangles: localTriangles });
        }
        if (object.name === 'Deformer' && object.properties[2] === 'Cluster') {
            bones++; const indices = child(object, 'Indexes'), weights = child(object, 'Weights');
            if (indices === undefined && weights === undefined) { clusterInfo.set(String(object.properties[0]), []); continue; }
            if (!indices?.Get || !weights?.Get || indices.count !== weights.count) throw new InvalidFormat('invalid FBX cluster arrays');
            const entries = []; let invalid = 0;
            for (let i = 0; i < weights.count; i++) {
                const weight = weights.Get(i), vertex = indices.Get(i);
                if (!Number.isFinite(weight) || weight < 0 || weight > 1 || vertex < 0) invalid++;
                if (weight > 0) entries.push([vertex, weight]);
            }
            if (invalid) Issue(result, 'INVALID_WEIGHTS', { cluster: object.properties[1], entries: invalid });
            clusterInfo.set(String(object.properties[0]), entries);
        }
        if (object.name === 'Texture' || object.name === 'Video') {
            const relative = child(object, 'RelativeFilename') ?? child(object, 'RelativeFileName'), absolute = child(object, 'FileName') ?? child(object, 'Filename');
            if (typeof relative === 'string' && relative) result.references.push({ value: relative.replaceAll('\\', '/'), kind: 'relative', location: object.properties[1], alternative: absolute, embedded: Boolean(child(object, 'Content')) });
            else if (typeof absolute === 'string' && absolute) result.references.push({ value: absolute.replaceAll('\\', '/'), kind: 'external', location: object.properties[1] });
        }
        if (object.name === 'AnimationCurve') {
            const times = child(object, 'KeyTime'), values = child(object, 'KeyValueFloat');
            if (!times?.Get || !values?.Get || times.count !== values.count) throw new InvalidFormat('invalid FBX animation curve');
            keys += times.count; let previous = null, invalid = 0;
            for (let i = 0; i < times.count; i++) { const time = times.Get(i); if ((previous !== null && time < previous) || !Number.isFinite(values.Get(i))) invalid++; previous = time; }
            if (invalid) Issue(result, 'INVALID_FBX_KEYS', { curve: object.properties[1], affectedKeys: invalid });
            curves.push({ name: object.properties[1], keys: times.count });
        }
        if (object.name === 'AnimationStack') {
            /// @see https://help.autodesk.com/cloudhelp/2020/ENU/FBX-API-Reference/cpp_ref/class_fbx_anim_stack.html LocalStart and LocalStop default to zero; span markers can differ from curve extents.
            /// @see https://download.autodesk.com/us/fbx/sdkdocs/fbx_sdk_help/files/fbxsdkref/ktime_8h-source.html KTIME_ONE_SECOND = 46186158000.
            const properties = object.children.find(n => n.name === 'Properties70')?.children ?? [];
            const explicitStart = properties.find(n => n.properties[0] === 'LocalStart')?.properties.at(-1), explicitStop = properties.find(n => n.properties[0] === 'LocalStop')?.properties.at(-1);
            const start = explicitStart ?? '0', stop = explicitStop ?? '0', seconds = Number(BigInt(stop) - BigInt(start)) / 46186158000;
            if (seconds !== null && seconds < 0) Issue(result, 'INVALID_CLIP_TIMING', { name: object.properties[1], seconds });
            clips.push({ name: object.properties[1], durationSeconds: seconds, meaning: 'FBX local span marker', defaultStart: explicitStart === undefined, defaultStop: explicitStop === undefined });
        }
    }
    const connections = roots.find(n => n.name === 'Connections')?.children ?? [], parent = new Map();
    for (const c of connections) if (c.properties[0] === 'OO') parent.set(String(c.properties[1]), String(c.properties[2]));
    const models = objects.filter(o=>o.name === 'Model'), modelIds = new Map(models.map((o,i)=>[String(o.properties[0]),i]));
    const modelParents = models.map(o=>modelIds.get(parent.get(String(o.properties[0]))) ?? -1), modelChildren = models.map(()=>[]);
    modelParents.forEach((p,i)=> { if (p >= 0) modelChildren[p].push(i); });
    Hierarchy(result,modelParents,modelChildren,modelParents.findIndex(p=>p === -1),'FBX models');
    const weightsByGeometry = new Map();
    for (const [cluster, entries] of clusterInfo) {
        const skin = parent.get(cluster), geometry = parent.get(skin), info = geometryInfo.find(g => g.id === geometry);
        if (!info) { result.limitations.push(`Unresolved skin connection ${cluster}; aggregate weight sums unavailable.`); continue; }
        if (!weightsByGeometry.has(geometry)) weightsByGeometry.set(geometry, new Map());
        const weights = weightsByGeometry.get(geometry);
        for (const [vertex, weight] of entries) {
            if (vertex >= info.vertices) { Issue(result, 'INVALID_VERTEX_INDEX', { cluster, vertex, vertices: info.vertices }); continue; }
            const previous = weights.get(vertex) ?? [0, 0]; weights.set(vertex, [previous[0] + 1, previous[1] + weight]);
        }
    }
    for (const [geometry, weights] of weightsByGeometry) {
        let invalid = 0; for (const [influences, sum] of weights.values()) { maxInfluences = Math.max(maxInfluences, influences); if (Math.abs(sum - 1) > 0.02) invalid++; }
        if (invalid) Issue(result, 'WEIGHT_SUM_CANDIDATE', { geometry, affectedVertices: invalid, tolerance: 0.02 }, 'warning', 'Check FBX skin link mode and normalize weights manually if appropriate.');
    }
    Object.assign(result.metrics, { version, vertices, triangles, polygons, bones, boneMeaning: 'skin cluster count', maxWeightsPerVertex: maxInfluences, materials: objects.filter(n => n.name === 'Material').length, keys, clips, geometry: geometryInfo, animationCurves: curves.length });
    return result;
}

/// @see https://www.w3.org/TR/png/ IHDR and chunk CRC; dimension inspection does not decode pixels.
function Crc32(bytes) { let crc = 0xffffffff; for (const byte of bytes) { crc ^= byte; for (let j = 0; j < 8; j++) crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0); } return (crc ^ 0xffffffff) >>> 0; }
/// @see https://learn.microsoft.com/en-us/windows/win32/direct3ddds/dds-header DDS_HEADER dimensions.
function Texture(buffer, extension) {
    const result = Result('texture header', 'partial'); let width, height;
    if (extension === '.png') {
        if (buffer.length < 33 || !buffer.subarray(0,8).equals(Buffer.from([137,80,78,71,13,10,26,10])) || buffer.toString('ascii',12,16) !== 'IHDR' || buffer.readUInt32BE(8) !== 13) throw new InvalidFormat('invalid PNG IHDR');
        if (Crc32(buffer.subarray(12,29)) !== buffer.readUInt32BE(29)) throw new InvalidFormat('PNG IHDR CRC mismatch');
        width = buffer.readUInt32BE(16); height = buffer.readUInt32BE(20); result.metrics.bitDepth = buffer[24]; result.metrics.colorType = buffer[25];
        let offset = 8, end = false;
        while (offset + 12 <= buffer.length) {
            const size = buffer.readUInt32BE(offset), type = buffer.toString('ascii',offset+4,offset+8);
            if (size > buffer.length - offset - 12) throw new InvalidFormat('truncated PNG chunk');
            offset += size + 12; if (type === 'IEND') { end = true; break; }
        }
        if (!end) throw new InvalidFormat('PNG IEND absent');
    } else if (extension === '.dds') {
        if (buffer.length < 128 || buffer.toString('ascii',0,4) !== 'DDS ' || buffer.readUInt32LE(4) !== 124) throw new InvalidFormat('invalid DDS header');
        height = buffer.readUInt32LE(12); width = buffer.readUInt32LE(16); result.metrics.mips = buffer.readUInt32LE(28); result.metrics.fourCC = buffer.toString('ascii',84,88);
    } else if (['.jpg','.jpeg'].includes(extension)) {
        if (buffer.length < 4 || buffer.readUInt16BE(0) !== 0xffd8) throw new InvalidFormat('invalid JPEG SOI');
        let offset = 2;
        while (offset < buffer.length) {
            if (buffer[offset++] !== 0xff) throw new InvalidFormat('invalid JPEG marker');
            while (buffer[offset] === 0xff) offset++;
            const marker = buffer[offset++]; if (marker === 0xda || marker === 0xd9) break;
            if (marker === 0x01 || marker >= 0xd0 && marker <= 0xd7) continue;
            if (offset + 2 > buffer.length) throw new InvalidFormat('truncated JPEG segment');
            const length = buffer.readUInt16BE(offset); if (length < 2 || offset + length > buffer.length) throw new InvalidFormat('invalid JPEG segment size');
            if ([0xc0,0xc1,0xc2,0xc3,0xc5,0xc6,0xc7,0xc9,0xca,0xcb,0xcd,0xce,0xcf].includes(marker)) { if (length < 8) throw new InvalidFormat('short JPEG SOF'); height = buffer.readUInt16BE(offset+3); width = buffer.readUInt16BE(offset+5); break; }
            offset += length;
        }
    } else if (extension === '.gif') {
        if (buffer.length < 13 || !['GIF87a','GIF89a'].includes(buffer.toString('ascii',0,6))) throw new InvalidFormat('invalid GIF header');
        width = buffer.readUInt16LE(6); height = buffer.readUInt16LE(8);
    } else if (extension === '.bmp') {
        if (buffer.length < 54 || buffer.toString('ascii',0,2) !== 'BM' || buffer.readUInt32LE(14) < 40) throw new UnknownFormat('unsupported BMP header');
        width = buffer.readInt32LE(18); height = Math.abs(buffer.readInt32LE(22));
    } else if (extension === '.tga') {
        if (buffer.length < 18 || ![1,2,3,9,10,11].includes(buffer[2])) throw new UnknownFormat('unsupported TGA header');
        width = buffer.readUInt16LE(12); height = buffer.readUInt16LE(14);
    } else throw new UnknownFormat(`unsupported texture format ${extension}`);
    if (!Number.isInteger(width) || !Number.isInteger(height) || width <= 0 || height <= 0 || width > 0x7fffffff || height > 0x7fffffff) throw new InvalidFormat('invalid texture dimensions');
    const rgbaBytes = BigInt(width) * BigInt(height) * 4n;
    Object.assign(result.metrics, { width, height, bytes: buffer.length, estimatedRGBA8Bytes: rgbaBytes <= BigInt(Number.MAX_SAFE_INTEGER) ? Number(rgbaBytes) : null, estimatedRGBA8BytesExact: rgbaBytes.toString() });
    result.limitations.push('Header dimensions and file capacity only; pixels, visual similarity, DDS surfaces and complete codec validation are not decoded. RGBA8 estimate excludes mips, compression and arrays.');
    return result;
}

/// @see https://toml.io/en/v1.0.0 Strings and comments; conservative lexical extraction, not a TOML schema validator.
function TextAsset(buffer, extension, limits) {
    const text = new TextDecoder('utf-8', { fatal: true }).decode(buffer), result = Result(extension === '.json' ? 'JSON' : 'TOML string inspection', 'partial');
    if (/[\x00-\x08\x0b\x0c\x0e-\x1f]/.test(text)) throw new UnknownFormat('binary/control data instead of recognized text asset');
    const tokens = []; let clean = '', line = 1;
    if (extension === '.json' || text.trimStart().startsWith('{')) {
        let value; try { value = JSON.parse(text); } catch (error) { throw new InvalidFormat(`invalid JSON: ${error.message}`); }
        const stack = [[value, '$', 0]];
        while (stack.length) {
            const [node, location, depth] = stack.pop(); if (depth > 64) throw new UnknownFormat('JSON nesting exceeds 64');
            if (typeof node === 'string') tokens.push({ value: node, location });
            else if (node && typeof node === 'object') for (const [key, child] of Object.entries(node)) stack.push([child, `${location}.${key}`, depth + 1]);
        }
        result.parser = 'JSON strings';
    } else {
        for (let i = 0; i < text.length;) {
            const c = text[i];
            if (c === '#') { while (i < text.length && text[i] !== '\n') i++; continue; }
            if (c === '\n') { clean += '\n'; line++; i++; continue; }
            if (c !== '"' && c !== "'") { clean += c; i++; continue; }
            const quote = c, startLine = line, triple = text.slice(i, i + 3) === quote.repeat(3), marker = triple ? quote.repeat(3) : quote;
            i += marker.length; let raw = '', closed = false;
            while (i < text.length) {
                if (text.slice(i, i + marker.length) === marker) { i += marker.length; closed = true; break; }
                if (text[i] === '\n') { if (!triple) throw new InvalidFormat('newline in single-line TOML string'); line++; }
                if (quote === '"' && text[i] === '\\') { raw += text[i++]; if (i < text.length) raw += text[i++]; }
                else raw += text[i++];
            }
            if (!closed) throw new InvalidFormat('unterminated TOML string');
            if (triple) raw = raw.replace(/^\r?\n/, '');
            let value = raw;
            if (quote === '"') {
                if (triple) raw = raw.replace(/\\[ \t]*\r?\n[ \t\r\n]*/g, '');
                value = '';
                const escapes = { b: '\b', t: '\t', n: '\n', f: '\f', r: '\r', '"': '"', '\\': '\\' };
                for (let ri = 0; ri < raw.length; ri++) {
                    if (raw[ri] !== '\\') { value += raw[ri]; continue; }
                    const escape = raw[++ri];
                    if (Object.hasOwn(escapes, escape)) value += escapes[escape];
                    else if (escape === 'u' || escape === 'U') {
                        const length = escape === 'u' ? 4 : 8, digits = raw.slice(ri + 1, ri + 1 + length), point = Number.parseInt(digits,16);
                        if (digits.length !== length || !/^[0-9a-fA-F]+$/.test(digits) || point > 0x10ffff || point >= 0xd800 && point <= 0xdfff) throw new InvalidFormat('invalid TOML Unicode escape');
                        value += String.fromCodePoint(point); ri += length;
                    } else throw new UnknownFormat('unsupported TOML string escape');
                }
            }
            tokens.push({ value, location: `line ${startLine}` }); clean += '""';
        }
        if (/(?:^|[\s=,[\]])[+-]?(?:nan|inf)(?=$|[\s,\]])/im.test(clean)) Issue(result, 'NON_FINITE', { location: 'TOML scalar', type: 'nan/inf' });
        result.limitations.push('Quoted values only; full TOML syntax, component schemas and numeric ranges are not validated.');
    }
    for (const token of tokens) {
        if (/^(?:guid:|Assets[\\/]|Library[\\/]Baked[\\/])/i.test(token.value)) result.references.push({ ...token, kind: 'project' });
    }
    if (extension === '.meta') {
        const match = /(?:^|\n)\s*\[meta\]\s*(?:#[^\n]*)?\r?\n([\s\S]*?)(?=\n\s*\[|$)/.exec(text);
        const guid = match && /(?:^|\n)\s*guid\s*=\s*(['"])([0-9a-fA-F]{32})\1\s*(?:#[^\n]*)?(?:\r?\n|$)/.exec(match[1]) || /(?:^|\n)\s*meta\s*=\s*\{\s*guid\s*=\s*(['"])([0-9a-fA-F]{32})\1\s*\}\s*(?:#[^\n]*)?(?:\r?\n|$)/.exec(text);
        if (!guid) {
            if (limits.metadataGuidExpected === false && /(?:^|\n)\s*\[texture\]/.test(text) && !/(?:^|\n)\s*\[meta\]/.test(text)) {
                result.metrics.identity = 'baked texture settings only; GUID is derived from source';
            } else throw new UnknownFormat('meta [meta].guid or inline meta.guid absent or unsupported');
        } else result.metrics.guid = guid[2].toLowerCase();
        result.status = 'partial';
    }
    if (extension === '.mat') {
        result.metrics.materials = 1;
        result.metrics.textureReferences = result.references.filter(r => /\.(?:png|jpg|jpeg|dds|tga|bmp|exr|hdr)(?:$|[:|])/i.test(r.value)).length;
        result.metrics.normalizedTextHashInput = clean.replace(/\s+/g, ' ').trim() + '\n' + JSON.stringify(tokens.map(t => t.value));
        result.limitations.push('Material similarity uses lexical values, not shader semantics or rendered appearance.');
    }
    return result;
}

function Obj(buffer) {
    const result = Result('OBJ statistics', 'partial'), text = new TextDecoder('utf-8', { fatal: true }).decode(buffer);
    let vertices = 0, triangles = 0, faces = 0; const materials = new Set();
    for (const [i, raw] of text.split(/\r?\n/).entries()) {
        const line = raw.replace(/#.*/, '').trim(), tokens = line.split(/\s+/);
        if (tokens[0] === 'v') { vertices++; Finite(result, tokens.slice(1).map(Number), `line ${i + 1}`); }
        if (tokens[0] === 'f') { faces++; triangles += Math.max(0, tokens.length - 3); if (tokens.length < 4) Issue(result, 'INVALID_OBJ_FACE', { line: i + 1 }); }
        if (tokens[0] === 'usemtl') materials.add(tokens.slice(1).join(' '));
        if (tokens[0] === 'mtllib') result.references.push({ value: tokens.slice(1).join(' '), kind: 'relative', location: `line ${i + 1}` });
    }
    Object.assign(result.metrics, { vertices, triangles, faces, materials: materials.size, bones: 0 });
    result.limitations.push('Control point counts and fan triangle estimate only; OBJ vertex indices, external material schemas and engine triangulation are not validated.');
    return result;
}

export const MODEL_EXTENSIONS = new Set(['.fbx','.obj','.mesh','.fzasset','.skel','.gltf','.glb','.blend']);
export const TEXTURE_EXTENSIONS = new Set(['.png','.jpg','.jpeg','.dds','.tga','.bmp','.gif','.hdr','.exr','.webp','.ktx','.ktx2']);
export const TEXT_EXTENSIONS = new Set(['.meta','.mat','.asset','.fzdata','.scene','.prefab','.terrain','.animcontroller','.animctrl','.avatarmask','.json','.toml','.ibl']);
export function Category(extension) { return MODEL_EXTENSIONS.has(extension) ? 'model' : TEXTURE_EXTENSIONS.has(extension) ? 'texture' : extension === '.anim' ? 'animation' : extension === '.meta' ? 'metadata' : extension === '.mat' ? 'material' : TEXT_EXTENSIONS.has(extension) ? 'references' : 'other'; }
export function Parse(buffer, extension, limits = { maxElements: 20000000, maxInflatedBytes: 268435456 }) {
    try {
        if (['.fzasset','.mesh','.skel'].includes(extension)) return Fz(buffer, extension, limits);
        if (extension === '.anim') return Animation(buffer, limits);
        if (extension === '.fbx') return Fbx(buffer, limits);
        if (extension === '.obj') return Obj(buffer);
        if (TEXTURE_EXTENSIONS.has(extension)) return Texture(buffer, extension);
        if (TEXT_EXTENSIONS.has(extension)) {
            if (buffer.length > (limits.maxTextBytes ?? 16777216)) throw new UnknownFormat('text parsing byte budget exceeded');
            if (extension === '.ibl' && buffer.subarray(0,6).equals(Buffer.from('FZIBL\0'))) {
                if (buffer.length !== 1040 || buffer.readUInt16LE(6) !== 1) throw new UnknownFormat('unsupported IBL layout');
                const result = Result('FBZZ IBL descriptor');
                for (let i = 0; i < 4; i++) { const value = buffer.subarray(16+i*256,16+(i+1)*256).toString('utf8').split('\0')[0]; if (value) result.references.push({ value, kind: 'relative', location: `IBL slot ${i}` }); }
                return result;
            }
            return TextAsset(buffer, extension, limits);
        }
        throw new UnknownFormat(`unsupported format ${extension || '(no extension)'}`);
    } catch (error) {
        const result = Result('unavailable', 'unknown');
        result.limitations.push(error.message);
        if (!(error instanceof UnknownFormat)) Issue(result, 'MALFORMED_ASSET', { reason: error.message });
        return result;
    }
}
