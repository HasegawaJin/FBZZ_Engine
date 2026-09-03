# ビルド時間の規約

フルビルドが 7 分かかる状態から、どこに時間が消えているかを実測して整理した記録
(2026-09-03)。数値は静的解析による見積りで、`#if` を評価しないため上限寄り。
計測スクリプトの考え方は本文の「測り方」に書いてある。

---

## 1. 何が支配的か

C++ のビルド時間は **翻訳単位 (TU) 数 × 1 TU あたりの前処理行数** でほぼ決まる。
本エンジンの内訳:

| | TU 数 | 自作ヘッダーの行数 / TU | 前処理行数 / TU |
|---|---|---|---|
| Engine | 237 | 12,821 | 362,672 |
| Editor | 146 | 約 13,000 | 424,782 |

**自作ヘッダーは全体の 3〜4% しかない。** 残りは標準ヘッダーと `<Windows.h>`。
つまり「自作コードを分割する」だけではフルビルドはほとんど縮まない。

主要ヘッダーの実測サイズ (MSVC 14.51 + Windows SDK 10.0.26100、推移的 include 込み):

> 以下の表は `#if` を評価しない静的な積算で、絶対値は上振れする。`cl /P` で
> 実際に前処理した `<Windows.h>` は **91,809 行**、`WIN32_LEAN_AND_MEAN` を
> 付けると **43,448 行** だった。順位付けには使えるが、実数は 4 倍ほど多く出る。

| ヘッダー | ファイル数 | 行数 |
|---|---:|---:|
| `<Windows.h>` | 196 | **349,373** |
| `<filesystem>` | 167 | 150,578 |
| `<chrono>` | 162 | 132,230 |
| `<queue>` | 133 | 117,491 |
| `<functional>` | 119 | 101,485 |
| `<unordered_map>` | 108 | 89,024 |
| `<algorithm>` | 102 | 86,203 |
| `<memory>` | 105 | 79,740 |
| `<string>` | 102 | 78,051 |
| `<vector>` | 99 | 76,093 |

`<Windows.h>` 1 本が、平均的な TU の自作ヘッダー全部の **27 倍**ある。

---

## 2. 規約

### ヘッダーに `<Windows.h>` を書かない

`.cpp` に書く。ヘッダーに書けば、そのヘッダーを (推移的にでも) include する
すべての TU に 35 万行が配られる。

実例: `Engine/Input/KeyCode.hpp` は 50 行の enum だが `VK_*` のために
`<Windows.h>` を include していた。このヘッダーは `Input.hpp` 経由で
Engine 237 TU 中 104 / Editor 146 TU 中 95 に届いていた。
**この 1 行を外すだけで前処理行数が Engine -27% / Editor -22%。**

OS の定数を列挙へ写すときは、値を数値リテラルで書き、
**Win32 境界の `.cpp` に `static_assert` を置いて一致を守る**
(`Engine/src/Input/Input.cpp` が実例)。ヘッダーから OS を切り離しても
値のずれはコンパイル時に捕まる。

`.cpp` で `<Windows.h>` が要るときは、その前に `WIN32_LEAN_AND_MEAN` を定義する。

### `NOMINMAX` はヘッダーで宣言しない

`<Windows.h>` の `min` / `max` マクロは `std::min` / `std::max` を C2589 で壊す。
これはルートの `CMakeLists.txt` が `add_compile_definitions(NOMINMAX)` で
**ビルド全体の前提として一度だけ宣言する**。`.cpp` や `.hpp` の先頭で
`#define NOMINMAX` してはいけない。

実例: 以前は `KeyCode.hpp` が `<Windows.h>` より先に `NOMINMAX` を定義することで、
それを include する 200 近い TU を偶然守っていた。`KeyCode.hpp` から
`<Windows.h>` を外した瞬間、その恩恵で通っていた `UISystem.cpp` などが
一斉に C2589 で落ちた。**include 順に依存した防御は、ヘッダーを 1 つ整理しただけで崩れる。**

同じ理由で、公開ヘッダーが引いていた `<Windows.h>` を外すときは
「そのヘッダー経由で Win32 を受け取っていた TU」を必ず洗い出す
(`VK_*` を使っていた `InputActionMap.cpp` がこれに該当した)。

