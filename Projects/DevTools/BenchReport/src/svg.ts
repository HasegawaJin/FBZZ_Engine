// FBZZ Engine
// svg.ts | Projects/DevTools/BenchReport
// 変化率の発散型横棒グラフ (0% を中心に、速くなった項目を左、遅くなった項目を右へ伸ばす)。
// WHY 変化率か: 計測値は us/step と ns/op が混在し、桁も 0.04〜90 と離れている。絶対値の棒では比べられない。
// README に <img> で貼っても配色が OS のダークモードに追従するよう、色は <style> の media query で持つ。

import { FormatChange, PhaseText, VERDICT_TEXT, type Comparison, type Row } from './compare.ts';
import { EscapeXml } from './text.ts';

const WIDTH = 760;
const LABEL_WIDTH = 300;
const VALUE_GUTTER = 64;
const ROW_HEIGHT = 26;
const BAR_HEIGHT = 14;
const TOP = 40;
const BOTTOM = 34;

/** 軸の片側の長さ。最大の変化を収める «きりのいい» 値にする。 */
function AxisExtent(rows: Row[]): number {
    const largest = Math.max(0.05, ...rows.map((row) => Math.abs(row.change)));
    for (const step of [0.1, 0.25, 0.5, 0.75, 1, 1.5, 2, 3, 5, 10]) {
        if (largest <= step) return step;
    }
    return Math.ceil(largest);
}

export const CHART_STYLE = `
  .fbzz-chart { --improved:#1a7f37; --neutral:#8c959f; --caution:#9a6700; --regressed:#cf222e;
                --ink:#1f2328; --muted:#59636e; --grid:#d1d9e0; font-family: system-ui, -apple-system, "Segoe UI", "Yu Gothic UI", sans-serif; }
  @media (prefers-color-scheme: dark) {
    .fbzz-chart { --improved:#3fb950; --neutral:#6e7681; --caution:#d29922; --regressed:#f85149;
                  --ink:#e6edf3; --muted:#9198a1; --grid:#3d444d; }
  }
  .fbzz-chart .label { fill: var(--ink); font-size: 12px; }
  .fbzz-chart .sub { fill: var(--muted); font-size: 11px; }
  .fbzz-chart .value { fill: var(--ink); font-size: 11.5px; font-variant-numeric: tabular-nums; }
  .fbzz-chart .tick { fill: var(--muted); font-size: 11px; font-variant-numeric: tabular-nums; }
  .fbzz-chart .grid { stroke: var(--grid); stroke-width: 1; }
  .fbzz-chart .zero { stroke: var(--muted); stroke-width: 1.25; }
  .fbzz-chart .bar-improved { fill: var(--improved); }
  .fbzz-chart .bar-neutral { fill: var(--neutral); }
  .fbzz-chart .bar-caution { fill: var(--caution); }
  .fbzz-chart .bar-regressed { fill: var(--regressed); }
`;

export interface ChartOptions {
    /** false なら <style> を埋め込まない (HTML 側がまとめて持つとき)。 */
    embedStyle?: boolean;
}

export function RenderChart(comparison: Comparison, options: ChartOptions = {}): string {
    const rows = comparison.rows;
    const height = TOP + rows.length * ROW_HEIGHT + BOTTOM;
    const plotLeft = LABEL_WIDTH + VALUE_GUTTER;
    const plotRight = WIDTH - VALUE_GUTTER;
    const center = (plotLeft + plotRight) / 2;
    const extent = AxisExtent(rows);
    const scale = (plotRight - center) / extent;

    const parts: string[] = [];
    const title = `${comparison.candidate.label} の ${comparison.baseline.label} に対する変化率`;
    parts.push(`<svg xmlns="http://www.w3.org/2000/svg" class="fbzz-chart" viewBox="0 0 ${WIDTH} ${height}" width="${WIDTH}" height="${height}" role="img" aria-labelledby="chart-title chart-desc">`);
    parts.push(`<title id="chart-title">${EscapeXml(title)}</title>`);
    parts.push(`<desc id="chart-desc">${EscapeXml(rows.map((row) => `${row.name} ${PhaseText(row.kind, row.phase)}: ${FormatChange(row.change)} (${VERDICT_TEXT[row.verdict]})`).join('。'))}</desc>`);
    if (options.embedStyle !== false) parts.push(`<style>${CHART_STYLE}</style>`);

    parts.push(`<text class="sub" x="${center - 8}" y="18" text-anchor="end">← 速くなった</text>`);
    parts.push(`<text class="sub" x="${center + 8}" y="18">遅くなった →</text>`);

    for (const fraction of [-1, -0.5, 0.5, 1]) {
        const x = center + fraction * extent * scale;
        parts.push(`<line class="grid" x1="${x}" x2="${x}" y1="${TOP - 8}" y2="${height - BOTTOM + 4}"/>`);
        parts.push(`<text class="tick" x="${x}" y="${height - 12}" text-anchor="middle">${FormatChange(fraction * extent)}</text>`);
    }
    parts.push(`<text class="tick" x="${center}" y="${height - 12}" text-anchor="middle">0%</text>`);

    rows.forEach((row, index) => {
        const y = TOP + index * ROW_HEIGHT;
        const middle = y + ROW_HEIGHT / 2;
        const width = Math.max(1.5, Math.abs(row.change) * scale);
        const x = row.change < 0 ? center - width : center;
        const phase = PhaseText(row.kind, row.phase);
        parts.push(`<text class="label" x="${LABEL_WIDTH - 8}" y="${middle + 4}" text-anchor="end">${EscapeXml(row.name)}<tspan class="sub"> · ${EscapeXml(phase)}</tspan></text>`);
        parts.push(`<rect class="bar-${row.verdict}" x="${x.toFixed(1)}" y="${(middle - BAR_HEIGHT / 2).toFixed(1)}" width="${width.toFixed(1)}" height="${BAR_HEIGHT}" rx="2"><title>${EscapeXml(`${row.name} ${phase}: ${FormatChange(row.change)}`)}</title></rect>`);
        const valueX = row.change < 0 ? x - 6 : x + width + 6;
        parts.push(`<text class="value" x="${valueX.toFixed(1)}" y="${middle + 4}" text-anchor="${row.change < 0 ? 'end' : 'start'}">${FormatChange(row.change)}</text>`);
    });

    parts.push(`<line class="zero" x1="${center}" x2="${center}" y1="${TOP - 8}" y2="${height - BOTTOM + 4}"/>`);
    parts.push('</svg>');
    return parts.join('\n');
}
