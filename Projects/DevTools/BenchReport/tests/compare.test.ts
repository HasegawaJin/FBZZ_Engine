// FBZZ Engine
// compare.test.ts | Projects/DevTools/BenchReport
// 集計・判定・描画の単体テスト。実行: node --test "Projects/DevTools/BenchReport/tests/*.test.ts"

import assert from 'node:assert/strict';
import { test } from 'node:test';
import { Classify, Compare, GroupByLabel, Median } from '../src/compare.ts';
import { RenderHtml } from '../src/html.ts';
import { RenderListing, MakeSummary } from '../src/listing.ts';
import { RenderMarkdown } from '../src/markdown.ts';
import { ParseRun, type BenchRun } from '../src/schema.ts';
import { RenderChart } from '../src/svg.ts';

function Run(label: string, values: Record<string, number>, cpu = 'Test CPU'): BenchRun {
    return {
        schema: 'fbzz-bench/1',
        label,
        startedAt: '2026-09-18T00:00:00Z',
        environment: { commit: `${label}-commit`, dirty: false, cpu, logicalCores: 8, compiler: 'MSVC 19.51.0', config: 'Development' },
        settings: { steps: 600, warmup: 120, repeats: 7, ops: 200000, fixedStep: 1 / 60 },
        results: Object.entries(values).map(([name, value]) => ({
            kind: name.startsWith('micro:') ? 'micro' as const : 'scene' as const,
            name: name.replace(/^micro:/, ''),
            phase: name.startsWith('micro:') ? 'op' : 'simulate',
            unit: name.startsWith('micro:') ? 'ns/op' : 'us/step',
            samples: [value], median: value, min: value, max: value,
        })),
        source: `${label}.json`,
    };
}

const META = { title: 'テスト <比較> & 検証', date: '2026-09-18', designLink: '../../design/benchmark-report.md' };

test('Median は奇数個なら中央、偶数個なら中央 2 つの平均', () => {
    assert.equal(Median([3, 1, 2]), 2);
    assert.equal(Median([4, 1, 3, 2]), 2.5);
});

test('Classify は ±3% を変化なし、+3〜5% を注意、+5% 以上を悪化にする', () => {
    assert.equal(Classify(-0.03), 'improved');
    assert.equal(Classify(-0.029), 'neutral');
    assert.equal(Classify(0.029), 'neutral');
    assert.equal(Classify(0.03), 'caution');
    assert.equal(Classify(0.049), 'caution');
    assert.equal(Classify(0.05), 'regressed');
});

test('Compare は実行ごとの中央値の中央値で比べ、何もしていない項目を外す', () => {
    const runs = [
        Run('base', { A: 10, B: 1.0, Idle: 0.01, 'micro:M': 4 }),
        Run('cand', { A: 5, B: 1.1, Idle: 0.02, 'micro:M': 4.05 }),
        Run('base', { A: 12, B: 1.0, Idle: 0.01, 'micro:M': 4 }),
        Run('cand', { A: 6, B: 1.1, Idle: 0.02, 'micro:M': 4.05 }),
        Run('base', { A: 11, B: 1.0, Idle: 0.01, 'micro:M': 4 }),
        Run('cand', { A: 5.5, B: 1.1, Idle: 0.02, 'micro:M': 4.05 }),
    ];
    const [baseline, candidate] = GroupByLabel(runs);
    const comparison = Compare(baseline, candidate);

    assert.deepEqual(comparison.rows.map((row) => row.name), ['A', 'B', 'M']);
    const a = comparison.rows[0];
    assert.equal(a.baseline.value, 11);
    assert.equal(a.candidate.value, 5.5);
    assert.equal(a.change, -0.5);
    assert.equal(a.verdict, 'improved');
    assert.equal(comparison.rows[1].verdict, 'regressed');
    assert.equal(comparison.rows[2].verdict, 'neutral');
    assert.deepEqual(comparison.counts, { improved: 1, neutral: 1, caution: 0, regressed: 1 });
    assert.ok(comparison.summary.micro && Math.abs(comparison.summary.micro.change - 0.0125) < 1e-9);
    assert.deepEqual(comparison.mismatches, []);
});

test('Compare は CPU が食い違えば不一致として報告する', () => {
    const [baseline, candidate] = GroupByLabel([Run('base', { A: 1 }, 'CPU X'), Run('cand', { A: 1 }, 'CPU Y')]);
    assert.deepEqual(Compare(baseline, candidate).mismatches, ['CPU: CPU X / CPU Y']);
});

test('描画結果は項目名と題名をエスケープする', () => {
    const [baseline, candidate] = GroupByLabel([Run('base', { 'A<b>&c': 10 }), Run('cand', { 'A<b>&c': 8 })]);
    const comparison = Compare(baseline, candidate);

    const svg = RenderChart(comparison);
    assert.ok(svg.startsWith('<svg '));
    assert.ok(svg.includes('A&lt;b&gt;&amp;c'));
    assert.ok(!svg.includes('A<b>&c'));

    const html = RenderHtml(comparison, META);
    assert.ok(html.includes('<title>テスト &lt;比較&gt; &amp; 検証</title>'));
    assert.ok(html.includes('−20.0%'));

    const markdown = RenderMarkdown(comparison, META);
    assert.ok(markdown.includes('| A<b>&c | Simulate | 10.00 us/step | 8.00 us/step | **−20.0%** | 改善 |'));

    const listing = RenderListing([['2026-09-18-test', MakeSummary(comparison, META)]]);
    assert.ok(listing.includes('[テスト <比較> & 検証](2026-09-18-test/report.md)'));
});

test('ParseRun は未対応の schema を拒む', () => {
    assert.throws(() => ParseRun('{"schema":"fbzz-bench/0"}', 'x.json'), /未対応の schema/);
    assert.throws(() => ParseRun('not json', 'y.json'), /JSON として読めない/);
});
