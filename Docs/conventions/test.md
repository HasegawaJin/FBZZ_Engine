# テスト規約

GoogleTest / GoogleMock による自動テストと、開発者が目で確かめる手動テストの運用ルール。
`git.md` と同じく、ポートフォリオとして採用担当者に読まれることを前提にしている。

新しいテストを追加する前に、**「分類」** と **「追加手順」** の 2 章だけは必ず読むこと。

---

## 1. 3 つの層

テストは性質の違う 3 つに分かれる。混ぜると CI が止まるか、誰も実行しないテストが増える。

| 層 | 置き場所 | CTest 登録 | 判定 | 目的 |
|---|---|---|---|---|
| **Auto** | `<Domain>/Auto/` | する | コードが自動判定 | 契約の回帰検出。CI で毎回回す |
| **Manual** | `<Domain>/ManualTest/` | **しない** | 開発者が目で判定 | OS 状態やデバイスを触る確認 |
| **Bench** | `Projects/Tests/Bench/` | しない | 開発者が絵を見て判定 | 数値では正しさが決まらない挙動の可視化 |

### どの層に書くか

```
そのテストは、実行結果だけで合否を機械判定できるか？
├─ はい ─────────────────────────────────────────→ Auto
└─ いいえ
   ├─ 判定材料が「絵」か「動き」である
   │   （XPBD の収束の様子、ラグドールの姿勢、IK の追従）→ Bench
   └─ 判定材料が「OS / デバイスの状態」である
       （ウィンドウが出る、カーソルが消える、音が鳴る） → Manual
```

**Auto に書けるものを Manual に逃がさない。** 「実行に 3 秒かかるから」「たまに落ちるから」は
Manual へ移す理由にならない。前者は設計、後者はテストかコードのバグ。

### Manual が満たすこと

- CTest に登録しないので、**`gtest_discover_tests` を呼ばない**（`fbzz_add_test_suite` に `MANUAL` を付ける）
- テスト本体の冒頭に、**何を目で確認すればよいかを日本語で `RecordProperty` か `SCOPED_TRACE` で出す**
- 実行後、触った OS 状態を必ず元に戻す（カーソル表示、ウィンドウ、クリップ領域）

---

## 2. ディレクトリとファイル名

```
Projects/Tests/
├── TestKit/                      テスト土台の静的ライブラリ（後述）
│   ├── include/TestKit/
│   └── src/
├── Math/
│   └── Auto/Vector3Tests.cpp
├── Core/
│   ├── Auto/Memory/PoolAllocatorTests.cpp
│   └── ManualTest/CursorManualTests.cpp
├── Physics/
│   └── Auto/GJKTests.cpp
├── Engine/
│   └── Auto/RagdollRigTests.cpp
└── Bench/                        ビジュアル検証ベンチ（ImGui アプリ）
```

| 対象 | 規則 | 例 |
|---|---|---|
| ファイル名 | `<テスト対象の型か機能>Tests.cpp` | `Vector3Tests.cpp` / `GJKTests.cpp` |
| 手動テスト | `<対象>ManualTests.cpp` | `CursorManualTests.cpp` |
| 名前空間 | `fbzz::tests` で囲む | |
| ヘルパー | ファイル内の無名 `namespace` に置く。2 ファイル以上で要るなら TestKit へ移す | |

**1 ファイル 1 対象。** `Vector3Tests.cpp` に Matrix4 のテストを書かない。
対象が大きくて 300 行を超えたら、`Auto/Memory/` のように観点でディレクトリを切って分割する。

ファイルヘッダーは全ファイル必須（`AGENTS.md` の規約どおり `@file` / `@brief` / `@author` / `@date`）。
`@brief` には「何の契約を守らせているか」を書く。「テストです」は情報がゼロなので書かない。

```cpp
/// @file    Vector3Tests.cpp
/// @brief   Vector3 の正規化・内積・外積が右手系の定義どおりであることを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-02
```

