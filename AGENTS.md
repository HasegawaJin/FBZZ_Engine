# FBZZ Engine — エージェント規約

C++20 自作 3D ゲームエンジン (Windows / DX11 + DX12)。数学・物理はゼロ実装。ImGui エディター・RenderGraph・NavMesh・地形・水面・VFX・スクリプト DLL ホットリロードを持つ。**ポートフォリオ公開リポジトリ**。

- 名前空間 `fbzz::` (`math` / `physics` / `fluid` / `renderer` / `scene` / `editor`)。ゲームスクリプトは `sandbox` (入れ子禁止)
- 依存方向 `Editor / GameHub / Sandbox / GreenWare → Engine → Physics / Fluid → Math`。逆転禁止
- `Fluid` (`Projects/Fluid`) は流体の**数式だけ**。ファイル形式 (`.fluid` / `速度場 PNG`) も RHI も Scene も知らず、依存は Math のみ。`Physics` とは兄弟で互いを知らない (`Docs/design/fluid-library.md`)
- エンジン本体は `Projects/`、それを使うゲームは `GreenWare/` (作業はほぼ `GreenWare/Assets/` で完結)
- 迷ったら `Docs/design/` の設計文書に従う。無ければ実装せず選択肢を提示する

## 絶対制約

| 禁止 | 代わりに |
|------|---------|
| GLM / GLFW / Bullet / PhysX / Box2D | 自作 `Math` / `Physics` |
| `new` / `delete` | `make_unique` / `make_shared`。raw pointer は非所有参照のみ |
| `throw` / `std::exception` | 回復可能は `bool` + Logger、回復不可能は `assert()`。HRESULT は `FBZZ_HR_CHECK`、数学契約は `FBZZ_MATH_CONTRACT` |
| `dynamic_cast` / `reinterpret_cast` | `GetComponent<T>()`。後者は定数バッファ転送のみ可 |
| `DX11Renderer*` / `DX12Renderer*` へのダウンキャスト | 上位層は `IRenderer&` のみ (DX ヘッダーは各バックエンドの PRIVATE) |
| スクリプトから Engine 実装型を include | `ScriptProxy` のメンバー (`transform` / `input` / `physics` …) |
| `.generated.hpp` の新規作成・include | 廃止済み。`FBZZ_FIELD*` + `FBZZ_REFLECT(T)` |
| ヘッダーへの `<Windows.h>` | `.cpp` に閉じ、`WIN32_LEAN_AND_MEAN` を先に定義 (`NOMINMAX` は CMake が定義済み) |
| エンジン `.hpp` での `using namespace` | スクリプト (`Assets/**/*.hpp`) だけ `fbzz::scene` / `math` / `input` を許可 |
| C++20 Modules / `std::ranges` / コルーチン | `Engine/Scene/Coroutine.hpp` の自作実装 |
| FetchContent | `ThirdParty/` へベンダー + `LICENSE` + `VERSION` + `THIRD-PARTY-NOTICES.md` へ追記 |
| `cmake` / `msbuild` の直叩き | 人は VS Code / VS のタスク、AI は `Tools/AgentBuild.ps1` (VS 環境を知るのは `Tools/VsEnvironment.ps1` だけ) |
| `git checkout -- <path>` / `git restore` | 戻したい対象を提示して判断を仰ぐ |
| 一回きりのスクリプト (調査・検査・プレビュー) をリポジトリへ置く | `Scratch/` (Git 対象外)。ツールとして残すなら «ツールの置き場所» の索引へ |
| コミットへの `Co-Authored-By` / `Generated with` | 付けない |

## コード規約

- クラス・関数・ファイル・ディレクトリは `PascalCase`、変数は `lowerCamelCase`、メンバーは `m_` 付き、定数・enum は `UPPER_SNAKE_CASE`、インターフェースは `I` 接頭。例外: スクリプトの公開フィールドは Inspector に出るので `m_` を付けない
- ヘッダーは `.hpp` + `#pragma once`。循環は前方宣言で切る
- **全ファイルの先頭に 4 行ヘッダー** (`@date` は作成日。更新で書き換えない):

```cpp
/// @file    Vector3.hpp
/// @brief   3次元ベクトルの演算と定数。
/// @author  Hasegawa Jin
/// @date    2026-08-21
#pragma once
```

