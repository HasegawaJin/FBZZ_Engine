# テスト規約

GoogleTest / GoogleMock による自動テストと、開発者が目で確かめる手動テストの運用ルール。
`git.md` と同じく、ポートフォリオとして採用担当者に読まれることを前提にしている。

新しいテストを追加する前に、**「分類」**、**「テスト対象の境界と公開 API」**、**「追加手順」** を必ず読むこと。

---

## 1. 3 つの層

テストは性質の違う 3 つに分かれる。混ぜると CI が止まるか、誰も実行しないテストが増える。

| 層 | 置き場所 | CTest 登録 | 判定 | 目的 |
|---|---|---|---|---|
| **Auto** | `<Domain>/Auto/` | 既定スイートはする | コードが自動判定 | 契約の回帰検出。既定は CI、明示実行は必要なときに回す |
| **Manual** | `<Domain>/ManualTest/` | **しない** | 開発者が目で判定 | OS 状態やデバイスを触る確認 |
| **Bench** | `Projects/Tests/Bench/` | しない | 開発者が絵を見て判定 (+ 場面自身の異常検知) | 数値では正しさが決まらない挙動の可視化 |

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

### 既定実行と明示実行

合否の判定と実行環境を分けて決める。実 DX12/DXR デバイスを作成し、描画・GPU 計算・
読戻しを行うテストは **Auto のまま明示実行専用**とする。既存の `OPT_IN` により
通常ビルドから外し、CTest に登録しない。テスト本体と失敗時の判定は維持する。

CPU の値・fake backend・描画計画・履歴・キャッシュ・光源/粒子抽出・設定の契約は通常の
CTest / CI に残す。名前に Render / Ray / GPU があることだけでは除外しない。
混在ファイルはケースを分割し、CPU スイートの exe を直接起動するカバレッジでも GPU を回さない。

| スイート | 実行対象 | 通常ビルド / CTest / CI | CTest ラベル |
|---|---|---|---|
| `FBZZTestsMathAuto` / `FBZZTestsCoreAuto` / `FBZZTestsPhysicsAuto` | 数学 / メモリ / 衝突・拘束 | 含む | `Math` / `Core` / `Physics` |
| `FBZZTestsFluidAuto` | 流体演算 / GPU 向け CPU パッキング | 含む | `Fluid` |
| `FBZZTestsEngineAuto` / `FBZZTestsEditorAuto` | シーン / アセット / CPU 抽出・設定 / エディター契約 | 含む | `Engine` / `Editor` |
| `FBZZTestsGraphicsAuto` | CPU 描画計画 / 履歴 / キャッシュ / fake profiler | 含む | `Graphics` |
| `FBZZTestsGraphicsStandalone` | 実 DX12 描画 / GPU 計測 / 読戻し | **明示指定のみ、CTest 非登録** | 未登録 |
| `FBZZTestsRayTracing` | DXR / Hybrid / PathTracing の GPU 検証 | **明示指定のみ、CTest 非登録** | 未登録 |
| `FBZZTestsRayTracingEngine` | GPU skinning / HDR 抽出 | **明示指定のみ、CTest 非登録** | 未登録 |

通常の `AgentBuild test` / `VcBuild test` / 生の `ctest` と標準 CI は CPU スイートを実行する。
描画スイートは標準 CI・カバレッジのビルド/実行対象へ加えない。
DX12、対象シェーダー、DXC runtime を準備し、DXR ケースは DXR 対応 GPU/ドライバで実行する。
Microsoft Basic Render Driver / WARP で DXR が使えるとは仮定しない。

