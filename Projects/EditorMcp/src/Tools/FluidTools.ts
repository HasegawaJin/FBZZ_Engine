/// @file    FluidTools.ts
/// @brief   流体レシピ・プレビュー・ベイクの MCP 操作。
/// @author  Hasegawa Jin
/// @date    2026-11-06
import * as z from 'zod/v4';
import type { McpServer } from '@modelcontextprotocol/sdk/server/mcp.js';
import type { EditorBus } from '../busClient.js';
import type { PermissionMode } from '../config.js';
import { FluidEffectNameSchema, FluidFieldsSchema, FluidOperatorIndexSchema, FluidOperatorListSchema, FluidOperatorTypeSchema, FluidJobIdSchema, FluidJobStatusResultSchema, FluidPathSchema, FluidPresetSchema } from '../editorContracts.js';
import { TextResult, Safely, CreateCommandContext } from './ToolSupport.js';

/// @note 識別子は MakeFluidPreset の enum 名。括弧内は Editor の Preset メニューの表示名 (FluidPresetName)。
const FluidPresetDescription = 'プリセット名。Smoke(Smoke Puff) / Fire(Fire (loop)) / Explosion / Steam(Steam (loop)) / '
    + 'DustBurst(Dust Burst) / Ink(Ink Swirl) / MagicWisp(Magic Wisp (loop)) / HeatHaze(Heat Haze (loop)) / '
    + 'WaterSplash(Water Splash) / WaterJet(Water Jet (loop)) / BloodBurst(Blood Burst) / LavaBlob(Lava Blob) / '
    + 'PlasmaBurst(Plasma Burst) / ArcHaze(Arc Haze (loop))。'
    + '正確な一覧は fluid_schema の presets。省略で Smoke';

