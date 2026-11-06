/// @file    ContractParity.test.ts
/// @brief   C++ と共有する入力ケースを wire 契約と公開 MCP ツールへ通す。
/// @author  Hasegawa Jin
/// @date    2026-11-06
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import test from 'node:test';
import * as z from 'zod/v4';
import { Client } from '@modelcontextprotocol/sdk/client/index.js';
import { InMemoryTransport } from '@modelcontextprotocol/sdk/inMemory.js';
import type { EditorBus } from '../busClient.js';
import { EDITOR_PROTOCOL, EditorCommandSchema, EditorQuerySchema } from '../editorContracts.js';
import { CreateEditorMcpServer } from '../server.js';

/// @note ソースの JSON を C++ と共有し、tsc の JSON コピー設定や実行時 Editor を必要としない。
const fixture = z.object({
    protocol: z.literal(EDITOR_PROTOCOL),
    nodeId: z.string().uuid(),
    cases: z.array(z.object({
        name: z.string().min(1),
        kind: z.enum(['query', 'command']),
        tool: z.string().min(1),
        accepted: z.boolean(),
        payload: z.object({ t: z.string() }).catchall(z.unknown()),
    })).min(1),
}).parse(JSON.parse(readFileSync(new URL('../../src/tests/fixtures/EditorContractCases.json', import.meta.url), 'utf8')));

test('C++ と共有する必須値・型・整数範囲・固定長配列の受理結果が wire 契約と一致する', () => {
    assert.equal(new Set(fixture.cases.map(entry => entry.name)).size, fixture.cases.length);
    for (const entry of fixture.cases) {
        const schema = entry.kind === 'query' ? EditorQuerySchema : EditorCommandSchema;
        assert.equal(schema.safeParse(entry.payload).success, entry.accepted, entry.name);
    }
});

for (const permission of ['dry-run', 'write'] as const) {
    test(`${permission} の公開 MCP 入力と転送内容が C++ 共有ケースに一致する`, async () => {
        const dispatched: Array<{ kind: string; payload: unknown; dryRun?: boolean }> = [];
        const bus: EditorBus = {
            async Query(query) {
                EditorQuerySchema.parse(query);
                dispatched.push({ kind: 'query', payload: query });
                return {};
            },
            async Command(command, dryRun) {
                EditorCommandSchema.parse(command);
                dispatched.push({ kind: 'command', payload: command, dryRun });
                return {};
            },
            Close() {},
        };
        const server = CreateEditorMcpServer(bus, permission);
        const client = new Client({ name: 'contract-parity-test', version: '1' });
        const [clientTransport, serverTransport] = InMemoryTransport.createLinkedPair();
        await server.connect(serverTransport);
        await client.connect(clientTransport);
        try {
            for (const entry of fixture.cases) {
                dispatched.length = 0;
                const { t: _, ...args } = entry.payload;
                const result = await client.callTool({ name: entry.tool, arguments: args });
                assert.equal(result.isError !== true, entry.accepted, entry.name);
                if (entry.accepted) {
                    assert.deepEqual(dispatched, [{ kind: entry.kind, payload: entry.payload,
                        ...(entry.kind === 'command' ? { dryRun: permission !== 'write' } : {}) }], entry.name);
                } else {
                    assert.deepEqual(dispatched, [], `${entry.name}: invalid input reached Editor`);
                }
            }
        } finally {
            await client.close();
            await server.close();
        }
    });
}