### 成果物の置き場

ビルドツリーは構成ごとに分かれる（`build/Debug` / `build/Development` / `build/Release`）。
その中の `Binaries/` は次のレイアウトで、**exe は必ずサブディレクトリに出る**。

```
build/<Preset>/Binaries/<Config>/
├── FBZZEngine.dll ...            共有 DLL だけ。exe は置かない
├── Editor/     FBZZEditor.exe    ターゲット名は FBZZEditorLauncher（OUTPUT_NAME で改名）
├── Sandbox/    Sandbox.exe / SandboxStandalone.exe
└── Tests/      FBZZTests<Suite>.exe / FBZZTestBench.exe
```

スイート名と exe 名は 1 対 1（`MathAuto` → `FBZZTestsMathAuto.exe`）。`Auto` を省いた
`FBZZTestsPhysics.exe` のような名前は存在しない。

> **ルート直下に `FBZZTests*.exe` を見つけたら、それは古いビルドの死骸。**
> CMake はターゲットの出力先を変えても既に置いた成果物を消さないため、
> 名前だけ現行と同じで中身が古い exe がテスト用の実体と並んでしまう。
> `CMake/FBZZArtifacts.cmake` の `fbzz_prune_stale_runtime_artifacts()` が
> configure のたびに掃除する。ターゲットを消したときは同ファイルの
> `FBZZ_RETIRED_RUNTIME_TARGETS` へ名前を足すこと。

---

## 3. テストの名前

名前は**失敗したときにしか読まれない**。CI のログに `[  FAILED  ] Vector3Test.Test3` と出て
何も分からない、という状態を避けることだけを考える。

```
TEST_F(<対象>Test, <主語が省略された仕様文>)
```

- fixture 名は `<対象>Test`（`Tests` ではなく単数）
- テスト名は **英語の仕様文**。動詞から始め、`ShouldXxx` は付けない（`Should` は全件に付いて情報量が無い）
- 条件がある場合は `WhenXxx` / `ForXxx` を後置する

```cpp
// 良い ─ 落ちたときに、何の契約が壊れたか読める
TEST_F(Vector3Test, CrossProductFollowsRightHandRule)
TEST_F(Vector3Test, NormalizedPreservesDirectionForTinyVectors)
TEST_F(PoolAllocatorTest, ReturnsNullptrWhenExhausted)
TEST_F(XPBDSolverTest, RestPositionIsIndependentOfSubstepCount)

// 悪い
TEST_F(Vector3Test, Test1)            // 何も分からない
TEST_F(Vector3Test, Cross)            // 何を期待しているのか無い
TEST_F(Vector3Test, ShouldWorkCorrectly)  // 全テストに当てはまる
TEST_F(Vector3Test, バグ修正)          // 検索できない・CI ログで化ける
```

---

## 4. `TEST` ではなく `TEST_F` を使う

**原則として `TEST_F` を使い、素の `TEST` は書かない。**

理由は 2 つ。

1. **後から共通の前処理が必要になったとき、`TEST` は書き換えが必要になる。** fixture が最初から
   あれば `SetUp` に 1 行足すだけで済む。
2. **グローバル状態の復帰を強制できる。** このエンジンは `Logger` / `MemorySystem` / `Time` の
   ように静的な状態を持つサブシステムが多く、`TearDown` を書ける場所が無いテストは
   **実行順によって結果が変わる**（他のテストを壊す、あるいは他のテストに壊される）。

fixture が本当に空でよい場合も、TestKit の基底 fixture を使う。

```cpp
// Arrange をメンバに持たせ、TearDown で必ず状態を戻す
class LoggerTest : public testkit::EngineFixture {
protected:
    void SetUp() override {
        EngineFixture::SetUp();
        core::Logger::SetMinLevel(core::LogLevel::DEBUG);
        core::Logger::AddSink(&sink);
    }
    void TearDown() override {
        core::Logger::RemoveSink(&sink);
        EngineFixture::TearDown();   // 最小レベルもここで戻る
    }
    testkit::RecordingLogSink sink;
};
```

