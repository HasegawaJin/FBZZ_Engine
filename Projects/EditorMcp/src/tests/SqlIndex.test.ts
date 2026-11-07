/// @file    SqlIndex.test.ts
/// @brief   実 SQLite 子プロセスで索引の範囲・検索・読み取り制限と打切りを検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-06
import assert from 'node:assert/strict';
import test from 'node:test';
import type { EditorBus } from '../busClient.js';
import { SceneSqlIndex } from '../Analysis/SceneSqlIndex.js';
import { ValidateReadSql } from '../Analysis/SqlQueryProcess.js';

const FIRST = '11111111-1111-4111-8111-111111111111';
const SECOND = '22222222-2222-4222-8222-222222222222';

function CreateBus() {
    const state = { sceneGeneration: 'scene-1', failInspection: false, failSnapshot: false, partialReferences: false,
        scopeAvailable: true, commandCalls: 0, queries: [] as string[] };
    const bus: EditorBus = {
        async Query(query) {
            state.queries.push(query.t);
            if (query.t === 'editor.op.query' && query.id === 'authoring.context') {
                if (!state.scopeAvailable) throw new Error('UNKNOWN_OPERATOR');
                return { id: 'authoring.context', data: { projectRoot: 'K:/Example', scenePath: 'Assets/Test.scene',
                    sceneGeneration: state.sceneGeneration, mode: 'edit' } };
            }
            if (query.t === 'editor.state') return { scene: 'Assets/Test.scene', playState: 'stopped' };
            if (query.t === 'scene.snapshot') {
                if (state.failSnapshot) throw new Error('NO_SCENE');
                return { count: 2, excludedGenerated: 1, nodes: [
                    { id: FIRST, name: 'Enemy', tag: 'Enemy', layer: 2, active: true, activeInHierarchy: true,
                        transform: { position: [1, 2, 3], worldPosition: [11, 2, 3] },
                        components: [{ type: 'Collider', fields: { radius: 2, enabled: true } }] },
                    { id: SECOND, name: 'Ground', parent: FIRST, tag: '', layer: 0, active: true, activeInHierarchy: true,
                        transform: { position: [0, 0, 0], worldPosition: [0, 0, 0] }, components: [{ type: 'Renderer', fields: {} }] },
                ] };
            }
            if (query.t === 'node.components') {
                if (state.failInspection) throw new Error('NODE_NOT_FOUND');
                return { scripts: query.id === FIRST ? [{ scriptId: 'entry/live', savedScriptId: 'saved-script',
                    type: 'EnemyScript', status: 'ready', enabled: true }] : [] };
            }
            if (query.t === 'script.inspect') return { settings: { hp: 100, target: { guid: SECOND, id: null,
                expectedType: 'GameObject', status: 'missing' } }, deferred: ['/settings/large'] };
            if (query.t === 'editor.op.query' && query.id === 'asset.references.status') {
                return { data: { job: 7, target: 'Assets/Enemy.mat', finished: !state.partialReferences,
                    stale: false, limited: state.partialReferences, ownerCount: state.partialReferences ? 3 : 1,
                    owners: [{ path: 'Assets/Enemy.prefab', kind: 'prefab', evidence: [{ field: 'material', kind: 'guid', line: 12 }] }],
                    processed: 1, candidates: state.partialReferences ? 3 : 1, unreadable: 0, unsupported: 0,
                    oversized: 0, malformed: 0, unresolvedHints: 0 } };
            }
            throw new Error(`Unexpected query: ${query.t}`);
        },
        async Command() { ++state.commandCalls; assert.fail('SQL must not change the Editor'); },
        Close() {},
    };
    return { bus, state };
}

test('SQL index joins snapshot components, inspected scripts and explicit reference evidence', async () => {
    const { bus, state } = CreateBus();
    const index = new SceneSqlIndex();
    const status = await index.Refresh(bus, { referenceJobs: [7] });
    assert.equal(status.refreshing, false);
    assert.equal(status.ready, true);
    const generation = String(status.generation);
    const result = await index.Query(bus, generation,
        'SELECT n.name,c.type,n.world_x FROM nodes n JOIN components c ON c.node_id=n.id WHERE c.type=?', ['Collider']);
    assert.deepEqual(result.rows, [{ name: 'Enemy', type: 'Collider', world_x: 11 }]);
    assert.equal((result.freshness as Record<string, unknown>).scopeVerified, true);
    assert.deepEqual((await index.Query(bus, generation,
        'SELECT s.type,s.saved_script_id,r.status,r.target_guid FROM scripts s JOIN object_references r ON r.script_id=s.script_id')).rows,
    [{ type: 'EnemyScript', saved_script_id: 'saved-script', status: 'missing', target_guid: SECOND }]);
    assert.deepEqual((await index.Query(bus, generation, 'SELECT owner_path,target_path,field,line FROM asset_references')).rows,
        [{ owner_path: 'Assets/Enemy.prefab', target_path: 'Assets/Enemy.mat', field: 'material', line: 12 }]);
    assert.equal(state.commandCalls, 0);
});

