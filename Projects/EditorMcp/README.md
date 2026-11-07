<!-- @file    README.md -->
<!-- @brief   Editor MCPの起動・制作解析・検証の入口。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-11-06 -->
# Editor MCP

EditorのローカルCommand Busをstdio MCPへ接続する。実行中EditorのOperatorRegistryと
公開C# SDKを正本とし、`read` / `dry-run` / `write`の権限を維持する。

## 前提とビルド

- Node.js 24.10以降。SQLは組み込み `node:sqlite` を使う。
- Roslyn解析には .NET 10 runtimeと、helperのビルドに .NET SDK 10.0.300を使う。
- 起動先は `dist/stdio.js`。Named Pipeと権限は既存 `FBZZ_EDITOR_PIPE` / `FBZZ_MCP_PERMISSION`で指定する。

このディレクトリで次を実行する。managed buildは共通AgentBuildの排他を使う。

```powershell
npm run build
npm run build:analysis
npm run test:analysis
npm test
```

通常のTypeScript buildとRoslyn helperのbuildは独立している。helperが未ビルドの場合でも
ツール一覧を取得でき、C#解析の実行時に不足する成果物を診断する。
helperは `Managed/Fbzz.EditorAnalysis.dll`、または
`Managed/bin/{Release,Development,Debug}/net10.0/Fbzz.EditorAnalysis.dll`から解決する。
helperのビルド出力にある依存DLL、runtimeconfig、depsも保持する。

## 制作の入口

| 入口 | 用途 |
|---|---|
| `csharp_analyze` | Assets/Scriptsのsource snapshotをRoslynで診断する。 |
| `csharp_symbols` | 宣言された型・メンバー等を列挙する。 |
| `csharp_find_references` | ソース位置のsymbolに束縛される宣言・参照を探す。 |
| `csharp_rename_preview` | 同一symbolの変更候補と保存互換性に関わる拒否理由を返す。 |
| `csharp_rename_apply` | 同じproject/source snapshotにだけ計画を適用する。readでは非公開。 |
| `editor_op_query` → `authoring.context` | 接続先project・Scene世代・現在のSDK配置先を取得する。 |
| `editor_op_query` → `authoring.placement.preview` | world整列・等間隔・水平面への配置の差分を取得する。 |
| `editor_op_invoke` → `authoring.placement.apply` | previewの計画状態を照合し、共通Undoへ一括適用する。 |
| `sql_index_refresh` | 現在Scene等の読み取り用索引を更新する。 |
| `sql_index_status` / `sql_index_schema` | 取得範囲・世代・欠測と、問い合わせ可能な表を確認する。 |
| `sql_query` | 索引に単一SELECT/CTEを実行する。 |

配置の引数は `editor_op_list`のparamsから取得する。applyにはpreviewが返す`expected`を渡す。
平面への配置は地形のraycast接地を意味しない。meshBottomは対応する静的メッシュの底面を使い、
形状が解決できないときは成功として扱わない。

C#解析はcompiler APIによるsource snapshotで、MSBuildのproject評価・generator・analyzer・
実行中Scriptの検証は含まない。結果の入力一覧・参照assembly・不足を確認する。
保存対象のfieldやScript/DataAsset型の改名は、保存キーやGUIDの移行を保証できないため適用を拒否する。
通常の `build_run(target="managed")`と要求IDのapplied確認は引き続き必要になる。

## SQLの問い合わせ

最初に `sql_index_refresh` と `sql_index_schema`を呼び、取得できた範囲を確認する。
node/componentの検索だけなら `sql_index_refresh(inspectNodeLimit=0)`で個別Script照会を省ける。
Scriptの設定・参照も調べる場合は、必要な範囲に合わせてinspectNodeLimitを指定する。
問い合わせにはrefreshが返したgenerationを渡す。
Scene、components、Script inspection、保存済アセット参照は取得元と時点が異なる。
結果が部分取得の場合、0行は全projectに対象が存在しないという意味にはならない。

```sql
SELECT n.id, n.name, c.type
FROM nodes AS n
JOIN components AS c ON c.node_id = n.id
ORDER BY n.name, c.type;
```

```sql
SELECT n.name, s.type, s.status, s.inspection_status
FROM nodes AS n
JOIN scripts AS s ON s.node_id = n.id
WHERE s.enabled = 0
ORDER BY n.name;
```

`asset_references`は既存 `asset.references.scan/status`の結果を取り込んだ場合に参照できる。
`asset_reference_scans`のfinished/stale/limitedと解析不能件数を確認する。
変更はSQLでは行わず、検索結果のIDを既存Opへ渡す。
問い合わせは子processで時間・出力・SQLiteメモリの上限を設定して実行する。

詳細は [AI制作ツールの契約](../../Docs/design/ai-authoring-tools.md)、
[Editor直接API](../../Docs/design/EditorDirectApi.md)を参照する。
