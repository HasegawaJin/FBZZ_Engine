/// @file    PlaytestTools.ts
/// @brief   Play・入力・シナリオ・画像比較の MCP 操作。
/// @author  Hasegawa Jin
/// @date    2026-11-06
import * as z from 'zod/v4';
import type { McpServer } from '@modelcontextprotocol/sdk/server/mcp.js';
import type { CallToolResult } from '@modelcontextprotocol/sdk/types.js';
import type { EditorBus } from '../busClient.js';
import type { PermissionMode } from '../config.js';
import { EditorCommandSchema, JsonValueSchema, NodeIdSchema, SemanticViewportResultSchema, type EditorCommand } from '../editorContracts.js';
import { Delay, TextResult, Safely, CreateCommandContext } from './ToolSupport.js';

const InputInjectionShape = {
    kind: z.enum(['key', 'axis', 'gamepadButton', 'gamepadAxis', 'mouseButton', 'mousePosition', 'mouseDelta', 'mouseScroll', 'clear']),
    key: z.string().min(1).max(32).optional(),
    buttonName: z.string().min(1).max(64).optional(),
    pressed: z.boolean().optional(),
    axis: z.string().min(1).max(64).optional(),
    button: z.number().int().min(0).max(2).optional(),
    value: z.union([z.number().finite(), z.tuple([z.number().finite(), z.number().finite()])]).optional(),
};

const InputInjectionSchema = z.object(InputInjectionShape).strict().superRefine((input, context) => {
    const isNumber = typeof input.value === 'number';
    const isVector = Array.isArray(input.value);
    const valid = input.kind === 'clear'
        || (input.kind === 'key' && input.key !== undefined && input.pressed !== undefined)
        || ((input.kind === 'axis' || input.kind === 'gamepadAxis') && input.axis !== undefined && isNumber)
        || (input.kind === 'gamepadButton' && input.buttonName !== undefined && input.pressed !== undefined)
        || (input.kind === 'mouseButton' && input.button !== undefined && input.pressed !== undefined)
        || ((input.kind === 'mousePosition' || input.kind === 'mouseDelta') && isVector)
        || (input.kind === 'mouseScroll' && isNumber);
    if (!valid) context.addIssue({ code: 'custom', message: `kind=${input.kind} に必要な入力が不足しています` });
});

type InputInjection = z.infer<typeof InputInjectionSchema>;

/// @note MCP入力からundefinedを除き、Command Busの厳密なinput.inject契約へ変換する。
function ToInputCommand(input: InputInjection): EditorCommand {
    return {
        t: 'input.inject', kind: input.kind,
        ...(input.key === undefined ? {} : { key: input.key }),
        ...(input.buttonName === undefined ? {} : { buttonName: input.buttonName }),
        ...(input.pressed === undefined ? {} : { pressed: input.pressed }),
        ...(input.axis === undefined ? {} : { axis: input.axis }),
        ...(input.button === undefined ? {} : { button: input.button }),
        ...(input.value === undefined ? {} : { value: input.value }),
    };
}