`Tests: Build & Run Graphics (Debug)` / `Tests: Build & Run RayTracing (Debug)` が明示実行入口。
短い確認では以下のように対象を絞る（exe 直接実行には CTest の TIMEOUT は適用されない）。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/AgentBuild.ps1 build FBZZTestsGraphicsStandalone -Preset debug
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/VcBuild.ps1 run debug FBZZTestsGraphicsStandalone --gtest_list_tests
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/VcBuild.ps1 run debug FBZZTestsGraphicsStandalone --gtest_filter=GraphicsStandaloneTest.DrawsAndReadsBackWithoutEngine
```

RayTracing は `AgentBuild build FBZZTestsRayTracing FBZZTestsRayTracingEngine -Preset debug` と
`VcBuild run debug FBZZTestsRayTracing --gtest_filter=<ケース名>` で同様に選ぶ。

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

スイート名と exe 名は 1 対 1（`MathAuto` → `FBZZTestsMathAuto.exe`）。
`GraphicsStandalone` / `RayTracing` / `RayTracingEngine` もそのまま exe 名に対応する。
`FBZZTestsPhysics.exe` のように既存スイート名から `Auto` を省いた名前は存在しない。

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
/// @note 失敗時に、どの契約が壊れたかを名前から読める例。
TEST_F(Vector3Test, CrossProductFollowsRightHandRule)
TEST_F(Vector3Test, NormalizedPreservesDirectionForTinyVectors)
TEST_F(PoolAllocatorTest, ReturnsNullptrWhenExhausted)
TEST_F(XPBDSolverTest, RestPositionIsIndependentOfSubstepCount)

/// @warning 次の名前は検証する契約を表さない。
TEST_F(Vector3Test, Test1)
TEST_F(Vector3Test, Cross)
TEST_F(Vector3Test, ShouldWorkCorrectly)
/// @warning 日本語識別子は検索しにくく、CI ログで文字化けすることがある。
TEST_F(Vector3Test, バグ修正)
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
/// @note グローバルな Logger 設定と非所有 sink は TearDown で必ず戻す。
class LoggerTest : public testkit::EngineFixture {
protected:
    void SetUp() override {
        EngineFixture::SetUp();
        core::Logger::SetMinLevel(core::LogLevel::DEBUG);
        core::Logger::AddSink(&sink);
    }
    void TearDown() override {
        core::Logger::RemoveSink(&sink);
        /// @note EngineFixture が最小ログレベルも復帰する。
        EngineFixture::TearDown();
    }
    testkit::RecordingLogSink sink;
};
```

例外は、**型パラメータ化テスト** (`TYPED_TEST`) と **値パラメータ化テスト** (`TEST_P`)。
アロケーター 4 種のように同じ契約を複数の型に課す場合は `TYPED_TEST` を積極的に使う。

### テスト対象の境界と公開 API

**テストは製品の契約を検証する。テストを成立させるためだけに製品の公開 API を増やさない。**
公開ヘッダーのメンバー、SDK、ScriptProxy、Editor の操作・バスも同じ基準で扱う。
テストが無くても利用者や本番の依存先に必要な操作・問い合わせだけを公開する。

`TryGet` という名前や「現在の呼び出し元がテストだけ」という事実だけで違反とは決めない。
失敗を安全に返す問い合わせが製品の契約として必要なら正当な API である。
内部コンテナ・途中状態を観測するためだけの getter、内部フラグを作るためだけの setter は追加しない。
診断 API も、エディターや運用など独立した利用目的と契約があるものを使う。

#### 検証方法を選ぶ順序

1. **守る契約を先に書く。** 入力、結果、失敗時の状態、必要な順序・所有権・座標系を決める。
   private のメンバー名やコンテナ構造を期待値にしない。
2. **本番と同じ操作で前提を作り、対象を実行する。** コンストラクター、通常の設定、登録、
   読み込み、Command などを使う。対象の private 状態を書いて実行経路を省略しない。
3. **既存の境界で結果を観測する。** 戻り値、利用者向け問い合わせ、保存結果、イベント、
   依存先へ渡したデータなど、守る契約に対応するものを比較する。
4. **外部依存をテスト側で制御する。** 既存の `I` インターフェースへ fake / mock を渡す。
   fake は入力の制御と出力の記録を担い、検証したい本体の処理を代行しない。
