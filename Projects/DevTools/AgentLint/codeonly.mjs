// FBZZ Engine
// codeonly.mjs | Projects/DevTools/AgentLint
// «コメントだけを触った» 編集の証明。コメントを剥いだコードのハッシュを取り、編集の前後で比べる。
//
// 使い方:
//   node Projects/DevTools/AgentLint/codeonly.mjs snapshot <out.json> <file|dir...>   編集前に撮る
//   node Projects/DevTools/AgentLint/codeonly.mjs verify   <in.json>  [file|dir...]   編集後に比べる (省略時は snapshot の全件)
//
// 文字列リテラルは残し、コメントと空白だけを落とす。コメント整理で文字列を書き換えた事故も検出する。
// 終了コード: 0 = 全件一致 / 1 = 差分あり / 2 = 使い方の誤り。
// 規約: Docs/conventions/comments.md §5

import { createHash } from 'node:crypto';
import { existsSync, readFileSync, readdirSync, statSync, writeFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const REPO_ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..', '..');
const CPP_EXTENSIONS = new Set(['.hpp', '.cpp', '.inl', '.h']);

/** `0x8000'0000u` の桁区切りか。文字リテラルの開始と取り違えると、同じ行の `//` コメントをコード扱いしてしまう。 */
function IsDigitSeparator(source, index) {
    if (!/[0-9A-Fa-f]/.test(source[index - 1] ?? '') || !/[0-9A-Fa-f]/.test(source[index + 1] ?? '')) return false;
    let start = index - 1;
    while (start > 0 && /[0-9A-Za-z']/.test(source[start - 1])) start--;
    return /[0-9]/.test(source[start]);
}

/** コメントを落とし、文字列と文字リテラルはそのまま残したコードを返す。 */
export function StripComments(source) {
    const out = [];
    const length = source.length;
    let i = 0;
    while (i < length) {
        const ch = source[i];
        const next = source[i + 1];
        if (ch === '/' && next === '/') {
            while (i < length && source[i] !== '\n') i++;
            continue;
        }
        if (ch === '/' && next === '*') {
            i += 2;
            while (i < length && !(source[i] === '*' && source[i + 1] === '/')) i++;
            i += 2;
            continue;
        }
        if (ch === 'R' && next === '"') {
            const open = source.indexOf('(', i + 2);
            if (open !== -1 && open - i < 20) {
                const delimiter = source.slice(i + 2, open);
                const close = source.indexOf(`)${delimiter}"`, open);
                if (close !== -1) {
                    const end = close + delimiter.length + 2;
                    out.push(source.slice(i, end));
                    i = end;
                    continue;
                }
            }
        }
        if (ch === '\'' && IsDigitSeparator(source, i)) { out.push(ch); i++; continue; }
        if (ch === '"' || ch === '\'') {
            const quote = ch;
            out.push(ch); i++;
            while (i < length && source[i] !== quote && source[i] !== '\n') {
                if (source[i] === '\\') { out.push(source[i]); i++; }
                out.push(source[i]); i++;
            }
            if (i < length) { out.push(source[i]); i++; }
            continue;
        }
        out.push(ch);
        i++;
    }
    return out.join('');
}

function CodeHash(source) {
    const code = StripComments(source).replace(/\s+/g, '');
    return createHash('sha1').update(code).digest('hex');
}

function ToRepoPath(file) {
    const absolute = path.isAbsolute(file) ? file : path.join(REPO_ROOT, file);
    return path.relative(REPO_ROOT, absolute).split(path.sep).join('/');
}

function CollectFiles(inputs) {
    const files = [];
    const visit = (absolute) => {
        if (!existsSync(absolute)) return;
        const stat = statSync(absolute);
        if (stat.isDirectory()) {
            for (const name of readdirSync(absolute)) {
                if (['build', 'out', 'node_modules', 'ThirdParty', '.git'].includes(name)) continue;
                visit(path.join(absolute, name));
            }
            return;
        }
        if (CPP_EXTENSIONS.has(path.extname(absolute).toLowerCase())) files.push(ToRepoPath(absolute));
    };
    for (const input of inputs) visit(path.isAbsolute(input) ? input : path.join(REPO_ROOT, input));
    return [...new Set(files)].sort();
}

function Snapshot(outFile, inputs) {
    const files = CollectFiles(inputs);
    const map = {};
    for (const file of files) map[file] = CodeHash(readFileSync(path.join(REPO_ROOT, file), 'utf8'));
    writeFileSync(outFile, JSON.stringify(map, null, 1));
    console.log(`SNAPSHOT files=${files.length} -> ${outFile}`);
    return 0;
}

function Verify(inFile, inputs) {
    const map = JSON.parse(readFileSync(inFile, 'utf8'));
    const files = inputs.length > 0 ? CollectFiles(inputs) : Object.keys(map);
    let changed = 0;
    let unknown = 0;
    for (const file of files) {
        const expected = map[file];
        if (expected === undefined) { unknown++; console.log(`UNKNOWN ${file} (snapshot に無い)`); continue; }
        const absolute = path.join(REPO_ROOT, file);
        if (!existsSync(absolute)) { changed++; console.log(`MISSING ${file}`); continue; }
        if (CodeHash(readFileSync(absolute, 'utf8')) !== expected) { changed++; console.log(`CHANGED ${file}`); }
    }
    console.log(`RESULT ${changed === 0 ? 'ok' : 'failed'} changed=${changed} unknown=${unknown} files=${files.length}`);
    return changed === 0 ? 0 : 1;
}

function Main(argv) {
    const [verb, jsonFile, ...inputs] = argv;
    if (verb === 'snapshot' && jsonFile && inputs.length > 0) return Snapshot(jsonFile, inputs);
    if (verb === 'verify' && jsonFile) return Verify(jsonFile, inputs);
    console.log('usage: codeonly.mjs snapshot <out.json> <file|dir...> | verify <in.json> [file|dir...]');
    return 2;
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
    process.exitCode = Main(process.argv.slice(2));
}
