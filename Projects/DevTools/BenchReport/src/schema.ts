// FBZZ Engine
// schema.ts | Projects/DevTools/BenchReport
// FBZZTestBench --measure が書く JSON (schema "fbzz-bench/1") の型と読み込み。
// 形の定義: Docs/design/benchmark-report.md §2

export interface BenchEnvironment {
    commit: string;
    dirty: boolean;
    cpu: string;
    logicalCores: number;
    compiler: string;
    config: string;
}

export interface BenchSettings {
    steps: number;
    warmup: number;
    repeats: number;
    ops: number;
    fixedStep: number;
}

export type ResultKind = 'scene' | 'micro';

export interface BenchResult {
    kind: ResultKind;
    name: string;
    phase: string;
    unit: string;
    samples: number[];
    median: number;
    min: number;
    max: number;
}

export interface BenchRun {
    schema: 'fbzz-bench/1';
    label: string;
    startedAt: string;
    environment: BenchEnvironment;
    settings: BenchSettings;
    results: BenchResult[];
    /** 読み込んだファイルのパス。レポートの raw/ へ複製するときに使う。 */
    source: string;
}

function Fail(source: string, message: string): never {
    throw new Error(`${source}: ${message}`);
}

function Expect<T>(source: string, value: unknown, check: (v: unknown) => boolean, what: string): T {
    if (!check(value)) Fail(source, `${what} が不正`);
    return value as T;
}

const isString = (v: unknown) => typeof v === 'string';
const isNumber = (v: unknown) => typeof v === 'number' && Number.isFinite(v);
const isObject = (v: unknown) => typeof v === 'object' && v !== null && !Array.isArray(v);

/** JSON テキストを検証して BenchRun にする。形が違えば例外 (CLI が捕まえて終了コード 1 にする)。 */
export function ParseRun(text: string, source: string): BenchRun {
    let data: unknown;
    try {
        data = JSON.parse(text);
    } catch (error) {
        Fail(source, `JSON として読めない (${(error as Error).message})`);
    }
    const root = Expect<Record<string, unknown>>(source, data, isObject, 'ルート');
    if (root.schema !== 'fbzz-bench/1') Fail(source, `未対応の schema: ${String(root.schema)}`);

    const env = Expect<Record<string, unknown>>(source, root.environment, isObject, 'environment');
    const settings = Expect<Record<string, unknown>>(source, root.settings, isObject, 'settings');
    const results = Expect<unknown[]>(source, root.results, Array.isArray, 'results');

    return {
        schema: 'fbzz-bench/1',
        label: Expect<string>(source, root.label, isString, 'label'),
        startedAt: Expect<string>(source, root.startedAt, isString, 'startedAt'),
        environment: {
            commit: Expect<string>(source, env.commit, isString, 'environment.commit'),
            dirty: env.dirty === true,
            cpu: Expect<string>(source, env.cpu, isString, 'environment.cpu'),
            logicalCores: Expect<number>(source, env.logicalCores, isNumber, 'environment.logicalCores'),
            compiler: Expect<string>(source, env.compiler, isString, 'environment.compiler'),
            config: Expect<string>(source, env.config, isString, 'environment.config'),
        },
        settings: {
            steps: Expect<number>(source, settings.steps, isNumber, 'settings.steps'),
            warmup: Expect<number>(source, settings.warmup, isNumber, 'settings.warmup'),
            repeats: Expect<number>(source, settings.repeats, isNumber, 'settings.repeats'),
            ops: Expect<number>(source, settings.ops, isNumber, 'settings.ops'),
            fixedStep: Expect<number>(source, settings.fixedStep, isNumber, 'settings.fixedStep'),
        },
        results: results.map((item, index) => {
            const r = Expect<Record<string, unknown>>(source, item, isObject, `results[${index}]`);
            const kind = r.kind === 'scene' || r.kind === 'micro' ? r.kind : Fail(source, `results[${index}].kind が不正`);
            const samples = Expect<unknown[]>(source, r.samples, Array.isArray, `results[${index}].samples`);
            if (samples.length === 0 || !samples.every(isNumber)) Fail(source, `results[${index}].samples が空か数値でない`);
            return {
                kind,
                name: Expect<string>(source, r.name, isString, `results[${index}].name`),
                phase: Expect<string>(source, r.phase, isString, `results[${index}].phase`),
                unit: Expect<string>(source, r.unit, isString, `results[${index}].unit`),
                samples: samples as number[],
                median: Expect<number>(source, r.median, isNumber, `results[${index}].median`),
                min: Expect<number>(source, r.min, isNumber, `results[${index}].min`),
                max: Expect<number>(source, r.max, isNumber, `results[${index}].max`),
            };
        }),
        source,
    };
}
