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
    assert.equal(EditorQuerySchema.safeParse({ t: 'script.catalog' }).success, true);
    assert.equal(EditorQuerySchema.safeParse({ t: 'script.catalog', type: '' }).success, false);
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
        for (const name of ['script_catalog', 'script_inspect']) {
            assert.equal(tools.find(tool => tool.name === name)?.annotations?.readOnlyHint, true);
        }
        const id = '11111111-1111-4111-8111-111111111111';
        const response = await client.callTool({ name: 'script_inspect', arguments: { id, scriptId: 'entry/instance' } });
        assert.notEqual(response.isError, true);
        await client.callTool({ name: 'script_catalog', arguments: { type: 'PlayerComponent' } });
        await client.callTool({ name: 'script_catalog', arguments: {} });
        assert.deepEqual(queries, [
            { t: 'script.inspect', id, scriptId: 'entry/instance' },
            { t: 'script.catalog', type: 'PlayerComponent' },
            { t: 'script.catalog' },
        ]);
    }
    finally {
        await client.close();
        await server.close();
    }
});
//# sourceMappingURL=scriptTools.test.js.map