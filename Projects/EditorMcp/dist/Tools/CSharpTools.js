/// @file    CSharpTools.ts
/// @brief   C# semantic source analysis and session-owned rename plans for MCP.
/// @author  Hasegawa Jin
/// @date    2026-11-06
import * as z from 'zod/v4';
import { CSharpSession } from '../Analysis/RoslynClient.js';
import { Safely } from './ToolSupport.js';
const ReferencePaths = z.array(z.string().min(1).max(1024)).max(64).default([]).describe('Additional metadata DLLs relative to the active project; the current Script API is included automatically.');
const Limit = z.number().int().min(1).max(1000).default(250);
const Position = { file: z.string().min(1).max(1024).describe('Project-relative Assets/Scripts/**/*.cs source file'),
    line: z.number().int().min(1).max(2147483647).describe('1-based source line'),
    column: z.number().int().min(1).max(2147483647).describe('1-based UTF-16 column on the identifier') };
function Result(value) {
    return { content: [{ type: 'text', text: JSON.stringify(value, null, 2) }], structuredContent: value,
        ...(value.status === 'failure' ? { isError: true } : {}) };
}
/// @note Analysis covers a source snapshot, not MSBuild imports or generators; source edits never imply a successful managed build.
export function RegisterCSharpTools(server, bus, permission) {
    const session = new CSharpSession(bus, permission);
    const readAnnotations = { readOnlyHint: true, openWorldHint: false };
    server.registerTool('csharp_analyze', {
        description: 'Roslynで現在プロジェクトのAssets/Scripts全体を診断します。SDK参照と明示DLLを使うsource snapshot解析で、csproj評価・Analyzer・Generator・managed buildは実行しません。',
        inputSchema: { referencePaths: ReferencePaths, limit: Limit }, annotations: readAnnotations,
    }, options => Safely(async () => Result(await session.Analyze('diagnostics', options))));
    server.registerTool('csharp_symbols', {
        description: 'C#宣言の型・シンボル・ソース位置と保存対象の可能性をRoslynで取得します。文字列一致で参照を推測しません。',
        inputSchema: { referencePaths: ReferencePaths, filter: z.string().max(256).optional(), limit: Limit }, annotations: readAnnotations,
    }, options => Safely(async () => Result(await session.Analyze('symbols', options))));
    server.registerTool('csharp_find_references', {
        description: '指定した識別子と同じC#シンボルの宣言・参照をソースsnapshot内で検索します。文字列による呼出しやScene保存参照は対象外です。',
        inputSchema: { ...Position, referencePaths: ReferencePaths, limit: Limit }, annotations: readAnnotations,
    }, options => Safely(async () => Result(await session.Analyze('references', options))));
    server.registerTool('csharp_rename_preview', {
        description: '同じC#シンボルだけを改名する差分と5分有効なplanIdを返します。保存field・Script/DataAsset型・公開契約・解決不能なコードは適用不可とし、改名後の診断と他の識別子のbindingを検査します。ファイルは変更しません。',
        inputSchema: { ...Position, referencePaths: ReferencePaths, newName: z.string().min(1).max(256) }, annotations: readAnnotations,
    }, options => Safely(async () => Result(await session.Preview(options))));
    if (permission !== 'read')
        server.registerTool('csharp_rename_apply', {
            description: 'このMCPセッションのrename planを全source/API/DLLのhash再検査後に適用します。dry-runは書込なし。複数ファイルのatomic保証はなく、失敗時は復旧と競合を明示します。managed buildは別途必要です。',
            inputSchema: { planId: z.string().uuid() },
            annotations: { readOnlyHint: false, destructiveHint: permission === 'write', openWorldHint: false, idempotentHint: false },
        }, ({ planId }) => Safely(async () => Result(await session.Apply(planId))));
}
//# sourceMappingURL=CSharpTools.js.map