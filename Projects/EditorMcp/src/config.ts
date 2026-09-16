// FBZZ Engine
// config.ts | EditorMcp
// ローカル限定の接続先と AI 権限モードを環境変数から確定する
import * as z from 'zod/v4';

export const PermissionModeSchema = z.enum(['read', 'dry-run', 'write']);
export type PermissionMode = z.infer<typeof PermissionModeSchema>;

export interface EditorMcpConfig {
    busEndpoint: string;
    permission: PermissionMode;
    requestTimeoutMs: number;
}

// 任意ファイルや外部ソケットへ接続しないよう、FBZZ 専用 Windows Named Pipe だけを許可する。
export function IsEditorPipeEndpoint(endpoint: string): boolean {
    return /^\\\\\.\\pipe\\FBZZEditor[A-Za-z0-9_-]*$/.test(endpoint);
}

// 環境変数が不正な場合は安全側の既定値へ戻し、stdio を汚さず stderr で警告できる結果を返す。
export function LoadConfig(environment: NodeJS.ProcessEnv = process.env): EditorMcpConfig {
    const defaultEndpoint = '\\\\.\\pipe\\FBZZEditorCommandBus';
    const requestedEndpoint = environment.FBZZ_EDITOR_PIPE ?? defaultEndpoint;
    const busEndpoint = IsEditorPipeEndpoint(requestedEndpoint) ? requestedEndpoint : defaultEndpoint;
    const permissionResult = PermissionModeSchema.safeParse(environment.FBZZ_MCP_PERMISSION ?? 'read');
    const timeoutValue = Number(environment.FBZZ_EDITOR_BUS_TIMEOUT_MS ?? '10000');
    const requestTimeoutMs = Number.isFinite(timeoutValue)
        ? Math.min(60000, Math.max(1000, Math.trunc(timeoutValue)))
        : 10000;
    return {
        busEndpoint,
        permission: permissionResult.success ? permissionResult.data : 'read',
        requestTimeoutMs,
    };
}
