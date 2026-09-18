// FBZZ Engine
// html.ts | Projects/DevTools/BenchReport
// ポートフォリオにそのまま置ける 1 ファイル完結の HTML レポート (外部の CSS / JS / フォントを読まない)。

import {
    DescribeCommit, FormatChange, FormatValue, NEUTRAL_BAND, PhaseText, REGRESSION_THRESHOLD, VERDICT_TEXT,
    type Comparison, type Row,
} from './compare.ts';
import type { ReportMeta } from './markdown.ts';
import type { ResultKind } from './schema.ts';
import { CHART_STYLE, RenderChart } from './svg.ts';
import { EscapeXml } from './text.ts';

const KIND_TITLE: Record<ResultKind, string> = {
    scene: '物理の場面',
    micro: 'Math の演算',
};

const KIND_NOTE: Record<ResultKind, string> = {
    scene: 'TestBench の各場面を固定刻みで進めたときの 1 刻みあたりの時間',
    micro: '1024 要素の入力を繰り返し処理したときの 1 演算あたりの時間',
};

const PAGE_STYLE = `
:root {
  color-scheme: light dark;
  --bg: #f6f8fa; --surface: #ffffff; --ink: #1f2328; --muted: #59636e; --line: #d1d9e0; --soft: #eef1f4;
  --improved: #1a7f37; --neutral: #6e7781; --caution: #9a6700; --regressed: #cf222e;
  --improved-bg: #dafbe1; --neutral-bg: #eff2f5; --caution-bg: #fff8c5; --regressed-bg: #ffebe9;
}
@media (prefers-color-scheme: dark) {
  :root {
    --bg: #0d1117; --surface: #151b23; --ink: #e6edf3; --muted: #9198a1; --line: #3d444d; --soft: #212830;
    --improved: #3fb950; --neutral: #9198a1; --caution: #d29922; --regressed: #f85149;
    --improved-bg: #12261e; --neutral-bg: #212830; --caution-bg: #272115; --regressed-bg: #2d1619;
  }
}
* { box-sizing: border-box; }
body { margin: 0; background: var(--bg); color: var(--ink);
  font: 15px/1.65 system-ui, -apple-system, "Segoe UI", "Yu Gothic UI", "Hiragino Sans", "Noto Sans JP", sans-serif; }
main { max-width: 980px; margin: 0 auto; padding: 40px 16px 64px; }
header .eyebrow { color: var(--muted); font-size: 13px; letter-spacing: .04em; margin: 0 0 6px; }
h1 { font-size: clamp(22px, 4vw, 30px); line-height: 1.3; margin: 0 0 8px; }
h2 { font-size: 18px; margin: 40px 0 4px; }
.lede { color: var(--muted); margin: 0; }
.note { color: var(--muted); font-size: 13px; margin: 0 0 12px; }
code { font: 13px ui-monospace, "Cascadia Code", Consolas, monospace; background: var(--soft); padding: 1px 6px; border-radius: 4px; overflow-wrap: anywhere; }
.tiles { display: grid; grid-template-columns: repeat(auto-fit, minmax(min(200px, 100%), 1fr)); gap: 12px; margin: 28px 0 0; }
.tile { background: var(--surface); border: 1px solid var(--line); border-radius: 10px; padding: 16px 18px; }
.tile .label { color: var(--muted); font-size: 13px; margin: 0; }
.tile .big { font-size: 32px; font-weight: 650; line-height: 1.2; margin: 4px 0 2px; font-variant-numeric: tabular-nums; }
.tile .sub { color: var(--muted); font-size: 12.5px; margin: 0; }
.big.improved { color: var(--improved); } .big.regressed { color: var(--regressed); } .big.caution { color: var(--caution); }
.counts { display: flex; flex-wrap: wrap; gap: 6px; margin-top: 8px; }
.panel { background: var(--surface); border: 1px solid var(--line); border-radius: 10px; padding: 16px; margin-top: 12px; }
.chart { overflow-x: auto; }
.chart svg { display: block; max-width: 100%; height: auto; margin: 0 auto; }
.chart .compact { display: none; }
.table-wrap { overflow-x: auto; }
table { width: 100%; border-collapse: collapse; font-variant-numeric: tabular-nums; }
th, td { padding: 8px 10px; border-bottom: 1px solid var(--line); text-align: left; white-space: nowrap; }
th { color: var(--muted); font-weight: 600; font-size: 12.5px; }
td.num, th.num { text-align: right; }
td.name { white-space: normal; min-width: 180px; }
tr:last-child td { border-bottom: 0; }
.pill { display: inline-block; padding: 1px 9px; border-radius: 999px; font-size: 12.5px; font-weight: 600; }
.pill.improved { color: var(--improved); background: var(--improved-bg); }
.pill.neutral { color: var(--neutral); background: var(--neutral-bg); }
.pill.caution { color: var(--caution); background: var(--caution-bg); }
.pill.regressed { color: var(--regressed); background: var(--regressed-bg); }
.change { font-weight: 650; }
.change.improved { color: var(--improved); } .change.regressed { color: var(--regressed); } .change.caution { color: var(--caution); }
.noise { color: var(--muted); font-size: 12px; margin-left: 4px; }
dl { display: grid; grid-template-columns: max-content 1fr; gap: 6px 20px; margin: 0; }
dt { color: var(--muted); } dd { margin: 0; min-width: 0; overflow-wrap: anywhere; }
.warning { border-left: 4px solid var(--caution); background: var(--caution-bg); padding: 12px 16px; border-radius: 6px; margin-top: 16px; }
ul.method { margin: 0; padding-left: 20px; } ul.method li { margin: 4px 0; }
footer { color: var(--muted); font-size: 12.5px; margin-top: 48px; }
@media (max-width: 560px) {
  dl { grid-template-columns: 1fr; gap: 2px; } dd { margin-bottom: 8px; }
  .chart .wide { display: none; }
  .chart .compact { display: block; }
}
`;

