/// @file    AnimationTools.ts
/// @brief   Animator・Layer・AvatarMask の MCP 操作。
/// @author  Hasegawa Jin
/// @date    2026-11-06
import * as z from 'zod/v4';
import { NodeIdSchema } from '../editorContracts.js';
import { NameSchema, TextResult, Safely, CreateCommandContext } from './ToolSupport.js';
/// @note ステート/遷移/Motion 編集の対象グラフを選ぶ。省略で Base Layer。
/// @note 上半身レイヤーに独自の遷移グラフを組むには、既存の編集ツールが
/// @note      「どのレイヤーのグラフか」を受け取れる必要がある。
const LayerOptionSchema = z.string().min(1).max(128).optional()
    .describe('対象レイヤー名。省略でBase Layer。animation_add_layerで作ったレイヤーの独自ステートマシンを編集する場合に指定');
export function RegisterAnimationQueryTools(server, bus) {
    server.registerTool('animation_get_state', {
        description: 'Animatorの再生/停止、clip、秒・正規化時間、遷移状態、ロード済みclip一覧を取得します。',
        inputSchema: { id: NodeIdSchema },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ id }) => Safely(async () => TextResult(await bus.Query({ t: 'animation.state', id }))));
    server.registerTool('animation_get_graph', {
        description: 'Animatorステートマシンの構造(states/transitions/parameters・BlendTree駆動パラメーター・anyState遷移・遷移条件)を、パラメーターのライブ値と現在ステート付きで取得します。layersにレイヤー一覧(weight/mode/maskPath/現在ステート/Slot状態)とbaseLayerMaskPathも常に含まれるため、上半身レイヤーの構成確認もこれ1本で行えます。layer引数でそのレイヤー独自のステートマシンを覗けます。',
        inputSchema: {
            id: NodeIdSchema,
            layer: z.string().min(1).max(128).optional()
                .describe('レイヤー名。指定でそのレイヤー独自のステートマシンを返す。省略でBase Layer'),
        },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ id, layer }) => Safely(async () => TextResult(await bus.Query({
        t: 'animation.graph', id, ...(layer === undefined ? {} : { layer }),
    }))));
    server.registerTool('animation_get_blend_tree', {
        description: '指定ステートのBlendTree構成を掘り下げ、駆動パラメーターとそのライブ値、各Motionのsource/clip/threshold(1D)またはposX/posY(2D)/speed/IK、直近フレームのランタイムWeightを返します。Clipステートやステート不在はエラーになります。',
        inputSchema: {
            id: NodeIdSchema,
            state: z.string().min(1).max(128).describe('animation_get_graph が返す BlendTree ステート名'),
        },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ id, state }) => Safely(async () => TextResult(await bus.Query({ t: 'animation.blendTree', id, state }))));
    server.registerTool('animation_get_pose', {
        description: '直近フレームで評価済みのSkeleton poseを、ノード名・親index・bone index・4x4 global matrixで取得します。',
        inputSchema: { id: NodeIdSchema, limit: z.number().int().min(1).max(512).default(128) },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ id, limit }) => Safely(async () => TextResult(await bus.Query({ t: 'animation.pose', id, limit }))));
}
export function RegisterAnimationCommandTools(server, bus, permission) {
    /// @note BlendTree ステートを追加する際の駆動パラメーターと座標系スキーマ (add/set 共通)。
    const StateModeSchema = z.enum(['clip', 'blendTree1D', 'blendTree2D']);
    /// @note ── Animator レイヤー / Slot ────────────────────────────────────────────
    /// @note 上半身だけ・下半身だけといった部分制御はレイヤーが入口になる。
    /// @note レイヤーを作る → マスクを割り当てる → そのレイヤーにステートを足す、の順で使う。
    const LayerNameSchema = z.string().min(1).max(128);
    const { dryRun, run, writeAnnotations } = CreateCommandContext(bus, permission);
    server.registerTool('animation_control', {
        description: 'Animatorのクリップ/ステート再生、一時停止、停止、秒またはフレーム位置へのシークを行います。',
        inputSchema: {
            id: NodeIdSchema,
            action: z.enum(['play', 'pause', 'stop', 'seek']),
            clipName: z.string().min(1).max(128).optional(),
            clipIndex: z.number().int().nonnegative().optional(),
            state: z.string().min(1).max(128).optional(),
            time: z.number().finite().nonnegative().optional(),
            frame: z.number().finite().nonnegative().optional(),
        },
        annotations: writeAnnotations,
    }, ({ id, action, clipName, clipIndex, state, time, frame }) => run({
        t: 'animation.control', id, action,
        ...(clipName === undefined ? {} : { clipName }),
        ...(clipIndex === undefined ? {} : { clipIndex }),
        ...(state === undefined ? {} : { state }),
        ...(time === undefined ? {} : { time }),
        ...(frame === undefined ? {} : { frame }),
    }));
    server.registerTool('animation_set_parameter', {
        description: 'Animatorパラメーターを名前で設定し、遷移やBlendTreeを駆動します。value型は宣言型に従います(float/int=数値、bool=真偽、trigger=真偽で発火/リセット、省略時は発火)。animation_get_graphで名前と型を確認してください。',
        inputSchema: {
            id: NodeIdSchema,
            name: z.string().min(1).max(128).describe('animation_get_graph が返すパラメーター名'),
            value: z.union([z.number().finite(), z.boolean()]).optional()
                .describe('float/int は数値、bool/trigger は真偽。trigger は省略で発火'),
        },
        annotations: writeAnnotations,
    }, ({ id, name, value }) => run({
        t: 'animation.setParameter', id, name,
        ...(value === undefined ? {} : { value }),
    }));
    server.registerTool('animation_add_transition', {
        description: dryRun
            ? 'Animator遷移追加の入力を検証します。'
            : 'Animatorステート間(またはAny State→ステート)にUndo可能な遷移を追加します。from省略でAny State遷移。遷移先が既にある場合はエラー。追加後はanimation_set_conditionで条件を付けます。',
        inputSchema: {
            id: NodeIdSchema,
            layer: LayerOptionSchema,
            from: z.string().min(1).max(128).optional().describe('遷移元ステート名。省略で Any State 遷移'),
            to: z.string().min(1).max(128).describe('遷移先ステート名'),
            hasExitTime: z.boolean().optional(),
            exitTime: z.number().finite().min(0).max(1).optional().describe('0..1 正規化再生位置'),
            fixedDuration: z.boolean().optional().describe('true=秒、false=遷移元Length比'),
            transitionDuration: z.number().finite().min(0).max(60).optional(),
        },
        annotations: writeAnnotations,
    }, ({ id, from, to, hasExitTime, exitTime, fixedDuration, transitionDuration, layer }) => run({
        t: 'animation.addTransition', id, ...(layer === undefined ? {} : { layer }), to,
        ...(from === undefined ? {} : { from }),
        ...(hasExitTime === undefined ? {} : { hasExitTime }),
        ...(exitTime === undefined ? {} : { exitTime }),
        ...(fixedDuration === undefined ? {} : { fixedDuration }),
        ...(transitionDuration === undefined ? {} : { transitionDuration }),
    }));
    server.registerTool('animation_set_condition', {
        description: dryRun
            ? 'Animator遷移条件の編集入力を検証します。'
            : '指定遷移の発火条件をUndo可能に編集します。action=add/update/remove/clear。add/updateはparameterとopが必要(true/false以外はthresholdも)、update/removeはconditionIndexが必要。transitionIndexはanimation_get_graphのstates[].transitionsまたはanyStateTransitionsの並び順です。',
        inputSchema: {
            id: NodeIdSchema,
            layer: LayerOptionSchema,
            from: z.string().min(1).max(128).optional().describe('遷移元ステート名。省略で Any State 遷移'),
            transitionIndex: z.number().int().nonnegative().describe('対象ソースの遷移リスト内index'),
            action: z.enum(['add', 'update', 'remove', 'clear']),
            parameter: z.string().min(1).max(128).optional().describe('add/update で必須。既存パラメーター名'),
            op: z.enum(['greater', 'less', 'equal', 'notEqual', 'true', 'false']).optional().describe('add/update で必須'),
            threshold: z.number().finite().optional().describe('Float/Int比較の閾値。true/falseでは無視'),
            conditionIndex: z.number().int().nonnegative().optional().describe('update/remove で必須'),
        },
        annotations: writeAnnotations,
    }, ({ id, from, transitionIndex, action, parameter, op, threshold, conditionIndex, layer }) => run({
        t: 'animation.setCondition', id, ...(layer === undefined ? {} : { layer }), transitionIndex, action,
        ...(from === undefined ? {} : { from }),
        ...(parameter === undefined ? {} : { parameter }),
        ...(op === undefined ? {} : { op }),
        ...(threshold === undefined ? {} : { threshold }),
        ...(conditionIndex === undefined ? {} : { conditionIndex }),
    }));
    server.registerTool('animation_add_state', {
        description: dryRun
            ? 'Animatorステート追加の入力を検証します。'
            : 'AnimatorにUndo可能なステートを追加します。mode省略でclip。最初のステートやsetAsDefault指定でデフォルトステートになります。BlendTreeはblendParameter(1D)またはblendParameterX/Y(2D)で駆動パラメーターを指定し、Motionはanimation_add_motionで足します。',
        inputSchema: {
            id: NodeIdSchema,
            layer: LayerOptionSchema,
            name: NameSchema.describe('新規ステート名。既存と重複不可'),
            mode: StateModeSchema.optional(),
            sourcePath: z.string().min(1).max(512).optional().describe('clip モードのアニメーションソース'),
            clipName: z.string().min(1).max(128).optional(),
            clipIndex: z.number().int().nonnegative().optional(),
            speed: z.number().finite().optional(),
            loop: z.boolean().optional(),
            ikWeight: z.number().finite().min(0).max(1).optional(),
            blendParameter: z.string().min(1).max(128).optional().describe('BlendTree1D 駆動パラメーター'),
            blendParameterX: z.string().min(1).max(128).optional().describe('BlendTree2D X 軸パラメーター'),
            blendParameterY: z.string().min(1).max(128).optional().describe('BlendTree2D Y 軸パラメーター'),
            setAsDefault: z.boolean().optional(),
        },
        annotations: writeAnnotations,
    }, ({ id, name, mode, sourcePath, clipName, clipIndex, speed, loop, ikWeight, blendParameter, blendParameterX, blendParameterY, setAsDefault, layer }) => run({
        t: 'animation.addState', id, ...(layer === undefined ? {} : { layer }), name,
        ...(mode === undefined ? {} : { mode }),
        ...(sourcePath === undefined ? {} : { sourcePath }),
        ...(clipName === undefined ? {} : { clipName }),
        ...(clipIndex === undefined ? {} : { clipIndex }),
        ...(speed === undefined ? {} : { speed }),
        ...(loop === undefined ? {} : { loop }),
        ...(ikWeight === undefined ? {} : { ikWeight }),
        ...(blendParameter === undefined ? {} : { blendParameter }),
        ...(blendParameterX === undefined ? {} : { blendParameterX }),
        ...(blendParameterY === undefined ? {} : { blendParameterY }),
        ...(setAsDefault === undefined ? {} : { setAsDefault }),
    }));
    server.registerTool('animation_set_state', {
        description: dryRun
            ? 'Animatorステート更新の入力を検証します。'
            : '既存ステートのプロパティをUndo可能に更新します。stateで対象を指定し、name指定でリネーム(全遷移参照・defaultStateを追従)。mode/clip/speed/loop/ikWeight/BlendTree駆動パラメーター/blend2DType/setAsDefaultを部分更新できます。',
        inputSchema: {
            id: NodeIdSchema,
            layer: LayerOptionSchema,
            state: z.string().min(1).max(128).describe('更新対象の現在のステート名'),
            name: z.string().min(1).max(128).optional().describe('新しい名前 (リネーム。参照追従)'),
            mode: StateModeSchema.optional(),
            sourcePath: z.string().min(1).max(512).optional(),
            clipName: z.string().min(1).max(128).optional(),
            clipIndex: z.number().int().nonnegative().optional(),
            speed: z.number().finite().optional(),
            loop: z.boolean().optional(),
            ikWeight: z.number().finite().min(0).max(1).optional(),
            blendParameter: z.string().min(1).max(128).optional(),
            blendParameterX: z.string().min(1).max(128).optional(),
            blendParameterY: z.string().min(1).max(128).optional(),
            blend2DType: z.enum(['simpleDirectional', 'freeformCartesian']).optional(),
            setAsDefault: z.boolean().optional(),
        },
        annotations: writeAnnotations,
    }, ({ id, state, name, mode, sourcePath, clipName, clipIndex, speed, loop, ikWeight, blendParameter, blendParameterX, blendParameterY, blend2DType, setAsDefault, layer }) => run({
        t: 'animation.setState', id, ...(layer === undefined ? {} : { layer }), state,
        ...(name === undefined ? {} : { name }),
        ...(mode === undefined ? {} : { mode }),
        ...(sourcePath === undefined ? {} : { sourcePath }),
        ...(clipName === undefined ? {} : { clipName }),
        ...(clipIndex === undefined ? {} : { clipIndex }),
        ...(speed === undefined ? {} : { speed }),
        ...(loop === undefined ? {} : { loop }),
        ...(ikWeight === undefined ? {} : { ikWeight }),
        ...(blendParameter === undefined ? {} : { blendParameter }),
        ...(blendParameterX === undefined ? {} : { blendParameterX }),
        ...(blendParameterY === undefined ? {} : { blendParameterY }),
        ...(blend2DType === undefined ? {} : { blend2DType }),
        ...(setAsDefault === undefined ? {} : { setAsDefault }),
    }));
    server.registerTool('animation_add_motion', {
        description: dryRun
            ? 'BlendTree Motion追加の入力を検証します。'
            : '指定BlendTreeステートにUndo可能なMotionを追加します。1Dはthreshold、2Dはpos X/Yで配置します。対象がClipステートの場合はエラー。animation_get_blend_treeで構成を確認できます。',
        inputSchema: {
            id: NodeIdSchema,
            layer: LayerOptionSchema,
            state: z.string().min(1).max(128).describe('BlendTree ステート名'),
            sourcePath: z.string().min(1).max(512).optional(),
            clipName: z.string().min(1).max(128).optional(),
            clipIndex: z.number().int().nonnegative().optional(),
            threshold: z.number().finite().optional().describe('BlendTree1D の位置'),
            posX: z.number().finite().optional().describe('BlendTree2D の X'),
            posY: z.number().finite().optional().describe('BlendTree2D の Y'),
            speed: z.number().finite().optional(),
            ikWeight: z.number().finite().min(0).max(1).optional(),
        },
        annotations: writeAnnotations,
    }, ({ id, state, sourcePath, clipName, clipIndex, threshold, posX, posY, speed, ikWeight, layer }) => run({
        t: 'animation.addMotion', id, ...(layer === undefined ? {} : { layer }), state,
        ...(sourcePath === undefined ? {} : { sourcePath }),
        ...(clipName === undefined ? {} : { clipName }),
        ...(clipIndex === undefined ? {} : { clipIndex }),
        ...(threshold === undefined ? {} : { threshold }),
        ...(posX === undefined ? {} : { posX }),
        ...(posY === undefined ? {} : { posY }),
        ...(speed === undefined ? {} : { speed }),
        ...(ikWeight === undefined ? {} : { ikWeight }),
    }));
    server.registerTool('animation_set_motion', {
        description: dryRun
            ? 'BlendTree Motion更新の入力を検証します。'
            : '指定BlendTreeステートのMotionをmotionIndexで指定してUndo可能に更新します。motionIndexはanimation_get_blend_treeのmotions並び順です。',
        inputSchema: {
            id: NodeIdSchema,
            layer: LayerOptionSchema,
            state: z.string().min(1).max(128),
            motionIndex: z.number().int().nonnegative().describe('motions リスト内 index'),
            sourcePath: z.string().min(1).max(512).optional(),
            clipName: z.string().min(1).max(128).optional(),
            clipIndex: z.number().int().nonnegative().optional(),
            threshold: z.number().finite().optional(),
            posX: z.number().finite().optional(),
            posY: z.number().finite().optional(),
            speed: z.number().finite().optional(),
            ikWeight: z.number().finite().min(0).max(1).optional(),
        },
        annotations: writeAnnotations,
    }, ({ id, state, motionIndex, sourcePath, clipName, clipIndex, threshold, posX, posY, speed, ikWeight, layer }) => run({
        t: 'animation.setMotion', id, ...(layer === undefined ? {} : { layer }), state, motionIndex,
        ...(sourcePath === undefined ? {} : { sourcePath }),
        ...(clipName === undefined ? {} : { clipName }),
        ...(clipIndex === undefined ? {} : { clipIndex }),
        ...(threshold === undefined ? {} : { threshold }),
        ...(posX === undefined ? {} : { posX }),
        ...(posY === undefined ? {} : { posY }),
        ...(speed === undefined ? {} : { speed }),
        ...(ikWeight === undefined ? {} : { ikWeight }),
    }));
    /// @note ── Animator 構造の削除系 + パラメーター CRUD ──
    server.registerTool('animation_remove_state', {
        description: dryRun
            ? 'Animatorステート削除の入力を検証します。'
            : '指定ステートをUndo可能に削除します。他ステート/Any Stateからこのステートへの遷移も併せて除去し、defaultStateだった場合は先頭ステートへ付け替えます。',
        inputSchema: {
            id: NodeIdSchema,
            layer: LayerOptionSchema,
            state: z.string().min(1).max(128).describe('削除するステート名'),
        },
        annotations: writeAnnotations,
    }, ({ id, state, layer }) => run({ t: 'animation.removeState', id, ...(layer === undefined ? {} : { layer }), state }));
    server.registerTool('animation_remove_transition', {
        description: dryRun
            ? 'Animator遷移削除の入力を検証します。'
            : '指定遷移をUndo可能に削除します。fromで遷移元ステート(省略でAny State)、transitionIndexはanimation_get_graphの並び順です。',
        inputSchema: {
            id: NodeIdSchema,
            layer: LayerOptionSchema,
            from: z.string().min(1).max(128).optional().describe('遷移元ステート名。省略で Any State'),
            transitionIndex: z.number().int().nonnegative().describe('対象ソースの遷移リスト内index'),
        },
        annotations: writeAnnotations,
    }, ({ id, from, transitionIndex, layer }) => run({
        t: 'animation.removeTransition', id, ...(layer === undefined ? {} : { layer }), transitionIndex,
        ...(from === undefined ? {} : { from }),
    }));
    server.registerTool('animation_remove_motion', {
        description: dryRun
            ? 'BlendTree Motion削除の入力を検証します。'
            : '指定BlendTreeステートのMotionをmotionIndexでUndo可能に削除します。motionIndexはanimation_get_blend_treeの並び順です。',
        inputSchema: {
            id: NodeIdSchema,
            layer: LayerOptionSchema,
            state: z.string().min(1).max(128).describe('BlendTree ステート名'),
            motionIndex: z.number().int().nonnegative().describe('motions リスト内 index'),
        },
        annotations: writeAnnotations,
    }, ({ id, state, motionIndex, layer }) => run({ t: 'animation.removeMotion', id, ...(layer === undefined ? {} : { layer }), state, motionIndex }));
    server.registerTool('animation_add_parameter', {
        description: dryRun
            ? 'Animatorパラメーター追加の入力を検証します。'
            : 'AnimatorパラメーターをUndo可能に追加します。type省略でfloat。valueで初期値を設定(float/int=数値、bool/trigger=真偽、省略で既定)。追加後はanimation_set_conditionで遷移条件に使えます。',
        inputSchema: {
            id: NodeIdSchema,
            name: NameSchema.describe('新規パラメーター名。既存と重複不可'),
            type: z.enum(['float', 'int', 'bool', 'trigger']).optional(),
            value: z.union([z.number().finite(), z.boolean()]).optional().describe('初期値'),
        },
        annotations: writeAnnotations,
    }, ({ id, name, type, value }) => run({
        t: 'animation.addParameter', id, name,
        ...(type === undefined ? {} : { type }),
        ...(value === undefined ? {} : { value }),
    }));
    server.registerTool('animation_remove_parameter', {
        description: dryRun
            ? 'Animatorパラメーター削除の入力を検証します。'
            : 'Animatorパラメーターを名前でUndo可能に削除します。このパラメーターを参照する遷移条件は残ります(発火しなくなるだけ)。参照状況はanimation_get_graphで確認してください。',
        inputSchema: {
            id: NodeIdSchema,
            name: z.string().min(1).max(128).describe('削除するパラメーター名'),
        },
        annotations: writeAnnotations,
    }, ({ id, name }) => run({ t: 'animation.removeParameter', id, name }));
    server.registerTool('avatar_mask_get', {
        description: '.mask アセット(Avatar Mask)の内容を返します。defaultInclude と、ボーンごとの weight / includeChildren / blendDepth の一覧が取れます。animation_set_layer で maskPath に割り当てる前の確認に使います。',
        inputSchema: {
            path: z.string().min(1).max(512).describe('.mask アセットのパス (例 Assets/Animation/UpperBody.mask)'),
        },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ path }) => Safely(async () => TextResult(await bus.Query({ t: 'avatarMask.get', path }))));
    server.registerTool('avatar_mask_write', {
        description: dryRun
            ? 'Avatar Mask 編集の入力を検証します。'
            : '.mask アセットを作成・編集します。action=create/setBone/removeBone/clear/setDefaultInclude。上半身レイヤーなら「defaultInclude=false + Spine を weight 1 / blendDepth 3 で setBone」が基本形です。blendDepth はそのボーンから下へ何階層かけて weight を立ち上げるかで、腰の継ぎ目でポーズが折れるのを防ぎます。Undo対象外(アセットファイルへ直接書き込み)。',
        inputSchema: {
            path: z.string().min(1).max(512).describe('.mask アセットのパス'),
            action: z.enum(['create', 'setBone', 'removeBone', 'clear', 'setDefaultInclude'])
                .describe('create=新規作成 / setBone=ボーン追加更新 / removeBone=削除 / clear=全消去 / setDefaultInclude=既定の切替'),
            name: z.string().min(1).max(128).optional().describe('create 時の表示名'),
            overwrite: z.boolean().optional().describe('create で既存を上書きする'),
            defaultInclude: z.boolean().optional()
                .describe('false=一覧にないボーンは無効 (上半身だけ有効などに使う) / true=一覧にないボーンは有効 (指だけ除外などに使う)'),
            skeletonSourcePath: z.string().min(1).max(512).optional().describe('参照スケルトンの .fbx'),
            bone: z.string().min(1).max(256).optional()
                .describe('setBone/removeBone の対象。ボーン名 ("Spine1") またはパス ("Hips/Spine/Spine1")'),
            weight: z.number().finite().min(0).max(1).optional().describe('setBone のウェイト 0..1'),
            includeChildren: z.boolean().optional().describe('setBone で子孫にも適用するか'),
            blendDepth: z.number().int().min(0).max(8).optional()
                .describe('0=配下一律。1以上でこのボーンから下へ段階的に weight を立ち上げる'),
        },
        annotations: writeAnnotations,
    }, ({ path, action, name, overwrite, defaultInclude, skeletonSourcePath, bone, weight, includeChildren, blendDepth }) => run({
        t: 'avatarMask.write', path, action,
        ...(name === undefined ? {} : { name }),
        ...(overwrite === undefined ? {} : { overwrite }),
        ...(defaultInclude === undefined ? {} : { defaultInclude }),
        ...(skeletonSourcePath === undefined ? {} : { skeletonSourcePath }),
        ...(bone === undefined ? {} : { bone }),
        ...(weight === undefined ? {} : { weight }),
        ...(includeChildren === undefined ? {} : { includeChildren }),
        ...(blendDepth === undefined ? {} : { blendDepth }),
    }));
    server.registerTool('animation_add_layer', {
        description: dryRun
            ? 'Animatorレイヤー追加の入力を検証します。'
            : 'AnimatorにUndo可能なレイヤーを追加します。上半身/下半身の部分制御はここから始めます。maskPathに.maskアセットを指定するとそのボーンだけに効きます。mode=additiveで加算レイヤーになります。追加後はanimation_add_stateにlayerを渡してレイヤー専用のステートマシンを組めます。',
        inputSchema: {
            id: NodeIdSchema,
            name: NameSchema.describe('新規レイヤー名。既存と重複不可'),
            weight: z.number().finite().min(0).max(1).optional().describe('レイヤー適用量 0..1'),
            mode: z.enum(['override', 'additive']).optional().describe('省略でoverride'),
            enabled: z.boolean().optional(),
            maskPath: z.string().min(1).max(512).optional().describe('.mask アセットのパス。省略で全身'),
            stateName: z.string().min(1).max(128).optional()
                .describe('レイヤー専用ステートマシンを使わない場合に再生する共有ステート名'),
            additiveSourcePath: z.string().min(1).max(512).optional()
                .describe('mode=additive の基準ポーズを取るアニメーションソース'),
            additiveClipName: z.string().min(1).max(128).optional(),
            additiveTime: z.number().finite().min(0).optional().describe('基準ポーズの秒位置'),
        },
        annotations: writeAnnotations,
    }, ({ id, name, weight, mode, enabled, maskPath, stateName, additiveSourcePath, additiveClipName, additiveTime }) => run({
        t: 'animation.addLayer', id, name,
        ...(weight === undefined ? {} : { weight }),
        ...(mode === undefined ? {} : { mode }),
        ...(enabled === undefined ? {} : { enabled }),
        ...(maskPath === undefined ? {} : { maskPath }),
        ...(stateName === undefined ? {} : { stateName }),
        ...(additiveSourcePath === undefined ? {} : { additiveSourcePath }),
        ...(additiveClipName === undefined ? {} : { additiveClipName }),
        ...(additiveTime === undefined ? {} : { additiveTime }),
    }));
    server.registerTool('animation_set_layer', {
        description: dryRun
            ? 'Animatorレイヤー更新の入力を検証します。'
            : '既存Animatorレイヤーのプロパティを部分的にUndo可能で更新します。layerで対象を指定し、name指定でリネーム。weight/mode/enabled/maskPath/stateName/defaultStateName/加算基準ポーズを個別に変更できます。上半身レイヤーのマスク差し替えやフェードイン量の調整に使います。',
        inputSchema: {
            id: NodeIdSchema,
            layer: LayerNameSchema.describe('更新対象の現在のレイヤー名'),
            name: z.string().min(1).max(128).optional().describe('新しい名前 (リネーム)'),
            weight: z.number().finite().min(0).max(1).optional(),
            mode: z.enum(['override', 'additive']).optional(),
            enabled: z.boolean().optional(),
            maskPath: z.string().max(512).optional().describe('.mask アセットのパス。空文字列でマスク解除'),
            stateName: z.string().max(128).optional(),
            defaultStateName: z.string().max(128).optional()
                .describe('レイヤー専用ステートマシンの初期ステート'),
            additiveSourcePath: z.string().max(512).optional(),
            additiveClipName: z.string().max(128).optional(),
            additiveTime: z.number().finite().min(0).optional(),
        },
        annotations: writeAnnotations,
    }, ({ id, layer, name, weight, mode, enabled, maskPath, stateName, defaultStateName, additiveSourcePath, additiveClipName, additiveTime }) => run({
        t: 'animation.setLayer', id, layer,
        ...(name === undefined ? {} : { name }),
        ...(weight === undefined ? {} : { weight }),
        ...(mode === undefined ? {} : { mode }),
        ...(enabled === undefined ? {} : { enabled }),
        ...(maskPath === undefined ? {} : { maskPath }),
        ...(stateName === undefined ? {} : { stateName }),
        ...(defaultStateName === undefined ? {} : { defaultStateName }),
        ...(additiveSourcePath === undefined ? {} : { additiveSourcePath }),
        ...(additiveClipName === undefined ? {} : { additiveClipName }),
        ...(additiveTime === undefined ? {} : { additiveTime }),
    }));
    server.registerTool('animation_remove_layer', {
        description: dryRun
            ? 'Animatorレイヤー削除の入力を検証します。'
            : 'Animatorレイヤーを名前でUndo可能に削除します。そのレイヤーが持つステートマシンとマスク参照も一緒に消えます。',
        inputSchema: {
            id: NodeIdSchema,
            layer: LayerNameSchema.describe('削除するレイヤー名'),
        },
        annotations: writeAnnotations,
    }, ({ id, layer }) => run({ t: 'animation.removeLayer', id, layer }));
    server.registerTool('animation_play_slot', {
        description: dryRun
            ? 'Slot再生の入力を検証します。'
            : '指定レイヤーへワンショットのクリップを割り込ませます。下半身の移動を流したまま上半身だけ攻撃モーションを差し込む、といった用途です。クリップ終端で自動的にフェードアウトして元のステートマシン出力へ戻ります。',
        inputSchema: {
            id: NodeIdSchema,
            layer: LayerNameSchema.describe('割り込ませる対象レイヤー名'),
            sourcePath: z.string().min(1).max(512).optional().describe('アニメーションソース'),
            clipName: z.string().min(1).max(128).optional(),
            fadeIn: z.number().finite().min(0).optional().describe('立ち上げ秒数 (既定0.15)'),
            fadeOut: z.number().finite().min(0).optional().describe('終了フェード秒数 (既定0.15)'),
            speed: z.number().finite().optional(),
            loop: z.boolean().optional().describe('trueで明示stopまでループ'),
        },
        annotations: writeAnnotations,
    }, ({ id, layer, sourcePath, clipName, fadeIn, fadeOut, speed, loop }) => run({
        t: 'animation.playSlot', id, layer,
        ...(sourcePath === undefined ? {} : { sourcePath }),
        ...(clipName === undefined ? {} : { clipName }),
        ...(fadeIn === undefined ? {} : { fadeIn }),
        ...(fadeOut === undefined ? {} : { fadeOut }),
        ...(speed === undefined ? {} : { speed }),
        ...(loop === undefined ? {} : { loop }),
    }));
    server.registerTool('animation_stop_slot', {
        description: dryRun
            ? 'Slot停止の入力を検証します。'
            : '指定レイヤーで再生中のSlotをフェードアウトさせます。loop指定で再生したSlotを止めるときに使います。',
        inputSchema: {
            id: NodeIdSchema,
            layer: LayerNameSchema.describe('対象レイヤー名'),
            fadeOut: z.number().finite().min(0).optional().describe('省略でPlay時に指定した値'),
        },
        annotations: writeAnnotations,
    }, ({ id, layer, fadeOut }) => run({
        t: 'animation.stopSlot', id, layer,
        ...(fadeOut === undefined ? {} : { fadeOut }),
    }));
}
//# sourceMappingURL=AnimationTools.js.map