export function RegisterFluidQueryTools(server: McpServer, bus: EditorBus): void {
    /// @note ── 流体 (.fluid) ──
    /// @note 流体の見た目は数値からは予測できない (浮力を 2 倍にしても
    /// @note     「2 倍上がる」とは限らない)。AI が自分で絵を見て直す反復が回らないと、
    /// @note     レシピを書けても狙った煙にはならない。
    server.registerTool('fluid_schema', {
        description: '流体レシピ (.fluid) の編集可能フィールド目録 (型・範囲・enum)、部品の種類ごとの項目 (operators)、'
            + 'プリセット名、焼きモード (2d / 3d)、上限 (limits) を返します。'
            + '流体エフェクトを作る前に必ず最初に読んでください — fluid_set の fields はここに載っている名前と入れ子で書きます。'
            + 'レシピは «部品» の組み合わせです: 発生源 source (形 sphere / box / cone / ring / texture、最大16)、力 force '
            + '(wind / attract / repulse / vortex / noise / drag、最大8)、障害物 collider (sphere / box / plane、最大8)。'
            + 'operators.source.fields.cone のように、'
            + 'その種類で効く項目だけが種類ごとに載っています (他の項目も保存はされますが、その種類では効きません)。'
            + 'collider は流体が入り込めない形で、plane は center を通り direction を法線とする面 (裏側がすべて固体。壁・斜めの床)。'
            + '動く collider は流体を押しのけます。床は collider ではなく gas.floor / liquid.floor で別に持ちます。'
            + 'source の shape="texture" は画像の形に湧きます (文字・ロゴ・魔法陣): texture に projectRoot 相対の画像パスを入れ、'
            + '白く不透明なところほど強く注ぎます。Sprite 参照 (sprite_list の reference) を入れるとそのコマだけを切り抜きます。'
            + '部品の motion.key ({time, offset} の配列、最大8) で部品を時間で動かせます。'
            + '発生源ごとに色を変えるには render.use_albedo_ramp=true にし、render.albedo_ramp (4 点固定の {color: リニア RGB, position} 配列) に色を並べ、'
            + '各 source の color_key (0〜1) でどの色かを選びます。気体は色が煙に乗って運ばれて混ざり、液体は粒子ごとに色を持ちます。'
            + '3d 焼きでもループ (output.loop) と歪み (render.shading="distortion") が焼け、全プリセットが 3d で焼けます。'
            + 'bake.solver="gpu" は液体でも使えます (新しいが粒子が多いほど速い。怪しければ "cpu" で焼き比べる)。'
            + '作業の流れ: fluid_schema → fluid_create_effect (または fluid_create) → fluid_add_operator / fluid_set で部品を組む → fluid_preview → '
            + 'fluid_job_status で画像を実際に見る → fluid_set で調整 → プレビューを繰り返す → fluid_bake → '
            + 'fluid_job_status で done まで待つ → できた .vfx を prefab_instantiate などで配置。',
        inputSchema: {},
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, () => Safely(async () => TextResult(await bus.Query({ t: 'fluid.schema' }))));

    server.registerTool('fluid_get', {
        description: '.fluid レシピの現在値を全部返します。部品は recipe.source (発生源)・recipe.force (力)・recipe.collider (障害物) の配列で、'
            + 'ここでの並び順がそのまま fluid_set の "source.<添字>" / fluid_remove_operator の index になります。'
            + 'enum は添字の数値で返りますが、書くときはラベル文字列でも構いません。'
            + 'fluid_set で書く前に今の値を確かめる、プレビューの絵と数値を突き合わせる、といった用途に使います。',
        inputSchema: { path: FluidPathSchema.describe('projectRoot 相対の .fluid パス') },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ path }) => Safely(async () => TextResult(await bus.Query({ t: 'fluid.get', path }))));

    server.registerTool('fluid_job_status', {
        description: 'fluid_preview / fluid_bake / fluid_create_effect が返した job の進み具合を返します。'
            + 'state は queued → running → encoding → done / failed / cancelled。done / failed / cancelled になるまでポーリングしてください。'
            + 'プレビューが done なら画像も返るので、必ず絵を見てから次の fluid_set を決めてください。'
            + '焼きが done になると outputs (書いたテクスチャ等)・materialPath・vfxPath が埋まります。'
            + 'simulationSeconds / renderSeconds / outputSeconds と最も遅いフレーム、3D の色・MV・6-way 書き出し時間でボトルネックを調べられます。'
            + 'fingerprint は出た絵の指紋で、前回と同じなら 1 画素も変わっていません (効かない値をいじり続けるのを防げます)。'
            + 'solverUsed は実際に解いたソルバー ("gpu" / "cpu")、fallbackReason は GPU を頼んだのに CPU へ落ちた理由です '
            + '(同じレシピなのに絵が違うときはここを見てください。落としたくなければ bake.solver="gpu")。'
            + 'failed の理由は message にあります。終わったジョブは直近 32 件まで引けます。',
        inputSchema: {
            job: FluidJobIdSchema.describe('fluid_preview / fluid_bake / fluid_create_effect の応答の job'),
            includeImage: z.boolean().optional().describe('既定 true。false でプレビュー画像を省く (進捗だけ見たいとき)'),
        },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ job, includeImage }) => Safely(async () => {
        const raw = FluidJobStatusResultSchema.parse(await bus.Query({
            t: 'fluid.jobStatus', job, ...(includeImage === undefined ? {} : { includeImage }),
        }));
        /// @note base64 を text に残すと、同じ画像を文字列でも読ませて context を浪費する。
        const { image, ...status } = raw;
        const text = { type: 'text' as const, text: JSON.stringify(status, null, 2) };
        return {
            content: image === undefined
                ? [text]
                : [text, { type: 'image' as const, data: image.base64, mimeType: image.mimeType }],
            structuredContent: status,
        };
    }));
}


