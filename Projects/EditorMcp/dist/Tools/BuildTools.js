/// @file    BuildTools.ts
/// @brief   Managed・配布ビルドの MCP 操作。
/// @author  Hasegawa Jin
/// @date    2026-11-06
import * as z from 'zod/v4';
import { PackageBuildOptionsSchema } from '../editorContracts.js';
import { TextResult, Safely, CreateCommandContext } from './ToolSupport.js';
export function RegisterBuildQueryTools(server, bus) {
    server.registerTool('build_get_status', {
        description: 'C++ DLL / HLSL の既存ビルド結果と、managed に C# の要求別状態を返します。'
            + 'C# は build_run(target="managed") の managedRequestId を渡し、managed.request.state と terminal を確認してください。'
            + 'applied だけが要求版の適用完了です。deferred は Play 中の保留、superseded は新しいソース版への置換、failed は診断付きの終端です。'
            + '配布は packageRequestId を渡して package.state / terminal / outputReady を確認し、outputExe と managedManifest の有無を読めます。'
            + 'トップレベル building はゲーム / シェーダーのビルド状態、managedBuildBusy は C# のビルド適用が Play を止めている状態です。C# 要求の完了は managed.request の terminal で判定してください。',
        inputSchema: {
            limit: z.number().int().min(1).max(20).default(5),
            managedRequestId: z.string().regex(/^managed:[1-9][0-9]{0,19}$/).optional().describe('build_run(target="managed") が返した要求 ID'),
            packageRequestId: z.string().regex(/^package:[1-9][0-9]{0,19}$/).optional().describe('build_run(target="package") が返した要求 ID'),
        },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ limit, managedRequestId, packageRequestId }) => Safely(async () => TextResult(await bus.Query({
        t: 'build.status', limit, ...(managedRequestId === undefined ? {} : { managedRequestId }),
        ...(packageRequestId === undefined ? {} : { packageRequestId }),
    }))));
}
export function RegisterBuildCommandTools(server, bus, permission) {
    const { run, writeAnnotations } = CreateCommandContext(bus, permission);
    server.registerTool('build_run', {
        description: 'target="managed" (既定) は C# の現在のソース版を非同期でビルドします。'
            + '返された managedRequestId を build_get_status に渡し、managed.request.state="applied" を待ってから新しい catalog を使います。'
            + 'C# の適用状態は要求 ID で読み、superseded / failed になった要求を待ち続けないでください。'
            + 'target="package" は package 設定で配布先・製品名・Release等を指定して完成品を書き出します。'
            + '先に editor_op_query("build.package_check", args) で事前確認し、Playや別ビルド中は開始できません。'
            + '完了は build_get_status(packageRequestId) の package.state="done" / outputReady=true、取消は editor_op_invoke("build.package_cancel",{packageRequestId}) です。',
        inputSchema: { target: z.enum(['managed', 'package']).default('managed'), package: PackageBuildOptionsSchema.optional() },
        annotations: writeAnnotations,
    }, ({ target, package: options }) => run({ t: 'build.run', target, ...(options === undefined ? {} : { package: options }) }));
}
//# sourceMappingURL=BuildTools.js.map