/// @file    WorldTools.ts
/// @brief   地形・NavMesh・環境・Audio・UI の MCP 操作。
/// @author  Hasegawa Jin
/// @date    2026-11-06
import * as z from 'zod/v4';
import type { McpServer } from '@modelcontextprotocol/sdk/server/mcp.js';
import type { EditorBus } from '../busClient.js';
import type { PermissionMode } from '../config.js';
import { NodeIdSchema, TerrainSculptOpSchema, Vec3Schema } from '../editorContracts.js';
import { TextResult, Safely, CreateCommandContext } from './ToolSupport.js';

export function RegisterWorldQueryTools(server: McpServer, bus: EditorBus): void {
    server.registerTool('terrain_inspect', {
        description: '地形のグリッド解像度・セルサイズ・最大高さ・層 (可変個) のマテリアル割当と、高さ/スプラットの統計を返します。'
            + 'layers は層数ぶん、stats.layerCoverage は層ごとの平均重み [0,1]、holeCount は穴セル数です。'
            + '生の heightData (65x65 なら 4225 個) は返しません — 特定地点の実値は terrain_sample で点指定して読みます。'
            + 'stats.flat=true は「まだ一度も彫られていない平面」を意味します。',
        inputSchema: { id: NodeIdSchema.optional().describe('省略で全 Terrain') },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ id }) => Safely(async () => TextResult(await bus.Query({
        t: 'terrain.inspect', ...(id === undefined ? {} : { id }),
    }))));

    server.registerTool('terrain_sample', {
        description: '指定ワールド座標の地形高さ・法線・斜度・層の重み・穴かどうかを返します。'
            + 'layerWeights は最寄り頂点の [{layer, weight}] (重みを持つ上位 4 層、重みの降順)、hole は穴セル上なら true です。'
            + 'points は [x,y,z] でも [x,z] でも構いません (高さを問う用途で y は使いません)。'
            + 'terrain_sculpt の前後で同じ点を測れば、狙った量だけ動いたかを画像ではなく数値で確認できます。'
            + 'slopeDegrees は NavMesh が歩行可能と判定するかに直結します。',
        inputSchema: {
            points: z.array(z.array(z.number().finite()).min(2).max(3)).min(1).max(256),
            id: NodeIdSchema.optional().describe('特定 Terrain に限定する場合'),
        },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ points, id }) => Safely(async () => TextResult(await bus.Query({
        t: 'terrain.sample', points, ...(id === undefined ? {} : { id }),
    }))));

    server.registerTool('navmesh_get_state', {
        description: 'NavMesh Surface のベイク設定・状態・ポリゴン数・歩行可能範囲(bounds)と、Agent の実行状態を返します。'
            + '「敵が来ない」の切り分けはここから始めます — Surface と Agent の agentTypeId が食い違っていれば経路は絶対に引けません。'
            + 'bakeState=done かつ polygonCount>0 でなければ navmesh_find_path は失敗します。'
            + 'stale=true はベイク後に地形か Modifier か設定が変わった状態で、経路は引けても現状と合っていません (navmesh_bake が必要)。'
            + 'polygonCount=0 のときは failReason に、bake.{tooSteep,tooHighStep,obstructed,eroded} にはセル判定の内訳が入るので、'
            + 'Max Slope / Max Climb / Agent Radius / NotWalkable Modifier のどれで落ちたのかを総当たりせずに特定できます。',
        inputSchema: { id: NodeIdSchema.optional() },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ id }) => Safely(async () => TextResult(await bus.Query({
        t: 'navmesh.state', ...(id === undefined ? {} : { id }),
    }))));

    server.registerTool('navmesh_find_path', {
        description: '2点間の経路を、NavMeshAgent が実行時に使うのと同じ A* + Funnel で引きます。'
            + '「そこへ歩けるのか」を Play せずに確かめる唯一の手段です — Play して眺めても「行かない」ことしか観測できず、'
            + '行けないのか行こうとしないのかは区別できません (後者は bt_runtime_state で見ます)。'
            + 'from/to は座標でも NodeId (fromId/toId) でも指定できます。fromId が Agent なら agentTypeId と areaMask をその Agent から引き継ぎます。'
            + 'found=false の reason は START_OR_GOAL_OFF_NAVMESH (点が面の外) か NO_PATH (到達不能または areaMask で遮断)。'
            + 'areaMask で遮断されているかは areaMask=-1 で引き直せば切り分けられます。'
            + 'detourRatio が大きいほど遠回り = 障害物か穴を迂回しています。',
        inputSchema: {
            from: Vec3Schema.optional(),
            to: Vec3Schema.optional(),
            fromId: NodeIdSchema.optional(),
            toId: NodeIdSchema.optional(),
            surfaceId: NodeIdSchema.optional().describe('省略で agentTypeId が一致するベイク済み Surface を選ぶ'),
            agentTypeId: z.number().int().min(0).max(31).optional(),
            areaMask: z.number().int().optional().describe('-1 = 全エリア通過可'),
        },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ from, to, fromId, toId, surfaceId, agentTypeId, areaMask }) => Safely(async () => TextResult(await bus.Query({
        t: 'navmesh.path',
        ...(from === undefined ? {} : { from }),
        ...(to === undefined ? {} : { to }),
        ...(fromId === undefined ? {} : { fromId }),
        ...(toId === undefined ? {} : { toId }),
        ...(surfaceId === undefined ? {} : { surfaceId }),
        ...(agentTypeId === undefined ? {} : { agentTypeId }),
        ...(areaMask === undefined ? {} : { areaMask }),
    }))));

    server.registerTool('navmesh_sample', {
        description: '指定点が NavMesh の上かを判定し、面上なら高さ、面外なら最近傍ポリゴンへ寄せた座標を返します。'
            + '敵の湧き位置やパトロール地点を決めるときに使います — 面の外に置いた Agent は最初の1歩で瞬間移動します。',
        inputSchema: {
            points: z.array(z.array(z.number().finite()).length(3)).min(1).max(256),
            surfaceId: NodeIdSchema.optional(),
            agentTypeId: z.number().int().min(0).max(31).optional(),
        },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ points, surfaceId, agentTypeId }) => Safely(async () => TextResult(await bus.Query({
        t: 'navmesh.sample', points,
        ...(surfaceId === undefined ? {} : { surfaceId }),
        ...(agentTypeId === undefined ? {} : { agentTypeId }),
    }))));

    server.registerTool('environment_inspect', {
        description: '空・太陽/月・大気散乱・環境光(IBL)・ボリューメトリック雲・ポストプロセスなど環境系コンポーネントと、'
            + '全ライトの設定を 1 回で返します。「なぜこの画がこの明るさなのか」を調べる入口です。'
            + '返る fields は editor_catalog のフィールド定義と 1 対 1 なので、component_set でそのまま書き戻せます。'
            + '対象は ComponentRegistry の Environment カテゴリ全体なので、環境コンポーネントが増えても自動で含まれます。',
        inputSchema: {},
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, () => Safely(async () => TextResult(await bus.Query({ t: 'environment.inspect' }))));

    server.registerTool('audio_inspect', {
        description: 'AudioSource の設定 (clipPath / volume / spatialBlend / 距離減衰 / busName) と再生状態、'
            + 'AudioListener の一覧、ミキサーバスの構成を返します。'
            + 'runtime.playing は保存対象ではないため component 照会には出ません — 鳴っているかはここでしか確認できません。'
            + 'AudioListener が 0 件なら 3D 音の距離減衰は効きません (warning に出ます)。'
            + 'busName に指定できるのは buses[].name にある名前だけで、未知の名前は黙って Master へ落ちます。'
            + 'AudioReverbZone の残響が乗るのは buses[].reverb が true のバスへ出している音だけです。'
            + 'activeVoices が voiceLimit に張り付いていると、AudioSource の priority が低い音から畳まれます。'
            + '手続き効果音 (.synth) の作成・調整は sfx.* Operator (editor_op_invoke / editor_op_query) 側です。',
        inputSchema: { id: NodeIdSchema.optional() },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ id }) => Safely(async () => TextResult(await bus.Query({
        t: 'audio.inspect', ...(id === undefined ? {} : { id }),
    }))));

    server.registerTool('ui_inspect', {
        description: 'UI Canvas を根とする UI ツリーを、各要素のコンポーネント値ごと返します。'
            + 'UI の位置は Transform ではなく矩形指定で決まるため、scene_get_tree の階層だけでは画面のどこに出るか分かりません。'
            + 'gameViewport のサイズも返るので、viewport_capture(view="game") の絵と座標を突き合わせられます。',
        inputSchema: { id: NodeIdSchema.optional().describe('省略で全 Canvas') },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ id }) => Safely(async () => TextResult(await bus.Query({
        t: 'ui.inspect', ...(id === undefined ? {} : { id }),
    }))));
}


