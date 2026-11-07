import type { McpServer } from '@modelcontextprotocol/sdk/server/mcp.js';
import { type EditorBus } from '../busClient.js';
import type { PermissionMode } from '../config.js';
import { type EditorCommand } from '../editorContracts.js';
import { ResourceControl, type ResourceToken } from './ResourceControl.js';
type RecordValue = Record<string, unknown>;
export declare class ReliabilitySession {
    private readonly bus;
    private readonly permission;
    private coordination?;
    private readonly now;
    readonly owner: `${string}-${string}-${string}-${string}-${string}`;
    private readonly plans;
    private readonly receipts;
    private readonly jobs;
    constructor(bus: EditorBus, permission: PermissionMode, coordination?: ResourceControl | undefined, now?: () => number);
    private get control();
    Observe(options?: {
        ids?: string[];
        afterSequence?: number;
        managedRequestId?: string;
        packageRequestId?: string;
    }): Promise<RecordValue>;
    private SceneTarget;
    Claim(ids: string[], ttlMs: number, mode: 'trial' | 'enforced'): Promise<RecordValue>;
    Renew(token: ResourceToken, ttlMs: number): Promise<unknown>;
    Release(token: ResourceToken): Promise<unknown>;
    Resources(): Promise<unknown>;
    Plan(cmds: EditorCommand[], label: string): Promise<RecordValue>;
    Apply(planId: string, token: ResourceToken): Promise<RecordValue>;
    Rollback(receipt: string, token: ResourceToken): Promise<RecordValue>;
    Verify(receipt: string, token: ResourceToken, options: {
        save: boolean;
        reload: boolean;
        render: boolean;
        rollbackOnFailure: boolean;
    }): Promise<RecordValue>;
    StartJob(name: string, command: EditorCommand): Promise<RecordValue>;
    Job(jobId: string): RecordValue;
}
export declare function RegisterReliabilityTools(server: McpServer, bus: EditorBus, permission: PermissionMode): void;
export {};