export function RegisterFluidCommandTools(server: McpServer, bus: EditorBus, permission: PermissionMode): void {
    const { run, writeAnnotations } = CreateCommandContext(bus, permission);

    /// @note ── 流体 (.fluid) ──
    server.registerTool('fluid_create', {
        description: 'プリセットから .fluid レシピを 1 つ作ります (テクスチャはまだ焼きません)。'
            + '既存ファイルは既定で拒否 (FLUID_EXISTS) します。'
            + 'レシピと .mat / .vfx をまとめて一度に作りたいなら fluid_create_effect のほうが手数が少なく済みます。'
            + '作った後は fluid_preview で見た目を確かめてから fluid_set で詰めます。',
        inputSchema: {
            path: FluidPathSchema.describe('作る .fluid の projectRoot 相対パス (例: Assets/VFX/Fluid/Smoke.fluid)'),
            preset: FluidPresetSchema.optional().describe(FluidPresetDescription),
            overwrite: z.boolean().optional().describe('既定 false。true で既存 .fluid を上書き'),
        },
        annotations: writeAnnotations,
    }, ({ path, preset, overwrite }) => run({
        t: 'fluid.create', path,
        ...(preset === undefined ? {} : { preset }),
        ...(overwrite === undefined ? {} : { overwrite }),
    }));

    server.registerTool('fluid_set', {
        description: '.fluid レシピの一部だけを書き換えます。fields は fluid_schema と同じ入れ子で渡します '
            + '(例: {"gas":{"buoyancy":2}} / {"bake":{"mode":"3d"}} / {"source.0.density":3} / {"force.1.type":"vortex"} / {"collider.0.shape":"plane"})。'
            + '部品は source (発生源)・force (力)・collider (障害物) の配列で、1 つだけ直すなら "source.0.xxx" のように添字で書きます。'
            + '配列ごと渡すと要素数がその長さになり (上限 source 16 / force 8 / collider 8 / motion.key 8 を超えた分は切り詰めて clamped に載ります)、'
            + '要素内で省いたキーは既存値のままです。部品を 1 つ足す・消す・並べ替えるだけなら fluid_add_operator / fluid_remove_operator / fluid_move_operator のほうが安全です。'
            + '部品を時間で動かすには "source.0.motion.key" に [{"time":0,"offset":[0,0,0]},{"time":1,"offset":[0.5,0,0]}] を time 昇順で渡します。'
            + 'enum (shape / type / shading / bake.mode など) はラベル文字列で書けます (大文字小文字は問いません)。'
            + 'shape="texture" の発生源の texture は projectRoot 相対の画像パス (例 "Assets/Textures/Logo.png") か、'
            + 'Sprite 参照 (sprite_list の reference。そのコマだけを切り抜きます) で、'
            + '画像がまだ無くても書き込みは通り、応答の warnings に "texture not found: ..." が載ります (projectRoot の外は BAD_PATH)。'
            + '存在しないコマを指した Sprite 参照は warnings に "sprite not found: ..." が載り、焼くと板の形で湧きます。'
            + '発生源ごとの色は {"render.use_albedo_ramp":true,"render.albedo_ramp":[{"color":[0.05,0.05,0.05],"position":0},'
            + '{"color":[0.3,0.3,0.3],"position":0.33},{"color":[0.6,0.35,0.1],"position":0.66},{"color":[0.9,0.5,0.1],"position":1}],'
            + '"source.1.color_key":1} のように書きます (albedo_ramp は 4 点固定・リニア RGB・position 昇順。1 点だけなら "render.albedo_ramp.2.color")。'
            + 'use_albedo_ramp が false の間は color_key は効きません。'
            + '応答の changed に実際に書いた項目が載ります。'
            + '焼き済みのテクスチャはここでは変わりません。見た目は fluid_preview で確かめ、確定したら fluid_bake で焼き直します。'
            + '立体 (ボリューム) で焼くには bake.mode を "3d" にしてから fluid_bake します (ループ・歪みも 3d で焼けます。'
            + 'bake.solver="gpu" は液体でも有効)。',
        inputSchema: {
            path: FluidPathSchema.describe('projectRoot 相対の .fluid パス'),
            fields: FluidFieldsSchema.describe('部分レシピ。キーと型は fluid_schema の fields に従う'),
        },
        annotations: writeAnnotations,
    }, ({ path, fields }) => run({ t: 'fluid.set', path, fields }));

    /// @note fluid_set で配列を渡すと «全要素を並べ直す» ことになり、
    /// @note     触るつもりのない部品の値を書き写し間違えると黙って変わる。1 つだけ足す・消すなら他は触らない。
    server.registerTool('fluid_add_operator', {
        description: '.fluid レシピに部品を 1 つ足します。list="source" は発生源 (気体は密度・温度・燃料を注ぎ、液体は粒子を撃ち出す)、'
            + 'list="force" は流れにかかる力、list="collider" は流体が入り込めない障害物 (動かせば流体を押しのける) です。'
            + 'type は source なら形 (sphere / box / cone / ring / texture)、force なら種類 (wind / attract / repulse / vortex / noise / drag)、'
            + 'collider なら形 (sphere / box / plane)。種類ごとに効く項目は fluid_schema の operators.<list>.fields.<type> にあります。'
            + 'fields で新しい部品の初期値を fluid_set と同じ書き方で渡せます (例: {"center":[0,-0.5,0],"size":[0.2,0.6,0.2]})。'
            + 'texture の発生源は画像の形に湧きます: {"texture":"Assets/Textures/Logo.png","direction":[0,0,1]} のように渡し、'
            + '白く不透明なところほど強く注ぎます (画像が無ければ warnings に載るだけで書き込みは通ります)。'
            + 'Sprite 参照 (sprite_list の reference) ならそのコマだけを切り抜きます。'
            + 'source の色は fields の color_key (0〜1) で render.albedo_ramp のどの色かを選びます (render.use_albedo_ramp=true のときだけ効く。'
            + '例: 煙の中に火の粉色の発生源を 1 つ足すなら {"color_key":1})。'
            + '上限は source 16 / force 8 / collider 8 で、満杯なら OPERATOR_LIMIT。応答は {path, list, index, count, changed, warnings?}。Undo で戻せます。',
        inputSchema: {
            path: FluidPathSchema.describe('projectRoot 相対の .fluid パス'),
            list: FluidOperatorListSchema.describe('"source" (発生源) / "force" (力) / "collider" (障害物)'),
            type: FluidOperatorTypeSchema.optional().describe('形 / 力の種類のラベル。省略で既定 (sphere / wind / sphere)'),
            index: FluidOperatorIndexSchema.optional().describe('差し込む位置。省略で末尾'),
            fields: FluidFieldsSchema.optional().describe('新しい部品の初期値 (部品 1 つぶんの部分指定)'),
        },
        annotations: writeAnnotations,
    }, ({ path, list, type, index, fields }) => run({
        t: 'fluid.addOperator', path, list,
        ...(type === undefined ? {} : { type }),
        ...(index === undefined ? {} : { index }),
        ...(fields === undefined ? {} : { fields }),
    }));

    server.registerTool('fluid_remove_operator', {
        description: '.fluid レシピの部品を 1 つ消します。後ろの部品の添字は 1 つずつ詰まります。'
            + '範囲外の index は BAD_ARG。応答は {path, list, removed, count}。Undo で戻せます。',
        inputSchema: {
            path: FluidPathSchema.describe('projectRoot 相対の .fluid パス'),
            list: FluidOperatorListSchema.describe('"source" (発生源) / "force" (力) / "collider" (障害物)'),
            index: FluidOperatorIndexSchema.describe('消す部品の添字 (fluid_get の recipe の配列順)'),
        },
        annotations: writeAnnotations,
    }, ({ path, list, index }) => run({ t: 'fluid.removeOperator', path, list, index }));

    server.registerTool('fluid_move_operator', {
        description: '.fluid レシピの部品の並び順を変えます (from の部品を取り出して to の位置へ差し込む)。'
            + '発生源は並び順に注ぐため、重なった発生源の見え方が変わることがあります。応答は {path, list, from, to}。Undo で戻せます。',
        inputSchema: {
            path: FluidPathSchema.describe('projectRoot 相対の .fluid パス'),
            list: FluidOperatorListSchema.describe('"source" (発生源) / "force" (力) / "collider" (障害物)'),
            from: FluidOperatorIndexSchema.describe('動かす部品の今の添字'),
            to: FluidOperatorIndexSchema.describe('動かした後の添字'),
        },
        annotations: writeAnnotations,
    }, ({ path, list, from, to }) => run({ t: 'fluid.moveOperator', path, list, from, to }));

    server.registerTool('fluid_preview', {
        description: '.fluid をその場でシミュレーションし、焼いたときの «そのコマ» を画像にするジョブを始めます。アセットは書きません。'
            + '応答の job を fluid_job_status でポーリングし、done になったら返る画像を必ず見てください。'
            + '**プレビューは焼きと同じ経路で解くので、ここで見た絵は fluid_bake で出るコマと 1 画素まで同じです。**'
            + 'frame は焼きの何コマ目か (0 = 最初)。contactSheet=true なら全コマを焼いたアトラスの並びで 1 枚にするので、'
            + '時間方向の当たり外れを 1 回で見られます (立ち上がり・ピーク・消え際を別々に頼む必要がありません)。'
            + 'variants=N は seed を 1 つずつずらした N 通りを並べます — 気に入った 1 枚の seed を fluid_set で seed に書けば、'
            + 'その絵が何度でも同じに焼けます (乱数の振り直しではなく seed の選択で «ばらつき» を扱ってください)。'
            + '応答の fingerprint が前回と同じなら、絵は 1 画素も変わっていません (変えた値が効いていない、の判定に使えます)。'
            + '同じ .fluid を焼いている最中は FLUID_BUSY で拒否されます。'
            + '3d (bake.mode="3d") では contactSheet / variants / seed は使えません。',
        inputSchema: {
            path: FluidPathSchema.describe('projectRoot 相対の .fluid パス'),
            frame: z.number().int().min(0).max(1023).optional().describe('焼きの何コマ目か。既定 0'),
            time: z.number().finite().min(0).max(600).optional()
                .describe('warmup 後の秒。frame を省いたときだけ使い、一番近いコマへ吸着します'),
            size: z.number().int().min(32).max(2048).optional()
                .describe('出力 PNG の 1 辺 [px]。既定 256。contactSheet ではシート全体の 1 辺'),
            contactSheet: z.boolean().optional().describe('全コマ (variants>1 なら seed 違い) を 1 枚に並べる'),
            variants: z.number().int().min(1).max(16).optional().describe('seed を 1 ずつずらした試しの本数。既定 1'),
            seed: z.number().int().min(0).max(4294967295).optional()
                .describe('0 以外でこの seed で解く (.fluid は書き換えません)'),
        },
        annotations: writeAnnotations,
    }, ({ path, frame, time, size, contactSheet, variants, seed }) => run({
        t: 'fluid.preview', path,
        ...(frame === undefined ? {} : { frame }),
        ...(time === undefined ? {} : { time }),
        ...(size === undefined ? {} : { size }),
        ...(contactSheet === undefined ? {} : { contactSheet }),
        ...(variants === undefined ? {} : { variants }),
        ...(seed === undefined ? {} : { seed }),
    }));

    server.registerTool('fluid_bake', {
        description: '.fluid をフリップブックテクスチャへ焼くジョブを始めます。数秒〜数十秒かかるため、すぐ job を返します'
            + '(ここで待つと Editor のメインスレッドごと止まるため)。fluid_job_status で done / failed になるまでポーリングしてください。'
            + '.fluid の隣に同名の .mat (Smoke.fluid → Smoke.mat) を必ず作る / 焼き結果へ追従させます。'
            + 'updateMaterial は旧クライアントとの互換用に受け取りますが、値にかかわらず .mat を更新します。'
            + '焼きモードはレシピの bake.mode で決まり、立体で焼くなら先に fluid_set で {"bake":{"mode":"3d"}} にします。'
            + '**焼いた出力 (テクスチャ・.mat) は Undo で消えません** (同名の既存アセットを壊しうるため)。'
            + '出力は必ず同じ名前へ上書きされるので、焼き直しても .mat の指す先はずれません。'
            + '応答 (fluid_job_status) の fingerprint で «前回と同じ絵か» を、solverUsed / fallbackReason で '
            + '«GPU で解けたか、CPU へ落ちたか» を確認できます。'
            + '見た目が固まるまでは fluid_preview で反復し、焼くのは最後にしてください。',
        inputSchema: {
            path: FluidPathSchema.describe('projectRoot 相対の .fluid パス'),
            updateMaterial: z.boolean().optional().describe('互換用。指定しても値にかかわらず隣の .mat を作る / 更新する'),
            seed: z.number().int().min(0).max(4294967295).optional()
                .describe('0 以外でこの seed で焼く (.fluid は書き換えません。2d のみ)'),
        },
        annotations: writeAnnotations,
    }, ({ path, seed }) => run({
        t: 'fluid.bake', path,
        ...(seed === undefined ? {} : { seed }),
    }));

    server.registerTool('fluid_cancel', {
        description: '待機中 / 実行中の流体ジョブ (プレビュー・焼き) を止めます。'
            + '終わっているか見つからなければ cancelled=false。止めても途中まで書いた出力は巻き戻りません。',
        inputSchema: { job: FluidJobIdSchema },
        annotations: writeAnnotations,
    }, ({ job }) => run({ t: 'fluid.cancel', job }));

    server.registerTool('fluid_create_effect', {
        description: '流体エフェクトを 1 回で組み立てます: <dir>/<name>.fluid を作り、fields を当て、'
            + 'bake=true (既定) なら焼いて同名の <name>.mat と、それを貼った 1 層の <name>.vfx まで書きます。'
            + '焼きは非同期なので応答の job を fluid_job_status で done までポーリングし、'
            + 'できた vfxPath を prefab_instantiate などでシーンへ置きます。'
            + '見た目を詰めたいときは bake=false で作り、fluid_add_operator (発生源 source / 力 force / 障害物 collider を足す) と fluid_set で部品を組み、'
            + 'fluid_preview → fluid_set を繰り返してから fluid_bake してください (焼いた出力は Undo で消えません)。'
            + 'fields で部品を渡すなら {"source":[{"shape":"cone","direction":[0,1,0]}],"force":[{"type":"vortex"}],'
            + '"collider":[{"shape":"sphere","center":[0,0.2,0],"size":[0.2,0.2,0.2]}]} のように配列で書き '
            + '(配列はプリセットの部品を丸ごと置き換えます。上限 source 16 / force 8 / collider 8)、enum はラベル文字列で構いません。'
            + 'collider は流体をよける障害物 (plane は壁・斜めの床。床そのものは gas.floor / liquid.floor)、'
            + 'source の shape="texture" は texture (projectRoot 相対の画像、または sprite_list の reference で 1 コマ) の形に湧き、'
            + '白く不透明なところほど強く注ぎます (画像が無ければ warnings に載ります)。'
            + '発生源ごとに色を分けるなら fields に "render":{"use_albedo_ramp":true,"albedo_ramp":[4 点の {color, position}]} を入れ、'
            + '各 source に color_key (0〜1) を持たせます (煙は混ざると色も混ざり、液体は粒子ごとの色)。'
            + '立体で焼くなら "bake":{"mode":"3d"} (ループ・歪みも焼け、液体も bake.solver="gpu" で解けます)。',
        inputSchema: {
            name: FluidEffectNameSchema.describe('ファイル名の素 (拡張子なし)。例: "CampfireSmoke"'),
            dir: z.string().min(1).max(1024).optional().describe('出力先ディレクトリ (projectRoot 相対)。既定 Assets/VFX/Fluid'),
            preset: FluidPresetSchema.optional().describe(FluidPresetDescription),
            fields: FluidFieldsSchema.optional().describe('プリセットの上に当てる部分レシピ (fluid_set と同じ形)'),
            bake: z.boolean().optional().describe('既定 true。false でレシピだけ作り、焼き・.mat・.vfx は後回し'),
        },
        annotations: writeAnnotations,
    }, ({ name, dir, preset, fields, bake }) => run({
        t: 'fluid.createEffect', name,
        ...(dir === undefined ? {} : { dir }),
        ...(preset === undefined ? {} : { preset }),
        ...(fields === undefined ? {} : { fields }),
        ...(bake === undefined ? {} : { bake }),
    }));
}
