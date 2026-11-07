/// @file    AssetTools.ts
/// @brief   Asset・Sprite・VFX 画像処理の MCP 操作。
/// @author  Hasegawa Jin
/// @date    2026-11-06
import * as z from 'zod/v4';
import { AssetThumbnailResultSchema, SpriteThumbnailResultSchema } from '../editorContracts.js';
import { TextResult, Safely, CreateCommandContext } from './ToolSupport.js';
export function RegisterAssetQueryTools(server, bus) {
    server.registerTool('asset_list', {
        description: 'プロジェクト内のアセット一覧を取得します。ファイルは変更しません。',
        inputSchema: { dir: z.string().min(1).optional().describe('プロジェクト相対ディレクトリ') },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ dir }) => Safely(async () => TextResult(await bus.Query({
        t: 'asset.list',
        ...(dir === undefined ? {} : { dir }),
    }))));
    server.registerTool('asset_inspect', {
        description: 'アセットのサイズ・更新時刻・参照先・参照元・直接サムネイル可否を取得します。',
        inputSchema: { path: z.string().min(1).describe('projectRoot 相対パス') },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ path }) => Safely(async () => TextResult(await bus.Query({ t: 'asset.inspect', path }))));
    server.registerTool('asset_find_unused', {
        description: 'Scene/Prefab/Material等のテキスト参照を走査し、未参照候補を返します。結果はheuristicです。',
        inputSchema: { limit: z.number().int().min(1).max(500).default(100) },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ limit }) => Safely(async () => TextResult(await bus.Query({ t: 'asset.findUnused', limit }))));
    server.registerTool('asset_thumbnail', {
        description: 'PNG/JPEGアセットをMCP画像として直接取得します。モデル等の生成プレビューは対象外です。',
        inputSchema: { path: z.string().min(1) },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ path }) => Safely(async () => {
        const thumbnail = AssetThumbnailResultSchema.parse(await bus.Query({ t: 'asset.thumbnail', path }));
        return {
            content: [{ type: 'image', data: thumbnail.base64, mimeType: thumbnail.mimeType }],
            structuredContent: { path: thumbnail.path },
        };
    }));
    /// @note ── Sprite ──
    /// @note Sprite はファイルではないので asset_list には出ない。切り出したコマへ
    /// @note 参照を張るには、この 2 つで「一覧を見る → 絵を見る」しかない。
    server.registerTool('sprite_list', {
        description: 'Sprite Texture が持つコマの一覧 (ID・名前・矩形・pivot) を返します。'
            + 'reference はそのまま component_set の texturePath / spritePath / .mat の albedo '
            + '(メッシュ描画の .mat のみ) / .fluid の texture 発生源へ渡せる完成形です。'
            + 'どのコマがどの絵かは sprite_thumbnail で確認してください。'
            + '名前は参照キーを兼ねるので、連番のままなら sprite_rename で意味のある名前にできます '
            + '(ID は変わらないため既存の参照は切れません)。',
        inputSchema: { path: z.string().min(1).describe('projectRoot 相対の画像パス') },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ path }) => Safely(async () => TextResult(await bus.Query({ t: 'sprite.list', path }))));
    server.registerTool('sprite_thumbnail', {
        description: 'Sprite 1 コマだけを切り抜いた画像を返します。長辺 256px へ間引かれます。'
            + 'シート全体を見ても「何番目がどの絵か」は判別できないため、割り当て先を選ぶ前にここで確認します。',
        inputSchema: {
            path: z.string().min(1).describe('projectRoot 相対の画像パス'),
            sprite: z.string().min(1).describe('Sprite の ID または名前 (sprite_list の id / name)'),
        },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ path, sprite }) => Safely(async () => {
        const thumbnail = SpriteThumbnailResultSchema.parse(await bus.Query({ t: 'sprite.thumbnail', path, sprite }));
        return {
            content: [{ type: 'image', data: thumbnail.base64, mimeType: thumbnail.mimeType }],
            structuredContent: { name: thumbnail.name, reference: thumbnail.reference },
        };
    }));
}
export function RegisterAssetCommandTools(server, bus, permission) {
    const { run, writeAnnotations } = CreateCommandContext(bus, permission);
    server.registerTool('vfx_generate_motion_vectors', {
        description: 'フリップブックアトラスを解析してモーションベクターアトラス(<name>_mv.png)を生成します。'
            + 'columns/rowsはアトラスの分割数です。MVは «次のコマへの移動量の逆符号» を、アトラス内の最大移動量Sで'
            + '正規化して保存するため、パーティクルの .mat には [textures] tex5 = <name>_mv.png と'
            + '[particle] motion_vector_strength = S / motion_vector_flipbook = true / flipbook_frame_blending = true'
            + 'が揃っている必要があります。materialPath を渡すとこれらを自動で書き込みます (Sは他の手段では分かりません)。',
        inputSchema: {
            texturePath: z.string().min(1).max(1024),
            columns: z.number().int().min(1).max(64),
            rows: z.number().int().min(1).max(64),
            searchRadius: z.number().int().min(1).max(64).optional(),
            loop: z.boolean().optional(),
            rowSequences: z.boolean().optional()
                .describe('各行を独立したアニメーションとして扱う (.mat の sprite_random_row と揃える)'),
            materialPath: z.string().min(1).max(1024).optional()
                .describe('生成結果を割り当てるパーティクル .mat (projectRoot 相対)'),
        },
        annotations: writeAnnotations,
    }, ({ texturePath, columns, rows, searchRadius, loop, rowSequences, materialPath }) => run({
        t: 'vfx.generateMotionVectors', texturePath, columns, rows,
        ...(searchRadius === undefined ? {} : { searchRadius }),
        ...(loop === undefined ? {} : { loop }),
        ...(rowSequences === undefined ? {} : { rowSequences }),
        ...(materialPath === undefined ? {} : { materialPath }),
    }));
    /// @note ── Sprite ──
    server.registerTool('sprite_rename', {
        description: 'Sprite の名前を変更します。ID は変わらないので、保存済みの参照は切れません。'
            + '名前は参照キーを兼ねるため、テクスチャ内で一意である必要があります。'
            + '連番 (_0.._271) のままだと人も AI もどのコマか呼べないので、意味のある名前を付けてください。',
        inputSchema: {
            path: z.string().min(1).max(1024).describe('projectRoot 相対の画像パス'),
            sprite: z.string().min(1).max(256).describe('現在の ID または名前'),
            name: z.string().min(1).max(128).describe('新しい名前 (テクスチャ内で一意)'),
        },
        annotations: writeAnnotations,
    }, ({ path, sprite, name }) => run({ t: 'sprite.rename', path, sprite, name }));
    server.registerTool('sprite_slice', {
        description: 'Sprite Texture を切り直します。Sprite Editor の Slice と同じ実装です。'
            + 'type=grid (既定) は等間隔グリッドで、columns/rows か cellWidth/cellHeight を指定します。'
            + 'type=automatic は alpha が連結した島ごとに外接矩形を作ります (コマ間隔が不揃いなシート向け)。'
            + '既定 (mode=smart) は重なった既存矩形の ID・名前・pivot・Border を残して矩形だけ合わせ直すため、'
            + '保存済みの参照も詰めた pivot も生き残ります。'
            + 'mode=safe は重なった既存矩形に一切触れず、新しい場所だけ足します。'
            + 'mode=replace は全 ID を作り直すので、その Sprite への参照はすべて切れます。',
        inputSchema: {
            path: z.string().min(1).max(1024).describe('projectRoot 相対の画像パス'),
            type: z.enum(['grid', 'automatic']).optional().describe('既定は grid'),
            columns: z.number().int().min(1).max(256).optional(),
            rows: z.number().int().min(1).max(256).optional(),
            cellWidth: z.number().int().min(1).max(16384).optional(),
            cellHeight: z.number().int().min(1).max(16384).optional(),
            offsetX: z.number().int().min(0).max(16384).optional().describe('左上の余白 (grid)'),
            offsetY: z.number().int().min(0).max(16384).optional(),
            paddingX: z.number().int().min(0).max(16384).optional().describe('セル間の隙間 (grid)'),
            paddingY: z.number().int().min(0).max(16384).optional(),
            pivotX: z.number().min(0).max(1).optional().describe('新しい矩形の pivot。既定 0.5'),
            pivotY: z.number().min(0).max(1).optional(),
            keepEmptyRects: z.boolean().optional().describe('false で不透明ピクセルの無いセルを捨てる (grid)'),
            prefix: z.string().min(1).max(64).optional().describe('名前の接頭辞。既定はファイル名 + "_"'),
            mode: z.enum(['smart', 'safe', 'replace']).optional().describe('既定は smart (ID を引き継ぐ)'),
        },
        annotations: writeAnnotations,
    }, ({ path, type, columns, rows, cellWidth, cellHeight, offsetX, offsetY, paddingX, paddingY, pivotX, pivotY, keepEmptyRects, prefix, mode }) => run({
        t: 'sprite.slice', path,
        ...(type === undefined ? {} : { type }),
        ...(columns === undefined ? {} : { columns }),
        ...(rows === undefined ? {} : { rows }),
        ...(cellWidth === undefined ? {} : { cellWidth }),
        ...(cellHeight === undefined ? {} : { cellHeight }),
        ...(offsetX === undefined ? {} : { offsetX }),
        ...(offsetY === undefined ? {} : { offsetY }),
        ...(paddingX === undefined ? {} : { paddingX }),
        ...(paddingY === undefined ? {} : { paddingY }),
        ...(pivotX === undefined ? {} : { pivotX }),
        ...(pivotY === undefined ? {} : { pivotY }),
        ...(keepEmptyRects === undefined ? {} : { keepEmptyRects }),
        ...(prefix === undefined ? {} : { prefix }),
        ...(mode === undefined ? {} : { mode }),
    }));
    server.registerTool('asset_import', {
        description: 'プロジェクト内の src を Assets 起点の dst へインポートします。',
        inputSchema: { src: z.string().min(1), dst: z.string().min(1) },
        annotations: writeAnnotations,
    }, ({ src, dst }) => run({ t: 'asset.import', src, dst }));
}
//# sourceMappingURL=AssetTools.js.map