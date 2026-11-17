/// @file    busClient.test.ts
/// @brief   実際の名前付きパイプを使い、要求と応答の相関およびエンベロープを検証する。
/// @author  Hasegawa Jin
/// @date    2026-07-20
import assert from 'node:assert/strict';
import { once } from 'node:events';
import { createServer } from 'node:net';
import test from 'node:test';
import { EditorBusClient } from '../busClient.js';
import { EDITOR_PROTOCOL } from '../editorContracts.js';

/// @brief EditorBusClient は Named Pipe 上で query 応答を request ID に相関する。
test('EditorBusClient は Named Pipe 上で query 応答を request ID に相関する', { timeout: 5000 }, async () => {
    /// @note テスト毎に一意なパイプ名を使い、同時実行や前回残骸との衝突を避ける。
    const pipePath = `\\\\.\\pipe\\FBZZEditorTest-${process.pid}-${Date.now()}`;
    let received: Record<string, unknown> | undefined;

    /// @note busClient と同じ NDJSON プロトコルを話す最小 loopback サーバ。受信 ID をそのまま応答へ相関させる。
    const server = createServer((socket) => {
        socket.setEncoding('utf8');
        let buffer = '';
        socket.on('data', (chunk: string) => {
            buffer += chunk;
            const newline = buffer.indexOf('\n');
            if (newline < 0) {
                return;
            }
            received = JSON.parse(buffer.slice(0, newline)) as Record<string, unknown>;
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
    } finally {
        client.Close();
        await new Promise<void>((resolve) => server.close(() => resolve()));
    }
});

/// @brief 応答が途絶えた要求を再送せず、後続要求だけを新しい接続へ送る。
test('EditorBusClient reconnects after a request times out', { timeout: 5000 }, async () => {
    const pipePath = `\\\\.\\pipe\\FBZZEditorTestTimeout-${process.pid}`;
    let connections = 0;
    const requests: string[] = [];
    const server = createServer(socket => {
        const responds = ++connections === 2;
        socket.setEncoding('utf8');
        let buffer = '';
        socket.on('data', (chunk: string) => {
            buffer += chunk;
            const lines = buffer.split('\n');
            buffer = lines.pop() ?? '';
            for (const line of lines) {
                const request = JSON.parse(line);
                requests.push(request.payload.t);
                if (responds) {
                    socket.write(`${JSON.stringify({ protocol: EDITOR_PROTOCOL, id: request.id, ok: true, result: { ready: true } })}\n`);
                }
            }
        });
    });
    server.listen(pipePath);
    await once(server, 'listening');
    const client = new EditorBusClient(pipePath, 500);
    try {
        await assert.rejects(client.Query({ t: 'editor.state' }), { code: 'BUS_TIMEOUT' });
        assert.deepEqual(await client.Query({ t: 'scene.tree' }), { ready: true });
        assert.deepEqual(requests, ['editor.state', 'scene.tree']);
        assert.equal(connections, 2);
    } finally {
        client.Close();
        await new Promise<void>(resolve => server.close(() => resolve()));
    }
});