function SummaryTiles(comparison: Comparison): string {
    const tiles: string[] = [];
    for (const kind of ['scene', 'micro'] as const) {
        const summary = comparison.summary[kind];
        if (!summary) continue;
        const tone = summary.change <= -NEUTRAL_BAND ? 'improved' : summary.change >= REGRESSION_THRESHOLD ? 'regressed'
            : summary.change >= NEUTRAL_BAND ? 'caution' : '';
        tiles.push(`<div class="tile"><p class="label">${KIND_TITLE[kind]}</p><p class="big ${tone}">${FormatChange(summary.change)}</p>` +
            `<p class="sub">${summary.count} 項目の幾何平均</p></div>`);
    }
    const counts = comparison.counts;
    tiles.push(`<div class="tile"><p class="label">判定</p><div class="counts">` +
        (['improved', 'neutral', 'caution', 'regressed'] as const)
            .map((verdict) => `<span class="pill ${verdict}">${VERDICT_TEXT[verdict]} ${counts[verdict]}</span>`).join('') +
        `</div><p class="sub" style="margin-top:10px">全 ${comparison.rows.length} 項目</p></div>`);
    return `<section class="tiles" aria-label="概要">${tiles.join('')}</section>`;
}

function RowHtml(row: Row): string {
    const phase = PhaseText(row.kind, row.phase);
    const runs = (side: Row['baseline']) => side.runs.map((value) => FormatValue(value)).join(' / ');
    const spread = Math.max(row.baseline.spread, row.candidate.spread);
    const noise = row.withinNoise && row.verdict !== 'neutral' ? '<span class="noise">ノイズ内</span>' : '';
    return `<tr>
  <td class="name">${EscapeXml(row.name)}</td>
  <td>${EscapeXml(phase)}</td>
  <td class="num" title="各実行: ${EscapeXml(runs(row.baseline))}">${FormatValue(row.baseline.value)} <span class="noise">${EscapeXml(row.unit)}</span></td>
  <td class="num" title="各実行: ${EscapeXml(runs(row.candidate))}">${FormatValue(row.candidate.value)} <span class="noise">${EscapeXml(row.unit)}</span></td>
  <td class="num"><span class="change ${row.verdict}">${FormatChange(row.change)}</span></td>
  <td><span class="pill ${row.verdict}">${VERDICT_TEXT[row.verdict]}</span>${noise}</td>
  <td class="num">${(spread * 100).toFixed(1)}%</td>
</tr>`;
}