例外は、**型パラメータ化テスト** (`TYPED_TEST`) と **値パラメータ化テスト** (`TEST_P`)。
アロケーター 4 種のように同じ契約を複数の型に課す場合は `TYPED_TEST` を積極的に使う。

---

## 5. テスト本体の構造

**Arrange / Act / Assert の 3 段を、空行で区切って必ずこの順に書く。** コメントで
`// Arrange` と書く必要はない（空行で読めるなら冗長）。書くのは、区切りが自明でないときだけ。

```cpp
TEST_F(PoolAllocatorTest, ReusesTheMostRecentlyFreedBlock)
{
    core::PoolAllocator pool(sizeof(Payload), 4);
    void* first  = pool.Allocate();
    void* second = pool.Allocate();

    pool.Free(second);
    void* reused = pool.Allocate();

    EXPECT_EQ(reused, second);
    EXPECT_NE(reused, first);
}
```

### 1 テスト = 1 つの振る舞い

「1 テスト 1 アサーション」ではない。**1 つの振る舞いを説明するのに必要なだけ**アサーションを書く。
上の例は 2 つ `EXPECT` があるが、確かめている振る舞いは「直近に返した区画を再利用する」の 1 つ。

逆に、`Allocate` と `Free` と `Reset` を 1 つのテストに詰めるのは分割する。落ちたときに
テスト名から原因が絞れなくなる。

### `EXPECT` と `ASSERT`

| | 使う場面 |
|---|---|
| `EXPECT_*` | **既定**。失敗しても続行し、1 回の実行で全部の不一致を報告する |
| `ASSERT_*` | **失敗したら以降が無意味になるとき**。ポインタの非 null、配列サイズ、`std::optional` の有無 |

`ASSERT_NE(ptr, nullptr)` を書かずに `EXPECT_EQ(ptr->value, 3)` を書くと、失敗時にテストプロセスが
落ちて他のテスト結果まで失われる。**参照外しの前には必ず `ASSERT`。**

---

## 6. 浮動小数点の比較

`EXPECT_EQ` で float を比べない。生の `EXPECT_NEAR` も**原則使わない** — TestKit の
マクロを使う。理由は、失敗メッセージに「どの成分が」「どれだけ」ずれたかを出すため。

```cpp
// 良い
EXPECT_VEC3_NEAR(actual, math::Vector3(0.0f, 1.0f, 0.0f), testkit::kTolerance);
EXPECT_QUAT_NEAR(rotation, expected, testkit::kTolerance);   // -q と q を同一視する

// 悪い
EXPECT_EQ(v.x, 1.0f);                       // 丸めで落ちる
EXPECT_NEAR(v.x, 1.0f, 0.001f);             // y と z を見ていない
EXPECT_TRUE(a == b);                        // 失敗時に値が出ない
```

許容誤差は**マジックナンバーを書かず**、意味のある定数を使う。

| 定数 | 値 | 用途 |
|---|---|---|
| `testkit::kTolerance` | `1e-5f` | 単発の演算（正規化・内積・行列積） |
| `testkit::kLooseTolerance` | `1e-3f` | 反復・積分を経た結果（XPBD の収束位置など） |

**クォータニオンは `q` と `-q` が同じ回転**なので、成分の直接比較は誤検出する。
必ず `EXPECT_QUAT_NEAR`（内積の絶対値で判定する）を使う。

---

## 7. GoogleMock を使う基準

gmock は**「呼ばれ方そのもの」が契約であるとき**にだけ使う。値や状態の検証には使わない。

