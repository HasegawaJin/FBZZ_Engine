# コメント規約 — Doxygen と API リファレンス

コメントは **AI と人が読む API リファレンスの原稿** である。`doxygen Docs/Doxyfile` が公開ヘッダーから
HTML と XML を生成し、`Projects/DevTools/ApiReference/ApiReference.py` が XML を AI 向けの圧縮 Markdown へ変換する。
宣言には値の意味と契約を、実装には処理のまとまりとその目的を日本語で残す。

| 生成物 | 場所 | 用途 |
|---|---|---|
| HTML | `build/docs/html/index.html` | 人がブラウザで読む |
| XML | `build/docs/xml/` | 機械可読の中間形式 |
| AI 向け Markdown | `build/docs/api/` (`index.md` + ヘッダーごと) | エージェントがヘッダーの代わりに読む |
| 警告ログ | `build/docs/doxygen-warnings.log` | 規約違反の残りを探す |

```
doxygen Docs/Doxyfile
python Projects/DevTools/ApiReference/ApiReference.py            # build/docs/xml → build/docs/api
node Projects/DevTools/AgentLint/lint.mjs --all <file>   # doxygen-comment 警告が 0 なら移行済み
```

---

## 1. 形式

- **宣言の前は `///`、メンバーの右は `///<`、関数内の説明は `//`。** 単位や失敗時の約束は宣言に、手順や評価順の理由は実装に書く。
- 名前空間の終端、`#endif`、`clang-format`、`NOLINT`、`// @@FBZZ_*` のマーカーは元の書式を保つ。ツールがマーカーと完全一致する行を探す。
- 引数名を示す `/*name=*/` (`Foo(/*removable=*/true)`) は許す
- 参考にした論文・公式仕様の URL は `/// @see <URL> 題名` で数式・アルゴリズムの直近に残す。圧縮の対象にしない
- **各タグは 1 行。** 段落が要る内容は `Docs/design/` に置き `@see` で指す
- 設計理由の長さを数えるときは、引数・テンプレート引数・戻り値の契約と出典を除く。引数の多い関数でも各引数の方向と用途を残す。
- 使うタグは `@file @brief @author @date @param @tparam @return @pre @post @note @warning @see @todo @name`。`@details` `@remark` `@ret` などは使わない
- `WHY:` `WHAT:` `NOTE:` `HOW:` のようなラベル儀式を書かない。理由は `@note` 1 行で事実として書く。`@note WHY xxxか:` のような混在も同じ
- **C++ 以外も同じタグで書く。** CMake (`CMakeLists.txt` / `.cmake`) と PowerShell (`.ps1`) は `# @file` の 4 行ヘッダー + `# @note` / `# @brief` / `# @name` / `# @see`、JSONC (`.vscode/*.json`) は `// @note`。周囲の旧コメントが `# WHY` でも真似せず、触ったファイルは全体を直す。前例は `Projects/Tests/CMakeLists.txt` の冒頭

### ファイルヘッダー (全ファイル必須・桁揃え)

```cpp
/// @file    Vector3.hpp
/// @brief   3次元ベクトルの演算と定数。
/// @author  Hasegawa Jin
/// @date    2026-08-21
#pragma once
```

`@date` は作成日で、更新しても書き換えない。ファイル全体の補足は 4 行の後に空 `///` を挟んで **4 行以内**。

### 宣言のコメント

```cpp
/// @brief レイと BVH の最近接交差を求める。
/// @param[in] ray 原点はワールド空間、方向は正規化済みであること。
/// @param[out] out 最近接の交差を格納する。ヒットしなければ変更しない。
/// @return ヒットなしなら false。out は未変更。
/// @note 順序依存: Build() の後でしか呼べない。
[[nodiscard]] bool Raycast(const Ray& ray, RaycastHit& out) const;
```

