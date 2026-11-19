/// @file    busClient.ts
/// @brief   Windows Named Pipe 上の Editor Command Bus へ NDJSON 要求を中継する。
/// @author  Hasegawa Jin
/// @date    2026-07-20
import { randomUUID } from 'node:crypto';
import { createConnection } from 'node:net';
import { EDITOR_PROTOCOL, EditorBusResponseSchema, } from './editorContracts.js';
export class EditorBusError extends Error {
    code;
    constructor(code, message) {
        super(message);
        this.name = 'EditorBusError';
        this.code = code;
    }
}
/// @note Named Pipe はローカル限定。結果不明の Command を自動再送すると二重適用になりうる。
export class EditorBusClient {
    endpoint;
    requestTimeoutMs;
    socket;
    connectionPromise;
    receiveBuffer = '';
    pending = new Map();
    /// @note Editor Pipe は React と共有するため、1 接続につき 1 要求に限る。
    sendQueue = Promise.resolve();
    isClosing = false;
    constructor(endpoint, requestTimeoutMs) {
        this.endpoint = endpoint;
        this.requestTimeoutMs = requestTimeoutMs;
    }
    Query(query) {
        return this.Enqueue('query', query, true);
    }
    Command(command, dryRun) {
        return this.Enqueue('command', command, dryRun);
    }
    Enqueue(kind, payload, dryRun) {
        const operation = this.sendQueue.then(() => this.Send(kind, payload, dryRun));
        this.sendQueue = operation.catch(() => undefined);
        return operation;
    }
    Close() {
        this.isClosing = true;
        this.FailPending(new EditorBusError('BUS_CLOSED', 'Editor Command Bus client を終了しました'));
        const completedSocket = this.socket;
        this.socket = undefined;
        completedSocket?.end();
        completedSocket?.destroy();
    }
    /// @note 起動前の接続失敗は次のツール呼び出しから復旧できる。
    async EnsureConnected() {
        if (this.socket !== undefined && !this.socket.destroyed && this.socket.writable) {
            return;
        }
        if (this.connectionPromise !== undefined) {
            return this.connectionPromise;
        }
        if (this.isClosing) {
            return Promise.reject(new EditorBusError('BUS_CLOSED', 'Editor Command Bus client は終了済みです'));
        }
        this.connectionPromise = new Promise((resolve, reject) => {
            const socket = createConnection(this.endpoint);
            this.socket = socket;
            let connected = false;
            socket.setEncoding('utf8');
            socket.setTimeout(this.requestTimeoutMs);
            socket.once('connect', () => {
                connected = true;
                socket.setTimeout(0);
                resolve();
            });
            socket.on('data', (chunk) => this.HandleData(chunk));
            socket.once('timeout', () => socket.destroy(new Error('Named Pipe connection timeout')));
            socket.once('error', (error) => {
                if (!connected) {
                    reject(new EditorBusError('BUS_UNAVAILABLE', error.message));
                }
            });
            socket.once('close', () => {
                if (this.socket !== socket) {
                    return;
                }
                this.socket = undefined;
                this.receiveBuffer = '';
                this.FailPending(new EditorBusError('BUS_DISCONNECTED', 'Editor Command Bus との接続が切れました'));
            });
        }).finally(() => {
            this.connectionPromise = undefined;
        });
        return this.connectionPromise;
    }
    async Send(kind, payload, dryRun) {
        await this.EnsureConnected();
        const socket = this.socket;
        if (socket === undefined || socket.destroyed || !socket.writable) {
            return Promise.reject(new EditorBusError('BUS_UNAVAILABLE', 'Editor Command Bus が未接続です'));
        }
        const id = randomUUID();
        const request = { protocol: EDITOR_PROTOCOL, id, kind, payload, dryRun, source: 'mcp' };
        return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
                this.pending.delete(id);
                /// @note サーバーは 1 接続につき 1 要求だけ処理する。遅延応答と受信途中のデータを次の要求へ渡さない。
                if (this.socket === socket) {
                    this.socket = undefined;
                    this.receiveBuffer = '';
                }
                socket.destroy();
                reject(new EditorBusError('BUS_TIMEOUT', `Editor Command Bus 応答が ${this.requestTimeoutMs}ms を超えました`));
            }, this.requestTimeoutMs);
            this.pending.set(id, { resolve, reject, timeout });
            socket.write(`${JSON.stringify(request)}\n`, (error) => {
                if (error === null || error === undefined) {
                    return;
                }
                const pending = this.pending.get(id);
                if (pending !== undefined) {
                    clearTimeout(pending.timeout);
                    this.pending.delete(id);
                    pending.reject(new EditorBusError('BUS_SEND_FAILED', error.message));
                }
            });
        });
    }
    /// @note Pipe の読み取り境界と NDJSON の行境界は一致しない。
    HandleData(chunk) {
        this.receiveBuffer += chunk;
        for (;;) {
            const newline = this.receiveBuffer.indexOf('\n');
            if (newline < 0) {
                return;
            }
            const line = this.receiveBuffer.slice(0, newline).trim();
            this.receiveBuffer = this.receiveBuffer.slice(newline + 1);
            if (line.length > 0) {
                this.HandleLine(line);
            }
        }
    }
    HandleLine(line) {
        let parsed;
        try {
            parsed = JSON.parse(line);
        }
        catch {
            return;
        }
        const response = EditorBusResponseSchema.safeParse(parsed);
        if (!response.success) {
            return;
        }
        const pending = this.pending.get(response.data.id);
        if (pending === undefined) {
            return;
        }
        clearTimeout(pending.timeout);
        this.pending.delete(response.data.id);
        if (response.data.ok) {
            pending.resolve(response.data.result);
        }
        else {
            pending.reject(new EditorBusError(response.data.error?.code ?? 'ENGINE_REJECTED', response.data.error?.message ?? 'Engine が要求を拒否しました'));
        }
        /// @note 他のローカル利用者に待受を返す。
        const completedSocket = this.socket;
        this.socket = undefined;
        completedSocket?.end();
    }
    FailPending(error) {
        for (const pending of this.pending.values()) {
            clearTimeout(pending.timeout);
            pending.reject(error);
        }
        this.pending.clear();
    }
}
//# sourceMappingURL=busClient.js.map