test('SQL authorizer and statement validation reject writes, external data and multiple statements', async () => {
    const { bus } = CreateBus();
    const index = new SceneSqlIndex();
    const status = await index.Refresh(bus, { inspectNodeLimit: 0 });
    for (const sql of [
        'PRAGMA table_info(nodes)', "ATTACH DATABASE 'external.db' AS external", 'DELETE FROM nodes',
        'WITH x AS (SELECT 1) DELETE FROM nodes', 'SELECT 1; SELECT 2', 'SELECT 1;;',
        "SELECT load_extension('external.dll')", "SELECT * FROM pragma_table_info('nodes')",
        'SELECT * FROM sqlite_master', 'SELECT count(*) FROM sqlite_master', 'CREATE TABLE extra(id)',
    ]) {
        await assert.rejects(index.Query(bus, String(status.generation), sql), /SQL_|authoriz|not allowed|prohibited/i, sql);
    }
    assert.deepEqual((await index.Query(bus, String(status.generation), 'SELECT count(*) AS count FROM nodes')).rows, [{ count: 2 }]);
    assert.deepEqual((await index.Query(bus, String(status.generation), "SELECT 'DELETE; ATTACH' AS text; -- allowed literal\n")).rows,
        [{ text: 'DELETE; ATTACH' }]);
});

test('SQL result limits and 64-bit values remain explicit', async () => {
    const { bus } = CreateBus();
    const index = new SceneSqlIndex();
    const status = await index.Refresh(bus, { inspectNodeLimit: 0 });
    const generation = String(status.generation);
    const limited = await index.Query(bus, generation, 'SELECT id FROM nodes ORDER BY id', [], 1);
    assert.equal(limited.returnedRows, 1);
    assert.equal(limited.truncated, true);
    assert.deepEqual(limited.truncationReasons, ['row-limit']);
    const bytes = await index.Query(bus, generation, "SELECT printf('%020000d', 1) AS text", [], 100, 1024);
    assert.equal(bytes.returnedRows, 0);
    assert.deepEqual(bytes.truncationReasons, ['byte-limit']);
    assert.deepEqual((await index.Query(bus, generation, 'SELECT 9223372036854775807 AS large, 1 AS small')).rows,
        [{ large: '9223372036854775807', small: 1 }]);
    await assert.rejects(index.Query(bus, generation, 'SELECT id,id FROM nodes'), /SQL_DUPLICATE_COLUMNS/);
});

test('SQL infinite recursive aggregate is terminated and later queries still work', async () => {
    const { bus } = CreateBus();
    const index = new SceneSqlIndex();
    const status = await index.Refresh(bus, { inspectNodeLimit: 0 });
    const generation = String(status.generation);
    await assert.rejects(index.Query(bus, generation,
        'WITH RECURSIVE t(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM t) SELECT sum(x) AS sum FROM t', [], 100, 4096, 500), /SQL_TIMEOUT/);
    assert.equal(index.Status().querying, false);
    assert.deepEqual((await index.Query(bus, generation, 'SELECT count(*) AS count FROM nodes')).rows, [{ count: 2 }]);
});

test('SQL scope, generation and failed refresh do not silently switch the indexed Scene', async () => {
    const { bus, state } = CreateBus();
    const index = new SceneSqlIndex();
    await assert.rejects(index.Query(bus, 'missing', 'SELECT 1'), /SQL_INDEX_NOT_READY/);
    const status = await index.Refresh(bus, { inspectNodeLimit: 0 });
    await assert.rejects(index.Query(bus, 'wrong', 'SELECT 1'), /SQL_INDEX_GENERATION/);
    state.sceneGeneration = 'scene-2';
    await assert.rejects(index.Query(bus, String(status.generation), 'SELECT 1'), /SQL_INDEX_STALE_SCOPE/);
    state.sceneGeneration = 'scene-1'; state.failSnapshot = true;
    await assert.rejects(index.Refresh(bus), /NO_SCENE/);
    assert.equal(index.Status().generation, status.generation);
    assert.equal(index.Status().refreshing, false);
});

test('SQL partial inspections and reference scans never become complete or healthy defaults', async () => {
    const { bus, state } = CreateBus();
    const index = new SceneSqlIndex();
    state.failInspection = true; state.partialReferences = true; state.scopeAvailable = false;
    const status = await index.Refresh(bus, { inspectNodeLimit: 1, referenceJobs: [7] });
    const coverage = status.coverage as Record<string, unknown>;
    assert.equal(coverage.nodeInspectionFailures, 1);
    assert.equal(coverage.inspectedNodes, 0);
    assert.equal((status.scope as Record<string, unknown>).verified, false);
    assert.ok((status.warnings as string[]).some(warning => warning.startsWith('reference-job-7')));
    const result = await index.Query(bus, String(status.generation), 'SELECT finished,limited,owner_count,owners_indexed FROM asset_reference_scans');
    assert.deepEqual(result.rows, [{ finished: 0, limited: 1, owner_count: 3, owners_indexed: 1 }]);
    assert.equal((result.freshness as Record<string, unknown>).scopeVerified, false);
});

test('SQL tokenizer distinguishes semicolons in strings, identifiers and comments', () => {
    ValidateReadSql("/* prefix; */ WITH x AS (SELECT ';' AS value) SELECT value FROM x; -- trailing;\n");
    ValidateReadSql('SELECT 1 AS "a;b"');
    assert.throws(() => ValidateReadSql("SELECT 'unterminated"), /SQL_INVALID/);
    assert.throws(() => ValidateReadSql('SELECT 1 /* unterminated'), /SQL_INVALID/);
    assert.throws(() => ValidateReadSql('SELECT 1; /* comment */ SELECT 2'), /SQL_MULTIPLE_STATEMENTS/);
});
