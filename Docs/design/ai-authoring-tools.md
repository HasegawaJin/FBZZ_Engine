<!-- @file    ai-authoring-tools.md -->
<!-- @brief   Roslyn・制作専用Op・読み取り専用SQLによるAI制作の契約。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-11-06 -->
# AI制作の解析・配置・横断検索

ユーザーが選択した C#＋Roslyn、C++の制作専用Op、SQL／SQLite を既存Editor MCPへ接続する。
操作と権限は [Editorの直接API](EditorDirectApi.md)、設定と保存は
[C#設定transaction](csharp-settings-transactions.md)を正本とする。

## 境界

- C#解析は外部 .NET 10 processで行う。ゲームコードやconstructorを実行しない。
- 配置はEditorのOperatorRegistryへ登録し、既存 `editor_op_query` / `editor_op_invoke` から呼ぶ。
- 川を配置したら、保存前に `editor_op_query` の `terrain.river.validate` で違反座標を調べる。しきい値と流出先は [川の自然ルール](river-nature-rules.md) に従う。
- HeightMap の取り込みや Sculpt 後は `terrain.relief.validate` で急斜面や孤立した起伏の座標を調べる。意図した崖・谷の許容値は [地形制作](terrain-authoring.md#起伏の制作時検査) に従う。
- 尾根・谷は `terrain.relief.sculpt` で成形し、池の水面は `control_points.fit_lake_shore` で岸へ合わせる。引数と Island.scene の制作例は [地形と岸の成形](terrain-authoring.md#地形と岸の成形) に従う。
- SQLはEditorから取得したデータの読み取り用索引であり、Sceneやアセットの正本ではない。
- `authoring.context` Queryが接続先projectとSceneを返す。解析対象を要求者の任意のルートへ切り替えない。
- 保存済アセット、現在Scene、解析source snapshotは異なる時点・範囲であり、一つのatomic snapshotと称しない。

水系を新規に作るときは `terrain.drainage.inspect` で集水域と流出口を確認し、`control_points.fit_river_drainage` で支流ごとの経路を合わせる。閉じた窪地は `control_points.fit_lake_shore` の `useSpillLevel` を使える。木・草・岩は [PCG プロファイル](world-partition-pcg.md) に水位・地形・道からの距離の条件を分けて記述し、生成前に `pcg.preview` で除外理由を確認する。

## Roslyn

インストール済みSDKのcompiler APIを使い、source snapshotの診断、symbol列挙、参照検索、
改名候補を返す。入力ファイル、参照assembly、解析範囲と不足を結果へ含める。
通常のmanaged buildやgenerator/analyzerを含むプロジェクト全体の合否とは区別する。

改名は同じsymbolに束縛される宣言と使用箇所を扱い、自由な文字列置換を行わない。
preview後にsourceやprojectが変わった場合はapplyを拒否する。
public / Setting field、Script / DataAsset型など保存互換性に関わる対象は影響を示し、
保存キー・型GUIDの移行を保証できないapplyを拒否する。`.cs.meta`を生成し直さない。
readでは書き込みを公開せず、dry-runでもファイルを書き換えない。

## 配置

整列・等間隔配置・明示した水平基準面への配置を、world空間で計算する。
基準が原点か形状の底面かを入力・結果へ明記する。地形へのraycast接地を実行していない結果を、
Colliderへの接地成功として返さない。

previewとapplyは同じ計算を使う。入力、全対象の寿命、親変換、有限値、逆変換可能性を
変更前に検査する。local位置を正本としてworldを同期する。共通Undoで一括して戻せるようにし、
別個体や外部変更をUndo/Redoで上書きしない。Editの保存Sceneへ差分を適用し、Sceneを再生成しない。

## SQL

Node.js 24.10以降の組み込み `node:sqlite` を使用する。外部npmのSQLite実装は導入しない。
索引はMCPセッション単位で保持し、project/Scene、取得時刻、generation、範囲と欠測を返す。
Sceneのnode/componentと、取得できた保存済アセット参照を区別して格納する。
未収集・未対応・未完了・staleを、存在しない参照や成功へ読み替えない。

問い合わせは単一SELECT/CTEに限定する。SQLite authorizerとquery-only設定を使用し、
DDL/DML、ATTACH、PRAGMAや拡張機能のロードを拒否する。行数と出力容量を制限し、
問い合わせ専用processを期限で終了して長い再帰などでMCP本体を停止させない。
結果は索引の取得範囲に対する回答であり、更新には既存Opを使用する。

## 検証

Roslynは同名の別symbol、参照範囲、source変更、保存対象の改名拒否を検証する。
配置はworld/local、親子、無効入力の無変更、Undo/Redoと競合をTestKitで検証する。
SQLはJOIN、世代・欠測、書き込みと複数文の拒否、実processの時間制限を検証する。
MCPの全権限モードと公開schemaも既存の回帰検証へ含める。

## 参照

- [Roslyn compiler API](https://learn.microsoft.com/dotnet/csharp/roslyn-sdk/compiler-api-model)
- [Node.js SQLite](https://nodejs.org/api/sqlite.html)
- [SQLite authorizer](https://sqlite.org/c3ref/set_authorizer.html)
- [SQLite recursive query](https://sqlite.org/lang_with.html)
