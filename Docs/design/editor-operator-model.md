# Editor Operator モデル設計

エディターの「操作」を第一級オブジェクトにして、メニュー・ホットキー・コマンドパレット・
AI (MCP) の 4 つをすべて **同じレジストリの投影** にする。

---

## 1. 解決したい問題

### 1.1 同じ操作が 6 回書かれている

現状、1 つの操作は次の 6 箇所へ独立して書かれる。

| 面 | 保持している形 | 実装 |
|----|----------------|------|
| ImGui メニューバー | `ImGui::MenuItem(label, shortcut, false, enabled)` | `EditorApp_MenuBar.cpp` |
| ネイティブ Win32 メニュー | `switch (id) { case NEW_SCENE: ... }` | 同上 `InstallNativeMenuBar` |
| Play ツールバー | `PlayToolbarButton(..., enabled, ...)` | 同上 `BuildPlayToolbar` |
| ホットキー | `Hotkey{name, imguiKey, callback, enabled}` | `Util/HotkeyManager.hpp` |
| コマンドパレット | `Command{category, label, action, enabled}` | `EditorApp_CommandPalette.cpp` |
| AI バス | `if (type == "...") { ... }` | `Ai/EditorBusDispatcher.cpp` (10,000 行超) |

形はほぼ同じなのに共有されていない。結果として **同じ操作の「実行可能条件」が面ごとに別々に書かれ、
すでに食い違っている**。

### 1.2 実際に発生しているドリフト

移行前に確認できた食い違いは次のとおり。いずれも「押せてしまうが押してはいけない」側に倒れている。

| 操作 | 条件を持っていた面 | 素通りしていた面 |
|------|--------------------|------------------|
| New Scene | ImGui メニュー (`!inPrefabEdit`) | `Ctrl+N` / パレット / ネイティブメニュー |
| Open Scene | ImGui メニュー (`hasScene && !inPrefabEdit`) | `Ctrl+O` / ネイティブメニュー |
| Save Scene | ImGui メニュー (`hasScene`) | `Ctrl+S` / ネイティブメニュー |
| Play | Play ツールバー (`!scriptReloadBusy`) | `Ctrl+P` / パレット / ネイティブメニュー |
| Reload Scripts | Play ツールバー (`!scriptReloadBusy`) | パレット / ネイティブメニュー |

`RequestNewScene()` 等の実体側にも判定は無いため、**メニューが意図的に禁じている操作が
ホットキーとパレットからは通ってしまう**。特にスクリプトのコンパイル中に `Ctrl+P` で
Play へ入れるのは、ツールバーがわざわざ止めていた状態そのものである。

これは「書き忘れ」ではなく、条件を 5 箇所に書く構造そのものの帰結で、面を増やすたびに再発する。
ネイティブメニューに至っては項目ごとの有効/無効表示を持てないため、
**表示で防ぐ設計では原理的に埋められない**。

### 1.3 AI 連携のコストが線形に増える

機能を 1 つ足すたびに、C++ ディスパッチャ・TypeScript のスキーマ (`tools.ts`)・
ドキュメントの 3 箇所を手で書いている。AI 側は「人が使う経路の写し」なので、
写し損ねると人と AI で結果が変わる。この食い違いは既に 8 回、症状が出てから手で修正している。

| # | 重複していた実装 | 事後に取った対処 |
|---|------------------|------------------|
| 1 | Add Object メニュー / AI のノード生成 | `ObjectPresets.hpp` へ共通化 |
| 2 | Terrain Tool のブラシ / AI の sculpt・paint | `TerrainBrush.hpp` を共有 |
| 3 | NavMeshAgent の経路探索 / AI の find_path | `NavMeshQuery.hpp` を共有 |
| 4 | Editor の Duplicate Subtree / AI の複製 | `ExtractSubgraph` を共有 |
| 5 | VFX Inspector の解析 / AI のテクスチャ解析 | `AnalyzeTexture` を共有 |
| 6 | Editor の警告バナー / AI の lint | `CollectBehaviorTreeWarnings` を正本化 |
| 7 | `BehaviorTreePanel::AutoLayout` / AI の整列 | 間隔の定数を共有 |
| 8 | Inspector の表示項目 / `bt_schema` | Inspector 側の不要フィールドを削除 |

いずれも判断は正しいが、**すべて事後対応**である。構造的に重複が作れないようにするのが本設計。

### 1.4 キーバインドが id を持たない