### 標準ヘッダーは PCH に任せる

`CMake/FBZZPch.cmake` の `fbzz_use_std_pch(<target>)` を使う。Engine / Editor に適用済み。
PCH に **`<Windows.h>` は入れない** — 入れると `min` / `max` / `GetMessage` 等の
マクロが全 TU に配られ、どの TU がその汚染に依存しているか分からなくなる。

PCH は include の書き忘れも通してしまうので、
`-DFBZZ_USE_PCH=OFF` でビルドすれば include 漏れがエラーとして出る。
PCH 起因が疑われるリンクエラーが出たときも、まずこれで切り分ける。

> **PCH と `WINDOWS_EXPORT_ALL_SYMBOLS` は素では共存できない。**
> `/Yc` は既定で `__@@_PchSym_@00@<符号化パス>` という参照シンボルを
> `cmake_pch.obj` へ埋める。`WINDOWS_EXPORT_ALL_SYMBOLS` はリンク前に全
> オブジェクトを bindexplib で走査して `exports.def` を作るが、この '@' 混じりの
> 装飾名を解釈できず `__` という壊れたエントリを吐く。ThirdParty の
> `DirectXTex.lib` も同種のシンボルを持つため「unique match が見つからない」
> (LNK4022) となり、最終的に「外部シンボル `__` は未解決」(LNK2001) で落ちる。
> `fbzz_use_std_pch()` が `/Yl-` を付けて参照の注入を止めている。
> **`WINDOWS_EXPORT_ALL_SYMBOLS` を `FBZZ_API` へ置き換えれば、この回避策ごと不要になる** (§4-1)。

### 重いサードパーティをヘッダーに載せない

`toml++` はヘッダーオンリーで、include した TU はライブラリ本体
(`parser.inl` だけで 3,917 行) を毎回コンパイルし、さらに `std_string.inl` 経由で
`<Windows.h>` まで引く。公開ヘッダーに書かず、`.cpp` に閉じるか前方宣言 + pimpl にする。

---

## 3. 効果 (実測)

前処理行数の合計:

`KeyCode.hpp` / `Input.hpp` の 2 本から `<Windows.h>` を外し、標準ヘッダーを PCH 化した結果:

| | 対策前 | ヘッダー整理後 | + PCH |
|---|---:|---:|---:|
| Engine (237 TU) | 85,953,274 | 61,951,651 (-28%) | 39,692,664 (-54%) |
| Editor (146 TU) | 62,018,246 | 47,992,413 (-23%) | 37,151,632 (-40%) |
| 合計 | 1億4,797万 | 1億0,994万 | **7,684万 (-48%)** |

`<Windows.h>` に到達する TU は Engine 156 → 79 / Editor 113 → 68 になった。

---

## 3-2. 翻訳単位の統合 (unity build)

`-DFBZZ_UNITY_BUILD=ON` で有効。既定は OFF。`CMake/FBZZUnity.cmake`。

バッチ 16 でまとめたときの TU 数と前処理行数 (2026-09-03 見積り):

| モジュール | TU | 統合後 | 前処理行数 | 統合後 |
|---|---:|---:|---:|---:|
| FBZZEngine | 168 | 11 | 22,267,249 | 4,504,327 (-80%) |
| FBZZCore | 25 | 2 | 5,294,701 | 937,129 (-82%) |
| FBZZRenderDX11 | 11 | 1 | 4,681,245 | 502,485 (-89%) |
| FBZZRenderDX12 | 15 | 1 | 6,593,382 | 546,198 (-92%) |
| FBZZEditor | 146 | 9 | 37,151,139 | 4,904,073 (-87%) |
| **合計** | | | **77,214,749** | **12,035,095 (-84%)** |

**フルビルドと増分ビルドで得失が逆になる。** 統合すると 1 ファイル直しただけで
同じバッチの 16 本が巻き添えで再コンパイルされる。**CI と配布ビルドでは ON、
日々の Development ビルドでは OFF** が使い分けの目安。

統合できない翻訳単位は `fbzz_use_unity_build(... EXCLUDE ...)` へ列挙する。
現在の除外は `StbFontImpl.cpp` / `StbImage.cpp` / `HdriLoader.cpp` (ヘッダーオンリー
実装の展開場所)、`Window.cpp` (`#undef` しないマクロ定義)、`Input.cpp`
(`WIN32_LEAN_AND_MEAN` が後続ファイルの Windows.h を変える)。