- **コメントはすべて Doxygen 形式** (`///` + `@brief` / `@param` / `@return` / `@pre` / `@note` / `@warning` / `@see`)。CMake・PowerShell は `# @note`、JSONC は `// @note` と記号だけ替えて同じタグで書く。`WHY:` などのラベルはどの言語でも書かない。`//` の自由記述は書かない。関数本体の中で補足が要る箇所も `/// @note` 1 行で書く (Doxygen は本体内を拾わないので、契約に関わる理由は宣言側の `@note` に置く)
- **短く書く**。各タグ 1 行。段落が要るなら `Docs/design/` に置いて `@see` で指す。生成は `doxygen Docs/Doxyfile` → `python Projects/DevTools/ApiReference/ApiReference.py` (AI 向け Markdown は `build/docs/api/`、警告は `build/docs/doxygen-warnings.log`)。詳細と移行表は `Docs/conventions/comments.md`
- **リファレンスは実装のコメントにも残す**。参考にした論文・公式仕様・公式ドキュメントの URL を、対応する数式・アルゴリズム・API 契約の宣言または実装の直近に `/// @see <URL>` で記載する。何を参照したか分かる題名・節名も添え、設計文書や作業報告だけにリンクを置かない。
- **書くのは「コードから読めないこと」だけ**。契約 (単位・座標系・所有権・スレッド・失敗時の戻り値) と、数式の根拠・順序依存・ドライバ回避策のような非自明な理由のみ。処理をなぞる説明・自明なゲッターの説明・引数名を言い換えただけの `@param` は書かない
- 旧コードの `// FBZZ Engine` バナー・`//` コメント・`@ret` などの独自タグは真似しない。**見つけ次第すべて Doxygen 形式へ直す** (触ったファイルは全体を直し切る)。`doxygen Docs/Doxyfile` の警告ログは旧コメントの残りを探す手掛かりになる

```cpp
/// @brief レイと BVH の最近接交差を求める。
/// @param ray 原点はワールド空間、方向は正規化済みであること。
/// @return ヒットなしなら false。out は未変更。
[[nodiscard]] bool Raycast(const Ray& ray, RaycastHit& out) const;
```
- 所有権: `unique_ptr` が既定、複数システムで共有するものだけ `shared_ptr`。GPU リソースも `unique_ptr` 単独所有
- カスタムアロケーター (`Core/Memory/`) は高頻度・大量確保のときだけ
- `Engine/src/` に新ディレクトリを作ったら `Projects/Engine/CMakeLists.txt` のモジュール定義へ足す (忘れると configure で FATAL_ERROR)。モジュールは OBJECT のまま、STATIC にしない (自己登録の静的初期化子が捨てられる)

## シーン・システム

- Unity 同様の GameObject / Component。新システムは `ISystem` を継承し Phase / RunMode / ComponentAccess を宣言して `SystemScheduler` に登録
- Phase 順: `PreScript → Script → PrePhysics → Physics(固定) → PostPhysics → Navigation → LateScript → Cleanup → LateUpdate`。描画と UI は Phase ではなく描画パスから駆動
- `worldPosition` だけ書いても `PrePhysics` が local から組み直す。位置を置くときは `position` (ローカル) も書く
- エディターの「操作」は `Editor/src/Op/` の登録簿へ 1 件足す。メニュー・ホットキー・パレット・AI バスはその投影なので面ごとに書かない

## スクリプト (`<Project>/Assets/Scripts/<領域>/XxxComponent.hpp`)

1 スクリプト = 1 ヘッダー。`.cpp` も `.generated.hpp` も不要。

```cpp
namespace sandbox {
class EnemyComponent : public Script {
    FBZZ_SCRIPT(EnemyComponent)
    FBZZ_REQUIRE_COMPONENT(RigidBodyComponent)   // 不足を Inspector / Play 開始時 / 実行時に名指しする。自動追加はしない
    FBZZ_GROUP("移動")
    FBZZ_FIELD_RANGE(float, moveSpeed, 4.0f, "移動速度 [m/s]", 0.0f, 20.0f)
    FBZZ_REF(GameObject, target, "追跡対象")
public:
    void OnUpdate(float dt) override;
};
FBZZ_REFLECT(EnemyComponent)   // フィールド宣言の締め。クラス外に必須
}
```

- マクロ一覧は `Engine/Scene/Script.hpp`、プロキシのメンバー名は `ScriptProxy/ScriptProxyMembers.inl` が正本
- `FBZZ_SCRIPT` を付けないヘッダーはユーティリティ (登録されない)。`FBZZ_DATA_ASSET` は `.fzdata` 用。`ScriptList.inl` / `DataAssetList.inl` は生成物なので手で編集しない
- `lifetime` / `time` / `random` など**基底のメンバー名をスクリプト側で再宣言しない** (プロキシが隠れる)
- Play の開始・停止は `SceneSerializer` を往復する。**シリアライズしていない値は既定値へ戻る**
- 破棄では `OnDisable` が呼ばれない。後始末は `OnDestroy`
- `FBZZ_EXECUTE_ALWAYS` は見た目を組み立てるスクリプト限定。編集中の書き込みはシーンの中身になるので、体力・スコア等のゲーム状態を触るものに付けない。物理系コールバックは編集中も呼ばれない
- `Script` に仮想関数を足すときは vtable 末尾に置き、`ScriptDllAbi.hpp` の `kScriptVtableAbiVersion` を上げる
- プロキシ追加: `ScriptProxy/ScriptXxxProxy.hpp` + `src/Scene/ScriptProxies.cpp` + `ScriptProxyMembers.inl` + `AllScriptProxies.hpp`。引数・戻り値に Engine 内部型を露出しない

