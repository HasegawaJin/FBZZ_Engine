/// @file    AgentLintCommentsTests.mjs
/// @brief   宣言の契約と関数内の説明を区別するコメント規約の検証。
/// @author  Hasegawa Jin
/// @date    2026-11-24
import assert from 'node:assert/strict';
import test from 'node:test';
import { LintContent } from '../../DevTools/AgentLint/lint.mjs';

function CommentWarnings(source) {
    return LintContent('Projects/Core/include/Core/Example.hpp', source)
        .filter(finding => finding.rule.startsWith('doxygen-'))
        .map(({rule, line}) => [rule, line]);
}

test('parameter contracts and source links do not count toward the design-prose limit', () => {
    const contract = [
        'void Previous();',
        '/// @brief 入力を検証して結果を返す。',
        ...Array.from({length: 7}, (_, i) => `/// @param[in] value${i} 入力値。`),
        '/// @return 成功時は true。',
        '/// @see https://example.com/ 仕様',
        'void Run();',
    ].join('\n');
    assert.deepEqual(CommentWarnings(contract), []);

    const longExplanation = ['void Previous();', ...Array(9).fill('/// @note 設計理由。'), 'void Run();'].join('\n');
    assert.deepEqual(CommentWarnings(longExplanation), [['doxygen-long-block', 2]]);
});

test('namespace and class declarations require documentation, inline bodies allow ordinary comments', () => {
    const source = [
        'namespace example {',
        '    // 公開する型の契約。',
        '    struct Owner {',
        '        int value; // 保持する値。',
        '        void Update() const noexcept {',
        '            // 計算の前提を確認する。',
        '            if (value) { // ここでは所有者が確定している。',
        '            }',
        '        }',
        '    };',
        '}',
    ].join('\n');
    assert.deepEqual(CommentWarnings(source), [['doxygen-comment', 2], ['doxygen-comment', 4]]);
});

test('functions with constructor initializers, templates and lambdas allow implementation comments', () => {
    const source = [
        '#define ENTER_BLOCK {',
        'template<class T> struct Example {',
        '    Example() : value{1} { // 初期化後の検証。',
        '        auto run = [&] { // この呼び出しの間だけ参照を借りる。',
        '            return "}";',
        '        };',
        '    }',
        '    T value; ///< 計測値。',
        '};',
        '// 公開される関数。',
        'template<class T> auto Read(T input) -> T {',
        '    // 変換前の値を返す。',
        '    return input;',
        '}',
    ].join('\n');
    assert.deepEqual(CommentWarnings(source), [['doxygen-comment', 10]]);
});

test('parameter directions describe value flow and preserve template parameter syntax', () => {
    const source = [
        '/// @param[in] input 入力。',
        '/// @param[out] result 成功時の結果。',
        '/// @param[in,out] state 更新前の状態を読み、結果を戻す。',
        '/// @tparam T 要素型。',
        '/// @param missing 方向なし。',
        '/// @param[wrong] invalid 対象外の方向。',
        '/// @param [in] spaced プロジェクト内ではタグの直後に書く。',
    ].join('\n');
    assert.deepEqual(CommentWarnings(source), [
        ['doxygen-param-direction', 5], ['doxygen-param-direction', 6], ['doxygen-param-direction', 7],
    ]);
});
