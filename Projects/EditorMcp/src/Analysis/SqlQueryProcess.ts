/// @file    SqlQueryProcess.ts
/// @brief   制限した子プロセス内で Scene 索引への単一 SELECT を実行する。
/// @author  Hasegawa Jin
/// @date    2026-11-06
import { DatabaseSync, constants } from 'node:sqlite';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { SQL_FUNCTIONS, SQL_LIMITS, SQL_TABLES, type SqlCell, type SqlProcessRequest, type SqlProcessResult } from './SqlSchema.js';

/// @note 文区切りを文字列・識別子・コメントから区別し、prepare が後続文を無視する場合も拒否する。
/// @see https://www.sqlite.org/lang_comment.html SQLite comment syntax
export function ValidateReadSql(sql: string): void {
    if (!sql.trim() || Buffer.byteLength(sql) > SQL_LIMITS.queryBytes || sql.includes('\0')) {
        throw new Error('SQL_INVALID: SQL は空でない16 KiB以内の文字列です');
    }
    const tokens: string[] = [];
    for (let offset = 0; offset < sql.length;) {
        const character = sql[offset];
        if (/\s/.test(character)) { ++offset; continue; }
        if (sql.startsWith('--', offset)) {
            const end = sql.indexOf('\n', offset + 2); offset = end < 0 ? sql.length : end + 1; continue;
        }
        if (sql.startsWith('/*', offset)) {
            const end = sql.indexOf('*/', offset + 2);
            if (end < 0) throw new Error('SQL_INVALID: コメントが閉じられていません');
            offset = end + 2; continue;
        }
        if (character === "'" || character === '"' || character === '`' || character === '[') {
            const closing = character === '[' ? ']' : character;
            ++offset;
            let closed = false;
            while (offset < sql.length) {
                if (sql[offset++] !== closing) continue;
                if (closing !== ']' && sql[offset] === closing) { ++offset; continue; }
                closed = true; break;
            }
            if (!closed) throw new Error('SQL_INVALID: 引用符が閉じられていません');
            tokens.push('QUOTED'); continue;
        }
        if (character === ';') { tokens.push(';'); ++offset; continue; }
        const word = /^[A-Za-z_][A-Za-z_0-9]*/.exec(sql.slice(offset));
        if (word) { tokens.push(word[0].toUpperCase()); offset += word[0].length; }
        else { tokens.push(character); ++offset; }
    }
    if (tokens[0] !== 'SELECT' && tokens[0] !== 'WITH') throw new Error('SQL_READ_ONLY: SELECT/CTE のみ実行できます');
    const separator = tokens.indexOf(';');
    if (separator >= 0 && separator !== tokens.length - 1) throw new Error('SQL_MULTIPLE_STATEMENTS: 一度に一文だけ実行できます');
}

/// @note 永続 DB を開かず、SQLite 自身の authorizer で SELECT/READ と限定した関数だけを許可する。
/// @see https://nodejs.org/api/sqlite.html#databasesetauthorizercallback Node SQLite authorization
/// @see https://www.sqlite.org/c3ref/set_authorizer.html SQLite authorizer action codes
export function ExecuteSql(request: SqlProcessRequest): SqlProcessResult {
    ValidateReadSql(request.sql);
    const database = new DatabaseSync(':memory:', { allowExtension: false, enableDoubleQuotedStringLiterals: false });
    try {
        database.exec('PRAGMA temp_store=MEMORY; PRAGMA cache_size=-2048; PRAGMA hard_heap_limit=67108864;');
        database.exec('BEGIN');
        for (const table of SQL_TABLES) {
            database.exec(`CREATE TABLE ${table.name} (${table.columns.join(',')}) STRICT`);
            const insert = database.prepare(`INSERT INTO ${table.name} VALUES (${table.columns.map(() => '?').join(',')})`);
            for (const row of request.tables[table.name]) insert.run(...row);
        }
        database.exec('COMMIT; PRAGMA query_only=ON; PRAGMA trusted_schema=OFF;');
        const tables = new Set<string>(SQL_TABLES.map(table => table.name));
        const functions = new Set<string>(SQL_FUNCTIONS);
        database.setAuthorizer((action, first, second, databaseName) => {
            if (action === constants.SQLITE_SELECT || action === constants.SQLITE_RECURSIVE) return constants.SQLITE_OK;
            /// @note COUNT(*) は既知表への列名空・databaseName=null の READ を通知する。通常列の別DB読取は許可しない。
            if (action === constants.SQLITE_READ && tables.has(first ?? '')
                && (databaseName === 'main' || databaseName === null && second === '')) return constants.SQLITE_OK;
            if (action === constants.SQLITE_FUNCTION && functions.has((second ?? '').toLowerCase())) return constants.SQLITE_OK;
            return constants.SQLITE_DENY;
        });
        const statement = database.prepare(request.sql);
        statement.setReadBigInts(true);
        const columns = statement.columns().map(column => column.name);
        if (new Set(columns).size !== columns.length) throw new Error('SQL_DUPLICATE_COLUMNS: 列名が重複しています。AS で別名を指定してください');
        const rows: Record<string, SqlCell>[] = [];
        const reasons: string[] = [];
        let bytes = Buffer.byteLength(JSON.stringify(columns)) + 256;
        for (const raw of statement.iterate(...request.parameters)) {
            if (rows.length >= request.maxRows) { reasons.push('row-limit'); break; }
            const row = Object.fromEntries(Object.entries(raw).map(([key, value]) => [key,
                typeof value === 'bigint' ? value >= BigInt(Number.MIN_SAFE_INTEGER) && value <= BigInt(Number.MAX_SAFE_INTEGER)
                    ? Number(value) : value.toString() : value instanceof Uint8Array ? Buffer.from(value).toString('base64') : value])) as Record<string, SqlCell>;
            const size = Buffer.byteLength(JSON.stringify(row)) + 1;
            if (bytes + size > request.maxBytes) { reasons.push('byte-limit'); break; }
            bytes += size; rows.push(row);
        }
        return { columns, rows, returnedRows: rows.length, truncated: reasons.length > 0, truncationReasons: reasons };
    } finally { database.close(); }
}

/// @note 親が deadline 超過時にプロセスを終了できるよう、SQLite の同期実行を MCP 本体から分離する。
async function Main(): Promise<void> {
    let input = '';
    for await (const chunk of process.stdin) {
        input += String(chunk);
        if (Buffer.byteLength(input) > SQL_LIMITS.inputBytes) throw new Error('SQL_INPUT_LIMIT: 索引が転送予算を超えました');
    }
    try {
        const result = ExecuteSql(JSON.parse(input) as SqlProcessRequest);
        process.stdout.write(JSON.stringify({ ok: true, result }));
    } catch (error) {
        process.stdout.write(JSON.stringify({ ok: false, error: error instanceof Error ? error.message : String(error) }));
        process.exitCode = 1;
    }
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
    Main().catch(error => { process.stderr.write(String(error)); process.exitCode = 1; });
}