function KindSection(comparison: Comparison, kind: ResultKind): string {
    const rows = comparison.rows.filter((row) => row.kind === kind);
    if (rows.length === 0) return '';
    return `<section>
<h2>${KIND_TITLE[kind]}</h2>
<p class="note">${KIND_NOTE[kind]}。数値にカーソルを合わせると各実行の値が出る。</p>
<div class="panel table-wrap"><table>
<thead><tr><th>項目</th><th>計測</th><th class="num">基準</th><th class="num">候補</th><th class="num">変化</th><th>判定</th><th class="num">ばらつき</th></tr></thead>
<tbody>
${rows.map(RowHtml).join('\n')}
</tbody></table></div>
</section>`;
}

export function RenderHtml(comparison: Comparison, meta: ReportMeta): string {
    const { baseline, candidate } = comparison;
    const env = candidate.environment;
    const settings = candidate.settings;
    const warning = comparison.mismatches.length === 0 ? '' :
        `<div class="warning"><strong>基準と候補で測定条件が異なる。この比較は参考値。</strong><ul class="method">` +
        comparison.mismatches.map((text) => `<li>${EscapeXml(text)}</li>`).join('') + '</ul></div>';

    return `<!doctype html>
<html lang="ja">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>${EscapeXml(meta.title)}</title>
<style>${PAGE_STYLE}${CHART_STYLE}</style>
</head>
<body>
<main>
<header>
<p class="eyebrow">FBZZ Engine · ベンチマーク · ${EscapeXml(meta.date)}</p>
<h1>${EscapeXml(meta.title)}</h1>
<p class="lede">基準 <code>${EscapeXml(DescribeCommit(baseline.environment))}</code> と候補 <code>${EscapeXml(DescribeCommit(env))}</code> を、同じマシンで交互に ${baseline.runs.length} 回ずつ実行して比べた。</p>
</header>
${SummaryTiles(comparison)}
${warning}
<section>
<h2>変化率</h2>
<p class="note">0% を中心に、速くなった項目を左、遅くなった項目を右へ伸ばしている。色は判定。</p>
<div class="panel chart"><div class="wide">${RenderChart(comparison, { embedStyle: false, idPrefix: 'chart-wide' })}</div><div class="compact">${RenderChart(comparison, { embedStyle: false, compact: true, idPrefix: 'chart-compact' })}</div></div>
</section>
${KindSection(comparison, 'scene')}
${KindSection(comparison, 'micro')}
<section>
<h2>測定条件</h2>
<div class="panel"><dl>
<dt>基準 (${EscapeXml(baseline.label)})</dt><dd><code>${EscapeXml(baseline.environment.commit)}</code>${baseline.environment.dirty ? ' + 未コミットの変更' : ''} × ${baseline.runs.length} 回</dd>
<dt>候補 (${EscapeXml(candidate.label)})</dt><dd><code>${EscapeXml(env.commit)}</code>${env.dirty ? ' + 未コミットの変更' : ''} × ${candidate.runs.length} 回</dd>
<dt>CPU</dt><dd>${EscapeXml(env.cpu)} (論理 ${env.logicalCores} コア。計測スレッドは 1 コアに固定し、優先度を上げた)</dd>
<dt>ビルド</dt><dd>${EscapeXml(env.compiler)} / ${EscapeXml(env.config)}</dd>
<dt>場面</dt><dd>warmup ${settings.warmup} 刻み → ${settings.steps} 刻み × ${settings.repeats} 回の中央値 (固定刻み ${(settings.fixedStep * 1000).toFixed(2)} ms)</dd>
<dt>演算</dt><dd>${settings.ops.toLocaleString('en-US')} 演算 × ${settings.repeats} 回の中央値</dd>
</dl></div>
</section>
<section>
<h2>判定の基準</h2>
<div class="panel"><ul class="method">
<li>変化 = (候補 − 基準) / 基準。値は各実行の中央値の中央値。</li>
<li>改善 −${NEUTRAL_BAND * 100}% 以下 / 変化なし ±${NEUTRAL_BAND * 100}% 未満 / 注意 +${NEUTRAL_BAND * 100}〜${REGRESSION_THRESHOLD * 100}% / 悪化 +${REGRESSION_THRESHOLD * 100}% 以上。</li>
<li>ばらつき = 各実行の中央値の (最大 − 最小) / 代表値。変化がこれより小さいものは «ノイズ内» と注記した。</li>
<li>基準と候補を交互に実行し、マシンの状態の変化による偏りを打ち消している。</li>
</ul></div>
</section>
<footer>Projects/DevTools/BenchReport で生成。元データは同じフォルダーの raw/ にある。</footer>
</main>
</body>
</html>
`;
}
