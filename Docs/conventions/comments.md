# コメント規約 — Doxygen と API リファレンス

コメントは **AI と人が読む API リファレンスの原稿** である。`doxygen Docs/Doxyfile` が公開ヘッダーから
HTML と XML を生成し、`Tools/ApiReference.py` が XML を AI 向けの圧縮 Markdown へ変換する。
書くのは「コードから読めない契約と理由」だけ。読めば分かることを書くと、リファレンスがノイズで薄まる。

| 生成物 | 場所 | 用途 |
|---|---|---|
| HTML | `build/docs/html/index.html` | 人がブラウザで読む |
| XML | `build/docs/xml/` | 機械可読の中間形式 |
| AI 向け Markdown | `build/docs/api/` (`index.md` + ヘッダーごと) | エージェントがヘッダーの代わりに読む |
| 警告ログ | `build/docs/doxygen-warnings.log` | 規約違反の残りを探す |

```
doxygen Docs/Doxyfile
python Tools/ApiReference.py            # build/docs/xml → build/docs/api
node Tools/AgentLint/lint.mjs --all <file>   # doxygen-comment 警告が 0 なら移行済み
```

---

## 1. 形式

- **`///` だけを使う。** `//` の自由記述・`/* */`・`/** */`・`//!` は書かない
- 例外として残してよい `//` は 5 種のみ: `} // namespace x`、`#endif // X`、`// clang-format off/on`、`// NOLINT...`、`// @@FBZZ_*` マーカー (ScriptCodeGen と GameHub が `"// " + marker` の完全一致で探す。`///` にすると自動同期が壊れる)
- 引数名を示す `/*name=*/` (`Foo(/*removable=*/true)`) は許す
- 参考にした論文・公式仕様の URL は `/// @see <URL> 題名` で数式・アルゴリズムの直近に残す。圧縮の対象にしない
- **各タグは 1 行。** 段落が要る内容は `Docs/design/` に置き `@see` で指す
- 使うタグは `@file @brief @author @date @param @tparam @return @pre @post @note @warning @see @todo @name`。`@details` `@remark` `@ret` などは使わない
- `WHY:` `WHAT:` `NOTE:` `HOW:` のようなラベル儀式を書かない。理由は `@note` 1 行で事実として書く

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
/// @param ray 原点はワールド空間、方向は正規化済みであること。
/// @return ヒットなしなら false。out は未変更。
/// @note 順序依存: Build() の後でしか呼べない。
[[nodiscard]] bool Raycast(const Ray& ray, RaycastHit& out) const;
```

- `@brief` は 1 行。クラスは `@brief` + 必要な `@note` を合わせて **4 行以内**
- enum 値・構造体フィールドの短い補足だけ末尾 `///<` を許す。関数の宣言には使わない
- **関数本体の中**は `/// @note` を該当文の直前に 1〜3 行。Doxygen は本体を読まないので、契約に関わる理由は宣言側の `@note` へ置く
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

迷ったら「このコメントが消えたら、次に触る AI は何を間違えるか」で決める。何も間違えないなら書かない。

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
| `// WHY xxxか: 3〜8 行の説明` | `/// @note 理由を 1〜3 行` (ラベル・症状の物語・日付を落とし、非自明な理由だけ残す) |
| `// NOTE:` / `// 補足:` | `/// @note` |
| `// TODO:` / `// FIXME:` | `/// @todo` |
| `// ---- 節名 ----` | ヘッダー: `@name` グループ、`.cpp`: `/// @name 節名`、意味が無ければ削除 |
| `int x = 0; // 説明` (関数本体内) | 直前の行に `/// @note 説明` |
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
| 旧形式の残り | `node Tools/AgentLint/lint.mjs --all <files>` | `doxygen-comment` 警告 0 |
| Doxygen の誤読 | `doxygen Docs/Doxyfile` → `build/docs/doxygen-warnings.log` | 対象ファイルの警告 0 |
| コメントだけ触ったか | `node Tools/AgentLint/codeonly.mjs snapshot <json> <paths>` → 編集 → `verify <json>` | `CHANGED` が 0 件 |

PostToolUse フックの `lint.mjs --hook` は「この編集で増えた違反」だけを報告する。旧コードの残りを一掃するときは `--all` で見る。

---

## 6. AI 駆動開発での使い方

- **API を調べるときはヘッダーでなく `build/docs/api/<Module>/<Header>.md` を読む。** 契約だけが並ぶので、ヘッダーを開くより少ない文脈で済む。無ければ上のコマンドで生成する
- **API を足したら契約を書く。** 単位・所有権・失敗時の戻り値が無い宣言は、リファレンスに載っても次のエージェントが正しく呼べない
- **コメントを増やすときは「読めないこと」だけ。** 処理の説明はコードを読めば済むので、リファレンスの信号対雑音比を下げるだけになる
- 移行作業を複数エージェントで分担するときは、ディレクトリ単位で分け、各エージェントに `codeonly.mjs` の snapshot / verify を持たせる (コードに触っていない証明)