5. **独立した内部ロジックに契約があるなら、その責務を分離する。** 本番が使う同じ実装を
   内部部品として試験し、元の型にも利用者から結果までを通すテストを残す。
   既存の設計と依存方向に従い、設計が無ければ抽出案を提示してから実装する。

| 検証したいこと | 使う境界 | 追加しない入口 |
|---|---|---|
| 登録・検索の結果 | 通常の登録と利用者向け検索 | 内部 map を返す getter |
| コマンドの適用・拒否 | Command / バスの応答と対象の結果 | dispatcher の内部表を読む getter |
| 失敗時の復帰 | 不正入力、fake が返す失敗、処理後の結果 | 本体の失敗フラグを直接立てる setter |
| 非同期処理の公開・破棄 | 依存側の完了を制御し、通常の進行 API と出力を観測 | private の段階や完了フラグの書き換え |
| 保存・読み込み | `TempDir` 内の保存結果と読み込み後の契約 | シリアライザーの作業バッファの公開 |
| メモリ・リソースの退役 | 既存の診断出力や依存先への解放通知 | テストだけが使う追加の内部件数 getter |

fixture は前提の組み立てと後始末、TestKit は共通 helper・記録 fake を持つ。
テスト側のコードから製品へ依存させ、製品から TestKit / GoogleTest / GoogleMock へ依存させない。
実装と同じ計算をテストへコピーせず、仕様に基づく既知の値・解析解・不変条件を期待値にする。
成功だけでなく、境界値・失敗・再試行など契約に必要な条件を選ぶ。

#### 内部部品と private 検証の例外

公開 API 経由では対象の契約を十分に切り分けられない場合、まず内部部品への責務分離を検討する。
内部宣言は各プロジェクトの `src/` 側に置き、テスト target に必要な include path だけを
`PRIVATE` で与える。SDK、公開 include path、ScriptProxy に追加しない。
内部部品の `public` はその部品の契約であり、利用者向けクラスの内部状態を公開する代替ではない。
本番と異なる実装のコピーをビルドして試験しない。

分離が不適切で、外部結果から確認できない重要な不変条件が残る場合だけ、
**限定した friend fixture / TestAccess** を例外として使える。
検証する不変条件、通常の境界で確認できない理由、アクセス範囲を宣言側の `/// @note` に残す。
helper の定義はテスト側へ置き、アクセスは必要な読み取り・操作に絞る。
private の全体を返す万能 accessor、内部状態の捏造、実行すべき本番処理の省略に使わない。
friend へ付け替えるだけで既存テストを移行したことにしない。

次の手段は使わない。

- テスト目的の `public` / `protected` 化、`TryGet...` / `Get...ForTesting` / `Set...ForTesting` の追加。
- テスト目的での具象クラスの virtual 化や、不要な差し替えインターフェースの追加。
- `#define private public` / `#define protected public`、対象の `.cpp` の直接 include。
- `#ifdef TEST` 等で製品クラスの宣言・配置や対象の挙動を変える別実装。
- friend や内部ヘッダーで得た途中状態を、利用者から見える結果の検証の代わりにすること。

#### 既存テストを見直すとき

宣言と全呼び出し元を追い、製品上の必要性とテストの観測目的を別々に確認する。
必要な契約を保つテストへ書き換えてから、不要な公開入口を削除・内部化する。
アサーションを削る、失敗ケースを skip する、期待値を実装に合わせることで解決しない。

例えば `EditorBusDispatcher` の契約突き合わせテストは、テスト専用だった `RegisteredTypes()` を
削除し、既存の `Handle()` に `editor.bus.list` を渡した応答から一覧と重複を検証する。
`DeveloperMode` も `SetForcedByLaunch()` を削除し、実際の環境変数と `InitFromCommandLine()` から
起動条件を検証する。引数の字句規則は本番も使う `src/Core/DeveloperModeLaunch.hpp` の内部契約で守る。
`ManagedScriptEditor` の完了・Stop 検証は `src/ManagedScriptPublicationState.hpp` に分離し、
本番も同じ部品を使う。内部状態を捏造する friend を削除し、サービスとの接続も公開の診断・適用結果で守る。

