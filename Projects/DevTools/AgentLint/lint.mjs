// FBZZ Engine
// lint.mjs | Projects/DevTools/AgentLint
// AGENTS.md の «絶対制約» と記述規約を機械で検査する。AI の編集直後 (Claude Code PostToolUse フック) と手動で使う。
//
// 使い方:
//   node Projects/DevTools/AgentLint/lint.mjs <file...>     指定ファイル (HEAD からの «増えた違反» だけ報告)
//   node Projects/DevTools/AgentLint/lint.mjs --changed     git の未コミット変更すべて
//   node Projects/DevTools/AgentLint/lint.mjs --base <ref>  <ref>...HEAD で変わったファイル (CI の PR 差分。<ref> 版から増えた違反)
//   node Projects/DevTools/AgentLint/lint.mjs --all <file>  HEAD と比べず全違反を報告
//   node Projects/DevTools/AgentLint/lint.mjs --hook        stdin の PostToolUse JSON から編集ファイルを読む
//
// WHY «増えた違反» だけか: 旧コードには `//` コメント等が大量に残っている。触るたびに
//     既存の違反まで報告すると、AI がその修正に追われて本来の変更が埋もれる。
//     HEAD 版と行テキスト単位で数を比べ、この編集で増えたものだけを出す。
// 設計: Docs/design/ai-verification-loop.md

