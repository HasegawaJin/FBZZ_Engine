import * as z from 'zod/v4';
import type { CallToolResult } from '@modelcontextprotocol/sdk/types.js';
import type { EditorBus } from '../busClient.js';
import type { PermissionMode } from '../config.js';
import { type EditorCommand } from '../editorContracts.js';
export declare const NameSchema: z.ZodString;
export interface SceneSnapshotLike {
    nodes?: Array<Record<string, unknown>>;
    count?: number;
}
export declare function DiffSceneSnapshots(beforeValue: unknown, afterValue: unknown): Record<string, unknown>;
export declare const Delay: (milliseconds: number) => Promise<void>;
export declare function TextResult(value: unknown): CallToolResult;
export declare function Safely(operation: () => Promise<CallToolResult>): Promise<CallToolResult>;
export declare function CreateCommandContext(bus: EditorBus, permission: PermissionMode): {
    dryRun: boolean;
    run: (command: EditorCommand) => Promise<{
        [x: string]: unknown;
        _meta?: {
            [x: string]: unknown;
            progressToken?: string | number | undefined;
            "io.modelcontextprotocol/related-task"?: {
                taskId: string;
            } | undefined;
        } | undefined;
        content: ({
            type: "text";
            text: string;
            annotations?: {
                audience?: ("assistant" | "user")[] | undefined;
                priority?: number | undefined;
                lastModified?: string | undefined;
            } | undefined;
            _meta?: {
                [x: string]: unknown;
            } | undefined;
        } | {
            type: "image";
            data: string;
            mimeType: string;
            annotations?: {
                audience?: ("assistant" | "user")[] | undefined;
                priority?: number | undefined;
                lastModified?: string | undefined;
            } | undefined;
            _meta?: {
                [x: string]: unknown;
            } | undefined;
        } | {
            type: "audio";
            data: string;
            mimeType: string;
            annotations?: {
                audience?: ("assistant" | "user")[] | undefined;
                priority?: number | undefined;
                lastModified?: string | undefined;
            } | undefined;
            _meta?: {
                [x: string]: unknown;
            } | undefined;
        } | {
            uri: string;
            description?: string | undefined;
            mimeType?: string | undefined;
            size?: number | undefined;
            annotations?: {
                audience?: ("assistant" | "user")[] | undefined;
                priority?: number | undefined;
                lastModified?: string | undefined;
            } | undefined;
            _meta?: {
                [x: string]: unknown;
            } | undefined;
            icons?: {
                src: string;
                mimeType?: string | undefined;
                sizes?: string[] | undefined;
                theme?: "dark" | "light" | undefined;
            }[] | undefined;
            name: string;
            title?: string | undefined;
            type: "resource_link";
        } | {
            type: "resource";
            resource: {
                uri: string;
                mimeType?: string | undefined;
                _meta?: {
                    [x: string]: unknown;
                } | undefined;
                text: string;
            } | {
                uri: string;
                mimeType?: string | undefined;
                _meta?: {
                    [x: string]: unknown;
                } | undefined;
                blob: string;
            };
            annotations?: {
                audience?: ("assistant" | "user")[] | undefined;
                priority?: number | undefined;
                lastModified?: string | undefined;
            } | undefined;
            _meta?: {
                [x: string]: unknown;
            } | undefined;
        })[];
        structuredContent?: {
            [x: string]: unknown;
        } | undefined;
        isError?: boolean | undefined;
    }>;
    writeAnnotations: {
        readOnlyHint: boolean;
        destructiveHint: boolean;
        idempotentHint: boolean;
        openWorldHint: boolean;
    };
};