| 検証したいこと | 使うもの |
|---|---|
| 結果の値・状態 | 素の `EXPECT_*` |
| 呼ばれた**回数**（1 回だけ通知される、二重解放しない） | `EXPECT_CALL(...).Times(1)` |
| 呼ばれる**順序**（Begin より先に End が来ない） | `InSequence` / `.After()` |
| **呼ばれないこと**（フィルタで捨てられる） | `EXPECT_CALL(...).Times(0)` |
| 呼び出しの記録を後でまとめて見たい | 手書きの記録用 fake（TestKit の `Recording*`） |

```cpp
class MockLogSink : public core::ILogSink {
public:
    MOCK_METHOD(void, OnLog, (const core::LogEntry&), (override));
};

TEST_F(LoggerTest, DoesNotForwardEntriesBelowTheMinimumLevel)
{
    testing::StrictMock<MockLogSink> sink;
    core::Logger::AddSink(&sink);
    core::Logger::SetMinLevel(core::LogLevel::WARN);

    EXPECT_CALL(sink, OnLog(testing::_)).Times(0);   // これが契約そのもの

    core::Logger::Info("dropped");
    core::Logger::RemoveSink(&sink);
}
```

### mock の注意

- **既定は `StrictMock`。** 素の mock は「期待していない呼び出し」を警告で流すので、
  契約違反がテスト成功のまま埋もれる。緩めたいときだけ `NiceMock` を明示する
- **mock はインターフェース（`I` 始まりの純粋仮想クラス）にだけ当てる。** 具象クラスを
  virtual 化してまで mock するのは、テストのために設計を歪めることになる
- `EXPECT_CALL` は **Act の前に書く**。gmock は宣言時点から記録を始めるため、後置すると効かない
- mock オブジェクトの寿命がテストの最後まであること。`Logger` のような**非所有 sink** に
  渡した mock は、`TearDown` より先に破棄されないよう fixture のメンバにする

---

## 8. 決定論

**同じテストは何度実行しても同じ結果になること。** これを壊す 3 つを禁止する。

| 禁止 | 代わりに |
|---|---|
| `rand()` / `std::random_device` / 時刻シード | `testkit::DeterministicRng`（固定シード） |
| `std::chrono::now()` に依存した経過時間 | `dt` を引数で渡す。`testkit::StepFixed(sim, frames, dt)` |
| `Sleep` / `std::this_thread::sleep_for` でのタイミング合わせ | 条件変数・`std::latch`・`TaskSystem` の完了待ち API |

スレッドのテストは「タイミングに依存しない不変条件」を検証する。
「1 ms 待てば終わっているはず」は、CI の負荷次第で必ずいつか落ちる。

```cpp
// 良い ─ 完了を待ってから、合計値という順序に依らない不変条件を見る
TEST_F(TaskSystemTest, ExecutesEverySubmittedTaskExactlyOnce)
{
    std::atomic<int> sum{0};
    for (int i = 1; i <= 100; ++i) system.Submit([&sum, i] { sum += i; });

    system.WaitForAll();

    EXPECT_EQ(sum.load(), 5050);
}

// 悪い
std::this_thread::sleep_for(10ms);
EXPECT_EQ(sum.load(), 5050);
```

---

## 9. 外部リソース

- **リポジトリ内のアセットを読まない。** シーンやモデルを読むテストは、アセットを差し替えた
  瞬間に落ちる。必要なデータはテスト内で組み立てる（`RagdollRigTests.cpp` の `StraightChain()` が手本）
- **ファイルを書くときは `testkit::TempDir`。** RAII でテスト終了時に消える。
  カレントディレクトリや `Assets/` に直接書かない
- **ネットワークアクセスは禁止**

---

## 10. 実行方法

### VS Code（普段の開発）

`Ctrl+Shift+P` → `Tasks: Run Task`。

