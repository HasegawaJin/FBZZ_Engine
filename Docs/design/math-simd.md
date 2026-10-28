# Math の SIMD 化

Math の演算を SSE4.1 の intrinsics で書き直す。前段の «小関数のヘッダー化» (`Docs/benchmarks/2026-09-18-math-inline/`) で呼び出しの壁は取れたので、次は 1 演算あたりの命令数を減らす。

## 1. 決めたこと

| 項目 | 決定 | 理由 (§2) |
|------|------|-----------|
| メモリ上の形 | **変えない** (`Vector3` 12B / `Matrix4` 64B、整列指定なし) | 配列を走査するループではデータ量が効く。GPU・ABI・シリアライズへ波及しない |
| 命令セット | **SSE4.1 固定**。`/arch` は変えない。FMA は使わない | 2008 年以降の x64 はすべて対応。丸めの変わり方を小さく保つ |
| SIMD の型 | 関数の中だけで使う。公開 API の引数・戻り値に `__m128` を出さない | 保存用と演算用を分ける (DirectXMath の `XMFLOAT3` / `XMVECTOR` と同じ分け方) |
| スカラー版 | 製品コードには残さない。テスト側に参照実装として置き、SIMD 版と突き合わせる | 製品に 2 経路を持つと片方が腐る |
| 置き場所 | 行列積・`M * v` など物体ごとに呼ぶ小関数はヘッダー、`Inverse` など重いものは `.cpp` | DLL 境界を越えるとインライン化されない (前段と同じ規則) |

## 2. 形を変えない理由

| | 形は変えない (12B) | 16B 整列にする |
|---|---|---|
| 1 個ずつの演算 | ロードが 2〜3 命令多い (`movsd` + `insertps`) | ロード 1 命令 |
| 配列の走査 (頂点・粒子・剛体・経路) | そのまま | データが 33% 増え、帯域で決まるループは遅くなる |
| GPU 転送 | `float3` のまま | 毎回詰め直しが要る |
| 波及先 | なし | 頂点構造体・cbuffer・スクリプト DLL の ABI・シリアライズ |

- 本当に速くしたいループは、`Vector3` の形と関係なく SoA (x[], y[], z[] を 4 要素ずつ) の一括 API で処理する (§4 段 3b)
- `Matrix4` の非整列ロード (`_mm_loadu_ps`) は、64B のキャッシュラインをまたがない限り整列ロードと同じ速さ。MSVC の x64 ヒープは 16B 境界で返すのでまたぎはまれ。計測で問題が出たら `Matrix4` だけ `alignas(16)` にする
- Unreal (`FVector` と `VectorRegister`)・DirectXMath・Unity (Burst の SoA) も同じ分け方をしている

## 3. 内部の道具 (`Math/Simd.hpp`)

- 名前空間 `fbzz::math::simd`。`<smmintrin.h>` を include するのはこのヘッダーだけ
- 中身は薄い関数だけ: `Load3` / `Load4` / `Store3` / `Store4`・`Splat`・`Transpose4x4`・`Dot3` / `Dot4`・`MulAdd` (FMA ではなく mul + add)
- 公開ヘッダー (`Matrix4.hpp` 等) から include されるので、前処理行数を `Docs/conventions/build-performance.md` の手順で前後比較する。大きく増えるなら行列積も `.cpp` へ移し、呼び出しの壁と比べ直す

## 4. 段取り

各段を 1 コミットにし、`Tools/BenchCompare.ps1` で直前の段と比べる。«アルゴリズムの改善» と «SIMD の効果» を同じ段に混ぜない。

| 段 | 内容 | 計測 |
|----|------|------|
| 0 | micro に `Matrix4 * Vector4`・`Frustum::IntersectsSphere` を足す。スカラー参照実装 (`Projects/Tests/Math/Reference/`) と突き合わせのテスト (`ScalarParityTests`) を置く | 以降の基準 |
| 1 | `TRS` を行列 3 つの積ではなく、回転行列の列に scale を掛けて平行移動を入れる形で直接組む (スカラーのまま) | `Matrix4::TRS` |
| 2 | `Matrix4` の積・`M * v`・`Transpose`・`Inverse` を SIMD 化 | 行列の 4 項目 + 物理の場面 |
| 3a | `Frustum` の 6 平面を SoA (4 レーン × 2 本、余りの 2 レーンは判定のビットマスクで捨てる。値に頼らないので NaN・無限大の入力でも余りが結果を変えない) で持ち、`IntersectsSphere` / `IntersectsAABB` を SIMD 化。API は変えない | 錐台の 2 項目 |
| 3b | 一括 API `Frustum::IntersectsSpheres(std::span<const Vector4> spheres, std::span<uint8_t> visible)` を足す (xyz = 中心・w = 半径、4 個ずつ転置して 6 平面と比べる)。micro に `Frustum::IntersectsSpheres` を足し、1 球あたりで `IntersectsSphere` と比べる | `Frustum::IntersectsSpheres` |
| 4 | 描画側で境界球を 1 ビューにつき 1 回集め、3b で判定した結果を各パスが引く形へ直す | エディターの Profiler (TestBench では測れない) |

