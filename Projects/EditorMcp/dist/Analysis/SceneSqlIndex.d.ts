import type { EditorBus } from '../busClient.js';
import { type SqlCell } from './SqlSchema.js';
export interface SqlRefreshOptions {
    inspectNodeLimit?: number;
    referenceJobs?: number[];
}
export declare class SceneSqlIndex {
    private m_index;
    private m_refreshing;
    private m_querying;
    Status(): Record<string, unknown>;
    Schema(): Record<string, unknown>;
    Refresh(bus: EditorBus, options?: SqlRefreshOptions): Promise<Record<string, unknown>>;
    Query(bus: EditorBus, generation: string, sql: string, parameters?: SqlCell[], maxRows?: number, maxBytes?: number, timeoutMs?: number): Promise<Record<string, unknown>>;
}
