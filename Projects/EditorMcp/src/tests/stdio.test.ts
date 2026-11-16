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
