// FBZZ Engine
// stdio.test.ts | EditorMcp
// 実子プロセスの stdio 上で MCP 初期化と tool discovery を検証する
import assert from 'node:assert/strict';
import { dirname, resolve } from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';
import { Client } from '@modelcontextprotocol/sdk/client/index.js';
import { StdioClientTransport } from '@modelcontextprotocol/sdk/client/stdio.js';
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
    }
    finally {
        await client.close();
    }
});
//# sourceMappingURL=stdio.test.js.map