import { execFileSync } from 'node:child_process';
import { existsSync, readFileSync, readdirSync, statSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const REPO_ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..', '..');

const CPP_EXTENSIONS = new Set(['.hpp', '.cpp', '.inl', '.h']);
const SHADER_EXTENSIONS = new Set(['.hlsl', '.hlsli']);
const GENERATED_FILES = new Set(['ScriptList.inl', 'DataAssetList.inl']);

/** シェーダーの複製がある根。同じ相対パスのファイルは中身が一致していなければならない。 */
function ShaderRoots() {
    const roots = ['Assets/Shaders', 'GreenWare/Assets/Shaders'];
    const templates = path.join(REPO_ROOT, 'Projects/GameHub/Templates');
    if (existsSync(templates)) {
        for (const name of readdirSync(templates)) {
            const candidate = `Projects/GameHub/Templates/${name}/Assets/Shaders`;
            if (existsSync(path.join(REPO_ROOT, candidate))) roots.push(candidate);
        }
    }
    return roots;
}

function ToRepoPath(file) {
    const absolute = path.isAbsolute(file) ? file : path.join(REPO_ROOT, file);
    return path.relative(REPO_ROOT, absolute).split(path.sep).join('/');
}

function IsLintTarget(repoPath) {
    if (/(^|\/)(ThirdParty|node_modules|build|dist|out|Binaries|Library|SDK)\//.test(repoPath)) return false;
    const extension = path.extname(repoPath).toLowerCase();
    if (CPP_EXTENSIONS.has(extension)) return repoPath.startsWith('Projects/') || /^[^/]+\/Assets\/Scripts\//.test(repoPath);
    if (SHADER_EXTENSIONS.has(extension)) return true;
    if (path.basename(repoPath) === 'CMakeLists.txt' || extension === '.cmake') return true;
    if (extension === '.ps1') return true;
    return false;
}

/** `0x8000'0000u` の桁区切りか。文字リテラルの開始と取り違えると、行末の `//` コメントを見落とす。 */
export function IsDigitSeparator(source, index) {
    if (!/[0-9A-Fa-f]/.test(source[index - 1] ?? '') || !/[0-9A-Fa-f]/.test(source[index + 1] ?? '')) return false;
    let start = index - 1;
    while (start > 0 && /[0-9A-Za-z']/.test(source[start - 1])) start--;
    return /[0-9]/.test(source[start]);
}

/**
 * コメントと文字列リテラルを空白へ潰したコードと、コメントの一覧を返す。
 * 行数と桁は保つ (報告する行番号を元ファイルと一致させるため)。
 */
export function SplitCode(source) {
    const code = [];
    const comments = [];
    let i = 0;
    let line = 1;
    const length = source.length;
    const blank = (ch) => (ch === '\n' ? '\n' : ' ');
    while (i < length) {
        const ch = source[i];
        const next = source[i + 1];
        if (ch === '/' && next === '/') {
            const start = i;
            while (i < length && source[i] !== '\n') { code.push(' '); i++; }
            comments.push({ line, text: source.slice(start, i) });
            continue;
        }
        if (ch === '/' && next === '*') {
            const start = i;
            const startLine = line;
            code.push('  '); i += 2;
            while (i < length && !(source[i] === '*' && source[i + 1] === '/')) {
                if (source[i] === '\n') line++;
                code.push(blank(source[i])); i++;
            }
            code.push('  '); i += 2;
            comments.push({ line: startLine, text: source.slice(start, i) });
            continue;
        }
        if (ch === 'R' && next === '"') {
            const open = source.indexOf('(', i + 2);
            if (open !== -1 && open - i < 20) {
                const delimiter = source.slice(i + 2, open);
                const close = source.indexOf(`)${delimiter}"`, open);
                if (close !== -1) {
                    const end = close + delimiter.length + 2;
                    for (; i < end; i++) { if (source[i] === '\n') line++; code.push(blank(source[i])); }
                    continue;
                }
            }
        }
        if (ch === '\'' && IsDigitSeparator(source, i)) { code.push(ch); i++; continue; }
        if (ch === '"' || ch === '\'') {
            const quote = ch;
            code.push(' '); i++;
            while (i < length && source[i] !== quote && source[i] !== '\n') {
                if (source[i] === '\\') { code.push(' '); i++; }
                code.push(' '); i++;
            }
            code.push(' '); i++;
            continue;
        }
        if (ch === '\n') line++;
        code.push(ch);
        i++;
    }
    return { code: code.join(''), comments };
}

/** @returns {{severity:'error'|'warn', rule:string, line:number, message:string, key:string}[]} */
function LintCpp(repoPath, source) {
    const findings = [];
    const lines = source.split(/\r?\n/);
    const baseName = path.basename(repoPath);
    const extension = path.extname(repoPath).toLowerCase();
    const isHeader = extension === '.hpp' || extension === '.h';
    const isScript = /^[^/]+\/Assets\//.test(repoPath);
    const isRendererBackend = /Renderer\/DX1[12]|RenderDX1[12]|\/DX1[12]\//.test(repoPath);
    // key は HEAD 版と突き合わせて «増えた違反» だけを残すための識別子 (OnlyNew)。
    // WHY 原文の行をそのまま使わないか: 行末コメントを別の行へ移しただけ・旧形式の見出しを少し書き換えただけで
    //     既存の違反が «新しい違反» に見え、PR の差分検査が関係の無い箇所で落ちる。規則ごとに不変な部分で比べる。
    const add = (severity, rule, line, message, keyText = (lines[line - 1] ?? '')) => findings.push({
        severity, rule, line, message, key: `${rule}|${keyText.replace(/\s+/g, ' ').trim()}`,
    });

    if (GENERATED_FILES.has(baseName)) return findings;

    // 4 行ヘッダー
    const header = lines.slice(0, 4);
    const expected = [
        new RegExp(`^/// @file\\s+${baseName.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}\\s*$`),
        /^\/\/\/ @brief\s+\S/,
        /^\/\/\/ @author\s+\S/,
        /^\/\/\/ @date\s+\d{4}-\d{2}-\d{2}\s*$/,
    ];
    const tags = ['@file <ファイル名>', '@brief', '@author', '@date YYYY-MM-DD'];
    for (let n = 0; n < 4; n++) {
        if (!expected[n].test(header[n] ?? '')) {
            add('error', 'file-header', n + 1, `4 行ヘッダーの ${n + 1} 行目は "/// ${tags[n]}" (AGENTS.md «コード規約»)`, `line ${n + 1}`);
            break;
        }
    }
    if (isHeader && !/^\s*#\s*pragma\s+once\b/m.test(source)) {
        add('error', 'pragma-once', 5, 'ヘッダーには #pragma once');
    }

    const { code, comments } = SplitCode(source);
    const codeLines = code.split(/\r?\n/);

    const rules = [
        { rule: 'no-new', re: /(?<!operator\s*)\bnew\s+(?=[A-Za-z_:(])/, message: '`new` 禁止。make_unique / make_shared' },
        { rule: 'no-delete', re: /(?<!=\s*)(?<!operator\s*)\bdelete\b/, message: '`delete` 禁止。所有は unique_ptr' },
        { rule: 'no-throw', re: /\bthrow\b/, message: '`throw` 禁止。回復可能は bool + Logger、不可能は assert' },
        { rule: 'no-std-exception', re: /\bstd::exception\b/, message: 'std::exception 禁止' },
        { rule: 'no-dynamic-cast', re: /\bdynamic_cast\s*</, message: 'dynamic_cast 禁止。GetComponent<T>()' },
        { rule: 'no-ranges', re: /\bstd::(ranges|views)::/, message: 'std::ranges 禁止' },
        { rule: 'no-coroutine', re: /\bco_(await|yield|return)\b/, message: 'C++20 コルーチン禁止。Engine/Scene/Coroutine.hpp' },
        { rule: 'no-modules', re: /^\s*(export\s+module\b|import\s+[\w.:]+\s*;|import\s*<)/, message: 'C++20 Modules 禁止' },
        { rule: 'no-generated-hpp', re: /#\s*include\s*[<"][^>"]*\.generated\.hpp[>"]/, message: '.generated.hpp は廃止。FBZZ_FIELD* + FBZZ_REFLECT' },
        { rule: 'no-banned-lib', re: /#\s*include\s*[<"](glm\/|GLFW\/|btBulletDynamicsCommon|PxPhysicsAPI|box2d\/|Box2D\/)/, message: 'GLM / GLFW / Bullet / PhysX / Box2D 禁止。自作 Math / Physics' },
        { rule: 'warn-reinterpret-cast', re: /\breinterpret_cast\s*</, message: 'reinterpret_cast は定数バッファ転送のみ可', severity: 'warn' },
    ];
    if (isHeader) {
        rules.push({ rule: 'no-windows-h-in-header', re: /#\s*include\s*<(W|w)indows\.h>/, message: 'ヘッダーへ <Windows.h> 禁止。.cpp に閉じる' });
        if (!isScript) rules.push({ rule: 'no-using-namespace-in-header', re: /^\s*using\s+namespace\b/, message: 'エンジン .hpp での using namespace 禁止' });
    }
    if (!isRendererBackend) {
        rules.push({ rule: 'no-backend-downcast', re: /\bDX1[12]Renderer\s*[*&]/, message: 'DX11Renderer* / DX12Renderer* 禁止。IRenderer& のみ' });
    }

    // #include は文字列扱いで潰れるので、原文の行で見る規則を分ける。
    const rawLineRules = new Set(['no-generated-hpp', 'no-banned-lib', 'no-windows-h-in-header']);
    for (let n = 0; n < codeLines.length; n++) {
        for (const { rule, re, message, severity } of rules) {
            const target = rawLineRules.has(rule) ? (lines[n] ?? '') : codeLines[n];
            if (re.test(target)) add(severity ?? 'error', rule, n + 1, message, target);
        }
    }

    // 長すぎる `///` ブロック (規約: ファイル説明 4 行・クラス 4 行・@note 1〜3 行)。8 行超だけ警告し、圧縮の目印にする。
    {
        let run = 0;
        let start = 0;
        const flush = (end) => {
            if (run > 8 && !(start === 1)) add('warn', 'doxygen-long-block', start, `\`///\` が ${run} 行続く。契約と非自明な理由だけ残し 8 行以内に (Docs/conventions/comments.md §4)`);
            run = 0;
        };
        lines.forEach((text, index) => {
            if (/^\s*\/\/\/(?!<)/.test(text)) { if (run === 0) start = index + 1; run++; } else flush(index);
        });
        flush(lines.length);
    }
    // コメント様式 (Docs/conventions/comments.md): `///` だけ。例外は namespace 閉じ・#endif・clang-format・NOLINT。
    for (const comment of comments) {
        const text = comment.text;
        const isDoxygen = text.startsWith('///') || text.startsWith('/**') || text.startsWith('//!');
        if (!isDoxygen) {
            if (/^\/\/\s*(NOLINT|clang-format|namespace\b)/.test(text)) continue;
            if (/^\/\/ @@FBZZ_/.test(text)) continue; // ScriptCodeGen / GameHub が `"// " + marker` の完全一致で探す。`///` にすると自動同期が壊れる
            if (/^\/\*\s*\w*\s*=?\s*\*\/$/.test(text)) continue; // 引数名のコメント `/*unused*/` `/*poolTag=*/`
            const codeBefore = codeLines[comment.line - 1] ?? '';
            if (/^\s*#\s*endif\b/.test(codeBefore)) continue;
            add('warn', 'doxygen-comment', comment.line, 'コメントは Doxygen 形式 (/// @brief / @note)');
            continue;
        }
        if (/@ret\b/.test(text)) add('warn', 'doxygen-tag', comment.line, '`@ret` は独自タグ。`@return`');
        if (/@(details|remarks?)\b/.test(text)) add('warn', 'doxygen-tag', comment.line, '`@details` / `@remark` は使わない。`@note` 1 行');
        if (/^\/\/[\/!]<?\s*(?:@\w+\s+)?(WHY|WHAT|HOW|NOTE|TODO|FIXME)\b[^\n]*[:：]/.test(text)) add('warn', 'doxygen-label', comment.line, 'ラベル儀式 (WHY: / NOTE: / TODO:) を書かない。`@note` / `@todo` で事実だけ');
        if (/<[A-Za-z][A-Za-z0-9_]*>/.test(text) && !/`[^`]*<[A-Za-z][A-Za-z0-9_]*>[^`]*`/.test(text) && !/(std|template|unique_ptr|shared_ptr|vector|span|array|optional|Ref|GetComponent|<T>|<[A-Z]\w*Component>)/.test(text)) {
            add('warn', 'doxygen-html-tag', comment.line, '`<名前>` は Doxygen が HTML タグと読む。バッククォートで囲む');
        }
    }
    return findings;
}

function LintShader(repoPath) {
    const findings = [];
    const roots = ShaderRoots();
    const root = roots.find((candidate) => repoPath.startsWith(`${candidate}/`));
    if (root === undefined) return findings;
    const relative = repoPath.slice(root.length + 1);
    const read = (p) => readFileSync(path.join(REPO_ROOT, p), 'utf8').replace(/\r\n/g, '\n');
    const mine = read(repoPath);
    const differing = [];
    for (const other of roots) {
        if (other === root) continue;
        const counterpart = `${other}/${relative}`;
        if (!existsSync(path.join(REPO_ROOT, counterpart))) continue;
        if (read(counterpart) !== mine) differing.push(counterpart);
    }
    if (differing.length > 0) {
        findings.push({
            severity: 'error', rule: 'shader-copies', line: 1, key: `shader-copies|${differing.join(',')}`,
            message: `シェーダーの複製と中身が違う: ${differing.join(', ')} (全コピーを同じ内容へ)`,
        });
    }
    return findings;
}

/**
 * `#` コメントの言語 (CMake / PowerShell) でラベル儀式を拾う。C++ の doxygen-label と同じ規則。
 * @see Docs/conventions/comments.md §1
 */
function LintHashComments(source) {
    const findings = [];
    source.split(/\r?\n/).forEach((text, index) => {
        const comment = /(?:^|\s)#\s*(?:@\w+\s+)?(WHY|WHAT|HOW|NOTE|TODO|FIXME)\b[^\n]*[:：]/.exec(text);
        if (comment) {
            findings.push({ severity: 'warn', rule: 'doxygen-label', line: index + 1, key: `doxygen-label|${text.trim()}`,
                message: 'ラベル儀式 (WHY: / NOTE: / TODO:) を書かない。`# @note` / `# @todo` で事実だけ' });
        }
    });
    return findings;
}

function LintCMake(repoPath, source) {
    const findings = LintHashComments(source);
    const lines = source.split(/\r?\n/);
    lines.forEach((text, index) => {
        if (/^\s*[^#]*\bFetchContent_/.test(text)) {
            findings.push({ severity: 'error', rule: 'no-fetchcontent', line: index + 1, key: `no-fetchcontent|${text.trim()}`,
                message: 'FetchContent 禁止。ThirdParty/ へベンダー + LICENSE + VERSION + THIRD-PARTY-NOTICES.md' });
        }
    });
    return findings;
}

/** テストの .cpp が Projects/Tests/CMakeLists.txt の SOURCES に載っているか。 */
function LintTestRegistration(repoPath) {
    if (!repoPath.startsWith('Projects/Tests/') || !repoPath.endsWith('.cpp')) return [];
    if (repoPath.includes('/TestKit/')) return [];
    const cmake = path.join(REPO_ROOT, 'Projects/Tests/CMakeLists.txt');
    if (!existsSync(cmake)) return [];
    const relative = repoPath.slice('Projects/Tests/'.length);
    if (readFileSync(cmake, 'utf8').includes(relative)) return [];
    return [{ severity: 'warn', rule: 'test-not-registered', line: 1, key: `test-not-registered|${relative}`,
        message: `Projects/Tests/CMakeLists.txt の SOURCES に ${relative} が無い (ビルドされない)` }];
}

/**
 * 入場を索引 (各根の README.md) で管理するツール置き場。
 * [根, 直下の項目名] を取り出す。GreenWare/Tools のような «プロジェクト/Tools» も含む。
 */
const TOOL_ROOTS = [
    /^(Tools)\/([^/]+)/,
    /^(Projects\/DevTools)\/([^/]+)/,
    /^((?!Projects\/)[^/]+\/Tools)\/([^/]+)/,
];

/**
 * ツール置き場のファイルが、その根の README.md の索引に載っているか。
 * WHY: AI は作業中の一回きりのスクリプトを «後で使うかも» と残し、Tools が溜まり場になる。
 *      索引に載せる (= 呼び出し元と役割を書く) ことを入場の条件にし、置いた瞬間に止める。
 * 設計: AGENTS.md «ツールの置き場所»
 */
function LintToolRegistration(repoPath) {
    if (/(^|\/)(node_modules|dist|out|__pycache__)\//.test(repoPath)) return [];
    for (const pattern of TOOL_ROOTS) {
        const match = pattern.exec(repoPath);
        if (!match) continue;
        const [, root, entry] = match;
        if (entry === 'README.md') return [];
        const index = path.join(REPO_ROOT, root, 'README.md');
        const escaped = entry.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
        if (existsSync(index) && new RegExp('`' + escaped + '/?`').test(readFileSync(index, 'utf8'))) return [];
        return [{ severity: 'error', rule: 'tool-unlisted', line: 1, key: `tool-unlisted|${root}/${entry}`,
            message: `${root}/README.md の索引に \`${entry}\` が無い。一回きりのスクリプトは Scratch/ へ置く (Git に入らない)。` +
                '繰り返し使うならユーザーの承認を得て、役割と呼び出し元を索引へ書く' }];
    }
    return [];
}

/**
 * ASCII 以外を含む .ps1 が UTF-8 の BOM を持つか。
 * WHY: Windows PowerShell 5.1 (タスクと AgentBuild の `powershell`) は BOM の無い .ps1 を CP932 として読む。
 *      日本語の文字列やコメントが化けて構文エラーになるのに、PowerShell 7 では通るので気づけない。
 * @see https://learn.microsoft.com/powershell/module/microsoft.powershell.core/about/about_character_encoding
 *      about_Character_Encoding «Character encoding in Windows PowerShell»
 */
function LintPowerShellEncoding(repoPath, absolute) {
    if (path.extname(repoPath).toLowerCase() !== '.ps1') return [];
    const bytes = readFileSync(absolute);
    const hasBom = bytes.length >= 3 && bytes[0] === 0xef && bytes[1] === 0xbb && bytes[2] === 0xbf;
    if (hasBom || bytes.every((byte) => byte < 0x80)) return [];
    return [{ severity: 'error', rule: 'ps1-bom', line: 1, key: 'ps1-bom',
        message: 'ASCII 以外を含む .ps1 は UTF-8 (BOM 付き) で保存する。無いと Windows PowerShell 5.1 が CP932 で読み、構文エラーになる' }];
}

function LintContent(repoPath, source) {
    const extension = path.extname(repoPath).toLowerCase();
    if (CPP_EXTENSIONS.has(extension)) return LintCpp(repoPath, source);
    if (path.basename(repoPath) === 'CMakeLists.txt' || extension === '.cmake') return LintCMake(repoPath, source);
    if (extension === '.ps1') return LintHashComments(source);
    return [];
}

function HeadVersion(repoPath, ref = 'HEAD') {
    try {
        return execFileSync('git', ['show', `${ref}:${repoPath}`], { cwd: REPO_ROOT, encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'], maxBuffer: 64 * 1024 * 1024 });
    } catch {
        return null;
    }
}

/** HEAD 版にも同じ (規則, 行テキスト) が同数以上あった指摘を落とす。 */
function OnlyNew(findings, baseline) {
    const budget = new Map();
    for (const finding of baseline) budget.set(finding.key, (budget.get(finding.key) ?? 0) + 1);
    return findings.filter((finding) => {
        const remaining = budget.get(finding.key) ?? 0;
        if (remaining > 0) { budget.set(finding.key, remaining - 1); return false; }
        return true;
    });
}

export function LintFile(file, { all = false, base = 'HEAD' } = {}) {
    const repoPath = ToRepoPath(file);
    const absolute = path.join(REPO_ROOT, repoPath);
    if (!existsSync(absolute) || !statSync(absolute).isFile()) return [];

    // 置き場所の検査は拡張子を問わない (.py / .mjs / .ts もツールとして置かれる)。
    const findings = LintToolRegistration(repoPath);
    if (!IsLintTarget(repoPath)) return findings.map((finding) => ({ ...finding, file: repoPath }));

    if (GENERATED_FILES.has(path.basename(repoPath))) {
        findings.push({ severity: 'error', rule: 'generated-file', line: 1, key: 'generated-file',
            message: `${path.basename(repoPath)} は生成物。手で編集しない (ScriptCodeGen が作り直す)` });
        return findings.map((finding) => ({ ...finding, file: repoPath }));
    }
    const source = readFileSync(absolute, 'utf8');
    let contentFindings = LintContent(repoPath, source);
    if (!all) {
        const head = HeadVersion(repoPath, base);
        if (head !== null) contentFindings = OnlyNew(contentFindings, LintContent(repoPath, head));
    }
    findings.push(...contentFindings);
    if (SHADER_EXTENSIONS.has(path.extname(repoPath).toLowerCase())) findings.push(...LintShader(repoPath));
    findings.push(...LintTestRegistration(repoPath));
    findings.push(...LintPowerShellEncoding(repoPath, absolute));
    return findings.map((finding) => ({ ...finding, file: repoPath }));
}

/** CI 用: base から HEAD までに変わったファイル (PR の差分)。 */
function ChangedSince(base) {
    return execFileSync('git', ['diff', '--name-only', '--diff-filter=ACMR', `${base}...HEAD`], { cwd: REPO_ROOT, encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'], maxBuffer: 64 * 1024 * 1024 })
        .split(/\r?\n/).filter(Boolean).filter((file) => !GENERATED_FILES.has(path.basename(file)));
}

function ChangedFiles() {
    const run = (args) => execFileSync('git', args, { cwd: REPO_ROOT, encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'], maxBuffer: 64 * 1024 * 1024 })
        .split(/\r?\n/).filter(Boolean);
    const tracked = run(['diff', '--name-only', '--diff-filter=ACMR', 'HEAD']);
    const untracked = run(['ls-files', '--others', '--exclude-standard']);
    // 生成物は ScriptCodeGen が書き換えるので、変更一覧の走査では咎めない (フックの時だけ見る)。
    return [...new Set([...tracked, ...untracked])].filter((file) => !GENERATED_FILES.has(path.basename(file)));
}

function Format(findings, limit = 30) {
    const out = [];
    const errors = findings.filter((finding) => finding.severity === 'error');
    const warnings = findings.filter((finding) => finding.severity === 'warn');
    for (const finding of [...errors, ...warnings].slice(0, limit)) {
        out.push(`${finding.severity === 'error' ? 'ERROR' : 'WARN'} ${finding.file}:${finding.line} ${finding.rule} ${finding.message}`);
    }
    if (findings.length > limit) out.push(`... ${findings.length - limit} more`);
    return { text: out.join('\n'), errors: errors.length, warnings: warnings.length };
}

async function ReadStdin() {
    const chunks = [];
    for await (const chunk of process.stdin) chunks.push(chunk);
    return Buffer.concat(chunks).toString('utf8');
}

async function Main(argv) {
    const all = argv.includes('--all');
    if (argv.includes('--hook')) {
        let payload = {};
        try { payload = JSON.parse(await ReadStdin()); } catch { return 0; }
        const input = payload.tool_input ?? {};
        const file = input.file_path ?? input.path ?? input.notebook_path;
        if (typeof file !== 'string') return 0;
        const findings = LintFile(file, { all });
        if (findings.length === 0) return 0;
        const { text, errors } = Format(findings, 15);
        if (errors > 0) {
            process.stderr.write(`[AgentLint] この編集で規約違反が増えた。直してから続ける:\n${text}\n`);
            return 2;
        }
        process.stdout.write(JSON.stringify({
            hookSpecificOutput: { hookEventName: 'PostToolUse', additionalContext: `[AgentLint] 警告:\n${text}` },
        }));
        return 0;
    }

    const baseIndex = argv.indexOf('--base');
    const base = baseIndex >= 0 ? argv[baseIndex + 1] : null;
    const files = base ? ChangedSince(base)
        : argv.includes('--changed') ? ChangedFiles()
        : argv.filter((arg, index) => !arg.startsWith('--') && argv[index - 1] !== '--base');
    if (files.length === 0) {
        console.log('usage: lint.mjs <file...> | --changed | --base <ref> | --hook  [--all]');
        return 2;
    }
    const findings = files.flatMap((file) => LintFile(file, { all, base: base ?? 'HEAD' }));
    const { text, errors, warnings } = Format(findings, 200);
    if (text) console.log(text);
    console.log(`RESULT ${errors > 0 ? 'failed' : 'ok'} errors=${errors} warnings=${warnings} files=${files.length}`);
    return errors > 0 ? 1 : 0;
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
    Main(process.argv.slice(2)).then((code) => { process.exitCode = code; });
}