`HotkeyManager::Rebind` は表示名 (`name`) で対象を引く。表示名を変えると保存済みの
リバインドが行方不明になる。操作に安定 id が無いことの副作用。

---

## 2. 方針 — Blender の operator モデル

Blender で Python API が完全なのは、API を丁寧に書いたからではなく
**UI のボタンがオペレータそのもの** だからである。UI が正で API が写し、という関係が存在しないため
ドリフトが原理的に起きない。

FBZZ でも同じ関係を作る。

```
                       OperatorRegistry  ← 唯一の正本
                              │
   ┌────────┬────────┬────────┴────────┬────────────┬──────────┐
   ▼        ▼        ▼                 ▼            ▼          ▼
ImGui   ネイティブ  Play           ホットキー   コマンド      AI
メニュー  メニュー  ツールバー                   パレット    (MCP)
```

- 操作の **実体・実行可能条件・表示名・Undo ラベル・引数スキーマ** はレジストリだけが持つ
- 4 つの面はいずれも「レジストリを読んで描く / 呼ぶ」だけになる
- **AI 用のコードは `op.list` と `op.invoke` の 2 本のみ**で、以後は機能を足しても増えない

---

## 3. データモデル

```cpp
// 引数 1 つの宣言。将来 AI 側の JSON Schema と Inspector の入力欄を同時に生成する。
struct OpParam {
    std::string name;
    OpParamType type;      // Bool / Int / Float / String / Vec3 / NodeId
    std::string desc;
    bool        required = true;
    OpValue     defaultValue;
};

struct EditorOperator {
    std::string id;        // "scene.new" — 安定識別子。保存・AI・リバインドの鍵
    std::string label;     // "New Scene" — UI 表示名 (変えても id は変わらない)
    std::string category;  // "File" — メニュー/パレットの分類
    std::string desc;      // 1 行説明。AI とツールチップが共有する
    std::string caution;   // 「これを実行すると失われるもの」。AI の事故防止

    std::vector<OpParam> params;
    OpKind      kind;      // Query / Action / Mutation
    std::string undoLabel; // Mutation のとき履歴へ出す名前

    OpPoll poll;           // 実行可能か (null なら常に可)
    OpExec exec;           // 実体
};
```

### 3.1 `OpKind` — 3 分類

| Kind | 意味 | Undo | AI 権限 |
|------|------|------|---------|
| `Query` | 状態を読むだけ | 不要 | `read` |
| `Action` | エディター UI 状態 / ファイル I/O を変える | 載らない | `write` |
| `Mutation` | シーンの中身を変える | **必須** | `write` |

`Action` と `Mutation` を分けるのは、`Save` や `Play` を Undo 履歴へ載せると
「Undo でファイル保存が巻き戻る」という事故になるため。現行の AI バスが
`scene_open` / `scene_save` / `build_run` を Undo 対象外としているのと同じ線引きを、
個別の但し書きではなく型で表す。

### 3.2 `poll` — 実行可能条件の唯一の置き場

```cpp
using OpPoll = std::function<bool(const OpContext&, const OpArgs&)>;
```

メニューのグレーアウト・パレットの淡色表示・ホットキーの発火抑止・AI の事前拒否が、
すべてこの 1 つの述語から導かれる。§1.2 のドリフトはこれで構造的に消える。

**文脈と引数の両方を見る。** 同じ操作でも、対象を引数で指定する呼び出し (AI) と、
文脈から暗黙に決まる呼び出し (メニュー・パレット・ホットキー) がある。
文脈しか見られないと、選択を必須にすれば引数付きの正当な要求を弾き、
シーン有無だけにすればメニュー上で「押せるのに何も起きない」が残る。
どちらにも倒せないので、判定する側に両方を見せる
(Blender の `poll()` は文脈のみだが、そちらは対象が常に文脈側にある)。

### 3.3 `exec` と Undo の契約

```cpp
struct OpResult {
    bool        ok = true;
    std::string errorCode;   // "NO_SCENE" 等。AI がそのまま受け取る
    std::string message;
    std::unique_ptr<ICommand> command;  // 非 null ならレジストリが UndoStack へ積む
};
using OpExec = std::function<OpResult(OpContext&, const OpArgs&)>;
```

`Mutation` は `command` を返すのが正しい形で、レジストリがそれを `UndoStack` へ積む。
**Undo の積み忘れがレジストリ 1 箇所で検出できる**ようになるのが要点で、
現状 `node_delete` の子階層 Undo が未対応、といった穴が個別に残っているのはこの検問が無いため。

