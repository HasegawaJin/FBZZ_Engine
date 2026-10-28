/// @file    scriptTools.test.ts
/// @brief   Script 観測ツールの read 権限・引数・転送内容を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-26
import assert from 'node:assert/strict';
import test from 'node:test';
import { Client } from '@modelcontextprotocol/sdk/client/index.js';
import { InMemoryTransport } from '@modelcontextprotocol/sdk/inMemory.js';
import { EditorQuerySchema } from '../editorContracts.js';
import { CreateEditorMcpServer } from '../server.js';
test('Script queries require a lifetime handle and reject unknown fields', () => {
    const id = '11111111-1111-4111-8111-111111111111';
    assert.equal(EditorQuerySchema.safeParse({ t: 'script.inspect', id }).success, false);
    assert.equal(EditorQuerySchema.safeParse({ t: 'script.inspect', id, scriptId: '' }).success, false);
    assert.equal(EditorQuerySchema.safeParse({ t: 'script.inspect', id, scriptId: 'entry/instance', index: 0 }).success, false);
    assert.equal(EditorQuerySchema.safeParse({ t: 'script.inspect', id, scriptId: 'entry/instance', path: '/values', start: 50000, count: 128 }).success, true);
    assert.equal(EditorQuerySchema.safeParse({ t: 'script.inspect', id, scriptId: 'entry/instance', observationId: '2', close: true }).success, true);
    assert.equal(EditorQuerySchema.safeParse({ t: 'script.inspect', id, scriptId: 'entry/instance', start: -1 }).success, false);
    assert.equal(EditorQuerySchema.safeParse({ t: 'script.catalog' }).success, true);
    assert.equal(EditorQuerySchema.safeParse({ t: 'script.catalog', type: '' }).success, false);
    assert.equal(EditorQuerySchema.safeParse({ t: 'script.api.catalog', type: 'Fbzz.Transform', member: 'TranslateWorld' }).success, true);
    assert.equal(EditorQuerySchema.safeParse({ t: 'script.api.catalog', type: '' }).success, false);
    assert.equal(EditorQuerySchema.safeParse({ t: 'script.api.catalog', invoke: true }).success, false);
});
test('Read connections can inspect and catalog scripts without commands', async () => {
    const queries = [];
    const bus = {
        async Query(query) { queries.push(query); return { status: 'ready', fields: { currentHealth: 73 } }; },
        async Command() { assert.fail('Script observation must not send commands'); },
        Close() { },
    };
    const server = CreateEditorMcpServer(bus, 'read');
    const client = new Client({ name: 'script-inspection-test', version: '1.0.0' });
    const [clientTransport, serverTransport] = InMemoryTransport.createLinkedPair();
    await server.connect(serverTransport);
    await client.connect(clientTransport);
    try {
        const { tools } = await client.listTools();
        for (const name of ['script_catalog', 'script_inspect', 'script_api_catalog']) {
            assert.equal(tools.find(tool => tool.name === name)?.annotations?.readOnlyHint, true);
        }
        const id = '11111111-1111-4111-8111-111111111111';
        const response = await client.callTool({ name: 'script_inspect', arguments: { id, scriptId: 'entry/instance' } });
        assert.notEqual(response.isError, true);
        await client.callTool({ name: 'script_inspect', arguments: { id, scriptId: 'entry/instance', path: '/settings/list', start: 50000, count: 32, wireBytes: 4096 } });
        await client.callTool({ name: 'script_inspect', arguments: { id, scriptId: 'entry/instance', observationId: '2', path: '/settings/list', start: 50032, count: 32 } });
        await client.callTool({ name: 'script_inspect', arguments: { id, scriptId: 'entry/instance', observationId: '2', close: true } });
        await client.callTool({ name: 'script_catalog', arguments: { type: 'PlayerComponent' } });
        await client.callTool({ name: 'script_catalog', arguments: {} });
        await client.callTool({ name: 'script_api_catalog', arguments: { type: 'Fbzz.Transform', member: 'TranslateWorld' } });
        assert.deepEqual(queries, [
            { t: 'script.inspect', id, scriptId: 'entry/instance' },
            { t: 'script.inspect', id, scriptId: 'entry/instance', path: '/settings/list', start: 50000, count: 32, wireBytes: 4096 },
            { t: 'script.inspect', id, scriptId: 'entry/instance', path: '/settings/list', start: 50032, count: 32, observationId: '2' },
            { t: 'script.inspect', id, scriptId: 'entry/instance', observationId: '2', close: true },
            { t: 'script.catalog', type: 'PlayerComponent' },
            { t: 'script.catalog' },
            { t: 'script.api.catalog', type: 'Fbzz.Transform', member: 'TranslateWorld' },
        ]);
    }
    finally {
        await client.close();
        await server.close();
    }
});
//# sourceMappingURL=scriptTools.test.js.map