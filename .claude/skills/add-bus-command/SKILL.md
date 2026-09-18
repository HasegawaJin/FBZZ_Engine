---
name: add-bus-command
description: FBZZ Editor の AI Command Bus (MCP から Editor を操作する口) に Query / Command を 1 つ足す手順。「AI から〇〇できるようにして」「MCP ツールを追加」「editor bus に型を足す」のときに使う。C++ のハンドラー表と TypeScript 契約と MCP ツールを同時に揃える。
---

# Command Bus に型を足す

まず **Operator で足りないか** を考える。メニュー / ホットキー / パレットにも出すべき操作なら `Editor/src/Op/` の登録簿に 1 件足せば、`editor_op_invoke` で AI からも呼べる (Docs/design/editor-operator-model.md)。バスに型を足すのは «AI にだけ要る照会・構造化された結果» のとき。

## 1. C++ ハンドラー (`Projects/Editor/src/Ai/Bus/<領域>Handlers.cpp`)

- 領域のファイルの無名 namespace に `Outcome DoXxx(BusCall& call)` を書く。既存の同種ハンドラーに倣う
- 同じファイルの `Register<領域>Handlers` に 1 行:
  - 副作用なし → `table.AddQuery("area.name", DoXxx);`
  - 直接実行 (dryRun は自分で扱う) → `table.AddCommand("area.name", DoXxx);`
  - Undo 可能な編集 → `BuilderFn` を書いて `table.AddBuilder("area.name", BuildXxx);` (適用・dryRun・transaction は Dispatcher が持つ)
- 守ること: `ctx.activeScene` 等は nullptr がありうる (Play 中の遷移)。必ず `Outcome::Err("NO_SCENE", ...)` を返す。dryRun では何も変えない
- 共有の補助は `Bus/BusInternal.hpp` (StringField / ReadVec3 / ResolveProjectFile / DryRunPreview …)
- 新しい領域ファイルを作ったら `BusInternal.hpp` に `Register…` を宣言し、`EditorBusDispatcher` のコンストラクタで呼ぶ (自己登録の静的初期化子は STATIC ライブラリで捨てられるので使わない)

## 2. 契約 (`Projects/EditorMcp/src/editorContracts.ts`)

- `EditorQuery` / `EditorCommand` の型の union に 1 行、対応する zod スキーマ (`EditorQuerySchema` / `EditorCommandSchema`) に 1 行
- 型名は C++ と一字一句同じ。`BusContractParityTests` が食い違いを落とす

## 3. MCP ツール (`Projects/EditorMcp/src/tools.ts`)

- Query は `RegisterQueryTools`、Command は `RegisterCommandTools` (`run(...)` が権限と dryRun を処理する)
- description に «いつ使うか・前提・次に呼ぶツール» を書く。非同期なら poll 先を書く
- ツール名は既存と衝突しないこと (起動時に `already registered` で落ちる)

## 4. テスト

- `Projects/Tests/Editor/Auto/EditorBusDispatcherTests.cpp` の `@@COMMANDS_BEGIN` 一覧へ型名を足す (シーン無し・空シーン・dryRun で落ちないことを全型に対して確かめる)
- 振る舞いのテストが要るなら同じディレクトリに `TEST_F` で足し、`Projects/Tests/CMakeLists.txt` に登録

## 5. 検証

```
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/AgentBuild.ps1 check Projects/Editor/src/Ai/Bus/<領域>Handlers.cpp
cd Projects/EditorMcp && npm test
```

C++ のテストは `verify-cpp` スキルの手順で `FBZZTestsEditorAuto` を回す (`-Filter "EditorBusDispatcher|BusContractParity"`)。