**例外は作らない。** Mutation がコマンドを返さなければレジストリが必ず警告する。
そのために `SceneEditUtils` へ「積まずに返す」版を用意してある
(`MakeSceneEditCommand` / `MakeDeleteSelectedCommand` / `MakeDuplicateSelectedCommand` /
`MakePasteClipboardCommand` / `MakeRenameNodeCommand`)。
従来の `...WithUndo` 系はその薄い包みになっており、実体は共有されたまま
「積む」責務だけがレジストリへ寄っている。

WHY 抜け道を残さないか: 一時期 `undoHandledInternally` という「exec が自分で積む」
宣言を置いたが、抜け道がある限りそこを通る新しい操作が必ず増える。
ヘルパー側を「返す」形にすれば宣言そのものが不要になる。

「成功したが何も変わらなかった」場合は `OpResult::noChange` を立てる。
これが無いと、同じ名前でリネームしたような正当な no-op まで検問に引っかかり、
それを黙らせるために失敗 (`ok=false`) を返すという嘘が生まれる。

### 3.4 モーダル操作 (Step 4 以降)

ブラシのドラッグやギズモ操作は「1 回の引数セットで完結する実行」に乗らない。
Blender も `exec` (引数で即実行) と `invoke` (対話的に開始してモーダルで確定) を分けている。
FBZZ も同じ二分法を取り、`TerrainTool` のスナップショット Undo は `invoke` 側に置く。
Step 1〜3 では `exec` のみを扱う。

---

## 4. 各面への投影

### 4.1 メニューバー

```cpp
MenuItemOp("scene.new");   // label / poll / ホットキー表示をレジストリと HotkeyManager から解決
```

`ImGui::MenuItem` の第 2 引数 (ショートカット表示) も `HotkeyManager` の実際の割り当てから
引くため、**リバインドした結果がメニュー表示にも反映される**。現状は `"Ctrl+S"` のような
固定文字列なので、リバインドすると表示が嘘になる。

### 4.2 ホットキー

`Hotkey` に `operatorId` を追加する。設定されている場合、`callback` と `enabled` は
レジストリから導出され、`Hotkey` 側には持たない。

リバインドの保存鍵も表示名から `operatorId` へ移す (§1.4)。

### 4.3 コマンドパレット

毎フレームの lambda 再構築をやめ、レジストリを列挙する。
アセット / GameObject への「Go to Anything」候補は操作ではないので、
従来どおり動的候補として別に積む。

### 4.4 AI (MCP)

```
editor.op.list   → id / label / desc / caution / params / kind / available を返す (Query)
editor.op.invoke → id + args で実行し、結果と Undo エントリ名を返す (Command)
```

MCP へは **operator ごとにツールを生やさず**、`editor_op_list` / `editor_op_invoke` の
2 本だけを公開する (ゲートウェイ方式)。理由は 2 つ。

1. MCP のツール一覧は毎リクエスト払う context コスト。operator が数百に増えても
   一覧は 2 行のままにできる
2. MCP は要求ごとに再接続し、Editor と Claude はどちらを先に起動してもよい設計。
   起動時に `op.list` を引いて動的登録すると、Editor が落ちている間はツールが 1 つも
   見えなくなり、この性質が壊れる

権限モードは MCP 側で分かれる (`op.list` は Query なので read 以上、`op.invoke` は
Command なので dry-run/write)。加えて実行時にも `poll` が効くため、
**メニューでグレーアウトされる状況では AI からも通らない**。

---

## 5. 副産物

1. **マクロ記録・再生** — `id + args` の列はシリアライズ可能。人の操作を記録すればそのまま
   回帰テストになる。ImGui の自動化は不要
2. **ドキュメント生成** — `Docs/ai-editor-integration.md` の手書きツール一覧 (249 行) を
   レジストリから生成できる。`caution` も同じ場所にあるので説明が実装から遅れない
3. **F1 ショートカット一覧の完全化** — 操作の全体像がレジストリに揃うため、
   キーが割り当たっていない操作も一覧に出せる
4. **AI が人と同じ操作しかできなくなる** — 専用経路が存在しなくなるので、
   §1.3 の食い違いが発生しうる場所自体が消える

---

## 6. 段階的移行

各 Step は単独で完結し、**途中で止めても得しかない**ように並べる。

### Step 1 — レジストリを作り、エディター内の 5 面を投影に変える (AI は触らない) ✅ 実装済み