記録 fake の既存例は `Projects/Tests/Engine/Auto/AudioStreamingTests.cpp` の `IAudioDevice` と、
`Projects/Tests/Core/Auto/Scheduler/SchedulerExecutionTests.cpp` の `ISystem`。
本体の内部を公開せず、既存の依存境界へ渡された操作を記録する。

参考: [GoogleTest: Testing Private Code](https://google.github.io/googletest/advanced.html#testing-private-code) は
公開インターフェースからの検証を基本とし、必要に応じた内部部品の分離・friend による検証を説明する。
ここでは friend を上記の条件に絞る。
[C++ Core Guidelines C.9](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#c9-minimize-exposure-of-members) の
公開範囲を最小にする方針にも従う。

---

## 5. テスト本体の構造

**Arrange / Act / Assert の 3 段を、空行で区切って必ずこの順に書く。**
区切りをコメントで繰り返さない。補足が必要なら、コードから読めない理由を `/// @note` で書く。

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
/// @note 成分ごとの差分と許容誤差が失敗ログに出る。
EXPECT_VEC3_NEAR(actual, math::Vector3(0.0f, 1.0f, 0.0f), testkit::kTolerance);
/// @note q と -q は同じ回転として比較する。
EXPECT_QUAT_NEAR(rotation, expected, testkit::kTolerance);

/// @warning 丸め誤差を許容しない。
EXPECT_EQ(v.x, 1.0f);
/// @warning y と z を検証できず、許容誤差の根拠も不明。
EXPECT_NEAR(v.x, 1.0f, 0.001f);
/// @warning 失敗時に比較した値が出ない。
EXPECT_TRUE(a == b);
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

    EXPECT_CALL(sink, OnLog(testing::_)).Times(0);

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
/// @note 合計値はタスクの実行順に依存しない。
TEST_F(TaskSystemTest, ExecutesEverySubmittedTaskExactlyOnce)
{
    std::atomic<int> sum{0};
    for (int i = 1; i <= 100; ++i) system.Submit([&sum, i] { sum += i; });

    system.WaitForAll();

    EXPECT_EQ(sum.load(), 5050);
}

/// @warning sleep の長さで完了を推測すると実行環境の負荷に依存する。
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
| Tests: Build & Run Graphics (Debug) | 実 DX12 描画・読戻しスイートを明示ビルド → 実行 |
| Tests: Build & Run RayTracing (Debug) | RayTracing の GPU / Engine スイートをビルドし、順に実行 |
| CMake: Build Tests (Debug) | CPU スイート + Manual + ベンチをビルド。実 GPU の Graphics / RayTracing は含まない |
| Tests: Run (Debug) | CTest の CPU スイート全件。GPU / Manual / Bench は含まない |
| Tests: Coverage (Debug) | 行 (C0) カバレッジを計測して HTML / バッジ / lcov を出す |
| Coverage: Branch + MC/DC (clang-cl) | 分岐 (C1) / 条件・MC/DC (C2) カバレッジを計測する |
| Bench: Build & Run (Debug) | ビジュアル検証ベンチ |

Graphics / RayTracing には `Tests: Build ... (Debug)` と `Tests: Run ... (Debug)` もある。
Run は既存の exe を実行するため、変更後は Build & Run を選ぶ。
Suite の選択肢から `RayTracing GPU` / `RayTracing Engine` を選ぶと、片方だけ実行できる。
スイートを分離した既存ツリーでは `CMake: Build Tests` を一度実行する。
CTest のケース一覧は exe のリンク後に更新されるため、configure や compile check だけでは旧一覧が残る。

デバッガーを付けたいときは `F5`（実行とデバッグ）から
`Tests: Math Auto (Debug)` などを選ぶ。`--gtest_filter` を対話で聞かれる。

> **既定スイートを追加したら `.vscode/tasks.json` の `CMake: Build Tests (*)` へ
> `--target` を 1 行足すこと。** 書き漏らしたスイートは「ビルドされない」だけで、
> エラーも警告も出ない。実際に Math とベンチが丸ごと抜けたことがある。
> `Coverage: Build (clang-cl)` も同じ理由で `--target` の列挙を持っている。
> 明示実行スイートは通常ビルド / CI / 計測の列挙へ足さず、Suite の選択肢と専用タスクへ足す。

### Visual Studio

**テスト exe をスタートアッププロジェクトにして `F5` / `Ctrl+F5`。**
ソリューションエクスプローラーで `Tests` フォルダの下から選び、右クリック →
`スタートアップ プロジェクトに設定`。

| プロジェクト | 対象 |
|---|---|
| `FBZZTestsMathAuto` | Vector / Quaternion / Matrix |
| `FBZZTestsPhysicsAuto` | GJK / EPA / Collider / AABB / BVH / XPBD |
| `FBZZTestsCoreAuto` | アロケーター / Scheduler / TaskSystem / Signal / Logger / Time |
| `FBZZTestsEngineAuto` | シーン / アセット / RagdollRig / CPU 抽出・設定 |
| `FBZZTestsGraphicsAuto` | CPU 描画計画 / 履歴 / キャッシュ |
| `FBZZTestsGraphicsStandalone` | 実 DX12 描画 / 読戻し（明示実行） |
| `FBZZTestsRayTracing` | RayTracing GPU（明示的にビルドして起動） |
| `FBZZTestsRayTracingEngine` | GPU skinning / HDR 抽出（明示的にビルドして起動） |

結果はコンソールに出る。**終了時に自動で閉じない** — 最後に「すべて成功 / 失敗あり」を
出して入力待ちで止まるので、緑と赤をそのまま読める。

> CMake が生成した `.sln` を開いている場合、テストエクスプローラーには CTest のケースが
> 並ばない（VS がフォルダを CMake プロジェクトとして開いたときだけ連携する）。
> `.sln` 運用では上の「exe を直接起動」が普段の経路になる。

文字が小さい / 大きいときは環境変数 `FBZZ_CONSOLE_FONT_SIZE` で上書きできる
（96dpi 基準の px。既定 17、画面の DPI に合わせて自動で拡大する）。

### CTest（CI / 既定スイートの確認）

```bash
ctest --test-dir build/Debug --output-on-failure -C Debug
ctest --test-dir build/Debug -C Debug -L '^Graphics$' --output-on-failure --no-tests=error
ctest --test-dir build/Debug -C Debug -L '^Physics$' --output-on-failure --no-tests=error
ctest --test-dir build/Debug -C Debug -L '^Managed$' --output-on-failure --no-tests=error
```

`Managed` ラベルの `FBZZManagedTests` は、ビルド済みの Host・generator 検査 runner を一つの CTest ケースとして実行する。EngineAuto / EditorAuto または `FBZZManagedTests` target のビルドで runner と fixture を用意する。Debug / Development は managed Debug、Release は managed Release を使い、ビルドの作業ディレクトリを `Projects/Managed` に固定して同じ `global.json` を適用する。

`Tools/AgentBuild.ps1 test -Filter '^FBZZManagedTests$' -Preset debug` はこの CTest ケースだけを実行する。`Tools/AgentBuild.ps1 managed-test -Preset debug` は従来どおり managed runner をビルドして実行する。CTest の名前フィルターは runner 内の個々の検査 group を分割しない。

`-L` で領域を選ぶ。ラベルは `Math` / `Core` / `Physics` / `Fluid` / `Engine` /
`Editor` / `Graphics` / `Managed`。`-R` は領域ではなくテスト名を絞る。
GraphicsStandalone と RayTracing の 2 スイートは未登録なので、`-L` / `-R` では実行できない。

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

- **既定スイートの上限を延ばして通そうとしない。** 30 秒に届くテストは、書き方かコードを見直す。
  CPU の契約テストは 1 件あたり数 ms で終わる前提で設計する（`sleep` 禁止・固定刻み）
- **ダイアログ抑止は自動実行のときだけ。** 対話実行（Visual Studio / ターミナル）では
  そのまま出す。人が居るなら、デバッガーで止めてスタックを見られる方が有益
- CI と `Tests: Run (Debug)` には `--no-tests=error` も付けてある。フィルタの打ち間違いで
  0 件になったとき、「全部成功」に見えてしまうのを防ぐ

### 単体 exe（絞り込み・調査）

```bash
FBZZTestsPhysicsAuto.exe --gtest_filter=GJKTest.*
FBZZTestsPhysicsAuto.exe --gtest_repeat=100 --gtest_shuffle
FBZZTestsPhysicsAuto.exe --gtest_break_on_failure
```

RayTracing は専用 Build タスクでビルドした後、
`build/Debug/Binaries/Debug/Tests/FBZZTestsRayTracing.exe` と
`FBZZTestsRayTracingEngine.exe` を直接実行する。`--gtest_filter` も使用できる。
この経路では CTest の 1 ケース 30 秒という制限は適用されない。

**`--gtest_shuffle` は定期的に回すこと。** 実行順に依存したテスト（`TearDown` の書き漏れ）は
これでしか見つからない。

### ビジュアル検証ベンチ

`FBZZTestBench.exe` を起動し、左のリストからシーンを選ぶ。判定は目で行う。

サイドバーの **「異常検知」** は、その場面が自分で申告した不変条件の破れを出す。
「検出なし」が緑で出ていれば、少なくとも **絵からは読み取れない破綻** (NaN・発散・貫通・
法線の裏返り・BVH の取りこぼし) は起きていない。赤が Error、橙が Warning。

各場面は `BenchScene::DetectAnomalies(AnomalyLog&)` を実装する。ここに書くのは
**設定を動かして初めて破れるもの** だけ ── 配置を固定して判定できる契約は Auto の担当で、
両方に書くと「どちらの結果が正しいのか」が分からなくなる。

> **常時点灯する警告を作らない。** 上限で切り捨てた点が包みの外に残る、といった
> 「仕様どおりだが不変条件は破れる」状態は Warning に落とすか、そもそも報告しない。
> いつも赤い画面は、誰も読まない画面と同じ。

1 フレームだけ出て消えた異常は、リストではなく「この場面で N フレーム検出 / 最初: …」の
履歴にしか残らない。放置して回している間に一度だけ出た NaN はここで拾う。

---

## 11. 新しいテストを追加する手順

1. 「テスト対象の境界と公開 API」で守る契約と観測方法を決め、「3 つの層」のフローチャートで **Auto / Manual / Bench** を決める。Auto は領域と既定 / 明示実行も決める
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
- [ ] テストだけのために製品の公開 API・virtual・差し替え口を増やしていないか
- [ ] 本番の操作を通し、戻り値・保存結果・イベント・依存先への出力で契約を検証しているか
- [ ] 内部部品・friend が必要なら、その理由とアクセス範囲が明確か
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

---

## 13. カバレッジ

率を上げるためではなく、**「自分が通したつもりの経路を本当に通したか」を確かめるため**に測る。
率そのものを目標にすると、`ReturnsNullptrWhenExhausted` のような契約テストより
「行を踏むだけのテスト」を書いた方が数字が伸びてしまう。

### 2 系統

| | 網羅基準 | 何が分かるか | ツールチェーン |
|---|---|---|---|
| **C0** | 命令網羅 | その行を 1 度でも実行したか | MSVC + OpenCppCoverage |
| **C1** | 分岐網羅 | `if` の真と偽を両方通ったか | clang-cl + llvm-cov |
| **C2** | 条件網羅 | `a && b` の `a` と `b` を個別に両方通ったか | 同上 |
| **MC/DC** | 改良条件判定網羅 | 各条件が**単独で**結果を変える組み合わせを通ったか | 同上 |

**普段は C0 だけを回す。** C1 以降は clang-cl での再ビルドが要るので、
分岐の入り組んだコード（`GJK` の縮退処理、`XPBDSolver` の収束判定、`Scheduler` の依存解決）
を触った後にだけ回す。

> C1 と C2 が同じ列に入るのは、clang が `&&` / `||` の**項ごと**に分岐リージョンを作るため。
> llvm-cov の `branch` は判定単位ではなく条件単位まで数えており、C1 を測ると C2 も付いてくる。

### 実行

```powershell
# @pre C0 は winget install OpenCppCoverage.OpenCppCoverage で計測ツールを入れる。
# @pre HTML / バッジ / lcov は dotnet tool install -g dotnet-reportgenerator-globaltool も要る。
.\Tools\Coverage\RunCoverage.ps1

# @pre C1 / C2 は VS Code タスク "Coverage: Build (clang-cl)" でビルドする。
.\Tools\Coverage\RunCoverageLLVM.ps1
```

`Tools/` の分担は次のとおり。

| スクリプト | 役割 |
|---|---|
| `RunCoverage.ps1` | OpenCppCoverage を回す入口。以下 3 つを順に呼ぶ |
| `NormalizeCoverageXml.ps1` | Cobertura のパスをリポジトリ相対へ直す（後述） |
| `CoverageReport.ps1` | `summary.md`。**CI のしきい値判定が読む正の数字** |
| `CoverageHtmlReport.ps1` | ReportGenerator で HTML / 履歴 / バッジ / lcov |
| `RunCoverageLLVM.ps1` | clang-cl 版の入口。実行 → profdata マージ → HTML / lcov / JSON |
| `CoverageReportLLVM.ps1` | llvm-cov の JSON から分岐・MC/DC の要約 |

### 計測対象を広げない

分母は `Projects/Math` / `Projects/Physics` / `Projects/Core` / `Projects/Engine/src/Core` の 4 つだけ。
「12. 何をテストするか」で**書かないと決めた領域を分母に入れると数字が意味を失う**ため、
Renderer / Editor / Tests / ThirdParty は両系統とも除外している。
`Projects/Engine/src/Core/Platform` (Window / Cursor / Application / EngineRebuildBootstrap) も
OS のウィンドウと起動の層でテストから生成しないため、Core の中から除外している。

**「基盤」は 2 つのディレクトリに分かれている。** Logger とアロケーターは `Projects/Core`
(`FBZZCore.dll`)、Time / Scheduler / TaskSystem / CrashHandler は `Projects/Engine/src/Core`
(`FBZZEngine.dll` の中の `FBZZEnginePlatform`)。**片方だけ書くと、もう片方が丸ごと
分母から消えて率が跳ねる。** 実際、`FBZZCore` を切り出したとき `--sources` 側だけが
`Projects/Engine/src/Core` のまま取り残され、Logger と全アロケーターが計測されなくなった。

対象を変えるときは 5 箇所を同時に直すこと。ずれると C0 と C1 が別の母集団の比較になる。

- `Tools/Coverage/RunCoverage.ps1` の `--sources`
- `Tools/Coverage/RunCoverageLLVM.ps1` の `$sourceFilters`
- `CMakeLists.txt` 末尾の `fbzz_instrument_for_coverage()` と `fbzz_link_coverage_runtime()`
  (計装するのはソースを持つターゲット、ランタイムをリンクするのは **それを実際にリンクする DLL**。
  `FBZZEnginePlatform` は OBJECT なので前者だけ、`FBZZCore` は SHARED なので両方に要る)
- `.github/workflows/tests.yml` の `--sources`
- `.github/workflows/tests.yml` «Decide scope» の `$pattern`
  (ここに無いディレクトリだけを触った PR は、そもそも計測されない)

### CI のカバレッジはテスト exe を直接走らせる

CI の «Measure coverage» は、スイートごとに exe を 1 回ずつ計測して binary で出し、
最後に `--input_coverage` で 1 本のレポートへ統合する。**計測対象の既定スイートを足したら
同ステップの `$suites` にも足すこと。**忘れるとそのスイートが踏んだ行が「未到達」に化ける。
RayTracing / RayTracingEngine はこの列挙にも含めない。

> ctest を通すと `gtest_discover_tests` が登録した **1 テスト 1 プロセス**(2148 本)すべてで
> OpenCppCoverage が PDB を読み直す。テスト本体は全スイート合わせて 25 秒なのに、
> プロセス起動だけで 20 分(開発機)・47 分超(ランナー)かかり、ジョブの上限で落ちる。

1 プロセスにまとめると、テスト間で持ち越した不正なメモリ操作が表に出る。
デバッガー(OpenCppCoverage)の下では Windows のデバッグヒープが有効になり、
解放済みメモリへの書き込みが `int 3`(終了コード `0xC0000005` / `0x80000003`)になるため、
**素の ctest では通るのに計測パスだけが落ちる**という形で現れる。

### 数字を見る前にレポートを見る

率は「今どこに居るか」しか言わない。次に何を書くかを決められるのはこちら。

| 見るもの | 場所 | 何が分かるか |
|---|---|---|
| **履歴グラフ** | `Artifacts/Coverage/report/index.html`（`history/` に蓄積） | コミットを跨いだ推移。下がった回が特定できる |
| **未到達の分岐 上位 10** | `Artifacts/CoverageLLVM/summary.md` | 1 本のテストで最も多くの分岐を潰せるファイル |
| **エディターの色** | Coverage Gutters（後述） | 読んでいるコードのどこを通っていないか |

> ReportGenerator の **Risk Hotspots**（循環的複雑度 × 低カバレッジ）は**出ない**。
> 複雑度を読めるのは dotCover / OpenCover 形式のときだけで、OpenCppCoverage の Cobertura は
> `complexity="0"` しか書かない（llvm-cov の lcov も同じ）。
> 「次にどこを触るか」を出すのは `CoverageReportLLVM.ps1` の未到達分岐ランキングの方。

### VS Code のエディター上で見る

`ryanluker.vscode-coverage-gutters`（`.vscode/extensions.json` で推奨済み）を入れて、
`Ctrl+Shift+P` → `Coverage Gutters: Watch`。行番号の横に到達 / 未到達が色で出る。

`.vscode/settings.json` は `.gitignore` 対象なので、各自で次を入れる。

```jsonc
{
    // @note 既定値の ** は node_modules 内も拾うので Artifacts/ に限定する。
    "coverage-gutters.coverageBaseDir": "Artifacts/**",
    "coverage-gutters.coverageFileNames": ["lcov.info", "coverage.xml"],
    "coverage-gutters.showGutterCoverage": true,
    "coverage-gutters.showLineCoverage": false,
    "coverage-gutters.showRulerCoverage": true
}
```

> **`NormalizeCoverageXml.ps1` を外さないこと。** OpenCppCoverage は Cobertura の
> `<source>` にドライブ文字だけ (`C:`) を置き、`filename` を「ドライブを外した絶対パス」
> (`Users\<名前>\...`) にする。計測した開発機でしか解けない形なので、
> これを直さないと Coverage Gutters も ReportGenerator もソースを開けず、
> **色が付かない / 行が空欄の HTML になる**。CI（ランナーのユーザー名は `runneradmin`）
> との突き合わせも同じ理由で壊れる。
