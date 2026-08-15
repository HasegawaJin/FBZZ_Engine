// FBZZ Engine
// busClient.test.ts | EditorMcp
// 実 loopback Named Pipe で request/response 相関と wire envelope を検証する
import assert from 'node:assert/strict';
import { once } from 'node:events';
import { createServer } from 'node:net';
import test from 'node:test';
import { EditorBusClient } from '../busClient.js';
import { EDITOR_PROTOCOL } from '../editorContracts.js';
test('EditorBusClient は Named Pipe 上で query 応答を request ID に相関する', { timeout: 5000 }, async () => {
    // テスト毎に一意なパイプ名を使い、同時実行や前回残骸との衝突を避ける。
    const pipePath = `\\\\.\\pipe\\FBZZEditorTest-${process.pid}-${Date.now()}`;
    let received;
    // busClient と同じ NDJSON プロトコルを話す最小 loopback サーバ。受信 ID をそのまま応答へ相関させる。
    const server = createServer((socket) => {
        socket.setEncoding('utf8');
        let buffer = '';
        socket.on('data', (chunk) => {
            buffer += chunk;
            const newline = buffer.indexOf('\n');
            if (newline < 0) {
                return;
            }
            received = JSON.parse(buffer.slice(0, newline));
            socket.write(`${JSON.stringify({
                protocol: EDITOR_PROTOCOL,
                id: received.id,
                ok: true,
                result: { nodes: [1, 2, 3] },
            })}\n`);
        });
    });
    server.listen(pipePath);
    await once(server, 'listening');
    const client = new EditorBusClient(pipePath, 2000);
    try {
        const result = await client.Query({ t: 'scene.tree' });
        assert.deepEqual(result, { nodes: [1, 2, 3] });
        assert.equal(received?.protocol, EDITOR_PROTOCOL);
        assert.equal(received?.kind, 'query');
        assert.equal(received?.dryRun, true);
        assert.equal(received?.source, 'mcp');
        assert.deepEqual(received?.payload, { t: 'scene.tree' });
    }
    finally {
        client.Close();
        await new Promise((resolve) => server.close(() => resolve()));
    }
});
//# sourceMappingURL=busClient.test.js.map