- `Editor/Op/EditorOperator.hpp` — 型定義とレジストリ
- `Editor/src/Op/OperatorRegistry.cpp` — 実装
- `Editor/src/Op/BuiltinOperators.cpp` — File / Edit / Selection / Viewport / Gizmo /
  Play / Tools / Panels の 42 操作を登録
- `EditorApp::MenuItemOp(id)` — ImGui メニュー項目を operator から描く
- `RegisterDefaultHotkeys` — 「キーを operator id へ割り当てる表」だけになった
- ネイティブメニューと Play ツールバーも `InvokeOperator` / `CanInvokeOperator` 経由へ
- コマンドパレットはレジストリを列挙するだけになった

得られたもの: §1.2 のドリフトが解消。リバインドの保存鍵が表示名から operator id へ移り、
ラベル変更で設定が失われなくなった。ネイティブメニューのように**表示で防げない面でも
実行時に poll が効く**ようになった。**AI とは無関係に単体で価値が出ている。**

### Step 2 — AI へ `op.list` / `op.invoke` を追加 ✅ 実装済み

- `Editor/Ai/OperatorBridge.hpp` / `src/Ai/OperatorBridge.cpp` — 登録簿 ↔ JSON の変換層
- `EditorBusDispatcher` は `editor.op.list` / `editor.op.invoke` の 2 分岐を足すだけ
  (10,000 行の if/else 本体には触っていない)
- `EditorContext::operators` で登録簿を公開 (`undoStack` / `hotkeyManager` と同じ扱い)
- MCP 側は `editor_op_list` / `editor_op_invoke` の 2 ツール

実装上の判断:

- **ツールを operator ごとに動的登録しない。** MCP は要求ごとに再接続する設計で、
  Editor と Claude はどちらを先に起動してもよいことになっている。起動時に
  `op.list` を引いて `registerTool` を回すと、この性質が壊れる (Editor が落ちている間は
  ツールが 1 つも見えない)。ゲートウェイ 2 本なら Editor の起動順に依存しない。
  これは §6 Step 5 が想定していた形に前倒しで到達したことでもある。
- **引数は Editor 側の `params` 宣言だけで検証する。** MCP 側は `Record<string, unknown>` で
  素通しする。ここで形を固定すると「Editor に操作を 1 つ足すたび TypeScript も直す」という、
  今回無くしたい重複そのものが復活する。未宣言のキーは C++ 側が拒否するので、
  綴り違いが「保存は通るのに何も起きない」形で埋もれることはない。
- **`available` を目録に含める。** 実行してみるまで可否が判らないと、AI は失敗してから
  理由を探すことになる。`available=false` の操作も既定で返すのは、「存在しない」と
  「今は使えない」を区別できないと、実在する手段を諦めて別の組み立てを始めるため。

**以後の新規操作は Operator としてだけ書けばよい。** C++ の dispatcher にも `tools.ts` にも
このドキュメントのツール一覧にも、追記は発生しない。

### Step 3 — 移送と、AI に欠けていた面の追加 🔶 進行中

移送は「実装を Operator へ寄せ、既存の AI ハンドラをその呼び出しに変える」形で行う。
**AI のツール名は消さない** — `editor_undo` のような既存名は使われているため、
入口は残したまま中身だけ 1 本化する。

済んだもの:

- `editor.undo` / `editor.redo` → `edit.undo` / `edit.redo` operator を呼ぶだけになった
  (実行可否の判定が AI 側だけ別式、という状態を解消)

同時に、**AI から到達できなかった操作**を Operator として足した。これらは移送ではなく
新規追加で、Operator として 1 度書いただけで 6 面すべてに現れる:

| Operator | 移行前どこにあったか |
|----------|----------------------|
| `transform.copy` / `paste` / `reset` | Inspector の Transform ヘッダー右クリックのみ |
| `component.move_up` / `move_down` | Inspector のカード ⋯ メニューのみ |
| `animation.auto_layout` | Animation Graph のキャンバス右クリックのみ |
| `animation.set_state_position` | ノードのドラッグのみ (数値指定は不可能) |
| `asset.save` | Save ボタン / Save All のみ |

`asset.save` の欠落はとくに悪質だった。AI は Animator の構造 (state / transition /
motion / parameter / layer) を一通り編集できるのに、**保存する手段が 1 つも無かった**。
編集直後は viewport にも正しく反映されるため、観察による反復では気づけず、
「AI が直したはずなのに次に開くと戻っている」という形でしか現れない。

共有実装への切り出しも併せて行った (別実装を書かないため):

