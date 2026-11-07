/// @file    AuthoringTools.test.ts
/// @brief   制作解析の公開入口とソース変更の権限境界を実MCP transportで検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-06
import assert from 'node:assert/strict';
import test from 'node:test';
import { Client } from '@modelcontextprotocol/sdk/client/index.js';
import { InMemoryTransport } from '@modelcontextprotocol/sdk/inMemory.js';
import { CreateEditorMcpServer } from '../server.js';
for (const permission of ['read', 'dry-run', 'write']) {
    test(`制作解析の公開入口は${permission}権限で利用範囲を宣言する`, async () => {
        const bus = {
            async Query() { assert.fail('Discovery must not read Editor state or launch analysis.'); },
            async Command() { assert.fail('Discovery must not mutate Editor state.'); },
            Close() { },
        };
        const server = CreateEditorMcpServer(bus, permission);
        const client = new Client({ name: 'authoring-discovery', version: '1' });
        const [clientTransport, serverTransport] = InMemoryTransport.createLinkedPair();
        await server.connect(serverTransport);
        await client.connect(clientTransport);
        try {
            const tools = new Map((await client.listTools()).tools.map(tool => [tool.name, tool]));
            for (const name of ['csharp_analyze', 'csharp_symbols', 'csharp_find_references', 'csharp_rename_preview',
                'sql_index_refresh', 'sql_index_status', 'sql_index_schema', 'sql_query']) {
                assert.equal(tools.get(name)?.annotations?.readOnlyHint, true, name);
            }
            assert.equal(tools.has('csharp_rename_apply'), permission !== 'read');
            if (permission !== 'read')
                assert.equal(tools.get('csharp_rename_apply')?.annotations?.readOnlyHint, false);
        }
        finally {
            await client.close();
            await server.close();
        }
    });
}
//# sourceMappingURL=AuthoringTools.test.js.map