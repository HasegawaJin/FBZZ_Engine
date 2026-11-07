/// @file    ToolSupport.ts
/// @brief   MCP 領域間で共有する応答・差分・Command 送信。
/// @author  Hasegawa Jin
/// @date    2026-11-06
import * as z from 'zod/v4';
import { EditorCommandSchema } from '../editorContracts.js';
export const NameSchema = z.string().min(1).max(128);
/// @note NodeId をキーに正規化済み Scene 状態を比較し、追加・削除・変更を小さい差分で返す。
export function DiffSceneSnapshots(beforeValue, afterValue) {
    const before = beforeValue;
    const after = afterValue;
    const beforeNodes = new Map((before.nodes ?? []).map((node) => [String(node.id), node]));
    const afterNodes = new Map((after.nodes ?? []).map((node) => [String(node.id), node]));
    const added = [];
    const removed = [];
    const changed = [];
    for (const [id, node] of afterNodes) {
        const previous = beforeNodes.get(id);
        if (previous === undefined)
            added.push(node);
        else if (JSON.stringify(previous) !== JSON.stringify(node))
            changed.push({ id, before: previous, after: node });
    }
    for (const [id, node] of beforeNodes) {
        if (!afterNodes.has(id))
            removed.push(node);
    }
    return {
        added,
        removed,
        changed,
        counts: { added: added.length, removed: removed.length, changed: changed.length },
    };
}
export const Delay = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));
export function TextResult(value) {
    return {
        content: [{ type: 'text', text: JSON.stringify(value, null, 2) }],
    };
}
/// @note transport/engine の失敗をプロセス例外にせず、AI が再判断できる tool error として返す。
export async function Safely(operation) {
    try {
        return await operation();
    }
    catch (error) {
        const message = error instanceof Error ? error.message : '不明な Editor Command Bus エラー';
        return {
            isError: true,
            content: [{ type: 'text', text: message }],
        };
    }
}
/// @note dry-run 権限は入力値に関係なく強制し、全領域で同じ Command 契約を検証する。
export function CreateCommandContext(bus, permission) {
    const dryRun = permission !== 'write';
    const run = (command) => Safely(async () => TextResult(await bus.Command(EditorCommandSchema.parse(command), dryRun)));
    const writeAnnotations = {
        readOnlyHint: false,
        destructiveHint: permission === 'write',
        idempotentHint: false,
        openWorldHint: false,
    };
    return { dryRun, run, writeAnnotations };
}
//# sourceMappingURL=ToolSupport.js.map