- `Editor/GraphEditor/AnimatorGraphOps.hpp` — 自動整列・保存・dirty 登録。
  移行前は `AnimationGraphPanel` の private/static にあり、パネルを描画していないと
  呼べなかった。整列を別実装にすると「AI が整列したグラフを Editor で整列し直すと
  座標が動く」= 差分に意味のない座標変更が毎回混ざる
- `EditorContext::transformClipboard` — 移行前は `InspectorCore.cpp` の関数内 static で、
  人がコピーした Transform を AI が貼ることも逆もできなかった
- `AssetDirtyRegistry::Save(path)` — パネルの Save ボタンと同じ `saveFunc` を通す

残り: `bt_*` / `vfx_*` / `animation_*` / `terrain_*` の本体移送。

### Step 4 — 契約の強化と、パネルを Operator の消費者にする 🔶 進行中

**1. `poll` が引数を見るようになった。**
`OpPoll` は `(const OpContext&, const OpArgs&)` を取る。文脈しか見られなかった頃は、
引数で対象を指定する操作 (`transform.copy` の `node`) で
「選択必須にすれば引数付きの正当な要求を弾く / シーン有無だけにすればメニュー上で
押せるのに何も起きない」のどちらかを選ぶしかなかった。両方を見せれば両立する。
`component.move_up` は「そのカードがその方向へ動かせるか」まで判定できるので、
端に居るカードはメニュー上でも淡色になる。

**2. Undo の検問に例外を無くした。**
`SceneEditUtils` に「積まずにコマンドを返す」版 (`MakeSceneEditCommand` ほか) を用意し、
`...WithUndo` 系をその薄い包みにした。これで Operator は必ず `OpResult::command` を返せる。
一時期は `undoHandledInternally` という「exec が自分で積む」宣言を置いていたが、
抜け道がある限りそこを通る新しい操作が必ず増えるため、宣言ごと廃止した。
正当な no-op のために `OpResult::noChange` を用意してある。

**3. パネルからの呼び出し口を用意した。**
`CanInvokeOperator(ctx, id, args)` / `InvokeOperator(ctx, id, args)` (自由関数)。
パネルは `EditorApp` を知らないので、これが無いとヘルパーを直接叩き続けることになり、
poll による一元化から外れる。

**4. 3 つの UI をレジストリの消費者に変えた。**
Inspector の Transform ヘッダーメニュー / コンポーネント並べ替えメニュー、
Scene Hierarchy の右クリックメニュー (2 箇所) とウィンドウメニュー。

このとき **Undo に載っていなかった操作が 3 つ見つかった**:

| 場所 | 症状 |
|------|------|
| Hierarchy 右クリック → Delete | `DestroySelected` を直呼び。Delete キー経由は Undo されるのに、メニュー経由は取り消せない |
| 検索結果の右クリック → Delete | `DestroyGameObject` を直呼び。同上 |
| Hierarchy 右クリック → Duplicate | 複製ループが `DuplicateSelectedWithUndo` の本体と 1 文字違わず同じで、違いは `ExecuteSceneEditWithUndo` で包んでいるかどうかだけ |

いずれも「同じ操作に実装が 2 つある」ことの帰結で、operator へ寄せた時点で消えた。

**5. リネームの実装を 1 本にした。**
Hierarchy のインライン編集は `ExecuteSceneEditWithUndo` を使っており、
**名前を 1 つ変えるためにシーン全体を 2 回 TOML シリアライズ**していた。
AI 側 (`node.rename`) は最初から対象だけを戻す軽いコマンドを持っており、
同じ操作の Undo の重さが経路によって違っていた。
`MakeRenameNodeCommand` へ寄せ、パネル・AI・operator の 3 者が同じ実体を通る。

`applyNow` を引数にしてあるのは積み方が違うため — operator は
UI へ反映してから `Push` (再実行しない契約)、AI バスは `Execute` で適用する。
AI 側で適用済みにすると **dryRun が実際にシーンを書き換えてしまう**。

**AssetBrowser のファイル実体を直接 Undo する操作は引き続き追加しない。**
一方で、AI の検証に必要な更新・参照先表示・インポート設定モーダルは
`asset.refresh` / `asset.reveal` / `asset.open_import_settings` として Operator 化した。
ファイル監視・パス解決・モーダル所有権は既存の AssetBrowser 側へ残し、Operator は one-shot request だけを発行する。

残り: §6 Step 5 を参照。

### Step 5 — `bt_*` / `vfx_*` / `terrain_*` をどうするか (方針を改める)

