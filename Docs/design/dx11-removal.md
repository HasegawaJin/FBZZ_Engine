# DirectX 11 バックエンドの撤去 (v1.0)

- 状態: 実装済み (v1.0)

**DirectX 11 サポートは v1.0 で終了した。** `v0.9` が DX11 を含む最後のリリースになる。

このエンジンは長く DX11 / DX12 の 2 バックエンドを `IRenderer` 抽象の裏に同居させてきた。
ここでは **なぜ片方を捨てたか**、**何を得たか**、**何を意図的に残したか** を記録する。

---

## なぜ捨てたか

### 1. すでに劣化パスになっていた

撤去直前の時点で、DX11 は `IRenderer` の 50 個の仮想関数のうち **45 個しか実装していなかった**。
欠けていたのは後から足した機能ばかりで、DX11 で起動すると次が黙って無効になる。

| 欠けていたもの | DX11 で起きること |
|---|---|
| `InitializeGpuProfiler` | GPU プロファイラがパス時間を取れない |
| `PrepareShaderReload` | シェーダーホットリロードが効かない |
| `BeginComputeBatch` / `EndComputeBatch` | コンピュートのバッチ記録が無い |
| `CreateNativeGpuLocalStructuredBuffer` | GPU ローカルバッファが通常 SRV へ縮退する |

「動く 2 つ目のバックエンド」ではなく「**機能が欠けた古い経路**」だった。
残しておくと、そうと知らずに選んだ利用者が原因不明の不調に当たる。

### 2. SM 5.0 がエンジン全体の床になっていた

DX11 は FXC / `D3DCompileFromFile` で SM 5.0 の DXBC を焼いていた。シェーダーは
バックエンド間で単一ソースなので、**DX11 が生きている限り SM 6.x 専用の機能は
エンジンの前提にできない**。具体的に閉じていたもの:

- Wave intrinsics (`WAVE_INTRINSICS_SUPPORTED` は DX11 側で 0 固定だった)
- SM 6.6 の `ResourceDescriptorHeap` = bindless
- CS の UAV スロット。SM 5.0 は `u0`〜`u7` の 8 本しか無く、`ComputeCall::uavOutputs` の
  枠数と `Binding.hlsli` の時分割はこの制約から来ている (DX12 は 64 本張れる)

撤去の対価はここにある。**DX11 を消すこと自体に価値は無く、SM 6.x を前提に降ろして初めて元が取れる。**
最初の回収先は bindless (`Docs/design/bindless.md`)。

### 3. 新機能を 2 回書くコストだけが残っていた

DX12 が既定になった後も、レンダラーの契約を変えるたびに DX11 側を書き足していた。
コミット履歴上、ライト Cookie・面光源・演出シーケンス・レンダーパスビューアなど
**DX12 のための変更が毎回 DX11 も巻き込んでいた**。その割に DX11 で起動する利用者はいない。

### 4. CI もテストも通っていなかった

DX11 経路を踏むのは `Projects/Tests/Bench` だけで、しかも「シェーダーを 1 本も使わないから
DXC を要求しない DX11 を選ぶ」という消極的な理由だった。CI のジョブは 1 つも DX11 を検証していない。

---

## 何を得たか

- **SM 6.x が前提になった。** `Platform/Backend.hlsli` は `DX12.hlsli` だけを含み、
  `WAVE_INTRINSICS_SUPPORTED` / `RAY_QUERY_SUPPORTED` は常に 1
- **DXC が全構成で必須**になり、「DXC のある構成と無い構成」の分岐が消えた
  (`BuildSettingsPanel` の Runtime DLL チェックが条件分岐を持たなくなった)
- コンパイル対象から `compiled/` (DXBC) が消え、出力は `compiled_dx12/` (DXIL) だけになった

---

## 何を残したか

**抽象境界は 1 つも壊していない。** 撤去は上位レイヤーに一切触れずに済んでいる。これが
「`IRenderer` 境界が実際に機能していた」ことの証拠なので、境界そのものは次のバックエンドの
ために形を保って残す。

