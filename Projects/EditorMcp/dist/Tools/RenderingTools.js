/// @file    RenderingTools.ts
/// @brief   描画・Material・Viewport の MCP 操作。
/// @author  Hasegawa Jin
/// @date    2026-11-06
import * as z from 'zod/v4';
import { NodeIdSchema, SemanticViewportResultSchema, Vec3Schema, ViewportCaptureResultSchema } from '../editorContracts.js';
import { TextResult, Safely, CreateCommandContext, ReadViewport } from './ToolSupport.js';
export function RegisterRenderingQueryTools(server, bus) {
    server.registerTool('shader_inspect', {
        description: 'シェーダーが公開する変数とテクスチャスロットの目録を返します。'
            + '.matのparamsやVFX MeshノードのanimatedParamは「シェーダー変数名」を要求しますが、'
            + 'その一覧を知る手段はこれだけです。'
            + '**ここに無い名前を書いても保存は通り、実行時に黙って無視されます**'
            + '(値を変えても絵が変わらない、という形でしか現れません)。'
            + 'componentsは渡す値の個数で、float3の変数へ1個だけ渡すと残りは0になります。'
            + '未使用変数はコンパイル時に消えるため、HLSLに宣言があってもここに出ないことがあります'
            + '(実際に効くのはコンパイル済みバイトコードの実体です)。'
            + 'pathは.matのshaderフィールド(guid:形式も可)またはシェーダーパスです。',
        inputSchema: {
            path: z.string().min(1).max(1024).describe('シェーダーパスまたはguid:参照'),
        },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ path }) => Safely(async () => TextResult(await bus.Query({ t: 'shader.inspect', path }))));
    server.registerTool('shader_get_compile_diagnostics', {
        description: 'DX11/DX12ランタイムコンパイルと外部HLSLビルドのエラー・警告を取得します。'
            + 'Editor の赤い Shader Compile Error バナーと同じ診断を返します。'
            + 'path/entryPoint/target/messageを読み、修正後は再コンパイルして一覧から消えたことを確認してください。',
        inputSchema: {},
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, () => Safely(async () => TextResult(await bus.Query({ t: 'shader.diagnostics' }))));
    server.registerTool('material_inspect', {
        description: 'NodeのMaterialパス、解決済みShader、インスタンス別parameter overrideを取得します。',
        inputSchema: { id: NodeIdSchema },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ id }) => Safely(async () => TextResult(await bus.Query({ t: 'material.inspect', id }))));
    server.registerTool('viewport_capture', {
        description: '要求以降に描画された viewport の PNG と出自を返します。w/h の枠内へ縦横比を保ってリサイズします。width/heightはPNG、sourceWidth/sourceHeightは描画元の寸法です。',
        inputSchema: {
            w: z.number().int().min(160).max(1920).default(960).describe('PNG の最大幅'),
            h: z.number().int().min(90).max(1080).default(540).describe('PNG の最大高さ'),
            view: z.enum(['scene', 'game']).default('scene').describe('Scene / Game'),
            cameraId: NodeIdSchema.optional().describe('描画に使用したカメラがこのノードであることを照合する'),
            minFrame: z.int().nonnegative().optional().describe('許容する最も古い描画フレーム'),
        },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ w, h, view, cameraId, minFrame }) => Safely(async () => {
        const capture = await ReadViewport(bus, { t: 'viewport.capture', w, h, view,
            ...(cameraId === undefined ? {} : { cameraId }), ...(minFrame === undefined ? {} : { minFrame }) }, ViewportCaptureResultSchema);
        return {
            content: [{ type: 'image', data: capture.base64, mimeType: capture.mimeType }],
            structuredContent: {
                width: capture.width,
                height: capture.height,
                sourceWidth: capture.sourceWidth ?? capture.width,
                sourceHeight: capture.sourceHeight ?? capture.height,
                view: capture.view ?? view,
                ...(capture.cameraId === undefined ? {} : { cameraId: capture.cameraId }),
                provenance: capture.provenance,
            },
        };
    }));
    server.registerTool('viewport_capture_semantic', {
        description: 'w/hの枠内へ縦横比を保ってリサイズしたViewport PNGと、そのPNG上のGameObject座標、シーン・カメラ・描画パス・フレームの出自を取得します。',
        inputSchema: {
            w: z.number().int().min(160).max(1920).default(960),
            h: z.number().int().min(90).max(1080).default(540),
            view: z.enum(['scene', 'game']).default('scene'),
            visibleOnly: z.boolean().default(true),
            minFrame: z.int().nonnegative().optional(),
        },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ w, h, view, visibleOnly, minFrame }) => Safely(async () => {
        const capture = await ReadViewport(bus, { t: 'viewport.semantic', w, h, view,
            ...(minFrame === undefined ? {} : { minFrame }) }, SemanticViewportResultSchema);
        const objects = visibleOnly ? capture.objects.filter((object) => object.visible) : capture.objects;
        const details = { width: capture.width, height: capture.height,
            sourceWidth: capture.sourceWidth ?? capture.width, sourceHeight: capture.sourceHeight ?? capture.height, view: capture.view ?? view,
            cameraPosition: capture.cameraPosition, objects, provenance: capture.provenance };
        return {
            content: [
                { type: 'text', text: JSON.stringify(details, null, 2) },
                { type: 'image', data: capture.base64, mimeType: capture.mimeType },
            ],
            structuredContent: details,
        };
    }));
}
export function RegisterRenderingCommandTools(server, bus, permission) {
    const { run, writeAnnotations } = CreateCommandContext(bus, permission);
    server.registerTool('material_assign', {
        description: 'GameObjectへマテリアルアセットを割り当てます。MaterialComponentがなければ追加します。',
        inputSchema: { id: NodeIdSchema, path: z.string().min(1) },
        annotations: writeAnnotations,
    }, ({ id, path }) => run({ t: 'material.assign', id, path }));
    server.registerTool('material_set_parameter', {
        description: 'GameObject単位のマテリアル値を上書きします。shader_inspectのwritable/type/componentsに従い、配列は要素順、行列は行優先、boolは0/1で指定します。型・要素数・範囲不一致は拒否します。',
        inputSchema: {
            id: NodeIdSchema,
            parameter: z.string().min(1).max(128),
            value: z.array(z.number().finite()).min(1).max(16384),
        },
        annotations: writeAnnotations,
    }, ({ id, parameter, value }) => run({ t: 'material.override', id, parameter, value }));
    server.registerTool('material_set_shader', {
        description: 'プロジェクト内の.matアセットが参照するシェーダーパスを変更します。',
        inputSchema: { path: z.string().min(1), shaderPath: z.string().min(1) },
        annotations: writeAnnotations,
    }, ({ path, shaderPath }) => run({ t: 'material.asset.setShader', path, shaderPath }));
    server.registerTool('viewport_camera_set', {
        description: 'Scene View カメラを座標へ移動し、座標または NodeId を注視させます。',
        inputSchema: {
            position: Vec3Schema.optional(),
            lookAt: Vec3Schema.optional(),
            targetId: NodeIdSchema.optional(),
        },
        annotations: writeAnnotations,
    }, ({ position, lookAt, targetId }) => run({
        t: 'viewport.camera',
        ...(position === undefined ? {} : { position }),
        ...(lookAt === undefined ? {} : { lookAt }),
        ...(targetId === undefined ? {} : { targetId }),
    }));
}
//# sourceMappingURL=RenderingTools.js.map