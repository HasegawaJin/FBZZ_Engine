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
| 3a | `Frustum` の 6 平面を SoA (4 レーン × 2 本、余りの 2 レーンは必ず通る平面) で持ち、`IntersectsSphere` / `IntersectsAABB` を SIMD 化。API は変えない | 錐台の 2 項目 |
| 3b | 一括 API `Frustum::IntersectsSpheres(std::span<const Vector4> spheres, std::span<uint8_t> visible)` を足す (xyz = 中心・w = 半径、4 個ずつ転置して 6 平面と比べる)。micro に `Frustum::IntersectsSpheres` を足し、1 球あたりで `IntersectsSphere` と比べる | `Frustum::IntersectsSpheres` |
| 4 | 描画側で境界球を 1 ビューにつき 1 回集め、3b で判定した結果を各パスが引く形へ直す | エディターの Profiler (TestBench では測れない) |

- micro 計測は結果の全成分を出力配列へ書かせる (`56214bc6`)。以前は 2 成分だけを足し込んでいたため、インライン化される演算は残りの成分の計算が最適化で消え、スカラー版が実際より速く測れていた (`Matrix4 * Vector4` 0.95 ns に見えて実際は 1.64 ns)。段 2 以降の比較はこの修正の後を基準にする
- 段 4 は描画の構造を変えるので、段 3 までの数字を見てから別の設計文書で決める。今のカリングは物体ループの中で `GetComponent` と境界の計算に混ざって呼ばれており、平面判定そのものが支配的かは測っていない
- `Frustum` は SoA を持つぶん大きくなるが、スクリプトにもシリアライズにも出ていない (Engine 内部だけ)。`planes` を直接書いているのは `FromViewProjection` だけなので、書き込みは生成関数に閉じ、読みは `GetPlane(i)` にする

## 5. 正しさの確かめ方

- 参照実装: SIMD 化前のスカラー実装の写しを `Projects/Tests/Math/Reference/ScalarReference.hpp` に置き、`ScalarParityTests` が決定的な乱数の入力 (アフィン・一般の行列・負のスケール・特異行列) で SIMD 版と突き合わせる
- 許容誤差は相対値: 積は 1e-5、逆行列は 1e-4 (成分の最大の絶対値で割る)。カリングは真偽の一致を見るが、境界から 1mm 以内の入力は丸めで反転しうるので外す
- 既存の `Projects/Tests/Math/` は変更なしで通ること
- 契約 (`FBZZ_MATH_CONTRACT` で報告して単位行列を返す等) は SIMD 版でも同じ条件・同じ戻り値にする
- 丸めが変わるので、段 2 の後に Playtest (`--batch`) を回し、基準画像のずれが許容内か確かめる。流体の決定論は «同じバイナリの中» の約束なので影響しない (`fluid-determinism.md` §3)

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
