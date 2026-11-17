/// @file    ToolSupport.ts
/// @brief   MCP 領域間で共有する応答・差分・Command 送信。
/// @author  Hasegawa Jin
/// @date    2026-11-06
import * as z from 'zod/v4';
import type { CallToolResult } from '@modelcontextprotocol/sdk/types.js';
import { EditorBusError, type EditorBus } from '../busClient.js';
import type { PermissionMode } from '../config.js';
import { EditorCommandSchema, type EditorCommand, type EditorQuery, type ViewportCaptureResult } from '../editorContracts.js';

export const NameSchema = z.string().min(1).max(128);

export interface SceneSnapshotLike {
    nodes?: Array<Record<string, unknown>>;
    count?: number;
}

/// @note NodeId をキーに正規化済み Scene 状態を比較し、追加・削除・変更を小さい差分で返す。
export function DiffSceneSnapshots(beforeValue: unknown, afterValue: unknown): Record<string, unknown> {
    const before = beforeValue as SceneSnapshotLike;
    const after = afterValue as SceneSnapshotLike;
    const beforeNodes = new Map((before.nodes ?? []).map((node) => [String(node.id), node]));
    const afterNodes = new Map((after.nodes ?? []).map((node) => [String(node.id), node]));
    const added: unknown[] = [];
    const removed: unknown[] = [];
    const changed: unknown[] = [];
    for (const [id, node] of afterNodes) {
        const previous = beforeNodes.get(id);
        if (previous === undefined) added.push(node);
        else if (JSON.stringify(previous) !== JSON.stringify(node)) changed.push({ id, before: previous, after: node });
    }
    for (const [id, node] of beforeNodes) {
        if (!afterNodes.has(id)) removed.push(node);
    }
    return {
        added,
        removed,
        changed,
        counts: { added: added.length, removed: removed.length, changed: changed.length },
    };
}

export const Delay = (milliseconds: number): Promise<void> => new Promise((resolve) => setTimeout(resolve, milliseconds));

/// @note 要求後に描画されたフレームを待つ。古い成功応答に現在の状態を付け替えない。
export async function ReadViewport<T extends ViewportCaptureResult>(bus: EditorBus,
    query: Extract<EditorQuery, { t: 'viewport.capture' | 'viewport.semantic' }>,
    schema: { parse(input: unknown): T }, state?: unknown): Promise<T & { provenance: NonNullable<T['provenance']> }> {
    const current = state ?? await bus.Query({ t: 'editor.state' });
    const index = current && typeof current === 'object' && 'frameIndex' in current ? current.frameIndex : undefined;
    const generation = current && typeof current === 'object' && 'sceneGeneration' in current ? current.sceneGeneration : undefined;
    const minFrame = Math.max(query.minFrame ?? 0, typeof index === 'number' && Number.isSafeInteger(index) ? index : 0);
    const deadline = performance.now() + 3000;
    for (;;) {
        try {
            const capture = schema.parse(await bus.Query({ ...query, ...(minFrame ? { minFrame } : {}) }));
            const source = capture.provenance;
            if (!source || source.frame < minFrame || source.view !== (query.view ?? 'scene')
                || source.renderPass !== 'ViewportOutput'
                || (query.t === 'viewport.capture' && query.cameraId !== undefined && source.cameraId !== query.cameraId)
                || (typeof generation === 'number' && source.sceneGeneration !== generation))
                throw new EditorBusError('STALE_CAPTURE', '指定したビューとフレームの出自を確認できません。');
            return { ...capture, provenance: source };
        } catch (error) {
            if (!(error instanceof EditorBusError) || error.code !== 'CAPTURE_NOT_READY' || performance.now() >= deadline)
                throw error;
            await Delay(40);
        }
    }
}

export function TextResult(value: unknown): CallToolResult {
    return {
        content: [{ type: 'text', text: JSON.stringify(value, null, 2) }],
    };
}

/// @note transport/engine の失敗をプロセス例外にせず、AI が再判断できる tool error として返す。
export async function Safely(operation: () => Promise<CallToolResult>): Promise<CallToolResult> {
    try {
        return await operation();
    } catch (error) {
        const message = error instanceof Error ? error.message : '不明な Editor Command Bus エラー';
        return {
            isError: true,
            content: [{ type: 'text', text: message }],
        };
    }
}

/// @note dry-run 権限は入力値に関係なく強制し、全領域で同じ Command 契約を検証する。
export function CreateCommandContext(bus: EditorBus, permission: PermissionMode) {
    const dryRun = permission !== 'write';
    const run = (command: EditorCommand, preview = false) => Safely(async () =>
        TextResult(await bus.Command(EditorCommandSchema.parse(command), dryRun || preview)));
    const writeAnnotations = {
        readOnlyHint: false,
        destructiveHint: permission === 'write',
        idempotentHint: false,
        openWorldHint: false,
    };
    return { dryRun, run, writeAnnotations };
}
