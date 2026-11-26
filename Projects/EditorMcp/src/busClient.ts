/// @file    busClient.ts
/// @brief   Windows Named Pipe 上の Editor Command Bus へ NDJSON 要求を中継する。
/// @author  Hasegawa Jin
/// @date    2026-07-20
import { randomUUID } from 'node:crypto';
import { createConnection, type Socket } from 'node:net';
import {
    EDITOR_PROTOCOL,
    EditorBusResponseSchema,
    type EditorBusRequest,
    type EditorCommand,
    type EditorQuery,
} from './editorContracts.js';

/// @brief Editor Command Bus の要求と応答の境界。
export interface EditorBus {
    Query(query: EditorQuery): Promise<unknown>;
    Command(command: EditorCommand, dryRun: boolean): Promise<unknown>;
    Close(): void;
}

export class EditorBusError extends Error {
    readonly code: string;
    constructor(code: string, message: string) {
        super(message);
        this.name = 'EditorBusError';
        this.code = code;
    }
}

interface PendingRequest {
    resolve: (value: unknown) => void;
    reject: (error: Error) => void;
    timeout: NodeJS.Timeout;
}

/// @note Named Pipe はローカル限定。結果不明の Command を自動再送すると二重適用になりうる。
export class EditorBusClient implements EditorBus {
    private readonly endpoint: string;
    private readonly requestTimeoutMs: number;
    private socket: Socket | undefined;
    private connectionPromise: Promise<void> | undefined;
    private receiveBuffer = '';
    private readonly pending = new Map<string, PendingRequest>();
    /// @note Editor Pipe は React と共有するため、1 接続につき 1 要求に限る。
    private sendQueue: Promise<unknown> = Promise.resolve();
    private isClosing = false;

    constructor(endpoint: string, requestTimeoutMs: number) {
        this.endpoint = endpoint;
        this.requestTimeoutMs = requestTimeoutMs;
    }

    Query(query: EditorQuery): Promise<unknown> {
        return this.Enqueue('query', query, true);
    }

    Command(command: EditorCommand, dryRun: boolean): Promise<unknown> {
        return this.Enqueue('command', command, dryRun);
    }

    private Enqueue(kind: 'query' | 'command', payload: EditorQuery | EditorCommand, dryRun: boolean): Promise<unknown> {
        const operation = this.sendQueue.then(() => this.Send(kind, payload, dryRun));
        this.sendQueue = operation.catch(() => undefined);
        return operation;
    }

    Close(): void {
        this.isClosing = true;
        this.FailPending(new EditorBusError('BUS_CLOSED', 'Editor Command Bus client を終了しました'));
        const completedSocket = this.socket;
        this.socket = undefined;
        completedSocket?.end();
        completedSocket?.destroy();
    }

    /// @note 起動前の接続失敗は次のツール呼び出しから復旧できる。
    private async EnsureConnected(): Promise<void> {
        if (this.socket !== undefined && !this.socket.destroyed && this.socket.writable) {
            return;
        }
        if (this.connectionPromise !== undefined) {
            return this.connectionPromise;
        }
        if (this.isClosing) {
            return Promise.reject(new EditorBusError('BUS_CLOSED', 'Editor Command Bus client は終了済みです'));
        }
        this.connectionPromise = new Promise<void>((resolve, reject) => {
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
            socket.on('data', (chunk: string) => this.HandleData(chunk));
            socket.once('timeout', () => socket.destroy(new Error('Named Pipe connection timeout')));
            socket.once('error', (error: Error) => {
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

    private async Send(kind: 'query' | 'command', payload: EditorQuery | EditorCommand, dryRun: boolean): Promise<unknown> {
        await this.EnsureConnected();
        const socket = this.socket;
        if (socket === undefined || socket.destroyed || !socket.writable) {
            return Promise.reject(new EditorBusError('BUS_UNAVAILABLE', 'Editor Command Bus が未接続です'));
        }
        const id = randomUUID();
        const request: EditorBusRequest = { protocol: EDITOR_PROTOCOL, id, kind, payload, dryRun, source: 'mcp' };
        return new Promise<unknown>((resolve, reject) => {
            const timeout = setTimeout(() => {
                this.pending.delete(id);
                // サーバーは 1 接続につき 1 要求だけ処理する。遅延応答と受信途中のデータを次の要求へ渡さない。
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
    private HandleData(chunk: string): void {
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

    private HandleLine(line: string): void {
        let parsed: unknown;
        try {
            parsed = JSON.parse(line);
        } catch {
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
        } else {
            pending.reject(new EditorBusError(response.data.error?.code ?? 'ENGINE_REJECTED', response.data.error?.message ?? 'Engine が要求を拒否しました'));
        }
        // 他のローカル利用者に待受を返す。
        const completedSocket = this.socket;
        this.socket = undefined;
        completedSocket?.end();
    }

    private FailPending(error: Error): void {
        for (const pending of this.pending.values()) {
            clearTimeout(pending.timeout);
            pending.reject(error);
        }
        this.pending.clear();
    }
}