| タスク | 何をするか |
|---|---|
| **Tests: Build & Run Suite (Debug)** | スイートを選んでビルド → 実行。結果がそのままターミナルに出る |
| Tests: Shuffle Suite (Debug) | 順序を混ぜて 10 回。実行順に依存したテストを炙り出す |
| CMake: Build Tests (Debug) | 全スイート + ベンチをビルド |
| Tests: Run (Debug) | CTest で全件 |
| Tests: Coverage (Debug) | OpenCppCoverage で計測して HTML を出す |
| Bench: Build & Run (Debug) | ビジュアル検証ベンチ |

デバッガーを付けたいときは `F5`（実行とデバッグ）から
`Tests: Math Auto (Debug)` などを選ぶ。`--gtest_filter` を対話で聞かれる。

> **スイートを追加したら `.vscode/tasks.json` の `CMake: Build Tests (*)` へ
> `--target` を 1 行足すこと。** 書き漏らしたスイートは「ビルドされない」だけで、
> エラーも警告も出ない。実際に Math とベンチが丸ごと抜けたことがある。

### Visual Studio

**テスト exe をスタートアッププロジェクトにして `F5` / `Ctrl+F5`。**
ソリューションエクスプローラーで `Tests` フォルダの下から選び、右クリック →
`スタートアップ プロジェクトに設定`。

| プロジェクト | 対象 |
|---|---|
| `FBZZTestsMathAuto` | Vector / Quaternion / Matrix |
| `FBZZTestsPhysicsAuto` | GJK / EPA / Collider / AABB / BVH / XPBD |
| `FBZZTestsCoreAuto` | アロケーター / Scheduler / TaskSystem / Signal / Logger / Time |
| `FBZZTestsEngineAuto` | RagdollRig |

結果はコンソールに出る。**終了時に自動で閉じない** — 最後に「すべて成功 / 失敗あり」を
出して入力待ちで止まるので、緑と赤をそのまま読める。

> CMake が生成した `.sln` を開いている場合、テストエクスプローラーには CTest のケースが
> 並ばない（VS がフォルダを CMake プロジェクトとして開いたときだけ連携する）。
> `.sln` 運用では上の「exe を直接起動」が普段の経路になる。

文字が小さい / 大きいときは環境変数 `FBZZ_CONSOLE_FONT_SIZE` で上書きできる
（96dpi 基準の px。既定 17、画面の DPI に合わせて自動で拡大する）。

### CTest（CI / 全件確認）

```bash
ctest --test-dir build/Debug --output-on-failure -C Debug
ctest --test-dir build/Debug -C Debug -R "Physics"        # 名前で絞る
```

`--test-dir` は CMakePresets の `binaryDir`（構成ごとに `build/<Preset>`）を指す。
`build` を渡しても `CTestTestfile.cmake` が無く「テスト 0 件」で成功扱いになる。

### 1 件で止まっても最後まで走ること

**「失敗」より「終わらない」の方が被害が大きい。** 失敗は次のテストへ進むが、
終わらないテストは残り全部を道連れにする。実際、EPA の縮退した配置でポリトープが
膨張し続け、そこから後ろのテストが 1 件も実行されないまま終わったことがある。

止まる原因は 2 つしかない。両方とも基盤側で塞いである。

| 止まり方 | 対策 | 置き場所 |
|---|---|---|
| 無限ループ・異常に遅い | **1 テスト 30 秒で強制終了**し、Timeout として記録して次へ進む | `CMake/FBZZTests.cmake` の `FBZZ_TEST_TIMEOUT`（`gtest_discover_tests` の `PROPERTIES TIMEOUT`）と `Tools/VcBuild.ps1` / CI の `--timeout` |
| 押されるまで消えないダイアログ<br>（`assert` 失敗・`abort`・アクセス違反） | 自動実行では**ダイアログを出さず stderr へ流して落とす** | `TestKit::SuppressBlockingErrorDialogs()` を `Main.cpp` が自動実行時だけ呼ぶ |

- **上限を延ばして通そうとしない。** 30 秒に届く自動テストは、書き方かコードのどちらかが
  間違っている。自動テストは 1 件あたり数 ms で終わる前提で設計している（`sleep` 禁止・固定刻み）