- `@brief` は 1 行。クラスは `@brief` + 必要な `@note` を合わせて **4 行以内**
- 構造体・クラスの各データメンバーは宣言の右に `///< 説明` を書く。何を表す値かを示し、必要に応じて単位・所有元・無効値を添える。列挙子にも同じ書式を使える。
- **関数本体の中**では段階の間に空行を置き、まとまりの先頭の `//` に目的を日本語で書く。見積もりと確定値のように似た判定が並ぶときは、各判定が何を守るかを示す。論文・仕様への `/// @see` は式の近くに残す。
- 引数の方向はタグの直後に書く。値を参照する入力は `@param[in]`、以前の値を使わずに結果を返す出力先は `@param[out]`、既存の内容を読んで更新・追記する場合は `@param[in,out]`。参照やポインターは実装と呼び出し側の契約で判断し、失敗時に出力を保つだけなら `[out]` とする。`@tparam` には方向を付けない。[Doxygen の param コマンド](https://www.doxygen.nl/manual/commands.html#cmdparam)
- 節の区切りは、ヘッダーでは `/// @name 節名` + `/// @{` … `/// @}` のメンバーグループ、`.cpp` では `/// @name 節名` 1 行。区切りに意味が無ければ消す

---

## 2. 何を書くか

| 書く (コードから読めない) | 書かない (コードから読める) |
|---|---|
| 単位・座標系・範囲 (`[m/s]`、ワールド空間、0..1) | 処理をなぞる説明 (「ループして加算する」) |
| 所有権・寿命 (誰が解放するか、返す参照はいつまで有効か) | 自明なゲッター / セッター |
| スレッド制約・呼べる Phase・順序依存 | 引数名を言い換えただけの `@param` |
| 失敗時の戻り値と副作用 (`false` のとき out は未変更) | 変更履歴・日付・「以前は〜だった」 |
| 数式の根拠・定数の出どころ・ドライバ回避策 | 症状の物語 (「〜のときカクついた」)。原因だけを書く |

実装を読み返すときは、段階の説明だけを順に読んでも処理の道筋が分かるかを確かめる。単純なアクセサーは宣言とメンバーの説明で足りる。

### 計算を初めて読む人に向けて

- 数式のそばに、求めたい量と式を選んだ理由を日本語で書く。例えば距離制約では「両端のずれを逆質量の割合で分けるので、軽い方が大きく動く」と説明すると、分母の意味まで分かる。
- 比率・角度・距離は基準や単位を示す。補間なら 0 と 1 が何を指すか、内積ならどの向きの成分を取り出すかを、その式の用途に合わせて書く。
- メンバーの宣言には、その状態を保持する目的を書く。履歴なら何を復元するか、キャッシュならいつ失効するか、参照なら誰が実体を所有するかを示す。
- 同じ記号が領域ごとに違う量を指すときは、実装名で区別する。例えば気体格子のセル幅は `m_cellWidth`、液体粒子の近傍半径は `m_kernelRadius`。論文上の記号は、対応を追う必要がある式のそばに添える。

---

## 3. Doxygen が誤読する書き方

| 書き方 | 問題 | 直し方 |
|---|---|---|
| `<projectRoot>/Assets` | HTML タグと解釈され警告 | `` `<projectRoot>/Assets` `` とバッククォートで囲む |
| `#原文` / `#include` | `#` がリンク要求になる | `\#原文`、またはバッククォートで囲む |
| `@` を含むメール・記号 | タグと解釈 | `\@` |
| 行頭の `-` を続けて段落を作る | 箇条書きとして描画され行数が増える | 1 行にまとめる。要るなら `Docs/design/` へ |

---

## 4. 旧コメントの移行表

触ったファイルは全体を直し切る (`@date` は保つ)。コードには触らない。

| 旧 | 新 |
|---|---|
| `// WHY xxxか: 3〜8 行の説明` | 根拠を一文にする。宣言は `/// @note`、関数内は `//`。 |
| `// NOTE:` / `// 補足:` | 見出しを外し、宣言は `/// @note`、関数内は `//` とする。 |
| `// TODO:` / `// FIXME:` | `/// @todo` |
| `// ---- 節名 ----` | ヘッダー: `@name` グループ、`.cpp`: `/// @name 節名`、意味が無ければ削除 |
| `/// @note` で書かれた関数内の段階説明 | まとまりの先頭に `// 目的や前提` |
| `Value, // 説明` (enum 値・フィールド) | `Value, ///< 説明` |
| `@ret` | `@return` |
| コメントアウトされたコード | 削除 (履歴は git にある) |
| `// 大文字小文字を無視した前方一致。` (関数の直前) | `/// @brief 大文字小文字を無視した前方一致。` |
| 8 行を超えて縮まらない設計理由 | `Docs/design/<機能>.md` に節を足し、コメントは `/// @see Docs/design/<機能>.md` 1 行 |

長い `///` ブロックも同じ基準で圧縮する。契約と非自明な理由が残っていれば、行数が減っても情報は減っていない。

---

## 5. 検証

| 確認 | コマンド | 合格 |
|---|---|---|
| 旧形式の残り | `node Projects/DevTools/AgentLint/lint.mjs --all <files>` | `doxygen-comment` / `doxygen-label` 警告 0 (CMake / `.ps1` はラベルだけを見る) |
| Doxygen の誤読 | `doxygen Docs/Doxyfile` → `build/docs/doxygen-warnings.log` | 対象ファイルの警告 0 |
| コメントだけ触ったか | `node Projects/DevTools/AgentLint/codeonly.mjs snapshot <json> <paths>` → 編集 → `verify <json>` | `CHANGED` が 0 件 |

PostToolUse フックの `lint.mjs --hook` は「この編集で増えた違反」だけを報告する。旧コードの残りを一掃するときは `--all` で見る。

---

## 6. AI 駆動開発での使い方

- **API を調べるときはヘッダーでなく `build/docs/api/<Module>/<Header>.md` を読む。** 契約だけが並ぶので、ヘッダーを開くより少ない文脈で済む。無ければ上のコマンドで生成する
- **API を足したら契約を書く。** 単位・所有権・失敗時の戻り値が無い宣言は、リファレンスに載っても次のエージェントが正しく呼べない
- **関数内は段階ごとに説明する。** 例えば [PerformanceDescriptorTable::Resolve](../../Projects/Core/src/Profiler/PerformanceDescriptorTable.cpp) では、既存 ID の再利用、確保前後の容量確認、文字列を借りる索引の登録を順に追える。
- 移行作業を複数エージェントで分担するときは、ディレクトリ単位で分け、各エージェントに `codeonly.mjs` の snapshot / verify を持たせる (コードに触っていない証明)
