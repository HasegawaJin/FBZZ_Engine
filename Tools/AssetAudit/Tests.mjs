/// @file    Tests.mjs
/// @brief   Small deterministic fixtures for parser and read only CLI contracts.
/// @author  Hasegawa Jin
/// @date    2026-10-11
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { createHash } from 'node:crypto';
import { deflateSync } from 'node:zlib';
import { Audit, Configuration, DeriveGuid, ExitCode, Html, WriteReport } from './Audit.mjs';
import { Parse } from './Parsers.mjs';

function U32(value) { const b=Buffer.alloc(4); b.writeUInt32LE(value); return b; }
function F64(value) { const b=Buffer.alloc(8); b.writeDoubleLE(value); return b; }
function FloatArray(values) { const b=Buffer.alloc(values.length*4); values.forEach((v,i)=>b.writeFloatLE(v,i*4)); return b; }
function Mesh(version=2, skinned=false) {
    const header=Buffer.alloc(36); header.write('FZMH'); header.writeUInt32LE(version,4); header.writeUInt32LE(skinned?1:0,8); header.writeUInt32LE(3,12); header.writeUInt32LE(3,16);
    const stride=skinned?76:version===2?60:44, vertices=Buffer.alloc(3*stride);
    if (skinned) for(let i=0;i<3;i++) vertices.writeFloatLE(1,i*stride+60);
    return Buffer.concat([header,vertices,U32(0),U32(1),U32(2)]);
}
function Animation() {
    const header=Buffer.alloc(160), ext=Buffer.alloc(48), track=Buffer.alloc(400), key=Buffer.alloc(24);
    header.write('FZAN'); header.writeUInt32LE(3,4); header.write('日本語Clip',8); header.writeDoubleLE(30,136); header.writeDoubleLE(30,144); header.writeUInt32LE(1,152);
    ext.writeDoubleLE(1,0); ext.writeFloatLE(30,8); ext.writeUInt32LE(0xffffffff,20); track.write('Root',256); track.writeUInt32LE(1,388); key.writeDoubleLE(0);
    return Buffer.concat([header,ext,track,key]);
}
function Skeleton(cycle=false) {
    const header=Buffer.alloc(84); header.write('FZSK'); header.writeUInt32LE(1,4); header.writeUInt32LE(1,12);
    const node=Buffer.alloc(180); node.writeInt32LE(cycle?0:-1,64); node.writeInt32LE(-1,68);
    return Buffer.concat([header,node]);
}
function Model() {
    const header=Buffer.alloc(48), lod=Buffer.alloc(8), submesh=Buffer.alloc(32), extension=Buffer.alloc(132);
    header.write('FZMD'); header.writeUInt32LE(5,4); header.writeUInt32LE(1,12); header.writeUInt32LE(1,16); lod.writeUInt32LE(1,4); submesh.writeUInt32LE(3,8); submesh.writeUInt32LE(3,12);
    return Buffer.concat([header,Buffer.alloc(64),lod,submesh,extension,Mesh().subarray(36)]);
}
function ArrayProperty(type, values, compressed=true) {
    const raw=type==='d'?Buffer.concat(values.map(F64)):Buffer.concat(values.map(v=>{ const b=Buffer.alloc(4);b.writeInt32LE(v);return b; }));
    const data=compressed?deflateSync(raw):raw; return Buffer.concat([Buffer.from(type),U32(values.length),U32(compressed?1:0),U32(data.length),data]);
}
function StringProperty(value) { const b=Buffer.from(value); return Buffer.concat([Buffer.from('S'),U32(b.length),b]); }
function LongProperty(value) { const b=Buffer.alloc(9); b.write('L'); b.writeBigInt64LE(BigInt(value),1); return b; }
function Fbx(version=7400, malformed=false, emptyCluster=false) {
    const wide=version>=7500, sentinel=wide?25:13;
    function Node(name, properties, children, start) {
        const header=Buffer.alloc(sentinel), title=Buffer.from(name), body=Buffer.concat(properties); let offset=start+sentinel+title.length+body.length;
        const nested=children.map(c=>{const child=Node(c[0],c[1],c[2],offset);offset+=child.length;return child;});
        if(children.length)offset+=sentinel;
        if(wide){header.writeBigUInt64LE(BigInt(offset),0);header.writeBigUInt64LE(BigInt(properties.length),8);header.writeBigUInt64LE(BigInt(body.length),16);header[24]=title.length;}
        else{header.writeUInt32LE(offset,0);header.writeUInt32LE(properties.length,4);header.writeUInt32LE(body.length,8);header[12]=title.length;}
        return Buffer.concat([header,title,body,...nested,...(children.length?[Buffer.alloc(sentinel)]:[])]);
    }
    const geometry=['Geometry',[LongProperty(1),StringProperty('Geometry::日本語'),StringProperty('Mesh')],[['Vertices',[ArrayProperty('d',[0,0,0,1,0,0,0,1,0])],[]],['PolygonVertexIndex',[ArrayProperty('i',[0,1,-3])],[]]]];
    const cluster=['Deformer',[LongProperty(2),StringProperty('UnusedBone'),StringProperty('Cluster')],[]];
    const b=Buffer.concat([Buffer.from('Kaydara FBX Binary  \0\x1a\0','binary'),U32(version),Node('Objects',[],[geometry,...(emptyCluster?[cluster]:[])],27),Buffer.alloc(sentinel)]);
    if(malformed)b.writeUInt32LE(0xffffffff,27);
    return b;
}
const limits={ maxElements:20000000, maxInflatedBytes:268435456 };
test('FBZZ mesh v1/v2, skinned weights and v5 model counts',()=>{
    for(const version of [1,2]){const r=Parse(Mesh(version),'.mesh',limits);assert.equal(r.status,'supported');assert.equal(r.metrics.vertices,3);assert.equal(r.metrics.triangles,1);}
    const skinned=Mesh(2,true);skinned.writeFloatLE(NaN,36+60);assert.ok(Parse(skinned,'.mesh',limits).findings.some(f=>f.code==='INVALID_WEIGHTS'));
    const model=Parse(Model(),'.fzasset',limits);assert.equal(model.status,'supported');assert.equal(model.metrics.materials,1);assert.equal(model.metrics.triangles,1);
});
test('bounded huge counts, truncation, unsupported version and index validation',()=>{
    const huge=Mesh();huge.writeUInt32LE(0xffffffff,12);assert.equal(Parse(huge,'.mesh',limits).status,'unknown');
    assert.ok(Parse(Mesh().subarray(0,39),'.mesh',limits).findings.some(f=>f.code==='MALFORMED_ASSET'));
    assert.equal(Parse(Mesh(77),'.mesh',limits).status,'unknown');const b=Mesh();b.writeUInt32LE(99,b.length-4);assert.ok(Parse(b,'.mesh',limits).findings.some(f=>f.code==='INVALID_VERTEX_INDEX'));
});
test('skeleton hierarchy cycle and animation length/key/NaN',()=>{
    assert.equal(Parse(Skeleton(),'.skel',limits).status,'supported');assert.ok(Parse(Skeleton(true),'.skel',limits).findings.some(f=>f.code==='HIERARCHY_CYCLE'));
    const good=Parse(Animation(),'.anim',limits);assert.equal(good.status,'supported');assert.equal(good.metrics.keys,1);assert.equal(good.metrics.durationSeconds,1);
    const bad=Animation();bad.writeDoubleLE(31,bad.length-24);bad.writeFloatLE(NaN,bad.length-16);const result=Parse(bad,'.anim',limits);assert.ok(result.findings.some(f=>f.code==='NON_FINITE'));assert.ok(result.findings.some(f=>f.code==='INVALID_KEY_TIMES'));
    const old=Animation();old.writeUInt32LE(2,4);assert.equal(Parse(old,'.anim',limits).status,'unknown');
});
test('binary FBX 32/64-bit headers and compressed arrays',()=>{
    for(const version of [7400,7500]){const r=Parse(Fbx(version),'.fbx',limits);assert.equal(r.status,'partial',JSON.stringify(r));assert.equal(r.metrics.vertices,3);assert.equal(r.metrics.triangles,1);}
    assert.ok(Parse(Fbx(7400,true),'.fbx',limits).findings.some(f=>f.code==='MALFORMED_ASSET'));
    const r=Parse(Fbx(),'.fbx',{...limits,maxInflatedBytes:4});assert.equal(r.status,'unknown');assert.equal(r.findings.length,0);
    const empty=Parse(Fbx(7400,false,true),'.fbx',limits);assert.equal(empty.status,'partial');assert.equal(empty.metrics.bones,1);assert.ok(!empty.findings.some(f=>f.code==='MALFORMED_ASSET'));
});
test('PNG header CRC/truncation and dimensions; unsupported texture',()=>{
    const png=Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+jN1sAAAAASUVORK5CYII=','base64');
    assert.equal(Parse(png,'.png',limits).metrics.width,1);const corrupt=Buffer.from(png);corrupt[18]=9;assert.ok(Parse(corrupt,'.png',limits).findings.length);
    assert.ok(Parse(png.subarray(0,40),'.png',limits).findings.length);assert.equal(Parse(png,'.exr',limits).status,'unknown');
});
test('TOML refs skip comments, preserve escaped Unicode and detect non finite',()=>{
    const text='# path = "Assets/ignored.png"\n[meta]\nguid = "0123456789abcdef0123456789abcdef"\n[material]\npath="Assets/\\u65e5本語/a.png"\nnumber = nan\n';
    const r=Parse(Buffer.from(text),'.meta',limits);assert.equal(r.metrics.guid,'0123456789abcdef0123456789abcdef');assert.equal(r.references.length,1);assert.equal(r.references[0].value,'Assets/日本語/a.png');assert.ok(r.findings.some(f=>f.code==='NON_FINITE'));
    assert.equal(Parse(Buffer.from('x="unterminated'),'.mat',limits).status,'unknown');assert.equal(Parse(Buffer.from('{bad'),'.json',limits).status,'unknown');
});
test('inline meta GUID and multiline/8-digit Unicode TOML strings',()=>{
    const text='meta = {guid = "0123456789abcdef0123456789abcdef"}\nx="""\n説明\n"""\ny="Assets/\\U000065e5本語/a.png"';
    const result=Parse(Buffer.from(text),'.meta',limits);assert.equal(result.metrics.guid,'0123456789abcdef0123456789abcdef');assert.equal(result.references[0].value,'Assets/日本語/a.png');
});
test('profiles and exit contract reject unsafe/invalid configuration',()=>{
    assert.equal(Configuration({profile:'mobile'}).thresholds.bones,64);assert.throws(()=>Configuration({workers:3}));assert.throws(()=>Configuration({thresholds:{bones:NaN}}));assert.throws(()=>Configuration({typo:1}));
    assert.equal(ExitCode({incomplete:false,findings:[]}),0);assert.equal(ExitCode({incomplete:true,findings:[]}),2);assert.equal(ExitCode({incomplete:true,findings:[{severity:'error',certainty:'pending'}]}),2);assert.equal(ExitCode({incomplete:true,findings:[{severity:'error',certainty:'confirmed'}]}),1);
    assert.equal(DeriveGuid('0123456789abcdef0123456789abcdef','anims/Walk.anim').length,32);
});
test('derived texture settings sidecar without GUID is partial only in baked scope',()=>{
    const bytes=Buffer.from('[texture]\nsrgb = true\n');
    assert.equal(Parse(bytes,'.meta',limits).status,'unknown');
    const result=Parse(bytes,'.meta',{...limits,metadataGuidExpected:false});assert.equal(result.status,'partial');assert.ok(result.metrics.identity);assert.equal(result.metrics.guid,undefined);
});
async function Fixture(body) {
    const root=await fs.mkdtemp(path.join(os.tmpdir(),'FBZZ-AssetAudit-'));const project=path.join(root,'日本語 & Project'), assets=path.join(project,'Assets');await fs.mkdir(assets,{recursive:true});
    try { await body({root,project,assets}); } finally {
        const temporaryRoot=path.resolve(os.tmpdir()), resolved=path.resolve(root), relative=path.relative(temporaryRoot,resolved);
        assert.ok(!relative.startsWith('..') && !path.isAbsolute(relative) && path.basename(resolved).startsWith('FBZZ-AssetAudit-'));
        await fs.rm(resolved,{recursive:true,force:true});
    }
}
test('end-to-end CLI: byte invariance, missing refs, duplicates, escaped HTML, reports isolated',async()=>Fixture(async({root,project,assets})=>{
    const mesh=Mesh();await fs.writeFile(path.join(assets,'日本語 & a.mesh'),mesh);await fs.writeFile(path.join(assets,'b.mesh'),mesh);
    await fs.writeFile(path.join(assets,'ref.mat'),'shader="Assets/<bad&>.hlsl"\n');
    await fs.writeFile(path.join(assets,'missing.mat'),'shader="Assets/missing.hlsl"');
    const report=await Audit(project,Configuration({settleSeconds:0}));assert.equal(report.scan.inputUnchanged,true);assert.equal(report.exitCode,1);assert.equal(report.duplicates.exact.length,1);assert.ok(Html(report).includes('&lt;bad&amp;&gt;'));assert.ok(!Html(report).includes('Assets/<bad&>'));
    for(const asset of report.assets){assert.equal(asset.before.sha256,asset.after.sha256);assert.equal(asset.before.sha256,createHash('sha256').update(await fs.readFile(path.join(project,asset.path))).digest('hex'));}
    await assert.rejects(WriteReport(report,path.join(project,'Saved','Reports')));const output=path.join(root,'report');await WriteReport(report,output);await assert.rejects(WriteReport(report,output));
    const cli=spawnSync(process.execPath,[fileURLToPath(new URL('./Audit.mjs',import.meta.url)),'--project',project,'--output',path.join(root,'cli-report'),'--settle-seconds','0'],{encoding:'utf8'});assert.equal(cli.status,1,cli.stderr);assert.equal(JSON.parse(cli.stdout).exitCode,1);
}));
test('changed hash with same size/mtime and tree additions suppress missing assertions',async()=>Fixture(async({project,assets})=>{
    const original=Mesh();const target=path.join(assets,'a.mesh');await fs.writeFile(target,original);await fs.writeFile(path.join(assets,'ref.mat'),'shader="guid:ffffffffffffffffffffffffffffffff|Assets/future.mesh"');const stamp=await fs.stat(target);
    const r=await Audit(project,Configuration({settleSeconds:0}),async phase=>{if(phase==='analyzing'){const changed=Buffer.from(original);changed[36]=1;await fs.writeFile(target,changed);await fs.utimes(target,stamp.atime,stamp.mtime);await fs.writeFile(path.join(assets,'future.mesh'),original);}});
    assert.equal(r.scan.inputUnchanged,false);assert.ok(r.scan.added.includes('Assets/future.mesh'));assert.ok(r.findings.filter(f=>f.code==='MISSING_GUID').every(f=>f.certainty==='pending'));assert.ok(r.assets.find(a=>a.path==='Assets/a.mesh').stability==='pending');assert.equal(r.exitCode,2);
}));
test('recent/truncated, parser budgets and unknown formats never report success',async()=>Fixture(async({project,assets})=>{
    await fs.writeFile(path.join(assets,'writing.mesh'),Buffer.from('FZMH'));await fs.writeFile(path.join(assets,'unreadable.blend'),Buffer.from('BLENDER'));
    const recent=await Audit(project,Configuration());assert.equal(recent.exitCode,2);assert.equal(recent.scan.inputUnchanged,true);assert.ok(recent.scan.recentInputs>0);assert.ok(recent.findings.filter(f=>f.code==='MALFORMED_ASSET').every(f=>f.certainty==='pending'));
    const budget=await Audit(project,Configuration({settleSeconds:0,maxTotalHashBytes:1}));assert.equal(budget.exitCode,2);assert.ok(budget.assets.some(a=>a.before.sha256===null));
}));
test('GUID authority/fallback and baked derived GUID lookup',async()=>Fixture(async({project,assets})=>{
    const source='0123456789abcdef0123456789abcdef';await fs.writeFile(path.join(assets,'a.mesh'),Mesh());await fs.writeFile(path.join(assets,'a.mesh.meta'),`[meta]\nguid='${source}'`);
    const baked=path.join(project,'Library','Baked',source,'anims');await fs.mkdir(baked,{recursive:true});await fs.writeFile(path.join(baked,'Walk.anim'),Animation());
    await fs.writeFile(path.join(assets,'ref.mat'),`x="guid:${source}|Assets/missing.mesh"\ny="guid:${DeriveGuid(source,'anims/Walk.anim')}|Library/Baked/${source}/anims/Walk.anim"\nz="guid:ffffffffffffffffffffffffffffffff|Assets/a.mesh"`);
    const r=await Audit(project,Configuration({settleSeconds:0}));assert.ok(!r.findings.some(f=>f.code==='MISSING_GUID'));assert.ok(r.findings.some(f=>f.code==='GUID_FALLBACK_ONLY'));assert.ok(r.assets.some(a=>a.path.includes('Library/Baked')));
}));
test('shared asset copies preserve engine semantics; actual local GUID conflict is an error',async()=>Fixture(async({root,project,assets})=>{
    const shared=path.join(root,'SharedAssets'), guid='0123456789abcdef0123456789abcdef';await fs.mkdir(shared);
    for(const directory of [assets,shared])await fs.writeFile(path.join(directory,'same.mesh.meta'),`[meta]\nguid='${guid}'`);
    await fs.writeFile(path.join(assets,'same.mesh'),Mesh());const modified=Mesh();modified[36]=1;await fs.writeFile(path.join(shared,'same.mesh'),modified);
    const copy=await Audit(project,Configuration({settleSeconds:0,sharedAssets:[shared]}));assert.ok(copy.findings.some(f=>f.code==='GUID_SHARED_COPY'));assert.ok(!copy.findings.some(f=>f.code==='GUID_CONFLICT'));
    await fs.writeFile(path.join(assets,'different.mesh'),modified);await fs.writeFile(path.join(assets,'different.mesh.meta'),`[meta]\nguid='${guid}'`);
    const collision=await Audit(project,Configuration({settleSeconds:0,sharedAssets:[shared]}));assert.ok(collision.findings.some(f=>f.code==='GUID_CONFLICT'&&f.certainty==='confirmed'));assert.equal(collision.exitCode,1);
}));
test('empty directory additions/deletions and CLI usage errors remain explicit',async()=>Fixture(async({root,project,assets})=>{
    await fs.writeFile(path.join(assets,'a.mesh'),Mesh());
    const result=await Audit(project,Configuration({settleSeconds:0,workers:2}),async phase=>{if(phase==='analyzing')await fs.mkdir(path.join(assets,'newEmptyDirectory'));});assert.equal(result.scan.inputUnchanged,false);assert.equal(result.exitCode,2);
    const cli=spawnSync(process.execPath,[fileURLToPath(new URL('./Audit.mjs',import.meta.url)),'--project',project,'--output',path.join(root,'report'),'--workers','10'],{encoding:'utf8'});assert.equal(cli.status,3);assert.equal(JSON.parse(cli.stderr).exitCode,3);
}));
test('symlink inputs and output junctions are excluded without traversal',async t=>Fixture(async({root,project,assets})=>{
    const outside=path.join(root,'outside');await fs.mkdir(outside);await fs.writeFile(path.join(outside,'secret.mesh'),Mesh());
    try { await fs.symlink(outside,path.join(assets,'linked'),'junction'); } catch(error) { if(['EPERM','EACCES'].includes(error.code)){t.skip('host does not allow symlink creation');return;}throw error; }
    const r=await Audit(project,Configuration({settleSeconds:0}));assert.ok(!r.assets.some(a=>a.path.includes('secret')));assert.equal(r.exitCode,2);
    const outputLink=path.join(root,'output-link');await fs.symlink(assets,outputLink,'junction');await assert.rejects(WriteReport(r,outputLink));
}));