当初は「AI ハンドラ本体を Operator へ移送する」と書いていたが、実装前に中身を確認した
結果、**この 3 ファミリーの AI ハンドラは既に共有実装のアダプタになっている**。

| ファミリー | AI ハンドラが呼んでいる共有実装 |
|-----------|--------------------------------|
| `terrain_*` | `Tools/TerrainBrush.hpp` (人が使うブラシと同一カーネル) |
| `vfx_*` | `VFXEditor/Document/VFXGraphOps.hpp` |
| `bt_*` | `ExtractSubgraph` / `CollectBehaviorTreeWarnings` |

つまり §1.3 の 8 件で解消済みの部分で、ここを Operator へ移すと
`JSON → OpArgs → Operator → 共有実装` と**層が 1 つ増えるだけ**になる。
103 個ぶんの引数宣言を手で書く対価が、poll/Undo の統一と
コマンドパレットへの露出しかない。`vfx.node.setField(path, nodeId, schemaPath, value)`
をパレットから呼びたい場面は無い。

**本当に残っている重複はパネル側にあった。** Behavior Tree では
`BehaviorTreePanel` と AI の `EditorBusDispatcher` が、ノードの追加・削除・親付け・
複製・整列を**それぞれ実装していた**。しかも AI 側には

> 親へ繋げてよいかを 1 か所で判定する。Editor の `TryReparent` と同じ規則で、
> 「AI からは繋げるが人間の UI では弾かれる」食い違いを作らない。

というコメントが書かれていた。つまり**同じ規則を手で 2 回書き、手で揃え続ける**
という運用になっていた。そして実際に既にずれていた:

| | Editor 側 | AI 側 |
|---|---|---|
| ルート重複の理由文 | 「ルートは 1 つだけです (既存のルートへ繋いでください)」 | 「ルートは 1 つだけです」 |
| Auto Layout の列間隔 | `NODE_MIN_WIDTH + 104` (計算値) | `300.0f` (直書き) |

後者はたまたま同じ値なので今は揃っているが、**ノード幅を変えた瞬間に黙ってずれる**。
そうなると「AI が整列した木を Editor で整列し直すと座標が動く」ため、
差分に意味のない座標変更が毎回混ざる。

#### 実施済み ✅

1. `Editor/GraphEditor/BehaviorTreeOps.hpp` を作り、`TryReparentNode` / `AddNode` /
   `RemoveSubtree` / `DuplicateSubtree` / `AutoLayout` / `ChildrenOf` / `IsDescendant` /
   `FindNodeType` を移した。整列の間隔定数もこちらが持つ
2. `BehaviorTreePanel` と AI ハンドラの両方をそれ経由にした
3. Operator として登録したのは `bt.auto_layout` **1 つだけ**

`AddNode` の `orphanOnReject` は、対話的な編集と API の要求の違いを引数にしたもの。
UI では繋げなくてもノードを残す (作った直後に消えると「追加できなかった」のか
「見えていない」のか区別できない)。API では追加ごと取り消して呼び出しを成否で完結させる。
**規則は共有し、振る舞いの違いは宣言する**という形にした。

`bt.auto_layout` がパネルへワンショット要求を出すのは、整列が Undo スタックを
通す必要があり、それを持つのがパネルだから。外から木の中身だけ書き換えると
整列前へ戻せなくなる。

#### 登録しなかったもの

残り 28 個の `bt_*` は `bt.node.setField(path, nodeId, field, value)` のような
「パスと id と名前を指定して値を書く」API で、コマンドパレットから呼びたい場面が無い。
Operator にすると引数宣言が増えるだけで、人が使う面には現れないまま層が 1 つ深くなる。
**本題だった実装の共有は完了しているので、ここを移す動機はもう無い。**

#### VFX でも同じ突き合わせを行った ✅

`VFXGraphCanvas` と AI の `vfx.node.*` を比べたところ、**削除の後始末が食い違っていた**。

| | Canvas (人) | AI (`vfx.node.remove`) |
|---|---|---|
| ノード削除 | する | する |
| リンク削除 | する | する |
| binding 削除 | **しない** | する |
| `parentNodeId` の掃除 | **しない** | する |

エディタ上でノードを消すと、公開パラメーターの binding が存在しないノードを指したまま残り、
そのノードを親にしていた子は解決できない `parentNodeId` を抱えたまま残る。
どちらも保存は通るので、「パラメーターを動かしても何も変わらない」
「子の位置が親から外れる」という形でしか現れない。複数選択の削除も同じ取りこぼしがあった。