- micro 計測は結果の全成分を出力配列へ書かせる (`56214bc6`)。以前は 2 成分だけを足し込んでいたため、インライン化される演算は残りの成分の計算が最適化で消え、スカラー版が実際より速く測れていた (`Matrix4 * Vector4` 0.95 ns に見えて実際は 1.64 ns)。段 2 以降の比較はこの修正の後を基準にする
- 段 4 は描画の構造を変えるので、段 3 までの数字を見てから別の設計文書で決める → [view-culling.md](view-culling.md)。測った結果、判定の合計はビューあたり最大 0.021 ms で基準 (0.2 ms) に届かず、見送った。今のカリングは物体ループの中で `GetComponent` と境界の計算に混ざって呼ばれており、平面判定そのものが支配的かは測っていない
- `Frustum` は SoA を持つぶん大きくなるが、スクリプトにもシリアライズにも出ていない (Engine 内部だけ)。`planes` を直接書いているのは `FromViewProjection` だけなので、書き込みは生成関数に閉じ、読みは `GetPlane(i)` にする

## 5. 正しさの確かめ方

- 参照実装: SIMD 化前のスカラー実装の写しを `Projects/Tests/Math/Reference/ScalarReference.hpp` に置き、`ScalarParityTests` が決定的な乱数の入力 (アフィン・一般の行列・負のスケール・特異行列) で SIMD 版と突き合わせる
- 許容誤差は相対値: 積は 1e-5、逆行列は 1e-4 (成分の最大の絶対値で割る)。カリングは真偽の一致を見るが、境界から 1mm 以内の入力は丸めで反転しうるので外す
- 既存の `Projects/Tests/Math/` は変更なしで通ること
- 契約 (`FBZZ_MATH_CONTRACT` で報告して単位行列を返す等) は SIMD 版でも同じ条件・同じ戻り値にする
- 丸めが変わるので、段 2 の後に Playtest (`--batch`) を回し、基準画像のずれが許容内か確かめる。流体の決定論は «同じバイナリの中» の約束なので影響しない (`fluid-determinism.md` §3)

### 結果 (2026-09-19)

- 積・M*v・転置・TRS は `ScalarParityTests` を許容誤差 0 にしても通る (ビット一致)。**値が変わるのは `Inverse` だけ**
- `Inverse` だけをスカラーに戻した `FBZZMath.dll` と SIMD 版を差し替え (ABI は同じ)、同じ場面を `--batch` で撮り比べた。基準はスカラー版で撮り、スカラー版をもう 1 回撮って実行ごとの揺れも測った

| 場面 | 揺れ (スカラー同士) の meanDiff | SIMD の meanDiff | 閾値を超えた画素 |
|------|---:|---:|---:|
| Title (90f) | 0 | 0 | 0 |
| Stage_01 (Play 120f) | 3.9e-6 | 5.9e-6 | 0.0013% (揺れと同程度) |
| Stage_02 (Play 120f) | 2.8e-6 | 2.2e-5 | 0 |
| Stage_03 (Play 120f) | 2.4e-9 | 1.5e-4 | 0 |

- どれも既定の閾値 (meanDiff 0.01・画素の差 0.1) の 1/60 以下で、差が 0.1 を超える画素は無い。Stage_03 は揺れより大きいが、絵を並べて見ても区別できない
- 既存の Playtest の基準画像は 12 枚中 6 枚が未作成 (`Title_90f`・`Fiber_30f`・`FiberLifecycle` の `Original` / `Shell` / `MissingMaterial` / `Restored`) で、そのままでは比較に使えなかったため、この手順で測った

## 6. やらないこと (今は)

- `/arch:AVX2` と FMA: 丸めがビルド全体で変わる。配布先の CPU の下限も上がる
- 実行時の命令セット切り替え: 関数ポインタ越しになり、1 回が小さい演算では遅くなる
- `Vector3` / `Quaternion` の 1 個ずつの演算の SIMD 化: ロード・ストアの往復で伸びない。`Normalized` は sqrt の待ち時間が支配的
- ARM (NEON): Windows x64 だけが対象

## 参考

- Intel Intrinsics Guide: https://www.intel.com/content/www/us/en/docs/intrinsics-guide/index.html
- DirectXMath «Code Optimization with the DirectXMath Library»: https://learn.microsoft.com/windows/win32/dxmath/pg-xnamath-optimizing
- Fabian Giesen «View frustum culling»: https://fgiesen.wordpress.com/2010/10/17/view-frustum-culling/
- 計測と比較の手順: [benchmark-report.md](benchmark-report.md)

