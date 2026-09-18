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
    /** true なら狭い画面向けに、項目名を棒の上へ置いた幅 360 の縦積み版を描く。 */
    compact?: boolean;
    /** 同じページに複数描くときの title / desc の id の接頭辞。 */
    idPrefix?: string;
}

/** 幅の広い版は項目名を左に、狭い版は棒の上に置く。どちらも棒は中央の 0% から伸ばす。 */
interface Layout {
    width: number;
    rowHeight: number;
    plotLeft: number;
    plotRight: number;
    /** 行の上端から棒の中心までの距離。 */
    barOffset: number;
    labelX: number;
    labelAnchor: 'start' | 'end';
    /** 行の上端から項目名のベースラインまでの距離。 */
    labelOffset: number;
}

const WIDE: Layout = {
    width: WIDTH, rowHeight: ROW_HEIGHT, plotLeft: LABEL_WIDTH + VALUE_GUTTER, plotRight: WIDTH - VALUE_GUTTER,
    barOffset: ROW_HEIGHT / 2, labelX: LABEL_WIDTH - 8, labelAnchor: 'end', labelOffset: ROW_HEIGHT / 2 + 4,
};

const COMPACT: Layout = {
    width: 360, rowHeight: 44, plotLeft: 52, plotRight: 308,
    barOffset: 30, labelX: 0, labelAnchor: 'start', labelOffset: 14,
};

export function RenderChart(comparison: Comparison, options: ChartOptions = {}): string {
    const rows = comparison.rows;
    const layout = options.compact ? COMPACT : WIDE;
    const prefix = options.idPrefix ?? 'chart';
    const height = TOP + rows.length * layout.rowHeight + BOTTOM;
    const plotLeft = layout.plotLeft;
    const plotRight = layout.plotRight;
    const center = (plotLeft + plotRight) / 2;
    const extent = AxisExtent(rows);
    const scale = (plotRight - center) / extent;

    const parts: string[] = [];
    const title = `${comparison.candidate.label} の ${comparison.baseline.label} に対する変化率`;
    parts.push(`<svg xmlns="http://www.w3.org/2000/svg" class="fbzz-chart" viewBox="0 0 ${layout.width} ${height}" width="${layout.width}" height="${height}" role="img" aria-labelledby="${prefix}-title ${prefix}-desc">`);
    parts.push(`<title id="${prefix}-title">${EscapeXml(title)}</title>`);
    parts.push(`<desc id="${prefix}-desc">${EscapeXml(rows.map((row) => `${row.name} ${PhaseText(row.kind, row.phase)}: ${FormatChange(row.change)} (${VERDICT_TEXT[row.verdict]})`).join('。'))}</desc>`);
    if (options.embedStyle !== false) parts.push(`<style>${CHART_STYLE}</style>`);

    parts.push(`<text class="sub" x="${center - 8}" y="18" text-anchor="end">← 速くなった</text>`);
    parts.push(`<text class="sub" x="${center + 8}" y="18">遅くなった →</text>`);

    for (const fraction of [-1, -0.5, 0.5, 1]) {
        const x = center + fraction * extent * scale;
        // 狭い版は項目名が棒の上の全幅に載るので、縦の補助線を引くと文字に重なる。目盛りの数字だけ残す。
        if (!options.compact) parts.push(`<line class="grid" x1="${x}" x2="${x}" y1="${TOP - 8}" y2="${height - BOTTOM + 4}"/>`);
        parts.push(`<text class="tick" x="${x}" y="${height - 12}" text-anchor="middle">${FormatChange(fraction * extent)}</text>`);
    }
    parts.push(`<text class="tick" x="${center}" y="${height - 12}" text-anchor="middle">0%</text>`);

    rows.forEach((row, index) => {
        const y = TOP + index * layout.rowHeight;
        const middle = y + layout.barOffset;
        const width = Math.max(1.5, Math.abs(row.change) * scale);
        const x = row.change < 0 ? center - width : center;
        const phase = PhaseText(row.kind, row.phase);
        parts.push(`<text class="label" x="${layout.labelX}" y="${y + layout.labelOffset}" text-anchor="${layout.labelAnchor}">${EscapeXml(row.name)}<tspan class="sub"> · ${EscapeXml(phase)}</tspan></text>`);
        parts.push(`<rect class="bar-${row.verdict}" x="${x.toFixed(1)}" y="${(middle - BAR_HEIGHT / 2).toFixed(1)}" width="${width.toFixed(1)}" height="${BAR_HEIGHT}" rx="2"><title>${EscapeXml(`${row.name} ${phase}: ${FormatChange(row.change)}`)}</title></rect>`);
        const valueX = row.change < 0 ? x - 6 : x + width + 6;
        parts.push(`<text class="value" x="${valueX.toFixed(1)}" y="${middle + 4}" text-anchor="${row.change < 0 ? 'end' : 'start'}">${FormatChange(row.change)}</text>`);
    });

    parts.push(`<line class="zero" x1="${center}" x2="${center}" y1="${TOP - 8}" y2="${height - BOTTOM + 4}"/>`);
    parts.push('</svg>');
    return parts.join('\n');
}
