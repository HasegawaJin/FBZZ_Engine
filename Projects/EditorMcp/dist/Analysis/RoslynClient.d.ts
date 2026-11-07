import type { EditorBus } from '../busClient.js';
import type { PermissionMode } from '../config.js';
type RecordValue = Record<string, unknown>;
export interface AnalysisOptions {
    referencePaths?: string[];
    limit?: number;
    filter?: string;
    file?: string;
    line?: number;
    column?: number;
    newName?: string;
}
export type AnalysisRunner = (request: RecordValue) => Promise<RecordValue>;
export declare function RunRoslyn(request: RecordValue): Promise<RecordValue>;
export declare class CSharpSession {
    private readonly bus;
    private readonly permission;
    private readonly runner;
    private readonly now;
    private readonly plans;
    private applying;
    constructor(bus: EditorBus, permission: PermissionMode, runner?: AnalysisRunner, now?: () => number);
    Analyze(operation: 'diagnostics' | 'symbols' | 'references', options?: AnalysisOptions): Promise<RecordValue>;
    Preview(options: AnalysisOptions): Promise<RecordValue>;
    private Execute;
    Apply(planId: string): Promise<RecordValue>;
    private ApplyPlan;
}
export {};
