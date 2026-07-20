import { type EditorCommand, type EditorQuery } from './editorContracts.js';
export interface EditorBus {
    Query(query: EditorQuery): Promise<unknown>;
    Command(command: EditorCommand, dryRun: boolean): Promise<unknown>;
    Close(): void;
}
export declare class EditorBusError extends Error {
    readonly code: string;
    constructor(code: string, message: string);
}
export declare class EditorBusClient implements EditorBus {
    private readonly endpoint;
    private readonly requestTimeoutMs;
    private socket;
    private connectionPromise;
    private receiveBuffer;
    private readonly pending;
    private sendQueue;
    private isClosing;
    constructor(endpoint: string, requestTimeoutMs: number);
    Query(query: EditorQuery): Promise<unknown>;
    Command(command: EditorCommand, dryRun: boolean): Promise<unknown>;
    private Enqueue;
    Close(): void;
    private EnsureConnected;
    private Send;
    private HandleData;
    private HandleLine;
    private FailPending;
}
