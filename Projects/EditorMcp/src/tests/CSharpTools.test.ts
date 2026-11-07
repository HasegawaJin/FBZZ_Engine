/// @file    CSharpTools.test.ts
/// @brief   Source ownership, semantic rename guards and real Roslyn bridge contracts.
/// @author  Hasegawa Jin
/// @date    2026-11-06
import assert from 'node:assert/strict';
import { mkdtemp, mkdir, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import test from 'node:test';
import { Client } from '@modelcontextprotocol/sdk/client/index.js';
import { InMemoryTransport } from '@modelcontextprotocol/sdk/inMemory.js';
import { McpServer } from '@modelcontextprotocol/sdk/server/mcp.js';
import { CSharpSession, RunRoslyn, type AnalysisRunner } from '../Analysis/RoslynClient.js';
import type { EditorBus } from '../busClient.js';
import { RegisterCSharpTools } from '../Tools/CSharpTools.js';

async function Fixture(source = 'class A { int value; int Read() => value; }') {
    const root = await mkdtemp(path.join(tmpdir(), 'fbzz-roslyn-'));
    await mkdir(path.join(root, 'Assets', 'Scripts'), { recursive: true });
    const file = path.join(root, 'Assets', 'Scripts', 'Test.cs');
    await writeFile(file, source, 'utf8');
    await writeFile(`${file}.meta`, 'type-guid-preserved', 'utf8');
    const context: Record<string, unknown> = { projectRoot: root, pendingRestore: false, managedBuildBusy: false, managed: { apiPath: null } };
    const bus: EditorBus = {
        async Query(query) { assert.deepEqual(query, { t: 'editor.op.query', id: 'authoring.context' }); return { data: context }; },
        async Command() { assert.fail('C# analysis must not send scene Commands.'); },
        Close() {},
    };
    return { root, file, context, bus, async Close() { assert.equal(path.dirname(root), tmpdir()); await rm(root, { recursive: true, force: true }); } };
}

const RenameRunner: AnalysisRunner = async request => {
    const sources = request.sources as Array<{ file: string; content: string }>;
    const source = sources[0];
    const edits = [...source.content.matchAll(/\bvalue\b/g)].map(match => ({ file: source.file, start: match.index!, length: 5, before: 'value', after: request.newName }));
    return { ok: true, blocked: false, blockers: [], edits };
};
const Position = { file: 'Assets/Scripts/Test.cs', line: 1, column: 15, newName: 'count' };

test('rename applies only reviewed bytes and preserves UTF-8 BOM, line endings and meta', async () => {
    const fixture = await Fixture('\uFEFFclass A {\r\n int value; int Read() => value;\r\n}\r\n');
    try {
        const session = new CSharpSession(fixture.bus, 'write', RenameRunner);
        const preview = await session.Preview(Position);
        const result = await session.Apply(String(preview.planId));
        assert.equal(result.status, 'success');
        assert.equal(result.filesWritten, 1);
        assert.equal(await readFile(fixture.file, 'utf8'), '\uFEFFclass A {\r\n int count; int Read() => count;\r\n}\r\n');
        assert.equal(await readFile(`${fixture.file}.meta`, 'utf8'), 'type-guid-preserved');
        await assert.rejects(session.Apply(String(preview.planId)), /absent or expired/);
    } finally { await fixture.Close(); }
});

test('a new source file invalidates the full-snapshot rename guard', async () => {
    const fixture = await Fixture();
    try {
        const session = new CSharpSession(fixture.bus, 'write', RenameRunner);
        const preview = await session.Preview(Position);
        await writeFile(path.join(fixture.root, 'Assets', 'Scripts', 'Added.cs'), 'class Added {}');
        await assert.rejects(session.Apply(String(preview.planId)), /snapshot.*changed/);
        assert.match(await readFile(fixture.file, 'utf8'), /int value/);
    } finally { await fixture.Close(); }
});

test('concurrent applies cannot publish the same plan twice', async () => {
    const fixture = await Fixture();
    try {
        const session = new CSharpSession(fixture.bus, 'write', RenameRunner);
        const preview = await session.Preview(Position);
        const first = session.Apply(String(preview.planId));
        await assert.rejects(session.Apply(String(preview.planId)), /already running/);
        assert.equal((await first).status, 'success');
        assert.equal(await readFile(fixture.file, 'utf8'), 'class A { int count; int Read() => count; }');
    } finally { await fixture.Close(); }
});

test('metadata content changes invalidate rename plans', async () => {
    const fixture = await Fixture();
    try {
        const reference = path.join(fixture.root, 'Dependency.dll');
        await writeFile(reference, 'before');
        const session = new CSharpSession(fixture.bus, 'write', RenameRunner);
        const preview = await session.Preview({ ...Position, referencePaths: ['Dependency.dll'] });
        await writeFile(reference, 'after');
        await assert.rejects(session.Apply(String(preview.planId)), /metadata reference changed/);
    } finally { await fixture.Close(); }
});

test('dry-run and read permissions never write C# sources', async () => {
    const fixture = await Fixture();
    try {
        const original = await readFile(fixture.file);
        const dryRun = new CSharpSession(fixture.bus, 'dry-run', RenameRunner);
        const preview = await dryRun.Preview(Position);
        assert.equal((await dryRun.Apply(String(preview.planId))).status, 'dry-run');
        assert.deepEqual(await readFile(fixture.file), original);
        const reader = new CSharpSession(fixture.bus, 'read', RenameRunner);
        const readPreview = await reader.Preview(Position);
        await assert.rejects(reader.Apply(String(readPreview.planId)), /Read mode/);
        assert.deepEqual(await readFile(fixture.file), original);
    } finally { await fixture.Close(); }
});

test('plans are session-owned, expire deterministically and reject project switches', async () => {
    const fixture = await Fixture();
    const second = await Fixture();
    try {
        let now = 1000;
        const owner = new CSharpSession(fixture.bus, 'write', RenameRunner, () => now);
        const other = new CSharpSession(fixture.bus, 'write', RenameRunner, () => now);
        const preview = await owner.Preview(Position);
        await assert.rejects(other.Apply(String(preview.planId)), /absent or expired/);
        fixture.context.projectRoot = second.root;
        await assert.rejects(owner.Apply(String(preview.planId)), /project.*changed/);
        fixture.context.projectRoot = fixture.root;
        now += 5 * 60 * 1000;
        await assert.rejects(owner.Apply(String(preview.planId)), /absent or expired/);
    } finally { await fixture.Close(); await second.Close(); }
});

test('source and reference path escapes are rejected before invoking the helper', async () => {
    const fixture = await Fixture();
    try {
        let called = false;
        const session = new CSharpSession(fixture.bus, 'read', async () => { called = true; return { ok: true }; });
        await assert.rejects(session.Analyze('references', { file: '../Outside.cs', line: 1, column: 1 }), /escapes/);
        await assert.rejects(session.Analyze('diagnostics', { referencePaths: ['../Outside.dll'] }), /escapes/);
        assert.equal(called, false);
    } finally { await fixture.Close(); }
});

test('public MCP surface exposes source analysis in read mode and apply only in editing modes', async () => {
    for (const permission of ['read', 'dry-run', 'write'] as const) {
        const fixture = await Fixture();
        const server = new McpServer({ name: 'csharp-tools-test', version: '1' });
        RegisterCSharpTools(server, fixture.bus, permission);
        const client = new Client({ name: 'csharp-tools-client', version: '1' });
        const [clientTransport, serverTransport] = InMemoryTransport.createLinkedPair();
        await server.connect(serverTransport);
        await client.connect(clientTransport);
        try {
            const names = (await client.listTools()).tools.map(tool => tool.name);
            for (const name of ['csharp_analyze', 'csharp_symbols', 'csharp_find_references', 'csharp_rename_preview']) assert.equal(names.includes(name), true);
            assert.equal(names.includes('csharp_rename_apply'), permission !== 'read');
            const invalid = await client.callTool({ name: 'csharp_find_references', arguments: { file: 'Assets/Scripts/Test.cs', line: 0, column: 1 } });
            assert.equal(invalid.isError, true);
        } finally { await client.close(); await server.close(); await fixture.Close(); }
    }
});

test('real Roslyn helper resolves symbols and keeps unrelated same-name fields untouched', async () => {
    const source = 'class A { int value; int Read() => value; } class B { int value; int Read() => value; }';
    const result = await RunRoslyn({ operation: 'references', sources: [{ file: 'Assets/Scripts/Test.cs', content: source }], file: 'Assets/Scripts/Test.cs', line: 1, column: source.indexOf('value') + 1 });
    assert.equal(result.total, 2);
    const fixture = await Fixture(source);
    try {
        const session = new CSharpSession(fixture.bus, 'read');
        const preview = await session.Preview({ ...Position, column: source.indexOf('value') + 1 });
        assert.equal(preview.blocked, false);
        assert.equal((preview.edits as unknown[]).length, 2);
        assert.equal((preview.snapshot as Record<string, unknown>).projectEvaluated, false);
    } finally { await fixture.Close(); }
});

test('real UTF-8 helper preserves Japanese text, LF/CRLF and UTF-16 positions through rename publication', async () => {
    const lines = [
        '/// @file    日本語Probe.cs',
        '/// @brief   Semantic renameとmanaged再適用を確認するための独立Script。',
        '/// @author  Hasegawa Jin',
        '/// @date    2026-11-06',
        'class AcceptanceProbe',
        '{',
        '    private string m_label = "😀";',
        '    private int m_回数 = 1;',
        '    private string Marker = "😀"; private int m_counter = 1;',
        '    public int Counter => m_counter + m_回数;',
        '    private void Tick() { m_counter += 1; }',
        '}',
        'class OtherProbe',
        '{',
        '    private int m_counter = 7;',
        '    public int Counter => m_counter;',
        '}',
        '',
    ];
    const sourceFile = 'Assets/Scripts/確認/日本語Probe.cs';
    const declarationColumn = lines[8].indexOf('m_counter') + 1;
    assert.notEqual(declarationColumn, [...lines[8].slice(0, declarationColumn - 1)].length + 1);
    const newName = 'm_更新回数';
    for (const lineEnding of ['\n', '\r\n']) {
        const source = lines.join(lineEnding);
        const sources = [{ file: sourceFile, content: source }];
        const symbols = await RunRoslyn({ operation: 'symbols', sources });
        const values = symbols.symbols as Array<{ name: string; location: { file: string; line: number; column: number; start: number; length: number } }>;
        const field = values.find(value => value.name === 'm_counter' && value.location.line === 9);
        assert.ok(field, `field declaration must remain on line 9 for ${JSON.stringify(lineEnding)}`);
        assert.equal(field.location.file, sourceFile);
        assert.equal(field.location.column, declarationColumn);
        assert.equal(field.location.start, source.indexOf('m_counter'));
        assert.equal(values.find(value => value.name === 'm_回数')?.location.line, 8);
        assert.equal(values.find(value => value.name === 'Counter')?.location.line, 10);

        const references = await RunRoslyn({ operation: 'references', sources, file: sourceFile, line: 9, column: declarationColumn });
        assert.equal((references.symbol as Record<string, unknown>).name, 'm_counter');
        const positions = references.references as Array<{ file: string; line: number; column: number; start: number; length: number }>;
        assert.deepEqual(positions.map(position => position.line), [9, 10, 11]);
        for (const position of positions) {
            assert.equal(position.file, sourceFile);
            assert.equal(source.slice(position.start, position.start + position.length), 'm_counter');
            assert.equal(position.column, lines[position.line - 1].indexOf('m_counter') + 1);
        }

        const fixture = await Fixture(source);
        try {
            const session = new CSharpSession(fixture.bus, 'write');
            const preview = await session.Preview({ file: 'Assets/Scripts/Test.cs', line: 9, column: declarationColumn, newName });
            assert.equal(preview.blocked, false);
            const edits = preview.edits as Array<{ start: number; length: number; before: string; after: string }>;
            assert.deepEqual(edits.map(edit => edit.start), positions.map(position => position.start));
            let expected = source;
            for (const edit of [...edits].sort((left, right) => right.start - left.start)) {
                assert.equal(expected.slice(edit.start, edit.start + edit.length), edit.before);
                assert.equal(edit.after, newName);
                expected = expected.slice(0, edit.start) + edit.after + expected.slice(edit.start + edit.length);
            }
            const applied = await session.Apply(String(preview.planId));
            assert.equal(applied.status, 'success');
            assert.deepEqual(await readFile(fixture.file), Buffer.from(expected, 'utf8'));
            assert.equal(await readFile(`${fixture.file}.meta`, 'utf8'), 'type-guid-preserved');
            assert.equal(expected.includes('Semantic renameとmanaged再適用を確認するための独立Script。'), true);
            assert.equal(expected.includes('"😀"'), true);
            assert.equal(expected.includes('private int m_counter = 7;'), true);
            const after = await session.Analyze('references', { file: 'Assets/Scripts/Test.cs', line: 9, column: declarationColumn });
            assert.equal((after.symbol as Record<string, unknown>).name, newName);
            assert.equal(after.total, 3);
        } finally { await fixture.Close(); }
    }
});
