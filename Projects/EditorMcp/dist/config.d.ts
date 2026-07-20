import * as z from 'zod/v4';
export declare const PermissionModeSchema: z.ZodEnum<{
    "dry-run": "dry-run";
    read: "read";
    write: "write";
}>;
export type PermissionMode = z.infer<typeof PermissionModeSchema>;
export interface EditorMcpConfig {
    busEndpoint: string;
    permission: PermissionMode;
    requestTimeoutMs: number;
}
export declare function IsEditorPipeEndpoint(endpoint: string): boolean;
export declare function LoadConfig(environment?: NodeJS.ProcessEnv): EditorMcpConfig;
