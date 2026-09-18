// FBZZ Engine
// markdown.ts | Projects/DevTools/BenchReport
// README・PR・コミット本文へそのまま貼れる Markdown のレポート。

import {
    DescribeCommit, FormatChange, FormatValue, NEUTRAL_BAND, PhaseText, REGRESSION_THRESHOLD, VERDICT_TEXT,
    type Comparison, type Row,
} from './compare.ts';
import type { ResultKind } from './schema.ts';
import { EscapeMarkdownCell } from './text.ts';

export interface ReportMeta {
    title: string;
    /** レポートを作った日 (YYYY-MM-DD)。 */
    date: string;
    /** 出力先から Docs/design/benchmark-report.md への相対パス (/ 区切り)。 */
    designLink: string;
}

const KIND_TITLE: Record<ResultKind, string> = {
    scene: '物理の場面 (1 刻みあたり)',
    micro: 'Math の演算 (1 演算あたり)',
};

function Table(rows: Row[]): string[] {
    const lines = [
        '| 項目 | 計測 | 基準 | 候補 | 変化 | 判定 | ばらつき |',
        '|---|---|---:|---:|---:|---|---:|',
    ];
    for (const row of rows) {
        const spread = Math.max(row.baseline.spread, row.candidate.spread);
        const verdict = VERDICT_TEXT[row.verdict] + (row.withinNoise && row.verdict !== 'neutral' ? ' (ノイズ内)' : '');
        lines.push(`| ${EscapeMarkdownCell(row.name)} | ${PhaseText(row.kind, row.phase)} | ${FormatValue(row.baseline.value)} ${row.unit} | ` +
            `${FormatValue(row.candidate.value)} ${row.unit} | **${FormatChange(row.change)}** | ${verdict} | ${(spread * 100).toFixed(1)}% |`);
    }
    return lines;
}

export function RenderMarkdown(comparison: Comparison, meta: ReportMeta, chartFile = 'chart.svg'): string {
    const { baseline, candidate } = comparison;
    const env = candidate.environment;
    const settings = candidate.settings;
    const lines: string[] = [];

    lines.push(`# ${meta.title}`, '');
    const headline: string[] = [];
    for (const kind of ['scene', 'micro'] as const) {
        const summary = comparison.summary[kind];
        if (summary) headline.push(`${KIND_TITLE[kind]}: **${FormatChange(summary.change)}** (${summary.count} 項目の幾何平均)`);
    }
    if (headline.length > 0) lines.push(...headline.map((text) => `- ${text}`), '');
    lines.push(`改善 ${comparison.counts.improved} / 変化なし ${comparison.counts.neutral} / 注意 ${comparison.counts.caution} / 悪化 ${comparison.counts.regressed}`, '');
    lines.push(`![${EscapeMarkdownCell(candidate.label)} の変化率](${chartFile})`, '');

    for (const kind of ['scene', 'micro'] as const) {
        const rows = comparison.rows.filter((row) => row.kind === kind);
        if (rows.length === 0) continue;
        lines.push(`## ${KIND_TITLE[kind]}`, '', ...Table(rows), '');
    }

    lines.push('## 測定条件', '');
    lines.push('| 項目 | 値 |', '|---|---|');
    lines.push(`| 基準 (${EscapeMarkdownCell(baseline.label)}) | \`${DescribeCommit(baseline.environment)}\` × ${baseline.runs.length} 回 |`);
    lines.push(`| 候補 (${EscapeMarkdownCell(candidate.label)}) | \`${DescribeCommit(env)}\` × ${candidate.runs.length} 回 |`);
    lines.push(`| CPU | ${EscapeMarkdownCell(env.cpu)} (論理 ${env.logicalCores} コア。計測スレッドは 1 コアに固定) |`);
    lines.push(`| ビルド | ${EscapeMarkdownCell(env.compiler)} / ${EscapeMarkdownCell(env.config)} |`);
    lines.push(`| 場面 | warmup ${settings.warmup} 刻み → ${settings.steps} 刻み × ${settings.repeats} 回の中央値 (固定刻み ${(settings.fixedStep * 1000).toFixed(2)} ms) |`);
    lines.push(`| 演算 | ${settings.ops.toLocaleString('en-US')} 演算 × ${settings.repeats} 回の中央値 |`);
    lines.push(`| 作成日 | ${meta.date} |`, '');

    if (comparison.mismatches.length > 0) {
        lines.push('> [!WARNING]', '> 基準と候補で測定条件が異なる。この比較は参考値。', ...comparison.mismatches.map((text) => `> - ${EscapeMarkdownCell(text)}`), '');
    }

    lines.push('## 判定の基準', '');
    lines.push(`- 変化 = (候補 − 基準) / 基準。値は各実行の中央値の中央値で、基準と候補を交互に実行して測った`);
    lines.push(`- 改善: −${NEUTRAL_BAND * 100}% 以下 / 変化なし: ±${NEUTRAL_BAND * 100}% 未満 / 注意: +${NEUTRAL_BAND * 100}〜${REGRESSION_THRESHOLD * 100}% / 悪化: +${REGRESSION_THRESHOLD * 100}% 以上`);
    lines.push('- ばらつき = 各実行の中央値の (最大 − 最小) / 代表値。変化がこれより小さいものは «ノイズ内»');
    lines.push(`- 方法の詳細: [Docs/design/benchmark-report.md](${meta.designLink})`, '');
    return lines.join('\n');
}
