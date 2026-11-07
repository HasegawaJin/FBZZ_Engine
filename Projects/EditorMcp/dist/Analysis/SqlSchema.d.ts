export type SqlCell = string | number | null;
export type SqlRow = SqlCell[];
export declare const SQL_LIMITS: {
    readonly inputBytes: number;
    readonly cellBytes: number;
    readonly propertyRows: 100000;
    readonly nodes: 10000;
    readonly queryBytes: 16384;
    readonly resultBytes: number;
    readonly resultRows: 500;
    readonly timeoutMs: 3000;
};
export declare const SQL_FUNCTIONS: readonly ['abs', 'avg', 'coalesce', 'concat', 'concat_ws', 'count', 'format', 'glob', 'group_concat', 'hex', 'if', 'ifnull', 'iif', 'instr', 'json', 'json_array', 'json_array_length', 'json_error_position', 'json_extract', 'json_group_array', 'json_group_object', 'json_object', 'json_quote', 'json_type', 'json_valid', 'length', 'like', 'likelihood', 'likely', 'lower', 'ltrim', 'max', 'min', 'nullif', 'octet_length', 'printf', 'quote', 'replace', 'round', 'rtrim', 'sign', 'substr', 'substring', 'sum', 'total', 'trim', 'typeof', 'unicode', 'unlikely', 'upper', 'row_number', 'rank', 'dense_rank', 'percent_rank', 'cume_dist', 'ntile', 'lag', 'lead', 'first_value', 'last_value', 'nth_value'];
export declare const SQL_TABLES: readonly [{
    readonly name: 'nodes';
    readonly columns: readonly ['id TEXT PRIMARY KEY', 'name TEXT', 'parent_id TEXT', 'tag TEXT', 'layer INTEGER', 'active INTEGER', 'active_in_hierarchy INTEGER', 'local_x REAL', 'local_y REAL', 'local_z REAL', 'world_x REAL', 'world_y REAL', 'world_z REAL', 'transform_json TEXT'];
}, {
    readonly name: 'components';
    readonly columns: readonly ['node_id TEXT', 'type TEXT', 'fields_json TEXT'];
}, {
    readonly name: 'scripts';
    readonly columns: readonly ['node_id TEXT', 'script_id TEXT', 'saved_script_id TEXT', 'type TEXT', 'status TEXT', 'enabled INTEGER', 'inspection_status TEXT', 'inspection_json TEXT'];
}, {
    readonly name: 'properties';
    readonly columns: readonly ['node_id TEXT', 'owner_kind TEXT', 'owner_type TEXT', 'script_id TEXT', 'path TEXT', 'value_type TEXT', 'text_value TEXT', 'number_value REAL', 'bool_value INTEGER', 'json_value TEXT'];
}, {
    readonly name: 'object_references';
    readonly columns: readonly ['node_id TEXT', 'owner_kind TEXT', 'owner_type TEXT', 'script_id TEXT', 'path TEXT', 'target_id TEXT', 'target_guid TEXT', 'expected_type TEXT', 'status TEXT'];
}, {
    readonly name: 'asset_references';
    readonly columns: readonly ['scan_job INTEGER', 'owner_path TEXT', 'target_path TEXT', 'field TEXT', 'kind TEXT', 'line INTEGER'];
}, {
    readonly name: 'asset_reference_scans';
    readonly columns: readonly ['job INTEGER', 'target_path TEXT', 'finished INTEGER', 'stale INTEGER', 'limited INTEGER', 'owner_count INTEGER', 'owners_indexed INTEGER', 'processed INTEGER', 'candidates INTEGER', 'unreadable INTEGER', 'unsupported INTEGER', 'oversized INTEGER', 'malformed INTEGER', 'unresolved_hints INTEGER'];
}];
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
export declare function EmptySqlTables(): SqlTables;
