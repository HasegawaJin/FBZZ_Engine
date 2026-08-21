// FBZZ Engine
// toolSchemas.test.ts | EditorMcp
// 公開ツールの input_schema が draft 2020-12 として合法であることを検証する
//
// WHY: Anthropic Messages API は tool.input_schema を draft 2020-12 で検証し、
//      1 ツールでも違反すると会話全体が 400 (tools.N.custom.input_schema is invalid)
//      で落ちる。MCP SDK の既定出力は draft-07 で、z.tuple() が `items: [..]`
//      になるなど 2020-12 では非合法な形が混ざるため、回帰をここで止める。
import assert from 'node:assert/strict';
import test from 'node:test';
import { Client } from '@modelcontextprotocol/sdk/client/index.js';
import { InMemoryTransport } from '@modelcontextprotocol/sdk/inMemory.js';
import { CreateEditorMcpServer } from '../server.js';
import { ToDraft2020Schema } from '../jsonSchemaDraft2020.js';
// スキーマ列挙しかしないので bus は呼ばれない。呼ばれたら明確に失敗させる。
class UnusedBus {
    async Query() {
        throw new Error('スキーマ検証で bus を呼んではいけません');
    }
    async Command() {
        throw new Error('スキーマ検証で bus を呼んではいけません');
    }
    Close() { }
}
async function ListToolSchemas(permission) {
    const server = CreateEditorMcpServer(new UnusedBus(), permission);
    const client = new Client({ name: 'fbzz-editor-mcp-schema-test', version: '0.1.0' });
    const [clientTransport, serverTransport] = InMemoryTransport.createLinkedPair();
    await server.connect(serverTransport);
    await client.connect(clientTransport);
    try {
        return (await client.listTools()).tools;
    }
    finally {
        await client.close();
        await server.close();
    }
}
// draft-07 でしか合法でない書き方を再帰的に洗い出す。見つかった箇所を JSON Pointer で返す。
function FindDraft07Violations(node, path, found) {
    if (Array.isArray(node)) {
        node.forEach((child, index) => FindDraft07Violations(child, `${path}/${index}`, found));
        return found;
    }
    if (typeof node !== 'object' || node === null) {
        return found;
    }
    const schema = node;
    if (Array.isArray(schema.items)) {
        found.push(`${path}/items (タプルは prefixItems であるべき)`);
    }
    if (schema.additionalItems !== undefined) {
        found.push(`${path}/additionalItems (2020-12 では items)`);
    }
    if (schema.definitions !== undefined) {
        found.push(`${path}/definitions (2020-12 では $defs)`);
    }
    if (typeof schema.$ref === 'string' && schema.$ref.startsWith('#/definitions/')) {
        found.push(`${path}/$ref -> ${schema.$ref}`);
    }
    if (typeof schema.exclusiveMinimum === 'boolean' || typeof schema.exclusiveMaximum === 'boolean') {
        found.push(`${path}/exclusive* (draft-04 の boolean 形)`);
    }
    for (const [key, value] of Object.entries(schema)) {
        FindDraft07Violations(value, `${path}/${key}`, found);
    }
    return found;
}
for (const permission of ['read', 'dry-run', 'write']) {
    test(`${permission} モードの全ツールが draft 2020-12 の input_schema を返す`, async () => {
        const tools = await ListToolSchemas(permission);
        assert.ok(tools.length > 0, 'ツールが 1 つも公開されていない');
        const violations = [];
        for (const tool of tools) {
            assert.equal(tool.inputSchema.$schema, 'https://json-schema.org/draft/2020-12/schema', `${tool.name} が draft 2020-12 を宣言していない`);
            for (const hit of FindDraft07Violations(tool.inputSchema, '', [])) {
                violations.push(`${tool.name}${hit}`);
            }
        }
        assert.deepEqual(violations, [], `draft-07 固有の書き方が残っている:\n${violations.join('\n')}`);
    });
}
test('固定長タプルは prefixItems と要素数制約へ変換される', () => {
    const converted = ToDraft2020Schema({
        type: 'object',
        properties: {
            origin: { type: 'array', items: [{ type: 'number' }, { type: 'number' }, { type: 'number' }] },
        },
    });
    const origin = converted.properties.origin;
    assert.deepEqual(origin.prefixItems, [{ type: 'number' }, { type: 'number' }, { type: 'number' }]);
    assert.equal(origin.items, undefined);
    assert.equal(origin.minItems, 3);
    assert.equal(origin.maxItems, 3);
});
test('rest 付きタプルは additionalItems を items へ移し上限を付けない', () => {
    const converted = ToDraft2020Schema({
        type: 'array',
        items: [{ type: 'string' }],
        additionalItems: { type: 'number' },
    });
    assert.deepEqual(converted.prefixItems, [{ type: 'string' }]);
    assert.deepEqual(converted.items, { type: 'number' });
    assert.equal(converted.additionalItems, undefined);
    assert.equal(converted.minItems, 1);
    assert.equal(converted.maxItems, undefined);
});
test('definitions は $defs へ移り $ref も追従する', () => {
    const converted = ToDraft2020Schema({
        type: 'object',
        properties: { value: { $ref: '#/definitions/__schema0' } },
        definitions: { __schema0: { type: 'string' } },
    });
    assert.deepEqual(converted.$defs, { __schema0: { type: 'string' } });
    assert.equal(converted.definitions, undefined);
    assert.equal(converted.properties.value.$ref, '#/$defs/__schema0');
});
//# sourceMappingURL=toolSchemas.test.js.map