## アセット

- 参照は `guid:`。`Assets/` の各ファイルに `.meta` が随伴し、改名・移動しても参照は切れる
- **ディスク上のアセットが正本**。シーン・プレファブを生成スクリプトで作り直さず、既存ファイルへ差分で当てる
- エディター起動中に git でツリーを巻き戻さない (`.meta` が作り直されて GUID が変わる)
- シェーダー (`Assets/Shaders/<Category>/*.hlsl`) はエンジン側とプロジェクト側に複製がある。直すときは全コピーを検索して全部直す
- 形式の定義は `Engine/Format/` と `Engine/Asset/` のヘッダーを読む

## ツールの置き場所

| 置き場所 | 置くもの |
|---------|---------|
| `Projects/DevTools/<Name>/` | エンジンが持つ開発用プログラム (AgentLint・ApiReference・FontAtlasGen …) |
| `Tools/` | 開発の入口になる薄いスクリプト (AgentBuild・VcBuild・Coverage …)。タスク・CI・スキルから呼ばれるもの |
| `<Project>/Tools/` | そのゲームのアセット制作パイプライン (`GreenWare/Tools/BlenderExport` …) |
| `Scratch/` | 一回きりのスクリプト・調査用コード。Git に入らない |

- 上の 3 つ (`Scratch/` 以外) は各根の `README.md` が索引。載っていないファイルは AgentLint の `tool-unlisted` が ERROR にする
- 索引へ足すのはユーザーの承認を得てから。役割と呼び出し元 (タスク・CI・スキル・設計文書) を書けないものはツールではない

## ビルド・テスト・Git

- AI の検証ループ (`Docs/design/ai-verification-loop.md`): C++ を変えたら `powershell -NoProfile -ExecutionPolicy Bypass -File Tools/AgentBuild.ps1 check <変えたファイル...>` でコンパイルだけ通す (リンクしないので起動中のエディターと衝突しない)。`build <target>` / `test -Filter <regex>` も同じ入口。出力は `ERROR path:line CODE msg` と `RESULT` 行、全文は `RESULT` 行の log= (最新は `build/agent/last-<verb>.txt` が指す)
- 動作の確認はシナリオ (`<Project>/Tests/Playtests/*.playtest.json`) で表明する。エディター起動中は MCP の `scenario_run` → `scenario_status`、起動せずに回すなら `FBZZEditor.exe --project <p> --batch <scenario> --hidden [--report <json>] [--update-baselines] [--warp]` (終了コード 0 合格 / 1 不合格 / 2 起動失敗)。基準画像は `<Project>/Tests/Golden/`、出力は `<Project>/Library/Playtests/`
- **ビルドは同時に 1 本だけ。** SDK 公開 (`CMake: Build SDK` / `GameHub + SDK`) も `build/<Config>` を使うので、人のタスク・別エージェントと重なりうる。`RESULT busy` や `HINT CONTENDED` (C1041 / `C1083 Permission denied` / LNK1104 / MSB3491 / "being used by another process") は**コードの誤りではない**。コードを直さず、プロセスを止めず、`build/` を消さず、終わるのを待つかユーザーに確かめてから 1 回だけ再実行する。別ツリー (`-Preset`) へ逃げない (全体の再コンパイルになる)
- フルビルドやエディターの再起動が要る変更 (DLL を掴まれてリンクできない) はユーザーに VS Code タスク (`CMake: Build All (Debug)` / `Tests: Build & Run Suite (Debug)` 等) を依頼する。VS 更新後に configure が落ちたら `build/<Config>/` を消して再 configure
- ビルド時間は前処理行数で決まる。標準ヘッダーは `fbzz_use_std_pch` に任せ、ヘッダーオンリーの重い実装 (`toml++` 等) を公開ヘッダーに載せない。詳細 `Docs/conventions/build-performance.md`
- テストは `TEST_F` + `TestKit` の fixture。float は `EXPECT_VEC3_NEAR` 等、乱数・時刻・sleep を持ち込まない。新規ファイルは `Projects/Tests/CMakeLists.txt` の `SOURCES` へ手で 1 行足す。詳細 `Docs/conventions/test.md`
- ブランチ `main → develop → feature/<name>`。コミットは `[Feature|Fix|Design|Build|Refactor|Chore|Release] + 動詞 + 概要`、本文は Markdown。詳細 `Docs/conventions/git.md`
- 報告の前に `git status` を見る (エディターが `.meta` やシーンを書き換える)
- 改名・削除は grep で全件を洗う。スクリプトを消すときは `ScriptList.inl` / `.meta` / シーン内参照も追う
