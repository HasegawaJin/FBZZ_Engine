/// @file    BehaviorTreeTools.ts
/// @brief   Behavior Tree の照会と編集を MCP へ公開する。
/// @author  Hasegawa Jin
/// @date    2026-11-06
import * as z from 'zod/v4';
import { JsonValueSchema } from '../editorContracts.js';
import { TextResult, Safely, CreateCommandContext } from './ToolSupport.js';
export function RegisterBehaviorTreeQueryTools(server, bus) {
    server.registerTool('bt_inspect_tree', {
        description: '.behaviortree の木構造・Blackboard・検証結果を返します。'
            + 'ノードは parentId と order で並べて返るため、配列の順序がそのまま優先順位です。'
            + 'order は Selector で「やりたいことの優先順位」そのものなので、'
            + '木の形だけから推測せずここを読んでください。',
        inputSchema: { path: z.string().min(1).describe('projectRoot 相対の .behaviortree') },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ path }) => Safely(async () => TextResult(await bus.Query({ t: 'bt.tree', path }))));
    server.registerTool('bt_lint', {
        description: '.behaviortree の「保存はできるが意図どおり動かない」構成を返します。'
            + 'valid=false は保存が拒否される致命的な不整合 (ルートが 0/2 個・循環・子数超過)。'
            + '各 issue には severity と直し方が fix (呼ぶべきツールと引数) と '
            + 'autoFixable (bt_repair で直せるか) として付きます。'
            + 'caution がある項目は、その修正で失われるものを確認してから実行してください。'
            + '最重要は no-lower-priority-abort で、Selector の高優先枝を守る条件に'
            + 'lowerPriority 中断が無い状態です (巡回中に敵を見つけても着くまで反応しない AI になります)。'
            + 'compileWarnings は保存も Validate も通るが実行時に効かないもの'
            + '(解決できなかった Blackboard キー等) で、静的検査だけでは見えない層です。',
        inputSchema: { path: z.string().min(1) },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ path }) => Safely(async () => TextResult(await bus.Query({ t: 'bt.lint', path }))));
    server.registerTool('bt_guide', {
        description: 'Behavior Tree を組む前に読むオーサリング規約と、'
            + '代表的な骨格 (Guard / Chaser / Turret)、全ノード種別の子数上限と'
            + 'abortMode を付けられるかの一覧を返します。'
            + '木を作る前に必ず読んでください。とくに abortMode=lowerPriority を'
            + '付けないと「巡回中に敵を見つけても着くまで反応しない」AI になります。',
        inputSchema: {},
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, () => Safely(async () => TextResult(await bus.Query({ t: 'bt.guide' }))));
    server.registerTool('bt_schema', {
        description: 'ノード種別ごとに「そのランタイムが実際に読むフィールド」を返します。'
            + 'bt_node_set_field の対で、field 名・型・受理する enum 値・範囲・'
            + 'そのフィールドを読む種別 (appliesTo) が載ります。'
            + 'WHY 要るか: 実在するがその種別では読まれないフィールド (Wait へ range、'
            + 'HasTarget へ duration) は以前は受理され保存まで通り、'
            + '「設定したのに行動が変わらない」としか見えませんでした。'
            + '木を組む前にこれを読めば、試行錯誤ではなく参照でフィールドが決まります。',
        inputSchema: {
            nodeType: z.string().min(1).max(64).optional()
                .describe('指定するとその種別だけに絞る。省略で全 26 種別'),
        },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ nodeType }) => Safely(async () => TextResult(await bus.Query({
        t: 'bt.schema', ...(nodeType === undefined ? {} : { nodeType }),
    }))));
    server.registerTool('bt_node_get_field', {
        description: 'ノードのフィールドの現在値を返します。bt_node_set_field の対になる読み出しで、'
            + 'value は set 側と同じ表現なので読んで一部だけ変えて書き戻せます。'
            + 'field 省略時はその種別が実際に読むフィールドだけを全部返します。'
            + 'WHY 要るか: 書く手段はあるのに読む手段が無く、duration や keyName に'
            + '今何が入っているかを API 越しに確かめられませんでした '
            + '(bt_inspect_tree は要約なので全フィールドを返しません)。',
        inputSchema: {
            path: z.string().min(1), nodeId: z.number().int().min(1),
            field: z.string().min(1).max(64).optional(),
        },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ path, nodeId, field }) => Safely(async () => TextResult(await bus.Query({
        t: 'bt.nodeField', path, nodeId, ...(field === undefined ? {} : { field }),
    }))));
    server.registerTool('bt_runtime_state', {
        description: 'Play 中のエージェントについて、各ノードの最終 status (DFS pre-order = 優先度順) と'
            + 'Blackboard の実値・最終書き込み時刻を返します。'
            + 'WHY 木だけでは足りないか: BT が意図どおり動かない原因は「条件が偽のまま」'
            + '「割り込めていない」「そもそも到達していない」の 3 通りあり、'
            + '木を読んでも bt_lint を掛けても区別できません。viewport_capture で敵の動きを見ても、'
            + 'なぜその行動を選んだかは映りません。written=false のキーは一度も書かれていないので、'
            + '条件が偽なのは木ではなく知覚側 (PerceptionSystem / スクリプト) の問題です。'
            + 'path / id 省略時は実行中の BT を 1 体選び、候補は常に agents で返します。',
        inputSchema: {
            path: z.string().min(1).optional().describe('この .behaviortree を使うエージェントに絞る'),
            id: z.string().min(1).optional().describe('GameObject の instanceId で 1 体を指定する'),
        },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ path, id }) => Safely(async () => TextResult(await bus.Query({
        t: 'bt.runtime',
        ...(path === undefined ? {} : { path }), ...(id === undefined ? {} : { id }),
    }))));
    server.registerTool('bt_diff', {
        description: '2 つの .behaviortree の構造差分を返します。ノードの追加・削除・'
            + 'フィールド変更に加え、parentId と order の変化 (= 木の意味そのものの変化) と'
            + 'Blackboard キーの増減を出します。editorX / editorY は挙動に無関係なので含めません。'
            + '元の木を残したまま別案を作って比較する使い方に使います。',
        inputSchema: { base: z.string().min(1), target: z.string().min(1) },
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, ({ base, target }) => Safely(async () => TextResult(await bus.Query({
        t: 'bt.diff', base, target,
    }))));
    server.registerTool('bt_template_catalog', {
        description: '取り込める Behavior Tree の骨格 (Assets/AI/Templates) を列挙します。'
            + 'bt_guide の recipes は文章なのでそのまま実体にはなりません。'
            + '動く木が既にあるなら、ゼロから積むより bt_template_apply で取り込んで直すほうが確実です。',
        inputSchema: {},
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, () => Safely(async () => TextResult(await bus.Query({ t: 'bt.templateCatalog' }))));
}
export function RegisterBehaviorTreeCommandTools(server, bus, permission) {
    const { run, writeAnnotations } = CreateCommandContext(bus, permission);
    /// @note ── Behavior Tree の編集 ──
    server.registerTool('bt_node_add', {
        description: 'Behavior Tree へノードを追加します。parentId 省略はルート作成で、'
            + '既にルートがあると拒否されます (木にルートは 1 つだけ)。'
            + '子数上限・循環はここで拒否されるため、保存時まで気付かない失敗になりません。',
        inputSchema: {
            path: z.string().min(1),
            nodeType: z.string().min(1).max(64)
                .describe('bt_guide の nodeTypes に載っている種別名 (Sequence / Selector / MoveTo …)'),
            parentId: z.number().int().min(1).optional(),
            name: z.string().min(1).max(128).optional(),
        }, annotations: writeAnnotations,
    }, ({ path, nodeType, parentId, name }) => run({
        t: 'bt.node.add', path, nodeType,
        ...(parentId === undefined ? {} : { parentId }),
        ...(name === undefined ? {} : { name }),
    }));
    server.registerTool('bt_node_remove', {
        description: 'ノードとその子孫をまとめて削除します。'
            + 'WHY 子孫ごとか: 親だけ消すと枝が浮き、残された枝が何のためのものか判らなくなります。',
        inputSchema: { path: z.string().min(1), nodeId: z.number().int().min(1) },
        annotations: writeAnnotations,
    }, ({ path, nodeId }) => run({ t: 'bt.node.remove', path, nodeId }));
    server.registerTool('bt_node_duplicate', {
        description: 'ノードを部分木ごと複製します。Editor の Duplicate Subtree と同じ規則で動くため、'
            + 'AI が作った木を人が触っても形が変わりません。'
            + 'parentId 省略で元と同じ親の末尾へ兄弟として並びます (ルートは複製できません)。'
            + '応答の detail に新しい id の対応表 (idMap) が載るので、'
            + '複製直後に中身を編集するために bt_inspect_tree を読み直す必要がありません。',
        inputSchema: {
            path: z.string().min(1), nodeId: z.number().int().min(1),
            parentId: z.number().int().min(1).optional(),
        }, annotations: writeAnnotations,
    }, ({ path, nodeId, parentId }) => run({
        t: 'bt.node.duplicate', path, nodeId,
        ...(parentId === undefined ? {} : { parentId }),
    }));
    server.registerTool('bt_node_set_parent', {
        description: 'ノードの親を張り替えます。循環・子数上限・葉への子付けは拒否されます。',
        inputSchema: {
            path: z.string().min(1), nodeId: z.number().int().min(1),
            parentId: z.number().int().min(0).describe('0 で親なし (ルート)'),
        }, annotations: writeAnnotations,
    }, ({ path, nodeId, parentId }) => run({ t: 'bt.node.setParent', path, nodeId, parentId }));
    server.registerTool('bt_node_set_order', {
        description: '同じ親の中での評価順 (優先度) を変えます。小さいほど先に評価されます。'
            + 'Selector ではこれが「やりたいことの優先順位」そのもので、'
            + '並べ替えを怠ると巡回が先に成功して戦闘へ入らない、という形で静かに壊れます。',
        inputSchema: {
            path: z.string().min(1), nodeId: z.number().int().min(1),
            order: z.number().int().min(0).max(4096),
        }, annotations: writeAnnotations,
    }, ({ path, nodeId, order }) => run({ t: 'bt.node.setOrder', path, nodeId, order }));
    server.registerTool('bt_node_set_field', {
        description: 'ノードのフィールドを 1 つ変更します。'
            + 'abortMode は純粋条件ノードにしか設定できず、それ以外へ渡すと理由付きで拒否されます。'
            + 'keyName / moveTargetKey は Blackboard に実在するキーだけを受け付けます '
            + '(存在しない名前は保存も通り、実行時に黙って無視されるため)。',
        inputSchema: {
            path: z.string().min(1), nodeId: z.number().int().min(1),
            field: z.string().min(1).max(64)
                .describe('bt_schema の fields に載っている名前。そのノード種別が読まないフィールドは '
                + 'BT_FIELD_NOT_APPLICABLE で拒否される'),
            value: JsonValueSchema,
        }, annotations: writeAnnotations,
    }, ({ path, nodeId, field, value }) => run({ t: 'bt.node.setField', path, nodeId, field, value }));
    server.registerTool('bt_blackboard_add', {
        description: 'Blackboard キーを追加します。ノードがキーを参照する前にこれで作ってください。',
        inputSchema: {
            path: z.string().min(1), name: z.string().min(1).max(64),
            type: z.enum(['bool', 'int', 'float', 'vector3', 'entity', 'string']).optional(),
        }, annotations: writeAnnotations,
    }, ({ path, name, type }) => run({
        t: 'bt.blackboard.add', path, name, ...(type === undefined ? {} : { type }),
    }));
    server.registerTool('bt_blackboard_remove', {
        description: 'Blackboard キーを削除します。予約キー (reserved=true) は拒否されます。',
        inputSchema: { path: z.string().min(1), name: z.string().min(1).max(64) },
        annotations: writeAnnotations,
    }, ({ path, name }) => run({ t: 'bt.blackboard.remove', path, name }));
    server.registerTool('bt_auto_layout', {
        description: '木の深さを列にして並べ直します。ノードを追加した後に呼ぶと、'
            + '人が開いたときに構造が読める配置になります。',
        inputSchema: { path: z.string().min(1) },
        annotations: writeAnnotations,
    }, ({ path }) => run({ t: 'bt.autoLayout', path }));
    server.registerTool('bt_repair', {
        description: 'bt_lint が autoFixable=true と言った不備を機械的に直します。'
            + '直し方が一意に決まるものだけを扱い、「何をする木か」のような設計判断には触れません。'
            + '対象は no-lower-priority-abort (abortMode を lowerPriority へ) / '
            + 'zero-cooldown・zero-duration-wait (duration を 1.0 秒へ) / '
            + 'zero-weights (重みを等確率へ) / unresolved-key (綴りの近い既存キーへ張り替え)。'
            + '応答の detail に直した項目と、直せずに残った issue が載ります。'
            + 'フラグは全て省略で有効。個別に false を渡したときだけその修復を止めます。',
        inputSchema: {
            path: z.string().min(1),
            fixAborts: z.boolean().optional(), fixDurations: z.boolean().optional(),
            fixWeights: z.boolean().optional(), fixKeys: z.boolean().optional(),
        }, annotations: writeAnnotations,
    }, ({ path, fixAborts, fixDurations, fixWeights, fixKeys }) => run({
        t: 'bt.repair', path,
        ...(fixAborts === undefined ? {} : { fixAborts }),
        ...(fixDurations === undefined ? {} : { fixDurations }),
        ...(fixWeights === undefined ? {} : { fixWeights }),
        ...(fixKeys === undefined ? {} : { fixKeys }),
    }));
    server.registerTool('bt_template_apply', {
        description: 'bt_template_catalog で見つけた骨格を .behaviortree として書き出します。'
            + 'path は存在しなくてよく、既存ファイルを指した場合は上書きして Undo で戻せます。'
            + 'ゼロから bt_node_add を積むより確実で、abortMode や Blackboard キーまで'
            + '欠落なく持ち込めます。取り込み後は bt_lint → bt_node_set_field で用途に合わせて調整します。',
        inputSchema: {
            template: z.string().min(1).max(260)
                .describe('bt_template_catalog の name か、projectRoot 相対の .behaviortree パス'),
            path: z.string().min(1).describe('書き出し先の .behaviortree'),
            name: z.string().min(1).max(128).optional(),
            description: z.string().max(512).optional(),
        }, annotations: writeAnnotations,
    }, ({ template, path, name, description }) => run({
        t: 'bt.template.apply', template, path,
        ...(name === undefined ? {} : { name }),
        ...(description === undefined ? {} : { description }),
    }));
}
//# sourceMappingURL=BehaviorTreeTools.js.map