| 残したもの | 理由 |
|---|---|
| `IRenderer` / `IImGuiRenderer` / `IIblBaker` 等の抽象 | 上位レイヤーは今も具象名を 1 つも知らない |
| `RendererFactory` / `BackendEntry` | 合成ルート (`Application`) から具象生成を隠す窓口 |
| `RendererBackend` enum (要素 1 つ) | 次のバックエンドを足すときに上位層の形を変えないため |
| `fbzz_engine_module` の `PRIVATE_INCLUDES` 隔離 | `d3d12.h` は `FBZZRenderDX12` からしか見えない。ダウンキャストが **コンパイルエラー** で止まる規約 (AGENTS.md) |
| `Platform/Backend.hlsli` の間接層 | シェーダー側はバックエンドを知らないまま |

要素 1 つの enum は一見冗長だが、ここを潰すと `ProjectSettings` から `Application` までが
「バックエンドという概念」を失う。撤去の目的は概念を消すことではなく、**動かない実装を消すこと**。

---

## 終了済み設定の扱い

既存プロジェクトの `renderer = "dx11"` や `--renderer=dx11` は **起動を止めない**。
DX12 へ倒した上で、倒したことを名指しで警告する。

```
[WARN] ProjectSettings: renderer="dx11" は v1.0 でサポートを終了しました。DirectX 12 で起動します
[WARN] Application: --renderer=dx11 は v1.0 でサポートを終了しました。DirectX 12 で起動します
```

黙って倒すと利用者は設定が効いているつもりのままになる。判定は
`renderer::IsRetiredBackendToken()` が持ち、未知トークンとは区別して扱う。

---

## 撤去した実体

| 対象 | 内容 |
|---|---|
| `Projects/Engine/src/Renderer/Platform/DX11/` | 21 ファイル / 約 4,450 行 |
| `Assets/Shaders/Platform/DX11.hlsli` | エンジン / GreenWare / GameHub テンプレート 2 種の計 4 コピー |
| `GreenWare/Assets/Shaders/compile_shaders.bat`<br>`GreenWare/Assets/Shaders/compile_ui_shaders.bat` | FXC 専用の旧スクリプト。`TestSharedSDKTemplates.cmake` が既に「legacy, must not exist」と宣言していた |
| `compile_shaders.ps1` の FXC 経路 | `-Backend` は `DX12` のみ。`fxc.exe` / `FBZZ_FXC` / `vs_5_0` 系プロファイルを削除 |
| `FBZZRenderDX11` モジュール | `Projects/Engine/CMakeLists.txt` |
| `CreateDX11Backend` | `BackendEntry.hpp` / `RendererFactory.cpp` |

### `FBZZ_ENABLE_DX12=OFF` の意味が変わった

このオプションは残してあるが、**DX11 撤去後は「描画バックエンドを 1 つも持たない構成」になった**。
GPU を使わないビルド (`coverage` プリセットの clang-cl カバレッジ計測) 専用と考えること。
この構成から SDK を公開すると、そこから作ったゲームは起動時に必ず落ちる。CMake が警告を出す。

---

## ソースを残さなかった理由

`Docs/design/` は「**なぜ前の方式を捨てたか**」を書く場所で (`Docs/README.md`)、捨てた実装の
置き場ではない。保守しない 4,450 行をツリーに残すと、読んだ人は「消し忘れ」か「消す判断が
できなかった」と読む。実装そのものは git が持っている。

- `v0.9` タグ — DX11 を含む最後のリリース
- 撤去コミットの diff — 上位レイヤーを 1 行も変えずに落ちていること自体が境界の証拠

---

## 次にやること

1. **bindless (SM 6.6)** — `ResourceDescriptorHeap`。DX11 撤去の対価を回収する本命 → `bindless.md`
2. `ComputeCall::uavOutputs` の 8 枠と `Binding.hlsli` の UAV 時分割を解く (SM 5.0 由来の制約)
3. `compiled_dx12/` を `compiled/` へ戻す (出力が 1 系統になったため。ランタイムの CSO 探索パスを伴う)
