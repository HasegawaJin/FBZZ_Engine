/**
 * 単一のツールスキーマを draft 2020-12 へ正規化する。
 * ルートには方言を明示しておき、受け手が draft-07 前提で解釈するのを防ぐ。
 */
export declare function ToDraft2020Schema(schema: unknown): unknown;
/**
 * tools/list 応答に含まれる全ツールの input/output スキーマを正規化する。
 * 形が想定と違えば素通しし、ツール一覧そのものを壊さない。
 */
export declare function ToDraft2020ToolsResult(result: unknown): unknown;
