// FBZZ Engine
// cli.ts | Projects/DevTools/BenchReport
// 使い方:
//   node Projects/DevTools/BenchReport/src/cli.ts report <json|dir...> --out <dir> [--title <題名>]
//                                                 [--baseline <label>] [--candidate <label>]
//   node Projects/DevTools/BenchReport/src/cli.ts index <Docs/benchmarks>
// 終了コード: 0 成功 / 1 入力不正 / 2 使い方の誤り
// 設計: Docs/design/benchmark-report.md

import { copyFileSync, existsSync, mkdirSync, readFileSync, readdirSync, statSync, writeFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { Compare, GroupByLabel } from './compare.ts';
import { RenderHtml } from './html.ts';
import { MakeSummary, RenderListing, type ReportSummary } from './listing.ts';
import { RenderMarkdown } from './markdown.ts';
import { ParseRun, type BenchRun } from './schema.ts';
import { RenderChart } from './svg.ts';

const REPO_ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..', '..', '..');
const DESIGN_DOC = path.join(REPO_ROOT, 'Docs', 'design', 'benchmark-report.md');

function Usage(): number {
    console.error('usage: cli.ts report <json|dir...> --out <dir> [--title <t>] [--baseline <label>] [--candidate <label>]');
    console.error('       cli.ts index <Docs/benchmarks>');
    return 2;
}

/** 引数を位置引数と --name value の組に分ける。 */
function SplitArgs(argv: string[]): { positional: string[]; named: Map<string, string> } {
    const positional: string[] = [];
    const named = new Map<string, string>();
    for (let i = 0; i < argv.length; i++) {
        if (argv[i].startsWith('--') && i + 1 < argv.length) {
            named.set(argv[i].slice(2), argv[i + 1]);
            i++;
        } else {
            positional.push(argv[i]);
        }
    }
    return { positional, named };
}

function CollectJson(inputs: string[]): string[] {
    const files: string[] = [];
    for (const input of inputs) {
        if (statSync(input).isDirectory()) {
            for (const name of readdirSync(input).sort()) {
                if (name.endsWith('.json')) files.push(path.join(input, name));
            }
        } else {
            files.push(input);
        }
    }
    return files;
}

function Today(): string {
    const now = new Date();
    const pad = (value: number) => String(value).padStart(2, '0');
    return `${now.getFullYear()}-${pad(now.getMonth() + 1)}-${pad(now.getDate())}`;
}

function ToPosix(relative: string): string {
    return relative.split(path.sep).join('/');
}

function Report(argv: string[]): number {
    const { positional, named } = SplitArgs(argv);
    const out = named.get('out');
    if (positional.length === 0 || !out) return Usage();

    const runs: BenchRun[] = CollectJson(positional).map((file) => ParseRun(readFileSync(file, 'utf8'), file));
    const groups = GroupByLabel(runs);
    const pick = (label: string | undefined, fallback: number) =>
        label === undefined ? groups[fallback] : groups.find((group) => group.label === label);
    const baseline = pick(named.get('baseline'), 0);
    const candidate = pick(named.get('candidate'), 1);
    if (!baseline || !candidate || baseline === candidate) {
        console.error(`基準と候補のラベルが揃わない。見つかったラベル: ${groups.map((group) => group.label).join(', ') || '(なし)'}`);
        return 1;
    }

    const comparison = Compare(baseline, candidate);
    const meta = {
        title: named.get('title') ?? `${candidate.label} と ${baseline.label} の比較`,
        date: Today(),
        designLink: ToPosix(path.relative(path.resolve(out), DESIGN_DOC)),
    };

    mkdirSync(path.join(out, 'raw'), { recursive: true });
    writeFileSync(path.join(out, 'chart.svg'), RenderChart(comparison) + '\n', 'utf8');
    writeFileSync(path.join(out, 'report.md'), RenderMarkdown(comparison, meta), 'utf8');
    writeFileSync(path.join(out, 'index.html'), RenderHtml(comparison, meta), 'utf8');
    writeFileSync(path.join(out, 'summary.json'), JSON.stringify(MakeSummary(comparison, meta), null, 2) + '\n', 'utf8');
    for (const group of [baseline, candidate]) {
        group.runs.forEach((run, index) => copyFileSync(run.source, path.join(out, 'raw', `${group.label}-${index + 1}.json`)));
    }

    console.log(`[BenchReport] ${comparison.rows.length} 項目 改善 ${comparison.counts.improved} / 変化なし ${comparison.counts.neutral} / ` +
        `注意 ${comparison.counts.caution} / 悪化 ${comparison.counts.regressed}`);
    for (const text of comparison.mismatches) console.log(`[BenchReport] 測定条件の不一致: ${text}`);
    console.log(`RESULT ok out=${out}`);
    return 0;
}

function Index(argv: string[]): number {
    const { positional } = SplitArgs(argv);
    const root = positional[0];
    if (!root) return Usage();
    mkdirSync(root, { recursive: true });
    const entries: [string, ReportSummary][] = [];
    for (const name of readdirSync(root)) {
        const file = path.join(root, name, 'summary.json');
        if (existsSync(file)) entries.push([name, JSON.parse(readFileSync(file, 'utf8')) as ReportSummary]);
    }
    writeFileSync(path.join(root, 'README.md'), RenderListing(entries), 'utf8');
    console.log(`RESULT ok reports=${entries.length} out=${path.join(root, 'README.md')}`);
    return 0;
}

function Main(argv: string[]): number {
    const [command, ...rest] = argv;
    try {
        if (command === 'report') return Report(rest);
        if (command === 'index') return Index(rest);
        return Usage();
    } catch (error) {
        console.error(`[BenchReport] ${(error as Error).message}`);
        return 1;
    }
}

process.exitCode = Main(process.argv.slice(2));
