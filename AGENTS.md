<!-- @file    AGENTS.md -->
<!-- @brief   FBZZ Engine の実装・変更・検証に共通する規約。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-05-15 -->
# FBZZ Engine の開発規約

Windows 向けの C++20 製 3D ゲームエンジン。描画は DirectX 11 / 12、数学・物理は自作、ゲームロジックは C#。初めてソースを開くときは、[機能別の読み順](Docs/source-reading-guide.md)から宣言・実装・テストをたどれる。

## コード規約

**初めて関数を開いた人が、上から順に目的・判断・結果を追えるように書く。**

- **変数の意味を書く。** 構造体・クラスのデータメンバーは一つずつ宣言し、右側の `///<` に日本語で用途を書く。距離ならどこからの距離か、添字ならどの配列を指すかを示す。ローカル変数も略語を避け、式から読み取れない前提は直前の `//` で補う。
- **関数内に道筋を残す。** 複数段階の関数は、各まとまりの先頭の `//` に何を何のために行うかを書く。計測マーカーの登録なら、既存 ID を探す理由、コピーの前後で容量を確認する理由、最後に索引を作る理由をその場で読めるようにする。[実装例](Projects/Core/src/Profiler/PerformanceDescriptorTable.cpp)
- **条件を一つずつ読めるようにする。** 独立した失敗条件は早期リターンにする。一緒に評価する条件には意味の分かる名前を付け、ポインターの有効性確認などの短絡評価を保つ。三項演算子は入れ子にしない。
- **段階の境目に空行を置く。** 入力確認、計算、状態の更新を分ける。`if` やループには波括弧を付け、条件と結果は別の行に書く。スロット・容量上限など意味を持つ数値には既存の名前付き定数を使う。

```cpp
size_t m_ownedStringBytes = 0; ///< 登録した名前と分類の合計バイト数。各文字列の予約容量と終端を含む。
```

型・関数・ファイル・ディレクトリは `PascalCase`、変数は `lowerCamelCase`、メンバーは `m_` 付き、定数・列挙子は `UPPER_SNAKE_CASE`。インターフェースは `I` で始める。Inspector に表示する C# の公開フィールドには `m_` を付けない。

宣言の契約は日本語の Doxygen で書き、ファイル先頭の 4 行と作成日を保つ。引数を書くときは `@param[in]`（入力）、`@param[out]`（結果を格納）、`@param[in,out]`（元の値を使って更新）のいずれかを付ける。失敗時の出力・単位・座標系・所有権もここに書く。参考にした公式仕様や論文は該当箇所の `/// @see` に URL と題名を残す。[書式と実例](Docs/conventions/comments.md)

## 絶対制約

エンジンの依存方向は `Editor / GameHub / Sandbox → Engine → Physics / Fluid → Math`。ゲームは公開 C# SDK、描画の上位層は `IRenderer&` を使う。C++ は `unique_ptr` による単独所有を基本とする。

変更する領域の規約を次の表から開き、実装前に確認する。設計判断は [Docs/design/](Docs/design/) に従い、根拠がなく判断に迷う場合は選択肢を示す。

| 変更するもの | 確認する規約 |
| --- | --- |
| C++ の型・所有権・依存 | [禁止 API、例外、キャスト、外部ライブラリ、モジュール](Docs/conventions/project-workflows.md#cpp) |
| シーン・Editor 操作 | [更新順、Transform、操作の登録](Docs/conventions/project-workflows.md#scene) |
| ゲームスクリプト | [公開 C# API、ビルド、Play の契約](Docs/conventions/project-workflows.md#scripts) |
| アセット・シェーダー | [原本、GUID、複製の扱い](Docs/conventions/project-workflows.md#assets) |
| 開発ツール | [ツールの置き場所と索引への登録](Docs/conventions/project-workflows.md#tools) |

## 変更を確認する

1. 機能追加・不具合修正では、まず契約を示すテストを書き、変更前に意図した理由で失敗することを確認する。
2. C++ を変更したら `Tools/AgentBuild.ps1 check <ファイル...>` でコンパイルし、`tidy <ファイル...>` で可読性の警告を確認する。VS Code では開いたファイルに **C++: clang-tidy (Current File)** を使える。
3. `build <target>` と `test -Filter <regex>` も同じ入口を使う。ビルドは SDK 公開を含めて同時に 1 本。競合時は完了を待ち、同じ構成で一度だけ再実行する。
4. Playtest は対象 Editor の MCP から実行する。最後に `git status` を確認する。[ビルド・テスト・Git の詳細](Docs/conventions/project-workflows.md#verification)