初めて有効にすると、無名 namespace の同名シンボル衝突が出る可能性がある
(144 の `.cpp` が無名 namespace を持つ)。衝突したら名前を直すか EXCLUDE へ足す。

## 3-3. 増分ビルド

**フルビルドと増分ビルドはボトルネックが違う。** フルビルドは前処理量で決まるが、
1 ファイル直しただけの増分ではリンクが支配する。現状の生成物 (Development):

| | サイズ / 数 |
|---|---:|
| `FBZZEngine` の export シンボル | 48,551 |
| `FBZZEngine.exp` | 32 MB |
| `FBZZEngine.lib` (インポートライブラリ) | 52 MB |
| `FBZZEngine.pdb` | 133 MB |
| `FBZZEditor.lib` | 701 MB |
| `FBZZEditor.pdb` | 227 MB |

`.cpp` を 1 本直すと、コンパイルは 1 TU で済むのに、この後ろで
「bindexplib が 246 個のオブジェクトを走査 → 32MB の `.def` 生成 → フルリンク →
PDB マージ → `FBZZEditor.lib` の再アーカイブ → exe のリンク」が毎回走る。

> **`/DEBUG:FASTLINK` は使えない。** MSVC 19.51 (VS 18) で削除されており、指定すると
> リンクのたびに LNK4315 が出て `/DEBUG:FULL` へ倒れる。`link /?` のヘルプには
> まだ載っているので、ヘルプではなく実際のリンク結果で確認すること。
> PDB マージを避ける手は残っていないので、**リンク時間は「何をリンクするか」を
> 減らすことでしか縮まない** (§4-1 / §4-2)。

- `/INCREMENTAL:NO` は Development で維持している。`WINDOWS_EXPORT_ALL_SYMBOLS` が
  毎ビルド `.def` を作り直し、内容が変われば増分リンクはどのみち破棄されるため。

**測り方**: `.vscode/tasks.json` の `Profile: Build Timing (Development)` を使う。
MSBuild の `/clp:PerformanceSummary` がタスク別 (CL / Link / Lib) の合計時間を出すので、
「コンパイルとリンクのどちらに何秒行っているか」が分かる。
増分を測るときは、直前に 1 ファイルだけ保存し直してから走らせる。

## 4. 残っている問題

着手順に並べてある。上ほど費用対効果が高い。

### 4-1. `WINDOWS_EXPORT_ALL_SYMBOLS` (リンク時間)

`FBZZEngine` は **46,544 シンボル**を export している (`FBZZPhysics` 4,553 / `FBZZMath` 106)。
`WINDOWS_EXPORT_ALL_SYMBOLS ON` は全オブジェクトをスキャンして `.def` を作り、
そのうえでリンクし直す 2 パス処理になる。生成物も `FBZZEngine.exp` 31MB /
インポートライブラリ 52MB まで膨らみ、Engine を参照する exe すべてがこれを読む。

これは同時に設計の問題でもある。46,544 シンボルが公開されているということは
**DLL 境界が事実上存在しない**。`FBZZ_API` マクロで公開 API を明示すれば、
リンク時間と「依存関係の整理」が同じ作業で片付く。

### 4-2. `FBZZEditor.lib` が 726MB

STATIC ライブラリ 1 本に 146 TU 分のデバッグ情報とテンプレート実体が詰まっている。
Editor の exe リンクは毎回これを読む。

### 4-3. 再コンパイルの波及範囲

1 ヘッダーを直したときに再コンパイルされる TU 数 (全 427 TU 中):

| ヘッダー | 波及 |
|---|---:|
| `Math/Vector3.hpp` | 320 (74%) |
| `Math/Quaternion.hpp` | 285 (66%) |
| `Engine/Renderer/ResourceHandle.hpp` | 264 (61%) |
| `Engine/Renderer/RenderState.hpp` | 259 (60%) |
| `Engine/Renderer/Mesh.hpp` | 236 (55%) |
| `Engine/Renderer/RenderSettings.hpp` | 219 (51%) |
| `Engine/Scene/Script.hpp` | 100 (Engine 内 42%) |

