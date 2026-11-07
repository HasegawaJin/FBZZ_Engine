/// @file    SqlSchema.ts
/// @brief   Scene の読み取り用 SQLite 索引の表と転送上限。
/// @author  Hasegawa Jin
/// @date    2026-11-06

export type SqlCell = string | number | null;
export type SqlRow = SqlCell[];

export const SQL_LIMITS = {
    inputBytes: 16 * 1024 * 1024,
    cellBytes: 64 * 1024,
    propertyRows: 100000,
    nodes: 10000,
    queryBytes: 16384,
    resultBytes: 256 * 1024,
    resultRows: 500,
    timeoutMs: 3000,
} as const;

export const SQL_FUNCTIONS = ['abs', 'avg', 'coalesce', 'concat', 'concat_ws', 'count', 'format', 'glob',
    'group_concat', 'hex', 'if', 'ifnull', 'iif', 'instr', 'json', 'json_array', 'json_array_length',
    'json_error_position', 'json_extract', 'json_group_array', 'json_group_object', 'json_object',
    'json_quote', 'json_type', 'json_valid', 'length', 'like', 'likelihood', 'likely', 'lower', 'ltrim',
    'max', 'min', 'nullif', 'octet_length', 'printf', 'quote', 'replace', 'round', 'rtrim', 'sign',
    'substr', 'substring', 'sum', 'total', 'trim', 'typeof', 'unicode', 'unlikely', 'upper',
    'row_number', 'rank', 'dense_rank', 'percent_rank', 'cume_dist', 'ntile', 'lag', 'lead',
    'first_value', 'last_value', 'nth_value'] as const;

export const SQL_TABLES = [
    { name: 'nodes', columns: ['id TEXT PRIMARY KEY', 'name TEXT', 'parent_id TEXT', 'tag TEXT', 'layer INTEGER',
        'active INTEGER', 'active_in_hierarchy INTEGER', 'local_x REAL', 'local_y REAL', 'local_z REAL',
        'world_x REAL', 'world_y REAL', 'world_z REAL', 'transform_json TEXT'] },
    { name: 'components', columns: ['node_id TEXT', 'type TEXT', 'fields_json TEXT'] },
    { name: 'scripts', columns: ['node_id TEXT', 'script_id TEXT', 'saved_script_id TEXT', 'type TEXT',
        'status TEXT', 'enabled INTEGER', 'inspection_status TEXT', 'inspection_json TEXT'] },
    { name: 'properties', columns: ['node_id TEXT', 'owner_kind TEXT', 'owner_type TEXT', 'script_id TEXT',
        'path TEXT', 'value_type TEXT', 'text_value TEXT', 'number_value REAL', 'bool_value INTEGER', 'json_value TEXT'] },
    { name: 'object_references', columns: ['node_id TEXT', 'owner_kind TEXT', 'owner_type TEXT', 'script_id TEXT',
        'path TEXT', 'target_id TEXT', 'target_guid TEXT', 'expected_type TEXT', 'status TEXT'] },
    { name: 'asset_references', columns: ['scan_job INTEGER', 'owner_path TEXT', 'target_path TEXT',
        'field TEXT', 'kind TEXT', 'line INTEGER'] },
    { name: 'asset_reference_scans', columns: ['job INTEGER', 'target_path TEXT', 'finished INTEGER',
        'stale INTEGER', 'limited INTEGER', 'owner_count INTEGER', 'owners_indexed INTEGER',
        'processed INTEGER', 'candidates INTEGER', 'unreadable INTEGER', 'unsupported INTEGER',
        'oversized INTEGER', 'malformed INTEGER', 'unresolved_hints INTEGER'] },
] as const;

export type SqlTableName = typeof SQL_TABLES[number]['name'];
export type SqlTables = Record<SqlTableName, SqlRow[]>;

export interface SqlProcessRequest {
    tables: SqlTables;
    sql: string;
    parameters: SqlCell[];
    maxRows: number;
    maxBytes: number;
}

export interface SqlProcessResult {
    columns: string[];
    rows: Record<string, SqlCell>[];
    returnedRows: number;
    truncated: boolean;
    truncationReasons: string[];
}

export function EmptySqlTables(): SqlTables {
    return { nodes: [], components: [], scripts: [], properties: [], object_references: [],
        asset_references: [], asset_reference_scans: [] };
}