## C# SDK の SIMD 化と接続側の改善（2026-10-20）

C# の SIMD 化は必須とし、`Fbzz.Vector3` と `Fbzz.Quaternion` の成分演算を `Vector128<float>` で行う。前節の C++ 単体演算を見送る方針とは実装・処理系が異なる。公開型は変更せず、Vector3 の 12B と Quaternion の 16B、フィールド順、保存・native ABI を維持する。Vector3 は各成分から安全に構築し、12B の領域を 16B 読み込まない。

`Dot` と長さの二乗は成分の積を並列に計算し、加算は従来の順序を保つ。近似逆数・近似平方根、FMA による融合、演算の再結合を導入しない。ゼロ長の fallback、EPSILON、非有限値、符号付きゼロ、補間の外挿・短弧選択、Euler の YXZ 順序を検証する。sqrt と三角関数は MathF のままとする。

実装根拠は Microsoft の [SIMD と hardware intrinsics](https://learn.microsoft.com/en-us/dotnet/standard/simd) と [.NET 10 Vector128](https://learn.microsoft.com/en-us/dotnet/api/system.runtime.intrinsics.vector128?view=net-10.0)。SIMD 命令の生成は JIT disassembly で確認し、通常環境と hardware intrinsics 無効時の両方で同じ数値・ABI テストを実行する。命令が生成されることと FPS 改善は別であり、速度向上率は同条件の実測なしに記載しない。

### 公式資料からの改善候補

以下は公式資料の知見を FBZZEngine のコードへ対応付けた提案であり、SIMD 化以外は本変更で実装しない。Unity の Mono / IL2CPP、REVM の独自 AOT・GC と FBZZ の CoreCLR を同じ処理系として扱わず、各資料の測定値を FBZZ の改善率へ転用しない。

| 優先 | 対策 | FBZZEngine への適用 | 公式資料 |
|---|---|---|---|
| 1 | 実装されていない callback を呼ばない | 世代別の callback 存在情報で native 側からの Update / FixedUpdate / LateUpdate の不要往復を省く。再読み込み・実行順・無効化・例外隔離の契約を維持する | Unity 6 [Use a custom update manager](https://docs.unity3d.com/6000.0/Documentation/Manual/events-per-frame-optimization.html), When to use |
| 2 | callback ごとの確保・boxing を減らす | `ScriptExecutionScope` と enum 検証を見直す。scope の再入・所有者・CallID・世代・終了後の失効を保ち、単なる共有オブジェクトの使い回しにはしない | Unity 6 [Reference type management](https://docs.unity3d.com/6000.0/Documentation/Manual/performance-reference-types.html), Avoid converting value types to reference types |
| 3 | 返却配列の生成より、呼出元の領域を埋める | QueryBuffer の ToArray、入力名 UTF-8 化、数値設定 packet の確保を頻度別に測り、既存 Span / stackalloc / 再利用領域を使う。全数値 property が確保するとはみなさない | Unity 6 [Optimizing arrays](https://docs.unity3d.com/6000.0/Documentation/Manual/performance-optimizing-arrays.html); RE:2023 [C#8.0／.NET対応](https://www.docswell.com/s/CAPCOM_RandD/5DEVXD-RE2023), p20–24 |
| 4 | 小さい関数より計算単位をまとめる | Transform の個別 read-modify-write を既存一括 API へ寄せる。Ocean AI の Step と姿勢転送は既に一括化済みなので新規実装扱いにしない | Unity Burst 1.8 [Function pointers](https://docs.unity3d.com/Packages/com.unity.burst@1.8/manual/csharp-function-pointers.html), Performance considerations |
| 5 | 連続データを収集し、個体をまたぐ SIMD を検討する | snapshot 後に残る LookRotation / Slerp などの演算回数と更新個体数を測定し、必要なら連続配列・chunk 処理へまとめる。Scene の個別変更は現在の安全な反映境界へ集約する | CAPCOM [CEDEC2018 グラフィックス最適化](https://www.docswell.com/s/CAPCOM_RandD/ZXYVJG-cedec2018), p5・7–13; Unity 6 [Job system overview](https://docs.unity3d.com/6000.0/Documentation/Manual/job-system-overview.html), Safety system |

RE:2023 の C# 資料（2023-11-27公開）は、当時の RE ENGINE のゲームロジックが C# で、REVM が開発時に MicroCode、製品時に C++ を出力する独自 AOT であることを説明する（p5–6）。その実装を CoreCLR へそのまま移植する方針にはせず、要素アクセスの inline 化、一時確保の削減、互換性を保った検証という観点を採る（p12・14・20–24）。

次の比較では Math 単体の処理量、callback の呼出し数、native 境界の往復数、確保 byte / 回数を分離し、その後に Release 実シーンの FPS・CPU 経過時間・p99 を確認する。既存の描画完了・30秒経過条件を守り、JIT 診断や hardware intrinsics 無効の検証結果は正式な実シーン性能値に混ぜない。

### 追加実装の契約と検証条件（2026-10-20）

上の候補表のうち、未実装 callback の省略、scope / packet の一時確保削減、個体をまたぐ回転 SIMD を追加実装した。以下は実装と検証の契約であり、性能測定結果ではない。

- **未実装 callback の省略**: 世代と型に対応する Host metadata が有効な場合だけ、override のない `Update` / `FixedUpdate` / `LateUpdate` の native→managed 往復を省く。継承された override は実装ありとして扱う。旧 Host、metadata 不明、所有者・世代の照合不成立、Host エラーでは保守的な従来経路へ戻す。他の lifecycle、実行順、障害隔離は変更しない。
- **scope**: `ScriptExecutionScope` は不変の値型とし、TLS ごとの単調増加 identity を追加する。identity は世代更新でも再使用せず、枯渇時は例外とする。bridge / component lookup / gizmo の参照、thread、世代、instance、owner、CallID、Scene lifetime、callback を含めて現在 scope と照合する。保持した古い値や同じ native ID で再構築した値は、新しい callback の権限にならない。入れ子・例外・native 呼出し中の一時解除は `finally` で元の nullable scope を復元する。
- **packet と query**: 小さい同期 UTF8 packet は 256B の stack 領域、大きい packet は呼出しごとの `ArrayPool<byte>` lease、数値だけの書込みは stack 上の wire bytes を使う。strict UTF8、型・範囲・status・owner / lifetime の検証は残す。読み取り query だけが従来の上限付き容量再試行を行い、借用 span の有効期間内に decode する。pool は必ず返却・clear し、入れ子呼出しで共有しない。返却文字列・GameObject・返却配列は所有結果として必要な確保を残し、初回 pool 拡張や利用者 callback まで無確保とはしない。enum 検証は generic `Enum.IsDefined` を使い boxing を避ける。
- **個体横断 SIMD**: `QuaternionBatch.LookRotation` / `Slerp` は呼出元の連続 span を 4 個体ずつ SoA に集める。末尾は従来の単体演算を使う。成分から明示的に gather し、Vector3 の 12B を越えて読まない。従来の演算順、EPSILON、平行 up の復帰、短弧・非 clamp 補間、呼出元が行っていた追加正規化を維持する。sqrt / 三角関数は lane ごとの `MathF` のまま。長さと重なりは書込み前に検証し、Slerp の完全同位置 in-place だけを許す。計算量は引き続き O(N) で、gather / scatter と三角関数の費用は残る。

有効な consumer は測定用 `ConsumerAfter` の `MarineNaturalEcosystemComponent`、`OceanAdditionalFaunaComponent`、`OceanBenthicFaunaComponent` の 3 種である。元の有効 slot の順で方向・開始姿勢を集め、従来の姿勢 commit 前に結果を戻す。AI の時刻、物理 query、速度による更新判定、Animator 更新、batch の失敗・rollback 境界は維持する。既存の AI Step / 姿勢転送 batch を今回の新規実装として数えない。元プロジェクトへの配布・適用は、測定用 clone の変更とは分けて扱う。

検証では単体・一括 SIMD を保存した scalar 式と比較し、有限値・無限大・符号付きゼロ、NaN の分類、Hamilton 積の符号、退化入力、補間の外挿、4 lane と端数、出力外の領域、overlap の失敗前無変更を確認する。符号反転・共役など bit 操作の契約は NaN payload も確認する。scope については保持値・入れ子・別 thread・世代更新、packet については大きい UTF8・再入・不正文字列・不正応答・失敗後の lease 回収を確認する。warmup 後の `GC.GetAllocatedBytesForCurrentThread` は隔離した fixture の一時確保の回帰であり、FPS や GC pause の測定には使わない。

正式比較は、保存した **Before（C# SIMD 前）／SimdOnly（単体 C# SIMD のみ）／After（上記追加実装と 3 consumer）** の 3 条件を分けて保持する。Release の `OceanEcosystem1km.scene` で、同じ入力・camera・描画設定・描画完了確認・30秒待機条件を使い、実 wall interval の passive 計測と native scope 計測を分ける。短い区間と長い区間を保持し、全 run を報告する。JIT 出力確認、intrinsics 無効テスト、追加診断は正式値へ混ぜない。

native scope は inclusive 時間として扱い、親 scope とその子、または重なった scope を加算して総 CPU 時間を作らない。callback 数・境界往復・確保量・実 wall FPS / p99 を別の指標として比較し、追加実装全体の差を単体 SIMD や一つの対策だけの因果効果と断定しない。性能数値は正式結果の検証後に別途記録する。