`Editor/VFXEditor/Document/VFXGraphEditOps.hpp` へ `NextNodeId` / `DefaultNodeDuration` /
`PlaceNodeOnDefaultGrid` / `MakeNode` / `AddLink` / `RemoveNode` を出し、双方をそこへ寄せた。
id 採番・既定 duration・既定座標の式も 1 文字違わず二重に書かれていたので併せて共有した。

`AddLink` の `validateSchedule` は BT の `orphanOnReject` と同じ考え方で、
**規則は共有し方針は宣言する**形にしてある。対話編集ではスケジュールが破綻するリンクを
張らない (黙って繋がない) が、AI 経路では受け取って `vfx_lint` に指摘させる
— 不正グラフを拒否すると AI が修復の起点を持てなくなるため。

#### Prefab Apply でも同じ形が出た ✅

`PrefabSerializer::Apply` の呼び出し元を並べると一目で判った。

| 呼び出し元 | 直後に `PropagateToInstances` |
|---|---|
| Inspector の Apply ボタン | する |
| Inspector の Apply All to Prefab | する |
| Hierarchy の Apply to Prefab | する |
| **AI の `prefab.apply`** | **しない** |

`PrefabSerializer.hpp` にはこの理由が既に書かれていた:

> Apply はアセットファイルを更新するだけで、既に配置済みの他インスタンスは
> 古い定義のまま残っていた。プレファブを直しても 100 個置いた実体に反映されない、
> という「繋がっていないプレファブ」状態の主因がこれ。

つまり**この問題は一度発見されて UI 側だけ直され、AI 側が取り残されていた**。
同じ「Apply」なのに、人が押すと 100 個の実体へ反映され、
AI が実行するとファイルだけ変わって画面は何も変わらない。

`Apply` + `Propagate` を `ApplyAndPropagate` として 1 つの操作に閉じ、4 箇所すべてを
そこへ寄せた。**呼び忘れが起きる場所そのものを無くす**のが要点で、
「Apply の後には Propagate を呼ぶこと」という規約をコメントで守らせる形にはしない。

AI 側の Undo も直した。伝播は他インスタンスを作り直すため、
アセットファイルだけ書き戻してもシーンは新定義のまま残る。
シーンのスナップショットも併せて持つようにした。

### Step 6 — ツール数対策 (Step 2 で前倒し済み)

Operator を MCP の実ツールとして 1 つずつ生やさず、`editor_op_list(search)` → `editor_op_invoke`
のゲートウェイで到達させる方式は Step 2 で採用済み (§4.4)。
Step 3 が進んで既存ツールが operator へ移るほど、`tools.ts` の実ツール数は減っていく。

残る判断は「常用の何個を実ツールとして残すか」だけ。`editor_perceive` / `scene_find` /
`viewport_capture` のように毎回使うものは、1 往復減らす価値があるので実ツールのまま残す。

### Step 7 — 「読む」を登録簿へ載せ、UI にしか無かった面を残らず投影にする ✅

Step 1〜6 で登録簿は 57 操作まで育ったが、内訳を数えると **Query が 1 つも無かった**。
`OpKind::Query` は Step 1 から型にあり、コマンドパレットもわざわざ
「Query は AI 用なので並べない」と分岐していたのに、実際には 1 つも登録されていない。

原因は `OpResult` にある。持てるのは `ok` / `errorCode` / `message` の 3 つで、
**読み取り結果を返す場所がどこにも無かった**。だから「読む機能」は Operator にできず、
10,000 行の `EditorBusDispatcher` へ書き続けるしかない。結果として AI から見た Editor は

- `op.list` に出る操作 (書く side)
- dispatcher にしか無い照会 (読む side)

の 2 系統に割れたままで、§1.3 で無くしたはずの「1 機能 3 箇所」が読み取り側にだけ残っていた。

**1. `OpData` — Operator の返り値。**
`Editor/Op/OpData.hpp` に、JSON を知らない小さな値ツリーを置いた
(`OpArgs` が JSON を知らないのと対称)。`OpResult::data` へ載せ、
JSON への変換は境界の `OperatorBridge` だけが行う。
Query 以外も使ってよい — `node.create` が作った id を返せないと、
AI は直後に `scene_find` で探し直すことになり、同名ノードがあると別のものを掴む。

**2. `editor.op.query` — 読み取り専用の入口。**
`editor_op_invoke` は MCP 側で write 権限のツールとして公開されている。
読むだけの操作までそこへ入れると、**read 権限で接続した AI は
「目録には出るのに 1 つも呼べない Query」を見る**ことになる。
`kind == Query` 以外を拒否する入口を分け、MCP 側は `editor_op_query` (readOnlyHint) にした。
ゲートウェイは 3 本になったが、operator が何個増えてもこの 3 本のままである性質は変わらない。