Math のように安定した土台が上位に来るのは正常。問題は
`RenderSettings.hpp` (794 行) や `Script.hpp` (1,467 行) のように
**まだ変更が入る大きなヘッダー**が半数の TU に届いていること。

### 4-4. ライブラリ分割 (2026-09-03 に実施)

**フルビルドの時間はライブラリを割っても縮まない** (コンパイルする TU 数は同じ)。
目的はビルド時間ではなく、依存方向を CMake に強制させることと、増分ビルドの波及縮小。

| モジュール | 種別 | TU | 中身 |
|---|---|---:|---|
| `FBZZCore` | OBJECT | 25 | Logger / Time / Memory / Scheduler / Util / Profiler / Input |
| `FBZZRHI` | OBJECT | 19 | `IRenderer` 等の抽象と共通実装 (Mesh / Material / ResourceManager) |
| `FBZZRenderPlatform` | OBJECT | 2 | D3D 共通 (GpuValidation / RenderTargetCapture) |
| `FBZZRenderDX11` | OBJECT | 11 | DirectX 11 バックエンド |
| `FBZZRenderDX12` | OBJECT | 15 | DirectX 12 バックエンド (`FBZZ_ENABLE_DX12`) |
| `FBZZEngine` | SHARED | 168 | Scene / Asset / Audio / AI と合成ルート |

**OBJECT を使う理由**: 本エンジンは `FBZZ_REGISTER_VOLUME_OVERRIDE` のように
「誰も参照しない静的初期化子で自分を登録する」実装を持つ。STATIC にすると
リンカが翻訳単位ごと捨て、機能が *エラーも警告も出さずに* 消える
(テスト基盤が OBJECT を強制しているのと同じ理由)。

**`$<TARGET_OBJECTS:>` で取り込む理由**: `install(EXPORT)` はリンク依存先も同じ
export set に居ることを要求する。オブジェクトとして取り込めばリンク依存にならず、
内部モジュールを SDK の公開ターゲットへ漏らさずに済む。
`WINDOWS_EXPORT_ALL_SYMBOLS` の走査対象 (`objects.txt`) には全モジュールの
オブジェクトが入ることを確認済みなので、export が痩せる心配はない。

**バックエンドの隔離**: DX11 / DX12 の具象ヘッダーは各モジュールの PRIVATE include
ディレクトリにしか無く、`FBZZEngine` の include パスから `src` を外してある。
`RendererFactory` からも DX 型は見えない。外へ出るのは
`Engine/Renderer/BackendEntry.hpp` の `CreateDX11Backend` / `CreateDX12Backend` だけ。
「上位レイヤーは `IRenderer&` のみ」がレビューではなくコンパイルエラーで守られる。

**`FBZZEngine_EXPORTS` はモジュール側でも定義する**: `Time.hpp` / `Cursor.hpp` は
「このマクロがあれば `dllexport`、無ければ `dllimport`」をヘッダー内で自前判定している。
CMake がこれを自動定義するのは SHARED ターゲットだけなので、OBJECT モジュールでは
明示的に足さないと `dllimport` に落ちて C2491 になる。**新しく
`__declspec(dllexport)` を書くヘッダーを増やさないこと** — この判定は
`FBZZ_API` 化のときに 1 本の生成ヘッダーへ統合して消す。

**残っている逆流**: `Application.hpp` は Renderer / Scene を参照するが、
Sandbox / GreenWare / Tests / GameHub テンプレートを含む 18 箇所が include する
公開 SDK API なので移動していない。翻訳単位 (`Application.cpp`) だけを
`FBZZEngine` 側へ寄せてある。`SaveStore.cpp` も同じ扱い
(ヘッダーは低層 API、実装が `scene::IReflector` を要求する)。

---

## 5. 測り方

`#include` を再帰的に辿って行数を積算するだけで、実際にビルドしなくても
上の数字は出せる。include の探索パスは `target_include_directories` と
MSVC / Windows SDK の include ディレクトリを並べる。`#if` は評価しないので
絶対値は上限寄りになるが、**施策の前後比較と順位付けには十分**。

実際のビルド時間で裏を取るときは MSVC の `/Bt+` (関数別の時間) と
`/d1reportTime` (テンプレート実体化の時間) を使う。
