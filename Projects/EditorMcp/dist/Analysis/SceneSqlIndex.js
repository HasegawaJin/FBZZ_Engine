/// @file    SceneSqlIndex.ts
/// @brief   Editor の実データを世代付き SQL 検索用スナップショットへ写像する。
/// @author  Hasegawa Jin
/// @date    2026-11-06
import { spawn } from 'node:child_process';
import { createHash, randomUUID } from 'node:crypto';
import { fileURLToPath } from 'node:url';
import { EmptySqlTables, SQL_FUNCTIONS, SQL_LIMITS, SQL_TABLES } from './SqlSchema.js';
function ObjectValue(value) {
    return typeof value === 'object' && value !== null && !Array.isArray(value) ? value : null;
}
function StringValue(value) { return typeof value === 'string' ? value : null; }
function NumberValue(value) { return typeof value === 'number' && Number.isFinite(value) ? value : null; }
function BooleanValue(value) { return typeof value === 'boolean' ? value ? 1 : 0 : null; }
function ArrayValue(value) { return Array.isArray(value) ? value : []; }
function Unwrap(value) {
    const result = ObjectValue(value);
    return ObjectValue(result?.data) ?? result;
}
async function ReadScope(bus) {
    try {
        const value = Unwrap(await bus.Query({ t: 'editor.op.query', id: 'authoring.context' }));
        if (value && typeof value.projectRoot === 'string' && 'sceneGeneration' in value) {
            return { source: 'authoring.context', projectRoot: value.projectRoot,
                scenePath: StringValue(value.scenePath), sceneGeneration: StringValue(value.sceneGeneration),
                mode: StringValue(value.mode), verified: true };
        }
    }
    catch { }
    try {
        const value = ObjectValue(await bus.Query({ t: 'editor.state' }));
        if (value && typeof value.scene === 'string')
            return { source: 'editor.state', projectRoot: null,
                scenePath: value.scene, sceneGeneration: null, mode: StringValue(value.playState), verified: false };
    }
    catch { }
    return { source: 'unavailable', projectRoot: null, scenePath: null, sceneGeneration: null, mode: null, verified: false };
}
function ScopeChanged(before, after) {
    return before.source === after.source && before.source !== 'unavailable'
        && (before.projectRoot !== after.projectRoot || before.scenePath !== after.scenePath
            || before.sceneGeneration !== after.sceneGeneration || before.mode !== after.mode);
}
function ErrorText(error) { return error instanceof Error ? error.message : String(error); }
/// @note 一つの索引は複数の Query に由来する。Scene 内容の同時点性や保存済み全プロジェクトの網羅を保証しない。
/// @see Docs/design/ai-authoring-tools.md
export class SceneSqlIndex {
    m_index;
    m_refreshing = false;
    m_querying = false;
    Status() {
        return this.m_index ? { ready: true, refreshing: this.m_refreshing, querying: this.m_querying,
            ...this.m_index.capture, tableCounts: Object.fromEntries(Object.entries(this.m_index.tables).map(([name, rows]) => [name, rows.length])) }
            : { ready: false, refreshing: this.m_refreshing, querying: this.m_querying };
    }
    Schema() {
        return {
            schemaVersion: 1,
            tables: SQL_TABLES.map(table => ({ name: table.name, columns: table.columns })),
            functions: SQL_FUNCTIONS,
            limits: SQL_LIMITS,
            integerEncoding: 'safe-number-or-decimal-string',
            notes: [
                '索引は取得時点の派生データです。現在のScene内容や全プロジェクトの完全性は保証しません。',
                'scripts はinspectNodeLimit件までの明示的な個体照会だけを含みます。deferred値は展開しません。',
                'properties.path はRFC 6901形式です。scriptのpathはinspection応答内のパスです。',
                'asset_references は指定した既存scan jobの結果だけです。未保存・動的参照は対象外です。',
                '単一SELECT/CTEのみ。parametersは位置パラメーター(?)です。SQLからEditorは変更できません。',
            ],
            examples: [
                "SELECT n.id,n.name,c.type FROM nodes n JOIN components c ON c.node_id=n.id WHERE c.type=? ORDER BY n.name",
                "SELECT n.name,s.type,s.status FROM nodes n JOIN scripts s ON s.node_id=n.id WHERE s.status='missing'",
                'SELECT owner_path,target_path,field,line FROM asset_references ORDER BY owner_path,line',
                "SELECT n.name,r.path,r.expected_type FROM nodes n JOIN object_references r ON r.node_id=n.id WHERE r.status='missing'",
            ],
        };
    }
    async Refresh(bus, options = {}) {
        if (this.m_refreshing)
            throw new Error('SQL_INDEX_BUSY: 索引を取得中です');
        const inspectionLimit = options.inspectNodeLimit ?? 128;
        if (!Number.isInteger(inspectionLimit) || inspectionLimit < 0 || inspectionLimit > 512)
            throw new Error('SQL_INDEX_ARGUMENT: inspectNodeLimit は0..512です');
        const jobs = [...new Set(options.referenceJobs ?? [])];
        if (jobs.length > 16 || jobs.some(job => !Number.isSafeInteger(job) || job < 1))
            throw new Error('SQL_INDEX_ARGUMENT: referenceJobs は最大16件の正整数です');
        this.m_refreshing = true;
        try {
            const scope = await ReadScope(bus);
            const snapshot = ObjectValue(await bus.Query({ t: 'scene.snapshot' }));
            if (!snapshot || !Array.isArray(snapshot.nodes))
                throw new Error('SQL_INDEX_SOURCE: scene.snapshot にnodes配列がありません');
            const sourceText = JSON.stringify(snapshot);
            if (Buffer.byteLength(sourceText) > SQL_LIMITS.inputBytes / 2)
                throw new Error('SQL_INDEX_LIMIT: Scene snapshotが8 MiBを超えています');
            const tables = EmptySqlTables();
            const warnings = [];
            const coverage = { source: 'scene.snapshot', sourceNodeCount: NumberValue(snapshot.count) ?? snapshot.nodes.length,
                indexedNodes: 0, excludedGenerated: NumberValue(snapshot.excludedGenerated) ?? 0,
                inspectedNodes: 0, inspectionNodeLimit: inspectionLimit, nodeInspectionFailures: 0, scriptInspectionFailures: 0,
                scriptInspections: 0, scriptsOutsideInspectedNodes: 'unverified', propertyRowsLimited: false,
                omittedCells: 0, skippedNestedValues: 0, assetReferences: 'requested-existing-scan-jobs-only', referenceJobs: jobs };
            const jsonCell = (value) => {
                const text = JSON.stringify(value);
                if (text === undefined)
                    return null;
                if (Buffer.byteLength(text) <= SQL_LIMITS.cellBytes)
                    return text;
                ++coverage.omittedCells;
                return null;
            };
            const textCell = (value) => {
                const text = StringValue(value);
                if (text === null || Buffer.byteLength(text) <= SQL_LIMITS.cellBytes)
                    return text;
                ++coverage.omittedCells;
                return null;
            };
            const properties = (value, nodeId, kind, type, scriptId, path = '', depth = 0) => {
                if (depth > 32) {
                    ++coverage.skippedNestedValues;
                    return;
                }
                if (tables.properties.length >= SQL_LIMITS.propertyRows) {
                    coverage.propertyRowsLimited = true;
                    return;
                }
                const object = ObjectValue(value);
                if (object && typeof object.status === 'string' && ('expectedType' in object || 'guid' in object)
                    && ('id' in object || 'guid' in object)) {
                    tables.object_references.push([nodeId, kind, type, scriptId, path, textCell(object.id), textCell(object.guid),
                        textCell(object.expectedType), object.status]);
                }
                if (Array.isArray(value)) {
                    if (value.length === 0)
                        tables.properties.push([nodeId, kind, type, scriptId, path, 'array', null, null, null, '[]']);
                    for (let index = 0; index < value.length; ++index) {
                        properties(value[index], nodeId, kind, type, scriptId, `${path}/${index}`, depth + 1);
                        if (coverage.propertyRowsLimited)
                            break;
                    }
                }
                else if (object) {
                    if (Object.keys(object).length === 0)
                        tables.properties.push([nodeId, kind, type, scriptId, path, 'object', null, null, null, '{}']);
                    for (const [key, child] of Object.entries(object)) {
                        properties(child, nodeId, kind, type, scriptId, `${path}/${key.replace(/~/g, '~0').replace(/\//g, '~1')}`, depth + 1);
                        if (coverage.propertyRowsLimited)
                            break;
                    }
                }
                else {
                    const valueType = value === null ? 'null' : typeof value;
                    tables.properties.push([nodeId, kind, type, scriptId, path, valueType, textCell(value), NumberValue(value), BooleanValue(value), jsonCell(value)]);
                }
            };
            const ids = new Set();
            for (const raw of snapshot.nodes.slice(0, SQL_LIMITS.nodes)) {
                const node = ObjectValue(raw);
                if (!node || typeof node.id !== 'string' || !node.id || ids.has(node.id))
                    throw new Error('SQL_INDEX_SOURCE: NodeIdが欠損または重複しています');
                ids.add(node.id);
                const transform = ObjectValue(node.transform);
                const local = ArrayValue(transform?.position);
                const world = ArrayValue(transform?.worldPosition);
                tables.nodes.push([node.id, textCell(node.name), textCell(node.parent), textCell(node.tag), NumberValue(node.layer),
                    BooleanValue(node.active), BooleanValue(node.activeInHierarchy),
                    ...[0, 1, 2].map(index => NumberValue(local[index])), ...[0, 1, 2].map(index => NumberValue(world[index])), jsonCell(transform)]);
                if (!Array.isArray(node.components))
                    throw new Error('SQL_INDEX_SOURCE: components配列がありません');
                for (const rawComponent of node.components) {
                    const component = ObjectValue(rawComponent);
                    if (!component || typeof component.type !== 'string')
                        throw new Error('SQL_INDEX_SOURCE: Component型がありません');
                    tables.components.push([node.id, component.type, jsonCell(component.fields)]);
                    properties(component.fields, node.id, 'component', component.type, null);
                }
            }
            coverage.indexedNodes = tables.nodes.length;
            if (coverage.sourceNodeCount !== snapshot.nodes.length)
                warnings.push('source-count-mismatch: snapshotのcountとnodes件数が一致しません');
            if (snapshot.nodes.length > SQL_LIMITS.nodes)
                warnings.push(`node-limit: ${snapshot.nodes.length}件中${SQL_LIMITS.nodes}件だけ取得`);
            for (const nodeId of [...ids].slice(0, inspectionLimit)) {
                try {
                    const node = ObjectValue(await bus.Query({ t: 'node.components', id: nodeId }));
                    if (!node || !Array.isArray(node.scripts))
                        throw new Error('scripts配列がありません');
                    ++coverage.inspectedNodes;
                    for (const rawScript of node.scripts) {
                        const script = ObjectValue(rawScript);
                        if (!script || typeof script.scriptId !== 'string' || typeof script.type !== 'string') {
                            ++coverage.scriptInspectionFailures;
                            continue;
                        }
                        let inspection = null;
                        let inspectionStatus = 'unverified';
                        if (coverage.scriptInspections + coverage.scriptInspectionFailures >= 1024) {
                            inspectionStatus = 'script-limit';
                        }
                        else
                            try {
                                inspection = ObjectValue(await bus.Query({ t: 'script.inspect', id: nodeId, scriptId: script.scriptId }));
                                if (!inspection)
                                    throw new Error('Script応答がobjectではありません');
                                inspectionStatus = 'summary';
                                ++coverage.scriptInspections;
                                for (const key of ['fields', 'settings', 'observations', 'references', 'deferred']) {
                                    if (key in inspection)
                                        properties(inspection[key], nodeId, 'script', script.type, script.scriptId, `/${key}`);
                                }
                            }
                            catch {
                                inspectionStatus = 'failed';
                                ++coverage.scriptInspectionFailures;
                            }
                        tables.scripts.push([nodeId, script.scriptId, textCell(script.savedScriptId), script.type, textCell(script.status),
                            BooleanValue(script.enabled), inspectionStatus, jsonCell(inspection)]);
                    }
                }
                catch {
                    ++coverage.nodeInspectionFailures;
                }
            }
            if (coverage.inspectedNodes < coverage.indexedNodes)
                warnings.push('scripts-partial: scripts表は照会済みノードだけを含みます');
            if (coverage.nodeInspectionFailures || coverage.scriptInspectionFailures)
                warnings.push('inspection-failed: 取得できない値を空値や正常状態で補完していません');
            for (const job of jobs) {
                try {
                    let offset = 0;
                    let report = null;
                    let ownersIndexed = 0;
                    do {
                        const page = Unwrap(await bus.Query({ t: 'editor.op.query', id: 'asset.references.status', args: { job, maxFiles: 1, offset, limit: 100 } }));
                        if (!page || !Array.isArray(page.owners) || typeof page.target !== 'string')
                            throw new Error('参照scan応答が不正です');
                        report = page;
                        for (const rawOwner of page.owners) {
                            const owner = ObjectValue(rawOwner);
                            if (!owner || typeof owner.path !== 'string')
                                continue;
                            ++ownersIndexed;
                            for (const rawEvidence of ArrayValue(owner.evidence)) {
                                const evidence = ObjectValue(rawEvidence);
                                if (evidence)
                                    tables.asset_references.push([job, owner.path, page.target, textCell(evidence.field), textCell(evidence.kind), NumberValue(evidence.line)]);
                            }
                        }
                        offset += page.owners.length;
                        if (!page.finished || page.stale || page.owners.length === 0)
                            break;
                    } while (offset < (NumberValue(report?.ownerCount) ?? 0) && offset < 10000);
                    if (report) {
                        tables.asset_reference_scans.push([job, textCell(report.target), BooleanValue(report.finished), BooleanValue(report.stale), BooleanValue(report.limited),
                            NumberValue(report.ownerCount), ownersIndexed, NumberValue(report.processed), NumberValue(report.candidates), NumberValue(report.unreadable),
                            NumberValue(report.unsupported), NumberValue(report.oversized), NumberValue(report.malformed), NumberValue(report.unresolvedHints)]);
                        if (!report.finished || report.stale || report.limited || ownersIndexed !== report.ownerCount)
                            warnings.push(`reference-job-${job}: 未完了・古い・制限された参照結果`);
                    }
                }
                catch (error) {
                    warnings.push(`reference-job-${job}: ${ErrorText(error).slice(0, 256)}`);
                }
            }
            const afterScope = await ReadScope(bus);
            if (ScopeChanged(scope, afterScope))
                throw new Error('SQL_INDEX_SCOPE_CHANGED: 取得中にSceneまたはプロジェクトが変わりました');
            if (!scope.verified || !afterScope.verified || scope.source !== afterScope.source)
                warnings.push('scope-unverified: Scene個体とprojectの同一性を検証できません');
            const capture = { generation: randomUUID(), capturedAt: new Date().toISOString(),
                sourceHash: createHash('sha256').update(sourceText).digest('hex'),
                scope: { ...scope, verified: scope.verified && afterScope.verified && scope.source === afterScope.source },
                consistency: 'single-scene-snapshot-plus-sequential-inspection', coverage, warnings };
            if (Buffer.byteLength(JSON.stringify(tables)) > SQL_LIMITS.inputBytes - SQL_LIMITS.queryBytes - 4096)
                throw new Error('SQL_INDEX_LIMIT: 索引の転送予算16 MiBを超えました');
            this.m_index = { capture, tables };
            return { ...this.Status(), refreshing: false };
        }
        finally {
            this.m_refreshing = false;
        }
    }
    async Query(bus, generation, sql, parameters = [], maxRows = 100, maxBytes = SQL_LIMITS.resultBytes, timeoutMs = SQL_LIMITS.timeoutMs) {
        const index = this.m_index;
        if (!index)
            throw new Error('SQL_INDEX_NOT_READY: sql_index_refreshを先に呼んでください');
        if (generation !== index.capture.generation)
            throw new Error('SQL_INDEX_GENERATION: 索引の世代が一致しません');
        if (this.m_querying)
            throw new Error('SQL_QUERY_BUSY: このセッションのSQLを実行中です');
        if (!Number.isInteger(maxRows) || maxRows < 1 || maxRows > SQL_LIMITS.resultRows
            || !Number.isInteger(maxBytes) || maxBytes < 1024 || maxBytes > SQL_LIMITS.resultBytes
            || !Number.isInteger(timeoutMs) || timeoutMs < 100 || timeoutMs > SQL_LIMITS.timeoutMs
            || parameters.length > 128 || parameters.some(value => value !== null && typeof value !== 'string' && (typeof value !== 'number' || !Number.isFinite(value)))) {
            throw new Error('SQL_QUERY_ARGUMENT: 行数・bytes・timeout・パラメーターが上限外です');
        }
        this.m_querying = true;
        try {
            const current = await ReadScope(bus);
            if (ScopeChanged(index.capture.scope, current))
                throw new Error('SQL_INDEX_STALE_SCOPE: 別Sceneの索引です。再取得してください');
            const result = await RunSqlProcess({ tables: index.tables, sql, parameters, maxRows, maxBytes }, timeoutMs);
            return { ...result, generation, capturedAt: index.capture.capturedAt,
                integerEncoding: 'safe-number-or-decimal-string',
                freshness: { content: 'captured-snapshot-only', scopeVerified: index.capture.scope.verified && current.verified && index.capture.scope.source === current.source,
                    comparedCurrentScope: current.source !== 'unavailable' }, coverage: index.capture.coverage, warnings: index.capture.warnings };
        }
        finally {
            this.m_querying = false;
        }
    }
}
/// @note SQLiteのnative同期呼出しにもdeadlineを効かせるため、timeout時は専用OSプロセスを終了する。
/// @see https://nodejs.org/api/child_process.html#child_processspawncommand-args-options Node child process lifecycle
function RunSqlProcess(request, timeoutMs) {
    const input = JSON.stringify(request);
    if (Buffer.byteLength(input) > SQL_LIMITS.inputBytes)
        return Promise.reject(new Error('SQL_INPUT_LIMIT: 転送予算を超えました'));
    return new Promise((resolve, reject) => {
        const child = spawn(process.execPath, ['--max-old-space-size=128', fileURLToPath(new URL('./SqlQueryProcess.js', import.meta.url))], {
            windowsHide: true, stdio: ['pipe', 'pipe', 'pipe'], env: { ...process.env, NODE_OPTIONS: '' },
        });
        let output = '';
        let stderr = '';
        let terminalError;
        const timer = setTimeout(() => { terminalError = new Error('SQL_TIMEOUT: SQLの実行時間上限を超えました'); child.kill(); }, timeoutMs);
        child.stdout.on('data', chunk => {
            output += String(chunk);
            if (Buffer.byteLength(output) > SQL_LIMITS.resultBytes + 4096) {
                terminalError = new Error('SQL_OUTPUT_LIMIT: 結果の転送上限を超えました');
                child.kill();
            }
        });
        child.stderr.on('data', chunk => { if (stderr.length < 2048)
            stderr += String(chunk).slice(0, 2048 - stderr.length); });
        child.stdin.on('error', error => { if (!terminalError)
            terminalError = error; });
        child.on('error', error => { terminalError = error; });
        child.on('close', () => {
            clearTimeout(timer);
            if (terminalError) {
                reject(terminalError);
                return;
            }
            try {
                const response = JSON.parse(output);
                if (!response.ok || !response.result)
                    reject(new Error(response.error ?? 'SQL_PROCESS_FAILED: 解析結果がありません'));
                else
                    resolve(response.result);
            }
            catch {
                reject(new Error(`SQL_PROCESS_FAILED: ${stderr || '子プロセスが有効な結果を返しませんでした'}`));
            }
        });
        child.stdin.end(input);
    });
}
//# sourceMappingURL=SceneSqlIndex.js.map