**3. `OpParam::enumValues` / `hasRange` — 引数の制約を宣言にした。**
移行前、`render.set_view_mode` は exec の中で 4 つの文字列と比較し、独自のエラー文を返していた。
候補は `desc` の文章にしか無く、機械可読ではないので AI は綴りを推測するしかない。
宣言にすれば `op.list` が候補をそのまま返し、検問は `ValidateArgs` 1 箇所で済む
(dryRun も同じ関数を通るので「dry-run では通ったのに実行すると BAD_ARG」が起きない)。
副産物として、**コマンドパレットが排他選択を値ごとの行へ展開できる**ようになった。
移行前 View Mode はパレットに「押せるが必ず失敗する 1 行」として並んでいた。

**4. `OpCheck` — トグルの現在状態を宣言にした。**
Debug メニューは `ImGui::MenuItem("Grid", nullptr, &ctx.showGrid)` とフラグを直接指しており、
**同じフラグを切り替える `render.show_grid` operator が別に存在していた** (10 項目)。
表示と実体が別経路なので、operator 側に条件を足しても見た目には反映されない。
AI から見ると「切り替えられるのに今どちらか読めない」非対称が残る
(`render.*` が `enabled` 省略時に反転する設計は、その読み取り不能を回避する折衷だった)。
`checked` を足して、メニューのチェック・パレット・`op.list` の `checked` が同じ式から出るようにした。
`poll` と同じ `(context, args)` を取るので、排他選択のラジオ表示も同じ宣言から引ける。

**5. UI にしか無かった操作を足した。**
「二重管理が無いから Operator にしない」という Step 1 の判断は、**人が使う面しか数えていなかった**。
重複が無いことは Operator にしない理由になるが、AI から到達できない理由にはならない。

| Operator | 移行前どこにあったか |
| `panel.list` (Query) / `panel.set_visible` / `panel.focus` | View > Panels とパレットのみ。AI はパネルを列挙も開閉もできなかった |
| `asset.open` | AssetBrowser のダブルクリックのみ |
| `prefab.edit` | AssetBrowser の Alt+ダブルクリックと右クリックのみ |
| `prefab.close` | File メニューのみ |
| `render.show_stats` / `debug.hot_reload` | Debug メニューのみ |
| `view.set_ui_scale` / `view.reset_ui_scale` | View メニューのみ |
| `ai.command_bus` | AI Settings パネルのみ (メニューは押せない状態表示だった) |

とくに `asset.open` の欠落は `asset.save` と同じ形をしている。AI は `.animcontroller` も
`.vfx` も `.behaviortree` も編集できるのに、**どれ一つ開けなかった**。実害は 2 つあり、
1 つは人へ結果を見せる導線が「Assets を辿ってダブルクリックしてください」しか無いこと、
もう 1 つは **パネルが開いていることを前提にした操作を自分で満たせない**こと
(`bt.auto_layout` は `BehaviorTreePanel` がワンショット要求を消費して初めて動く)。
`prefab.close` も同様で、入る手段が無いまま出る手段だけを足しても対称にならない。

**6. 投影に変えた面で見つかった嘘。**
Build Settings のメニュー項目は `"Ctrl+Shift+B"` と表示していたが、
その文字列は直書きで **実際にはどこにも割り当てられていなかった**。
`MenuItemOp` が実割り当てから表示を引くので、投影へ変えた時点で
「表示だけあるショートカット」は成立しなくなる — 表示を消すか実装するかの二択になり、実装した。
§1.2 で消したドリフト (条件のずれ) と同じものが、表示側にも残っていたことになる。

残り: `bt_*` / `vfx_*` / `terrain_*` の照会系は Step 5 の判断どおり移送しない
(共有実装のアダプタで、Operator にすると層が 1 つ深くなるだけ)。
ただし `OpData` が入ったことで、**移送したくなったときに阻む理由はもう無い**。

---

## 8. 適用範囲外

- **連続値のドラッグ編集** (Inspector のスライダー、ギズモ) は 1 操作 = 1 レジストリ呼び出しに
  ならない。§3.4 のモーダル operator で扱い、Step 1〜3 では対象外
- **Play 中のランタイム状態** は Undo 履歴に載せない現行方針を維持する
  (`UndoStack::SetRecordingEnabled(false)`)
