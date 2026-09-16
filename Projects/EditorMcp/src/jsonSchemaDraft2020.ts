// FBZZ Engine
// jsonSchemaDraft2020.ts | EditorMcp
// MCP SDK が吐く draft-07 JSON Schema を draft 2020-12 へ正規化する
//
// WHY: @modelcontextprotocol/sdk 1.29 の tools/list は zod → JSON Schema 変換で
//      target を 'draft-7' に固定している (server/zod-json-schema-compat.js)。
//      一方 Anthropic Messages API の tool.input_schema は **draft 2020-12** で
//      検証されるため、draft-07 でしか合法でない書き方が 1 つでも混ざると
//      リクエスト全体が 400 で落ちる:
//        API Error: 400 tools.N.custom.input_schema: JSON schema is invalid.
//      実際に z.tuple() (Vec3 など) が `"items": [ ... ]` (配列形) になり、
//      2020-12 では items が単一スキーマでなければならないため弾かれていた。
//      SDK 側に target を渡す口が無いので、出力後にここで一度だけ変換する。

/** JSON Schema の中身は任意なので、走査用に緩いレコード型で扱う。 */
type JsonSchemaObject = Record<string, unknown>;

/** draft 2020-12 の正式な $schema URI。Anthropic 側の期待方言を明示する。 */
const DRAFT_2020_12 = 'https://json-schema.org/draft/2020-12/schema';

function IsPlainObject(value: unknown): value is JsonSchemaObject {
    return typeof value === 'object' && value !== null && !Array.isArray(value);
}

// draft-07 のタプル表現 `items: [A, B]` を 2020-12 の prefixItems へ移す。
// rest 無しタプル (zod の既定) は要素数が固定なので minItems/maxItems で締め、
// rest 付き (additionalItems) はそれを items へ移して可変長のまま保つ。
function ConvertTupleItems(schema: JsonSchemaObject): void {
    const items = schema.items;
    if (!Array.isArray(items)) {
        return;
    }
    schema.prefixItems = items;
    delete schema.items;

    if (schema.additionalItems !== undefined) {
        schema.items = schema.additionalItems;
        delete schema.additionalItems;
        if (schema.minItems === undefined) {
            schema.minItems = items.length;
        }
        return;
    }
    // 追加要素を許さない固定長タプル。boolean スキーマ (items: false) を使わずに
    // 件数で表現しておくと、素朴な JSON Schema 実装でも解釈が揺れない。
    if (schema.minItems === undefined) {
        schema.minItems = items.length;
    }
    if (schema.maxItems === undefined) {
        schema.maxItems = items.length;
    }
}

// draft-07 の `definitions` は 2020-12 では `$defs`。参照している $ref も併せて書き換える。
function ConvertDefinitions(schema: JsonSchemaObject): void {
    if (schema.definitions !== undefined && schema.$defs === undefined) {
        schema.$defs = schema.definitions;
        delete schema.definitions;
    }
    const ref = schema.$ref;
    if (typeof ref === 'string' && ref.startsWith('#/definitions/')) {
        schema.$ref = `#/$defs/${ref.slice('#/definitions/'.length)}`;
    }
}

// draft-04 由来の boolean 形 exclusiveMinimum/Maximum を数値形へ寄せる。
// 現状の zod 出力には現れないが、変換系を差し替えたときに黙って壊れないよう防御する。
function ConvertExclusiveBounds(schema: JsonSchemaObject): void {
    if (schema.exclusiveMinimum === true && typeof schema.minimum === 'number') {
        schema.exclusiveMinimum = schema.minimum;
        delete schema.minimum;
    } else if (schema.exclusiveMinimum === false) {
        delete schema.exclusiveMinimum;
    }
    if (schema.exclusiveMaximum === true && typeof schema.maximum === 'number') {
        schema.exclusiveMaximum = schema.maximum;
        delete schema.maximum;
    } else if (schema.exclusiveMaximum === false) {
        delete schema.exclusiveMaximum;
    }
}

// スキーマ木を深さ優先で複製しつつ変換する。入力は SDK が握っている値なので破壊しない。
function ConvertNode(node: unknown): unknown {
    if (Array.isArray(node)) {
        return node.map(ConvertNode);
    }
    if (!IsPlainObject(node)) {
        return node;
    }
    const copy: JsonSchemaObject = {};
    for (const [key, value] of Object.entries(node)) {
        copy[key] = ConvertNode(value);
    }
    ConvertTupleItems(copy);
    ConvertDefinitions(copy);
    ConvertExclusiveBounds(copy);
    return copy;
}

/**
 * 単一のツールスキーマを draft 2020-12 へ正規化する。
 * ルートには方言を明示しておき、受け手が draft-07 前提で解釈するのを防ぐ。
 */
export function ToDraft2020Schema(schema: unknown): unknown {
    if (!IsPlainObject(schema)) {
        return schema;
    }
    const converted = ConvertNode(schema) as JsonSchemaObject;
    converted.$schema = DRAFT_2020_12;
    return converted;
}

/**
 * tools/list 応答に含まれる全ツールの input/output スキーマを正規化する。
 * 形が想定と違えば素通しし、ツール一覧そのものを壊さない。
 */
export function ToDraft2020ToolsResult(result: unknown): unknown {
    if (!IsPlainObject(result) || !Array.isArray(result.tools)) {
        return result;
    }
    return {
        ...result,
        tools: result.tools.map((tool) => {
            if (!IsPlainObject(tool)) {
                return tool;
            }
            const normalized: JsonSchemaObject = { ...tool };
            if (normalized.inputSchema !== undefined) {
                normalized.inputSchema = ToDraft2020Schema(normalized.inputSchema);
            }
            if (normalized.outputSchema !== undefined) {
                normalized.outputSchema = ToDraft2020Schema(normalized.outputSchema);
            }
            return normalized;
        }),
    };
}
