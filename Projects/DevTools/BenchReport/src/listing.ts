// FBZZ Engine
// listing.ts | Projects/DevTools/BenchReport
// Docs/benchmarks/ の各レポートの summary.json を集め、一覧 README.md を作る (最適化の履歴)。

import type { Comparison, Verdict } from './compare.ts';
import { FormatChange } from './compare.ts';
import type { ReportMeta } from './markdown.ts';
import type { ResultKind } from './schema.ts';
import { EscapeMarkdownCell } from './text.ts';

export interface ReportSummary {
    schema: 'fbzz-bench-report/1';
    title: string;
    date: string;
    baseline: { label: string; commit: string; dirty: boolean };
    candidate: { label: string; commit: string; dirty: boolean };
    cpu: string;
    config: string;
    summary: Record<ResultKind, { change: number; count: number } | null>;
    counts: Record<Verdict, number>;
}

export function MakeSummary(comparison: Comparison, meta: ReportMeta): ReportSummary {
    const side = (group: Comparison['baseline']) => ({
        label: group.label, commit: group.environment.commit, dirty: group.environment.dirty,
    });
    return {
        schema: 'fbzz-bench-report/1',
        title: meta.title,
        date: meta.date,
        baseline: side(comparison.baseline),
        candidate: side(comparison.candidate),
        cpu: comparison.candidate.environment.cpu,
        config: comparison.candidate.environment.config,
        summary: comparison.summary,
        counts: comparison.counts,
    };
}

function Commit(side: ReportSummary['baseline']): string {
    const short = side.commit.length > 12 ? side.commit.slice(0, 8) : side.commit;
    return side.dirty ? `${short}+` : short;
}

/** entries は [ディレクトリ名, summary] の組。新しい順に並べて表にする。 */
export function RenderListing(entries: [string, ReportSummary][]): string {
    const sorted = [...entries].sort((a, b) => (a[1].date === b[1].date ? b[0].localeCompare(a[0]) : b[1].date.localeCompare(a[1].date)));
    const lines = [
        '# ベンチマーク',
        '',
        '最適化の前後を `Tools/BenchCompare.ps1` で測った結果の一覧 (新しい順)。方法は [Docs/design/benchmark-report.md](../design/benchmark-report.md)。',
        'このファイルは `Projects/DevTools/BenchReport` が作り直す。手で編集しない。',
        '',
        '| 日付 | 題名 | 基準 → 候補 | 物理の場面 | Math の演算 | 改善 / 悪化 | CPU |',
        '|---|---|---|---:|---:|---:|---|',
    ];
    const cell = (value: { change: number } | null) => (value ? FormatChange(value.change) : '—');
    for (const [directory, summary] of sorted) {
        lines.push(`| ${summary.date} | [${EscapeMarkdownCell(summary.title)}](${directory}/report.md) · [HTML](${directory}/index.html) | ` +
            `\`${Commit(summary.baseline)}\` → \`${Commit(summary.candidate)}\` | ${cell(summary.summary.scene)} | ${cell(summary.summary.micro)} | ` +
            `${summary.counts.improved} / ${summary.counts.regressed} | ${EscapeMarkdownCell(summary.cpu)} |`);
    }
    if (sorted.length === 0) lines.push('| — | まだ無い | | | | | |');
    lines.push('', '`+` は未コミットの変更を含むビルド。変化は各項目の比の幾何平均。', '');
    return lines.join('\n');
}
