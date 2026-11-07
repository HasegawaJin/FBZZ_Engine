export declare const EXECUTION_RESOURCE = "execution";
export interface ResourceToken {
    leaseId: string;
    capability: string;
}
export interface ResourceLease extends ResourceToken {
    owner: string;
    targets: string[];
    expiresAt: number;
}
export interface ResourceClaim {
    leaseId: string;
    owner: string;
    targets: string[];
    expiresAt: number;
    expired: boolean;
    operations: ResourceOperation[];
}
export interface ResourceOperation {
    jobId: string;
    jobName: string;
    targets: string[];
    pid: number;
    startedAt: number;
}
export interface ResourceControlOptions {
    now?: () => number;
    maxAttempts?: number;
}
export interface ExclusiveOptions {
    ttlMs?: number;
    targets?: readonly string[];
}
export declare function ResourceNamespace(editorEndpoint: string, projectRoot: string): string;
export declare class ResourceControlError extends Error {
    readonly code: string;
    readonly conflicts: ResourceClaim[];
    constructor(code: string, message: string, conflicts?: ResourceClaim[]);
}
export declare class ResourceControl {
    private readonly directory;
    private readonly namespaceHash;
    private readonly now;
    private readonly maxAttempts;
    private readonly processInstance;
    constructor(namespace: string, stateDirectory?: string, options?: ResourceControlOptions);
    Acquire(owner: string, targets: readonly string[], ttlMs: number): Promise<ResourceLease>;
    Renew(owner: string, token: ResourceToken, ttlMs: number): Promise<ResourceLease>;
    Release(owner: string, token: ResourceToken): Promise<boolean>;
    Assert(owner: string, token: ResourceToken, targets: readonly string[]): Promise<ResourceClaim>;
    Inspect(): Promise<ResourceClaim[]>;
    RunWithLease<T>(owner: string, token: ResourceToken, targets: readonly string[], operation: (context: ResourceOperation) => Promise<T> | T, jobName?: string): Promise<T>;
    RunExclusive<T>(owner: string, jobName: string, operation: (context: ResourceOperation) => Promise<T> | T, options?: ExclusiveOptions): Promise<T>;
    private EndPin;
    private Update;
    private Read;
    private Publish;
    private GenerationPath;
}
