import { type SqlProcessRequest, type SqlProcessResult } from './SqlSchema.js';
export declare function ValidateReadSql(sql: string): void;
export declare function ExecuteSql(request: SqlProcessRequest): SqlProcessResult;
