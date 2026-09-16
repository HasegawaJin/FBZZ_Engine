// FBZZ Engine
// server.ts | EditorMcp
// transport 非依存の MCP サーバと Editor ツール集合を生成する
import { McpServer } from '@modelcontextprotocol/sdk/server/mcp.js';
import type { EditorBus } from './busClient.js';
import type { PermissionMode } from './config.js';
import { ToDraft2020ToolsResult } from './jsonSchemaDraft2020.js';
import { RegisterEditorTools } from './tools.js';

/** Protocol が method 名で引く生ハンドラ。SDK は公開 getter を持たないため型だけ借りる。 */
type RawRequestHandler = (request: unknown, extra: unknown) => Promise<unknown>;

// tools/list の応答を draft 2020-12 へ正規化する。
// WHY: SDK は zod → JSON Schema 変換の target を draft-7 に固定しており
//      (server/zod-json-schema-compat.js)、Anthropic API は draft 2020-12 で
//      input_schema を検証するため z.tuple() などがそのままだと 400 になる。
//      SDK に target を渡す口が無いので、登録済みハンドラを一段包んで変換する。
//      transport ではなくここで包むことで stdio / in-memory の双方に効く。
function InstallDraft2020ToolSchemas(server: McpServer): void {
    const handlers = (server.server as unknown as { _requestHandlers?: Map<string, RawRequestHandler> })._requestHandlers;
    const original = handlers?.get('tools/list');
    if (!handlers || !original) {
        // SDK の内部構造が変わった場合。ツール一覧自体は動くので落とさず警告に留める。
        console.error('[FBZZ Editor MCP] warn: tools/list ハンドラを包めず、JSON Schema は draft-07 のままです');
        return;
    }
    handlers.set('tools/list', async (request, extra) => ToDraft2020ToolsResult(await original(request, extra)));
}

// stdio 以外の transport を後日追加しても同じツール実装を再利用できる factory を提供する。
export function CreateEditorMcpServer(bus: EditorBus, permission: PermissionMode): McpServer {
    const server = new McpServer({
        name: 'fbzz-editor',
        version: '0.1.0',
    }, {
        instructions: [
            'FBZZ Editor をローカル Command Bus 経由で操作します。',
            `現在の権限モードは ${permission} です。`,
            '編集前に editor_catalog で正確なコンポーネント型・フィールド契約を確認し、scene_find / scene_get_tree と node_get_components で対象を特定してください。',
            '自律作業は editor_perceive → dry-run/write Command → editor_perceive を反復し、見た目と Scene 状態の両方で完了条件を確認してください。',
            'VFX制作は vfx_knowledge_catalog → vfx_candidate_fork → schema駆動編集 → vfx_candidate_evaluate → vfx_candidate_accept → vfx_knowledge_promote の閉ループを使い、元アセットを直接試行錯誤で上書きしないでください。',
            '流体エフェクト (煙・炎・水しぶき) は fluid_schema → fluid_create_effect(bake=false) → fluid_preview → fluid_job_status で画像を見る → fluid_set を反復し、見た目が固まってから fluid_bake(立体は bake.mode="3d") → fluid_job_status で done を確認 → できた .vfx を配置してください。焼いた出力は Undo で消えません。',
            '流体は決定論的です: 同じ .fluid と同じ seed なら同じ絵が焼け、fluid_preview(frame=n) は fluid_bake の n コマ目と 1 画素まで同じです。時間方向をまとめて見るには fluid_preview(contactSheet=true)、ばらつきを探すには variants=N (seed 違いを並べる) を使い、気に入った seed を fluid_set で seed に固定してください。変えた値が効いたかは fluid_job_status の fingerprint の変化で判定できます。',
            '流体レシピは部品の組み合わせです: 発生源 (source、最大16)・力 (force、最大8)・障害物 (collider、最大8) を fluid_add_operator で足し、fluid_remove_operator / fluid_move_operator で消す・並べ替え、各部品の値は fluid_set の "source.0.density" のように書きます。部品の種類ごとに効く項目は fluid_schema の operators.<source|force|collider>.fields.<種類> を見てください。部品を時間で動かすには motion.key に {time, offset} を time 昇順で並べます (最大8)。enum はラベル文字列 ("cone" / "vortex" / "plane") で書けます。',
            'collider (sphere / box / plane) は流体が入り込めない障害物で、動かすと流体を押しのけます。床は collider でなく gas.floor / liquid.floor のままです。文字・ロゴ・魔法陣の形に湧かせるには source の shape="texture" と texture (projectRoot 相対の画像パス) を使い、白く不透明なところほど強く注ぎます。画像が無いときは応答の warnings を確認してください。',
            '流体の色を発生源ごとに変えるには render.use_albedo_ramp=true と render.albedo_ramp (4 点固定の {color: リニア RGB, position} 配列) を置き、各 source の color_key (0〜1) で色を選びます。気体は煙に乗って色が運ばれ混ざり、液体は粒子ごとに色を持ちます。bake.mode="3d" でもループ (output.loop) と歪み (render.shading="distortion") を焼け、液体も bake.solver="gpu" で解けます (新しい経路なので怪しければ "cpu" で焼き比べる)。bake.solver は既定 "auto" (GPU が使えなければ CPU へ落ち、理由が fallbackReason に出ます) で、環境によらず同じ絵が要るなら "gpu" か "cpu" を明示してください。',
            '実行確認は可能なら playtest_run を使い、今回発生したログ、性能、意味付きviewportを一つのレポートで検証してください。',
        ].join(' '),
    });
    RegisterEditorTools(server, bus, permission);
    InstallDraft2020ToolSchemas(server);
    return server;
}