export function RegisterWorldCommandTools(server: McpServer, bus: EditorBus, permission: PermissionMode): void {
    const { run, writeAnnotations } = CreateCommandContext(bus, permission);

    server.registerTool('terrain_sculpt', {
        description: '地形の高さをブラシで彫ります。人が使う Terrain Tool と同じブラシカーネルを通るので、'
            + '同じ radius / strength を指定すれば手作業と同じ結果になります。'
            + 'position はワールド座標で、ブラシ範囲に重なる全 Terrain へ同時に効きます (境界に段差を残しません)。'
            + 'iterations は「マウスを押し続けた回数」に相当します — smooth と flatten は 1 回では収束しないため、'
            + '10〜30 程度を指定してください (同じ要求を何度も投げると Undo 履歴が汚れます)。'
            + 'flatten の targetHeight を省略するとブラシ中心の現在高さが基準になります (対話ツールの「最初にクリックした高さ」と同じ意味)。'
            + 'noise は fBm ノイズの加算 (noiseScale / noiseOctaves / seed)、thermalErosion は安息角 talus [度] を超えた斜面を崩し、'
            + 'hydraulicErosion は水滴 droplets 個で削って低所へ運び (seed と反復番号で決定的)、terrace は terraceStep [m] の段に寄せます。'
            + '彫った後は NavMesh が古い形のままなので、必ず navmesh_bake を実行してください。'
            + '結果は terrain_sample で数値確認できます。操作全体が 1 回の Undo で戻ります。',
        inputSchema: {
            position: Vec3Schema.describe('ブラシ中心 (ワールド座標)'),
            op: TerrainSculptOpSchema.default('raise'),
            radius: z.number().finite().gt(0).max(500).default(5).describe('ブラシ半径 [m]'),
            strength: z.number().finite().gt(0).max(1).default(0.05).describe('1 回あたりの最大変化量'),
            falloff: z.enum(['linear', 'smooth', 'gaussian']).default('smooth'),
            iterations: z.number().int().min(1).max(64).default(1).describe('ブラシを重ねる回数'),
            targetHeight: z.number().finite().optional().describe('flatten の基準高さ (ワールド Y)'),
            id: NodeIdSchema.optional().describe('特定 Terrain だけに限定する場合'),
            noiseScale: z.number().finite().gt(0).max(1000).optional().describe('noise: 最も粗いオクターブの周期 [m] (既定 8)'),
            noiseOctaves: z.number().int().min(1).max(8).optional().describe('noise: オクターブ数 (既定 4)'),
            seed: z.number().int().min(0).max(4294967295).optional().describe('noise / hydraulicErosion の乱数の種 (既定 1)'),
            terraceStep: z.number().finite().gt(0).max(1000).optional().describe('terrace: 段の高さ [m] (既定 2)'),
            terraceSharpness: z.number().finite().min(0).max(1).optional().describe('terrace: 0 = 変化なし、1 = 垂直な段 (既定 0.5)'),
            talus: z.number().finite().gt(0).lt(90).optional().describe('thermalErosion: 安息角 [度] (既定 35)'),
            droplets: z.number().int().min(1).max(4096).optional().describe('hydraulicErosion: 1 回に落とす水滴数 (既定 48)'),
        },
        annotations: writeAnnotations,
    }, ({ position, op, radius, strength, falloff, iterations, targetHeight, id,
        noiseScale, noiseOctaves, seed, terraceStep, terraceSharpness, talus, droplets }) => run({
            t: 'terrain.sculpt', position, op, radius, strength, falloff, iterations,
            ...(targetHeight === undefined ? {} : { targetHeight }),
            ...(id === undefined ? {} : { id }),
            ...(noiseScale === undefined ? {} : { noiseScale }),
            ...(noiseOctaves === undefined ? {} : { noiseOctaves }),
            ...(seed === undefined ? {} : { seed }),
            ...(terraceStep === undefined ? {} : { terraceStep }),
            ...(terraceSharpness === undefined ? {} : { terraceSharpness }),
            ...(talus === undefined ? {} : { talus }),
            ...(droplets === undefined ? {} : { droplets }),
        }));

    server.registerTool('terrain_paint', {
        description: '地形のスプラット (頂点ごとに上位 4 層の番号と重み) をブラシで塗ります。'
            + '重みの整数和は常に 255 に保たれるので、ある層を増やすと他が比率を保ったまま減り、5 層目以下は捨てられます。'
            + 'layer は 0〜layerCount-1 (layerCount は terrain_inspect で確認)。範囲外はエラーです。'
            + '塗る前に terrain_inspect で layers[].material を確認してください — 空のレイヤーを塗っても見た目は変わりません'
            + '(その場合は terrain_set_layer_material で .mat を割り当てます)。',
        inputSchema: {
            position: Vec3Schema.describe('ブラシ中心 (ワールド座標)'),
            layer: z.number().int().min(0).max(254).describe('塗る層 index (0〜layerCount-1)'),
            radius: z.number().finite().gt(0).max(500).default(5),
            strength: z.number().finite().gt(0).max(1).default(0.5),
            falloff: z.enum(['linear', 'smooth', 'gaussian']).default('smooth'),
            iterations: z.number().int().min(1).max(64).default(1),
            id: NodeIdSchema.optional(),
        },
        annotations: writeAnnotations,
    }, ({ position, layer, radius, strength, falloff, iterations, id }) => run({
        t: 'terrain.paint', position, layer, radius, strength, falloff, iterations,
        ...(id === undefined ? {} : { id }),
    }));

    server.registerTool('terrain_set_layer_material', {
        description: 'Terrain の層へ .mat を割り当てます (空文字でクリア)。layer == layerCount なら末尾に層を追加します'
            + ' (Undo で追加した層が消えます)。'
            + 'レイヤーが空のまま terrain_paint しても見た目が変わらないため、塗る前にここで中身を決めます。',
        inputSchema: {
            id: NodeIdSchema,
            layer: z.number().int().min(0).max(254).describe('0〜layerCount (layerCount で追加)'),
            material: z.string().max(512).describe('projectRoot 相対の .mat パス (空文字でクリア)'),
        },
        annotations: writeAnnotations,
    }, ({ id, layer, material }) => run({ t: 'terrain.setLayerMaterial', id, layer, material }));

    server.registerTool('terrain_ramp', {
        description: '始点から終点へ一定勾配で傾く坂を作ります。start / end はワールド座標で、y が坂の両端の高さです。'
            + '線分から radius 以内をフォールオフ付きで坂の高さへ寄せ、strength=1 で線分上は坂に一致します。'
            + '1 回で完結するので iterations はありません。重なる全 Terrain へ同時に効き、1 回の Undo で戻ります。'
            + '作った後は navmesh_bake を実行し、terrain_sample で両端と中点の高さを確認してください。',
        inputSchema: {
            start: Vec3Schema.describe('坂の始点 (ワールド座標、y が高さ)'),
            end: Vec3Schema.describe('坂の終点 (ワールド座標、y が高さ)'),
            radius: z.number().finite().gt(0).max(500).default(3).describe('坂の半幅 [m]'),
            strength: z.number().finite().gt(0).max(1).default(1),
            falloff: z.enum(['linear', 'smooth', 'gaussian']).default('smooth'),
            id: NodeIdSchema.optional().describe('特定 Terrain だけに限定する場合'),
        },
        annotations: writeAnnotations,
    }, ({ start, end, radius, strength, falloff, id }) => run({
        t: 'terrain.ramp', start, end, radius, strength, falloff,
        ...(id === undefined ? {} : { id }),
    }));

    server.registerTool('terrain_hole', {
        description: '地形に穴を開けます (erase=true で塞ぎます)。セル中心がブラシ円に入るセルが対象で、'
            + '穴のセルは描画・物理・NavMesh のいずれにも三角形を作りません (洞窟の入口など)。'
            + '変化するセルが無いと NO_CHANGE エラーです。開けた後は navmesh_bake を実行してください。'
            + '確認は terrain_sample の hole と terrain_inspect の holeCount で行います。1 回の Undo で戻ります。',
        inputSchema: {
            position: Vec3Schema.describe('ブラシ中心 (ワールド座標)'),
            radius: z.number().finite().gt(0).max(500).default(2).describe('ブラシ半径 [m]'),
            erase: z.boolean().default(false).describe('true で穴を塞ぐ'),
            id: NodeIdSchema.optional().describe('特定 Terrain だけに限定する場合'),
        },
        annotations: writeAnnotations,
    }, ({ position, radius, erase, id }) => run({
        t: 'terrain.hole', position, radius, erase,
        ...(id === undefined ? {} : { id }),
    }));

    server.registerTool('navmesh_bake', {
        description: 'NavMesh Surface の再ベイクを要求します。id 省略で有効な全 Surface が対象です。'
            + 'terrain_sculpt やコライダーの追加/削除の後は必ず実行してください — 古い NavMesh のまま経路を引くと'
            + '「壁を通り抜ける経路」が返り、その経路は実行時にも使われます。'
            + 'ベイクはバックグラウンドスレッドで走るので即座には完了しません。navmesh_get_state で bakeState=done を確認してください。',
        inputSchema: { id: NodeIdSchema.optional() },
        annotations: writeAnnotations,
    }, ({ id }) => run({ t: 'navmesh.bake', ...(id === undefined ? {} : { id }) }));

    server.registerTool('audio_control', {
        description: 'AudioSource を再生 / 停止 / 一時停止 / 再開します。実行は次フレームの AudioSystem で、'
            + '結果は audio_inspect の runtime.playing で確認します。Undo は逆方向の操作になります。',
        inputSchema: { id: NodeIdSchema, action: z.enum(['play', 'stop', 'pause', 'resume']) },
        annotations: writeAnnotations,
    }, ({ id, action }) => run({ t: 'audio.control', id, action }));
}