export function RegisterPlaytestQueryTools(server: McpServer, bus: EditorBus): void {
    /// @note ── 検証ループ (Docs/design/ai-verification-loop.md) ──
    /// @note 操作の口だけでは «直った» を言えない。シナリオの合否・絵の差分・Editor が実際に
    /// @note      受け付ける型の一覧を読めて初めて、AI が自分の変更を自分で検証できる。
    server.registerTool('scenario_status', {
        description: 'Playtest シナリオの進捗と結果を返します。state は running / passed / failed / idle。'
            + 'failed のときは failure が最初に落ちた手順の理由、steps が手順ごとの記録、images が絵の比較結果'
            + '(meanDiff / badPixelRatio / 差分画像 diff のパス) です。scenario_run の後は state が running でなくなるまでここを呼びます。',
        inputSchema: {},
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, () => Safely(async () => TextResult(await bus.Query({ t: 'playtest.status' }))));

    server.registerTool('scenario_list', {
        description: 'プロジェクトの Tests/Playtests 以下にある *.playtest.json を列挙します。scenario_run の path に渡せます。',
        inputSchema: {},
        annotations: { readOnlyHint: true, openWorldHint: false },
    }, () => Safely(async () => TextResult(await bus.Query({ t: 'playtest.list' }))));
}


export function RegisterPlaytestCommandTools(server: McpServer, bus: EditorBus, permission: PermissionMode): void {
    const { dryRun, run, writeAnnotations } = CreateCommandContext(bus, permission);

    server.registerTool('input_inject', {
        description: 'Play Modeへキー、仮想軸、ゲームパッドボタン/軸、マウス入力を注入します。clearで全注入状態を解除します。',
        inputSchema: InputInjectionShape,
        annotations: writeAnnotations,
    }, (input) => run(ToInputCommand(InputInjectionSchema.parse(input))));

    server.registerTool('play_control', {
        description: 'Play Mode の開始・停止・一時停止・再開・1フレームステップを制御します。',
        inputSchema: { action: z.enum(['start', 'stop', 'pause', 'resume', 'step']) },
        annotations: writeAnnotations,
    }, ({ action }) => run({ t: 'play.control', action }));

    server.registerTool('playtest_run', {
        description: dryRun
            ? '自動プレイテストの入力列と判定条件を検証します。待機や実行は行いません。'
            : 'Play開始、時系列入力、待機、今回のログ・性能・衝突・意味付き画像の収集、元のPlay状態への復元を1回で行います。',
        inputSchema: {
            startPlay: z.boolean().default(true).describe('editor状態ならPlay開始、pausedならresumeする'),
            steps: z.array(z.object({
                delayMs: z.number().int().min(0).max(10000).default(0).describe('直前stepからの待機時間'),
                input: InputInjectionSchema,
            }).strict()).max(64).default([]),
            settleMs: z.number().int().min(0).max(10000).default(250).describe('最終入力後の観測待機時間'),
            restorePlayState: z.boolean().default(true).describe('終了時に呼び出し前のeditor/paused/playingへ戻す'),
            view: z.enum(['scene', 'game']).default('game').describe('判定と画像取得に使うViewport'),
            assertions: z.object({
                noErrors: z.boolean().default(true).describe('今回発生したerrorログを失敗にする'),
                noWarnings: z.boolean().default(false).describe('今回発生したwarningログも失敗にする'),
                minFps: z.number().finite().positive().max(1000).optional(),
                maxFrameMs: z.number().finite().positive().max(10000).optional(),
                logContains: z.string().min(1).max(256).optional(),
                visibleNodeIds: z.array(NodeIdSchema).max(64).default([]),
            }).strict().default({ noErrors: true, noWarnings: false, visibleNodeIds: [] }),
            w: z.number().int().min(160).max(1920).default(960),
            h: z.number().int().min(90).max(1080).default(540),
        },
        annotations: writeAnnotations,
    }, ({ startPlay, steps, settleMs, restorePlayState, view, assertions, w, h }) => Safely(async () => {
        const totalWaitMs = settleMs + steps.reduce((total, step) => total + step.delayMs, 0);
        if (totalWaitMs > 60000) throw new Error('playtest_run の合計待機時間は60000ms以下にしてください');

        const inputCommands = steps.map((step) => ({
            delayMs: step.delayMs,
            command: EditorCommandSchema.parse(ToInputCommand(step.input)),
        }));
        const initialState = await bus.Query({ t: 'editor.state' }) as Record<string, unknown>;
        const initialPlayState = String(initialState.playState ?? 'editor');

        /// @note dry-runでは時系列待機を省略し、全CommandをEditor側のdryRun検証へ通す。
        if (dryRun) {
            const previews: unknown[] = [];
            if (startPlay && initialPlayState === 'editor')
                previews.push(await bus.Command({ t: 'play.control', action: 'start' }, true));
            else if (startPlay && initialPlayState === 'paused')
                previews.push(await bus.Command({ t: 'play.control', action: 'resume' }, true));
            for (const step of inputCommands) previews.push(await bus.Command(step.command, true));
            previews.push(await bus.Command({ t: 'input.inject', kind: 'clear' }, true));
            if (restorePlayState && startPlay && initialPlayState === 'editor')
                previews.push(await bus.Command({ t: 'play.control', action: 'stop' }, true));
            else if (restorePlayState && startPlay && initialPlayState === 'paused')
                previews.push(await bus.Command({ t: 'play.control', action: 'pause' }, true));
            return TextResult({ dryRun: true, initialPlayState, totalWaitMs, steps: inputCommands.length, previews });
        }

        const baselineLogs = await bus.Query({ t: 'console.logs', minLevel: 'debug', limit: 1 }) as Record<string, unknown>;
        const baselineCursor = Number(baselineLogs.cursor ?? 0);
        const cleanupFailures: string[] = [];
        let operationError: unknown;
        let state: Record<string, unknown> = {};
        let logs: Record<string, unknown> = {};
        let profiler: Record<string, unknown> = {};
        let physicsEvents: unknown = {};
        let capture: z.infer<typeof SemanticViewportResultSchema> | undefined;

        const waitForState = async (expected: string, requireReady: boolean): Promise<void> => {
            const deadline = Date.now() + 10000;
            do {
                const current = await bus.Query({ t: 'editor.state' }) as Record<string, unknown>;
                const ready = !requireReady || (current.restorePending === false && current.managedBuildBusy === false);
                if (current.playState === expected && ready) return;
                await Delay(50);
            } while (Date.now() < deadline);
            throw new Error(`Play状態が ${expected} へ遷移しませんでした`);
        };

        try {
            if (startPlay && initialPlayState === 'editor') {
                await bus.Command({ t: 'play.control', action: 'start' }, false);
                await waitForState('playing', false);
            } else if (startPlay && initialPlayState === 'paused') {
                await bus.Command({ t: 'play.control', action: 'resume' }, false);
                await waitForState('playing', false);
            }

            for (const step of inputCommands) {
                if (step.delayMs > 0) await Delay(step.delayMs);
                await bus.Command(step.command, false);
            }
            if (settleMs > 0) await Delay(settleMs);

            const [rawState, rawLogs, rawProfiler, rawPhysicsEvents, rawCapture] = await Promise.all([
                bus.Query({ t: 'editor.state' }),
                bus.Query({ t: 'console.logs', minLevel: 'debug', limit: 512, afterSequence: baselineCursor }),
                bus.Query({ t: 'profiler.snapshot', limit: 20 }),
                bus.Query({ t: 'physics.events' }),
                bus.Query({ t: 'viewport.semantic', w, h, view }),
            ]);
            state = rawState as Record<string, unknown>;
            logs = rawLogs as Record<string, unknown>;
            profiler = rawProfiler as Record<string, unknown>;
            physicsEvents = rawPhysicsEvents;
            capture = SemanticViewportResultSchema.parse(rawCapture);
        } catch (error) {
            operationError = error;
        } finally {
            try {
                await bus.Command({ t: 'input.inject', kind: 'clear' }, false);
            } catch (error) {
                cleanupFailures.push(`input clear: ${error instanceof Error ? error.message : String(error)}`);
            }

            if (restorePlayState) {
                try {
                    const current = await bus.Query({ t: 'editor.state' }) as Record<string, unknown>;
                    if (initialPlayState === 'editor' && current.playState !== 'editor') {
                        await bus.Command({ t: 'play.control', action: 'stop' }, false);
                        await waitForState('editor', true);
                    } else if (initialPlayState === 'paused' && current.playState === 'playing') {
                        await bus.Command({ t: 'play.control', action: 'pause' }, false);
                        await waitForState('paused', false);
                    } else if (initialPlayState === 'playing' && current.playState === 'paused') {
                        await bus.Command({ t: 'play.control', action: 'resume' }, false);
                        await waitForState('playing', false);
                    }
                } catch (error) {
                    cleanupFailures.push(`play state restore: ${error instanceof Error ? error.message : String(error)}`);
                }
            }
        }

        if (operationError !== undefined) throw operationError;
        if (capture === undefined) throw new Error('意味付きviewportを取得できませんでした');

        const failures: string[] = [...cleanupFailures];
        const entries = Array.isArray(logs.entries) ? logs.entries as Array<Record<string, unknown>> : [];
        const errorEntries = entries.filter((entry) => entry.level === 'error');
        const warningEntries = entries.filter((entry) => entry.level === 'warning');
        if (logs.dropped === true) failures.push('ログリングバッファが溢れ、テスト中ログの一部を失いました');
        if (assertions.noErrors && errorEntries.length > 0) failures.push(`errorログ: ${errorEntries.length}件`);
        if (assertions.noWarnings && warningEntries.length > 0) failures.push(`warningログ: ${warningEntries.length}件`);

        const fps = Number(profiler.fps ?? 0);
        const frameMs = Number(profiler.frameMs ?? 0);
        if (assertions.minFps !== undefined && fps < assertions.minFps)
            failures.push(`FPS ${fps.toFixed(2)} < ${assertions.minFps}`);
        if (assertions.maxFrameMs !== undefined && frameMs > assertions.maxFrameMs)
            failures.push(`frameMs ${frameMs.toFixed(2)} > ${assertions.maxFrameMs}`);
        if (assertions.logContains !== undefined) {
            const needle = assertions.logContains.toLowerCase();
            const found = entries.some((entry) => String(entry.message ?? '').toLowerCase().includes(needle));
            if (!found) failures.push(`ログに「${assertions.logContains}」がありません`);
        }

        const visibleIds = new Set(capture.objects.filter((object) => object.visible).map((object) => object.id));
        for (const id of assertions.visibleNodeIds) {
            if (!visibleIds.has(id)) failures.push(`Node ${id} がviewportに表示されていません`);
        }

        const restoredState = await bus.Query({ t: 'editor.state' });
        const report = {
            passed: failures.length === 0,
            failures,
            initialState,
            observedState: state,
            restoredState,
            baselineCursor,
            logs,
            profiler,
            physicsEvents,
            semantic: {
                width: capture.width,
                height: capture.height,
                view: capture.view ?? view,
                cameraPosition: capture.cameraPosition,
                objects: capture.objects,
            },
        };
        return {
            content: [
                { type: 'text', text: JSON.stringify(report, null, 2) },
                { type: 'image', data: capture.base64, mimeType: capture.mimeType },
            ],
            structuredContent: report,
        };
    }));

    /// @note ── 検証ループ ──
    server.registerTool('scenario_run', {
        description: 'Playtest シナリオを Editor 側で開始します (すぐ戻ります)。playtest_run (MCP 側で実時間待機する簡易版) と違い、'
            + 'フレーム単位で進むのでビルドの速さに結果が左右されず、ファイルに残して --batch / CI でも同じものを回せます。'
            + 'path は projectRoot 相対の .playtest.json、'
            + 'または scenario にシナリオ本体を直接渡します。手順 (steps[].do) は play / stop / pause / resume / frames / '
            + 'bus / input / assert / waitUntil / replay / capture / compareImage / log。assert と waitUntil は '
            + '{query:{t:...}, path:"a.0.b", op:"==|!=|<|<=|>|>=|exists|missing|contains|length==|length>=|length<=", value} で'
            + 'バスの Query 結果を表明します。lockstep (既定 1/60) の間は実時間に依らず 1 フレーム = lockstep 秒で進みます。'
            + '基準画像は Tests/Golden/<baseline>.png。無いと失敗するので、初回は updateBaselines=true で作り、絵を目で確かめてから使います。'
            + '完了は scenario_status で確認します。',
        inputSchema: {
            path: z.string().min(1).max(1024).optional().describe('例: Tests/Playtests/Stage01_Start.playtest.json'),
            scenario: z.record(z.string(), JsonValueSchema).optional().describe('インラインのシナリオ {name, scene?, lockstep?, steps:[...]}'),
            updateBaselines: z.boolean().optional().describe('compareImage の基準画像を実画像で作り直す'),
            skipImages: z.boolean().optional().describe('compareImage を飛ばす'),
        },
        annotations: writeAnnotations,
    }, ({ path, scenario, updateBaselines, skipImages }) => run({
        t: 'playtest.run',
        ...(path === undefined ? {} : { path }),
        ...(scenario === undefined ? {} : { scenario }),
        ...(updateBaselines === undefined ? {} : { updateBaselines }),
        ...(skipImages === undefined ? {} : { skipImages }),
    }));

    server.registerTool('scenario_cancel', {
        description: '実行中の Playtest を中断します。Play は止まり、結果は failed として残ります。',
        inputSchema: {},
        annotations: writeAnnotations,
    }, () => run({ t: 'playtest.cancel' }));

    server.registerTool('input_record', {
        description: 'Play 中の人の操作を記録します。action=start で開始、stop で projectRoot 相対の path '
            + '(省略時 Tests/Playtests/Recordings/<日時>.inputrec.json) へ書き出します。'
            + '書き出した列はシナリオの {"do":"replay","file":"..."} で同じフレーム番号に再生されます。'
            + '再生の決定性は保証しません (乱数・非同期ロード)。結果は assert で見ます。',
        inputSchema: {
            action: z.enum(['start', 'stop']),
            path: z.string().min(1).max(1024).optional(),
        },
        annotations: writeAnnotations,
    }, ({ action, path }) => run({ t: 'input.record', action, ...(path === undefined ? {} : { path }) }));

    server.registerTool('visual_compare', {
        description: '今のビューポートを基準画像 Tests/Golden/<baseline>.png と 1 回だけ比べ、meanDiff (全画素の平均差 0..1) と'
            + 'badPixelRatio (差が pixelThreshold を越えた画素の割合) と、違う画素を赤で塗った差分画像を返します。'
            + 'シナリオの compareImage に書く閾値を決める前の下見に使います。updateBaseline=true で基準画像を今の絵で作り直します。',
        inputSchema: {
            baseline: z.string().min(1).max(128).regex(/^[A-Za-z0-9_\-/]+$/),
            view: z.enum(['game', 'scene']).default('game'),
            updateBaseline: z.boolean().optional(),
            pixelThreshold: z.number().min(0).max(1).optional(),
        },
        annotations: writeAnnotations,
    }, ({ baseline, view, updateBaseline, pixelThreshold }) => Safely(async () => {
        const raw = await bus.Command(EditorCommandSchema.parse({
            t: 'visual.compare', baseline, view,
            ...(updateBaseline === undefined ? {} : { updateBaseline }),
            ...(pixelThreshold === undefined ? {} : { pixelThreshold }),
        }), dryRun) as Record<string, unknown>;
        const { diffBase64, ...summary } = raw ?? {};
        const content: CallToolResult['content'] = [{ type: 'text', text: JSON.stringify(summary, null, 2) }];
        if (typeof diffBase64 === 'string' && diffBase64.length > 0) {
            content.push({ type: 'image', data: diffBase64, mimeType: 'image/png' });
        }
        return { content };
    }));
}
