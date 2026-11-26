/// @file    stdio.test.ts
/// @brief   子プロセスの標準入出力を使い、MCP の初期化とツール発見を検証する。
/// @author  Hasegawa Jin
/// @date    2026-07-20
import assert from 'node:assert/strict';
import { dirname, resolve } from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';
import { Client } from '@modelcontextprotocol/sdk/client/index.js';
import { StdioClientTransport } from '@modelcontextprotocol/sdk/client/stdio.js';

/// @brief stdio entry は外部サーバー無しで起動し read tools を公開する。
test('stdio entry は外部サーバー無しで起動し read tools を公開する', { timeout: 10000 }, async () => {
    const currentDirectory = dirname(fileURLToPath(import.meta.url));
    const entryPath = resolve(currentDirectory, '..', 'stdio.js');
    const transport = new StdioClientTransport({
        command: process.execPath,
        args: [entryPath],
        env: {
            FBZZ_MCP_PERMISSION: 'read',
            FBZZ_EDITOR_PIPE: '\\\\.\\pipe\\FBZZEditorCommandBus',
        },
        stderr: 'pipe',
    });
    const client = new Client({ name: 'fbzz-stdio-test', version: '0.1.0' });
    try {
        await client.connect(transport);
        const tools = await client.listTools();
        assert.equal(tools.tools.some((tool) => tool.name === 'scene_get_tree'), true);
        assert.equal(tools.tools.some((tool) => tool.name === 'node_create'), false);
    } finally {
        await client.close();
    }
});

for (const permission of ['read', 'dry-run', 'write']) {
    /// @brief MCP の案内が権限モードで使用可能なツールだけを参照する。
    test(`MCP guidance references tools published in ${permission} mode`, { timeout: 10000 }, async () => {
        const entryPath = fileURLToPath(new URL('../stdio.js', import.meta.url));
        const transport = new StdioClientTransport({
            command: process.execPath,
            args: [entryPath],
            env: { FBZZ_MCP_PERMISSION: permission },
            stderr: 'pipe',
        });
        const client = new Client({ name: 'fbzz-guidance-test', version: '0.1.0' });
        try {
            await client.connect(transport);
            const tools = new Set((await client.listTools()).tools.map(tool => tool.name));
            const instructions = client.getInstructions() ?? '';
            // Operator ID はドット区切りの名前全体で読み、構成要素を MCP ツール名として扱わない。
            const identifiers: string[] = instructions.match(/\b[a-z][a-z0-9_]*(?:\.[a-z][a-z0-9_]*)*\b/g) ?? [];
            const mentioned = identifiers.filter(name => name.includes('_') && !name.includes('.'));
            assert.ok(mentioned.includes('editor_op_list'));
            assert.deepEqual([...new Set(mentioned.filter(tool => !tools.has(tool)))], []);
            assert.ok(identifiers.includes('asset.move_batch.preview'));
            assert.equal(identifiers.includes('asset.move_batch.apply'), permission === 'write');
        } finally {
            await client.close();
        }
    });
}