- **ダイアログ抑止は自動実行のときだけ。** 対話実行（Visual Studio / ターミナル）では
  そのまま出す。人が居るなら、デバッガーで止めてスタックを見られる方が有益
- CI と `Tests: Run (Debug)` には `--no-tests=error` も付けてある。フィルタの打ち間違いで
  0 件になったとき、「全部成功」に見えてしまうのを防ぐ

### 単体 exe（絞り込み・調査）

```bash
FBZZTestsPhysicsAuto.exe --gtest_filter=GJKTest.*
FBZZTestsPhysicsAuto.exe --gtest_repeat=100 --gtest_shuffle   # 順序依存の炙り出し
FBZZTestsPhysicsAuto.exe --gtest_break_on_failure             # 失敗行でデバッガに落とす
```

**`--gtest_shuffle` は定期的に回すこと。** 実行順に依存したテスト（`TearDown` の書き漏れ）は
これでしか見つからない。

### ビジュアル検証ベンチ

`FBZZTestBench.exe` を起動し、左のリストからシーンを選ぶ。判定は目で行う。

---

## 11. 新しいテストを追加する手順

1. 「3 つの層」のフローチャートで **Auto / Manual / Bench** を決める
2. `Projects/Tests/<Domain>/Auto/<対象>Tests.cpp` を作り、ファイルヘッダーを書く
3. `Projects/Tests/CMakeLists.txt` の該当スイートの `SOURCES` に**ファイルパスを 1 行足す**
   （`file(GLOB)` は使わない。新規ファイルが CMake 再実行まで拾われず、
   「書いたのに実行されていないテスト」が生まれるため）
4. `TEST_F` で書く。fixture が要らなくても TestKit の基底を使う
5. **わざと壊して、テストが赤くなることを確認する。** 実装を 1 行書き換えて実行し、
   落ちなければそのテストは何も検証していない
6. 元に戻して緑を確認する

### レビュー観点（自分で見る）

- [ ] テスト名だけ読んで、何の契約か分かるか
- [ ] Arrange / Act / Assert が空行で分かれているか
- [ ] float を `EXPECT_EQ` で比べていないか
- [ ] グローバル状態を触ったなら `TearDown` で戻しているか
- [ ] `--gtest_shuffle` を付けても通るか
- [ ] `sleep` でタイミングを合わせていないか
- [ ] 実装を壊したら赤くなることを確認したか

---

## 12. 何をテストするか / しないか

このエンジンは**数学・物理・メモリをゼロから実装している**ことが価値なので、そこを厚く守る。

| 優先度 | 対象 | 理由 |
|---|---|---|
| **高** | `Math`（Vector / Matrix / Quaternion / Ray / Plane / Frustum） | 純粋関数で書きやすく、壊れると全レイヤーが静かに狂う |
| **高** | `Physics`（GJK / EPA / XPBD / Constraint / BVH） | 自作の中核。数値が合っているかは目視で分からない |
| **高** | `Core`（アロケーター / Scheduler / TaskSystem / Signal） | 壊れ方がクラッシュや競合で、原因究明に最も時間を食う |
| **中** | シリアライズ往復（`.scene` / `.mat` / `.anim`） | 「保存して読んだら同じ」は自動化しやすく、事故が多い |
| **中** | `Asset`（GUID 解決 / パス正規化） | 純ロジックで、壊れると全アセット参照が飛ぶ |
| **低** | `Renderer` の描画結果 | 実デバイス依存。ベンチで目視するほうが速い |
| **書かない** | ImGui パネルの UI 操作 | 変更頻度が高く、テストが実装の写経になる |
| **書かない** | 単なる getter / setter | 契約が無いのでテストが実装のコピーになる |

**カバレッジ率を目標にしない。** 「壊れたときに一番痛い順」に書く。
