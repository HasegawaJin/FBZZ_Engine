# DirectX 12 移行設計ドキュメント (Step 6b)
## DX11 依存の抽象化仕上げと DX12 バックエンドの実装計画

---

## 0. このドキュメントの位置づけ

`feature/directx12` ブランチで進める Step 6b (DX12 バックエンド追加) の設計・実装追跡ドキュメント。
**現状の抽象化がどこまで完成しているか**を棚卸しし、残る DX11 依存の解消方針と DX12 実装のフェーズ計画・進捗を定める。

### 実装状況 (2026-07-14)

| Phase | 状態 | 残作業 |
|---|---|---|
| Phase 0 | ✅ 実装済み | Visual Studio でのDX11回帰確認 |
| Phase 1 | ✅ 実装済み | 実機でDebug Layer警告確認 |
| Phase 2 | ✅ 実装済み | DDS圧縮・mip保持の最適化 |
| Phase 3 | ✅ 主要経路実装済み | 全RenderPassのA/B画像検証 |
| Phase 4 | ✅ 実装済み | 実機 A/B (DX11/DX12) 画像比較を VS で確認 |
| Phase 5 | 🔵 DXC / SM 6.8 基盤実装済み | VS で全 DX12 シェーダー再生成、DXIL PSO 作成、DXIL reflection、A/B 描画を確認 |
| Phase 6 | 🟡 DXR 基盤着手 | capability 判定済み。BLAS/TLAS、RTPSO、Shader Table、RayQuery 利用パスは未実装 |

**結論の先出し:**

- `IRenderer` / `RendererFactory` / `Platform/Backend.hlsli` による抽象化は**既にほぼ完成している**。上位レイヤー (Editor / Scene / Asset / GameHub) に DX11 型は一切漏れておらず、`IRenderer` の公開 API を変更する必要は**ない**。
- 残る DX11 依存は (1) CMake の死にリンク指定、(2) `Application.cpp` のバックエンド固定、(3) DX12 側 `.hlsli` の未作成、の 3 点だけ。いずれも軽微。
- 本質的な作業は **DX12 バックエンドの新規実装**であり、その要石は次の 2 つ:
  1. **Submit 時キャプチャ** — `IRenderer` の即時実行風 API (`Update → Submit` を draw ごとに繰り返す) を、コマンドリスト記録時に「その時点の値」を確定させることで意味論を保ったまま DX12 に写像する。
  2. **フレームリソースのバージョニング** — 定数バッファ 1 本を draw ごとに使い回す既存パターン (`objectCB`) を、フレーム内アップロードリングのスライス割り当てで吸収する。

---

## 1. 現状分析 — 抽象化はどこまで済んでいるか

### 1-1. 完成している抽象化 (変更不要)

| 領域 | 実体 | 状態 |
|---|---|---|
| レンダラー本体 | `IRenderer` (Submit / Dispatch / SetRenderTarget / Resize / GpuProf*) | ✅ 上位は `IRenderer&` のみ参照 |
| バックエンド選択 | `RendererFactory` — 具象ヘッダーを include するのは `RendererFactory.cpp` のみ | ✅ `RendererBackend::DX12` の追加位置までコメント済み |
| ImGui バックエンド | `IImGuiRenderer` — SRV の具体型を `ResourceHandle` で隠蔽 | ✅ `imgui_impl_dx12` への差し替え口が確保済み |
| リソース抽象 | `IBuffer` / `IConstantBuffer` / `IShader` / `ITexture` / `IStructuredBuffer` / `IPipelineState` / `IRenderTarget` | ✅ 生成は `ResourceManager` → `CreateNative*` に集約 |
| 描画要求 | `DrawCall` / `ComputeCall` — 全リソースを `ResourceHandle` で参照する値型 | ✅ ネイティブ型なし |
| パス構造 | `RenderGraph` — `ResourceAccess (Read/Write)` 宣言・lifetime・alias group | ✅ 将来のバリア一括化の下地になる |
| シェーダー側 | `Platform/Backend.hlsli` — `FBZZ_BACKEND_DX12` 定義で `DX12.hlsli` へ切り替わる間接ヘッダー | ✅ ルーティング済み (DX12.hlsli 本体のみ未作成) |
| IBL ベイク | `IIblBaker` + `IRenderer::CreateIblBaker()` factory | ✅ 未対応バックエンドは nullptr 返却で縮退可 |
| 空連動 IBL | `IRenderer::BakeSkyLight()` / `SetRenderTargetFace()` — 未対応バックエンドは false / no-op | ✅ 縮退経路がインターフェースに明記済み |
| GPU プロファイル | `GpuProf*` 仮想関数群 — 未対応バックエンドは no-op | ✅ |

インターフェースのコメントには既に DX12 の実装方針が予告されている
(`IConstantBuffer`: 「DX12: Upload ヒープへの memcpy」、`IRenderTarget::GetNativeSRV`: 「DX12 は GPU descriptor handle を uint64_t として扱う」)。この設計方針を本ドキュメントで正式化する。

### 1-2. 残存する DX11 依存の棚卸し

`DX11|ID3D11|IDXGI|d3d11` を全プロジェクトで検索した結果、**コード上の漏れは実質ゼロ**。残りは以下:

| # | 箇所 | 内容 | 対応 |
|---|---|---|---|
| 1 | `Projects/Sandbox/CMakeLists.txt:153,169` | `sandbox` / `SandboxStandalone` が `d3d11` / `dxgi` を直リンク。**ソース上の使用箇所ゼロ** (GPU バックエンドが `fbzz_engine` 内部に閉じた後の残骸) | Phase 0 で削除 |
| 2 | `Projects/EditorLauncher/CMakeLists.txt:39` | 同上の死にリンク指定 | Phase 0 で削除 |
| 3 | `Projects/Engine/src/Core/Application.cpp:53` | `RendererBackend::DX11` がハードコード | ✅ 設定経由へ変更し、Phase 5で既定をDX12へ切替済み |
| 4 | `Assets/Shaders/Platform/DX12.hlsli` | 未作成 (`Backend.hlsli` が include する先) | Phase 0 で追加。シェーダー二重コピー先 (`DemoGame/Assets` / `GameHub/Templates`) にも同時追加 |
| 5 | ヘッダー内コメント (`DrawCall.hpp` 「DX11は b0〜b13」等) | コメントのみで実害なし | 実装時に両バックエンド前提の記述へ随時更新 |

つまり **「DX11 依存部分の抽象化」は掃除レベルの残作業のみ**で、以降は DX12 実装そのものが主戦場になる。

---

## 2. DX11 → DX12 で何が本質的に変わるか

`IRenderer` の API は DX11 の即時実行モデルを前提に書かれている。DX12 で変わる点と、それを**インターフェースを変えずにバックエンド内部で吸収する**方針の対応表:

| モデル差分 | DX11 (現状) | DX12 | 吸収方法 (DX12Renderer 内部) |
|---|---|---|---|
| コマンド発行 | `ID3D11DeviceContext` が即時実行 | コマンドリストに記録 → キューへ submit | フレーム全体を 1 本の direct コマンドリストに記録し `EndFrame` で実行。**API 呼び出し順 = 記録順**なので意味論は保たれる |
| CPU/GPU 同期 | ドライバが自動管理 | フェンスで明示管理 | フレームインフライト (2〜3) + フェンスリング。`Resize` / `Shutdown` は GPU 完全フラッシュ |
| パイプライン状態 | Rasterizer/Blend/Depth を個別ステートで組合せ | 全部入りの `ID3D12PipelineState` (シェーダー・InputLayout・RT フォーマット込み) | **PSO キャッシュ**: (シェーダー, `PipelineStateDesc`, トポロジー, RT フォーマット構成) をキーに遅延生成 |
| リソースバインド | スロットへ個別セット (ステージごとに独立した register 空間) | ルートシグネチャ + ディスクリプタヒープ | 固定スロットモデル (b0〜b13 / t0〜t31 / u0〜u7) をそのまま写した**汎用ルートシグネチャ** 1 本 (+Compute 用 1 本) |
| 定数バッファ更新 | `Map(DISCARD)` — 更新のたびドライバが新メモリを割当 | 自前管理 | **アップロードリング**: `Update()` = スライス割当 + memcpy、`Submit()` = その時点の GPU VA を root CBV に記録 |
| ハザード管理 | ランタイムが自動 (RTV/SRV 競合時は SRV をサイレント解除) | リソースバリアを明示 | **遅延ステート追跡**: リソースごとに現在ステートを保持し、バインド地点で必要な遷移バリアを発行 |
| リソース破棄 | 参照カウントでドライバが遅延解放 | GPU 使用中の解放は未定義動作 | **遅延破棄キュー**: 破棄要求をフェンス値付きで積み、完了フレームのものだけ実解放 |
| シェーダー | fxc / SM 5.0 / DXBC | DXC / SM 6.8 / DXIL (**DXBC も受理される**) | DX11 は `compiled/`、DX12 は `compiled_dx12/` に分離。DX12 の欠落・期限切れは DXC で再生成 |

重要な確認事項 (実装済みコードから):

- **`objectCB` は 1 本を draw ごとに `resources.Update()` → `Submit()` で使い回している** (`ForwardPasses.cpp` / `DeferredPasses.cpp` / `DecalPass.cpp` 等の全域)。DX12 で素朴に実装すると「最後の Update が全 draw に効く」ため、上記のアップロードリング + Submit 時 VA キャプチャが**必須**。
- 深度 SRV の read/write 競合は既に**コピーで回避済み** (`WaterRenderPass` / `DecalPass` / `CausticsPass` の HAZARD コメント参照)。DX11 の「サイレント解除」に描画結果が依存している箇所はなく、DX12 のバリアで正しく置き換わる。
- VS の `t0` (インスタンスバッファ) と PS の `t0` (Albedo) は**ステージ別 register 空間**で共存している。DX12 では `D3D12_SHADER_VISIBILITY_VERTEX` / `_PIXEL` で分離したディスクリプタテーブルにすることで同じ意味論を再現する。

---

## 3. インターフェース変更方針 — 「変えない」が基本

| 対象 | 変更 |
|---|---|
| `IRenderer` の公開 API | **変更なし**。全メソッドが DX12 で実装可能 (§2 の吸収方法) |
| `IBuffer` / `ITexture` / `IShader` / `IPipelineState` / `IRenderTarget` | **変更なし** |
| `IConstantBuffer::Update` / `IStructuredBuffer::Update` | **シグネチャ変更なし**。意味論を「呼び出し時点の内容が以降の Submit に使われる」と明文化 (現状の実挙動どおり) |
| `IRenderTarget::GetNativeSRV` | **変更なし**。DX12 実装は `D3D12_GPU_DESCRIPTOR_HANDLE::ptr` (uint64) を void* に格納 (x64 前提、コメント済みの既定方針) |
| `RendererBackend` enum | `DX12` 列挙子を追加 (コメントで予約済みの位置) |
| `RendererFactory.cpp` | `CreateDX12()` を追加し switch に case を足す。DX12 具象ヘッダーを include するのは引き続きこのファイルだけ |
| `Application` | バックエンド選択を設定値とコマンドラインから受け取る。既定はDX12、`--renderer=dx11`で互換経路へ切替可能 |
| `DrawCall` / `ComputeCall` | **変更なし**。スロット割り当て (b0〜b13 / t0〜t31 / u0〜u7) は両バックエンド共通の契約としてコメントを更新 |

**やらないこと (アンチゴール):**

- コマンドバッファ / バリアを上位レイヤーに露出する API 改修。RenderPass の execute ラムダが `IRenderer` を直接呼ぶ現行構造を維持し、DX12 の複雑さはバックエンド内部に閉じる。
- DX11 シェーダーの SM 6 化。DX11 は FXC / SM 5.0 / DXBC を維持し、DX12 のみ DXC / DXIL に移行する。

---

## 4. DX12 バックエンド設計

### 4-1. ファイル構成 (`Projects/Engine/src/Renderer/Platform/DX12/`)

DX11 実装とファイル単位で対になる構成 + DX12 固有の基盤クラス:

| ファイル | 役割 | DX11 対応物 |
|---|---|---|
| `DX12Renderer.hpp/.cpp` | `IRenderer` 実装。フレームサイクル・Submit/Dispatch・RT 管理 | `DX11Renderer` |
| `DX12Context.hpp/.cpp` | Device / Queue / フェンス / 遅延破棄キューの共有基盤。全 DX12 リソースが参照する | (なし — DX12 固有) |
| `DX12DescriptorAllocator.hpp/.cpp` | CPU ステージングヒープ + shader-visible リングヒープ (CBV/SRV/UAV, Sampler, RTV, DSV) | (なし — DX12 固有) |
| `DX12UploadArena.hpp/.cpp` | フレームインフライト分のアップロードリング (CB スライス / テクスチャ・バッファ初期データ転送) | (なし — DX12 固有) |
| `DX12PsoCache.hpp/.cpp` | (シェーダー, PipelineStateDesc, トポロジー, RT 構成) → PSO のハッシュキャッシュ | `DX11PipelineState` が統合される |
| `DX12StateTracker.hpp/.cpp` | リソースごとの現在ステート追跡とバリア発行 | (なし — DX11 はランタイム任せ) |
| `DX12Buffer.hpp/.cpp` | 頂点・インデックスバッファ (default ヒープ + 生成時アップロード) | `DX11Buffer` |
| `DX12ConstantBuffer.hpp/.cpp` | `Update()` = リングスライス割当。現在スライスの GPU VA を保持 | `DX11ConstantBuffer` |
| `DX12StructuredBuffer.hpp/.cpp` | フレームインフライト数だけ多重化した upload バッファ + SRV/UAV | `DX11StructuredBuffer` |
| `DX12Shader.hpp/.cpp` | .cso (DXBC / DXIL) ロード + DXC compile / container reflection で InputLayout / `ShaderDescriptor` 構築 | `DX11Shader` |
| `DX12Texture.hpp/.cpp` | DirectXTex による WIC/DDS ロード (DirectXTex は DX12 生成関数を同梱) | `DX11Texture` |
| `DX12RenderTarget.hpp/.cpp` | MRT / 深度 / キューブマップ RT。RTV/DSV/SRV ディスクリプタ管理 | `DX11RenderTarget` |
| `DX12ImGuiRenderer.hpp/.cpp` | `imgui_impl_dx12` ラッパー。サムネイル用の永続 SRV ヒープ (フリーリスト) | `DX11ImGuiRenderer` |
| `DX12IblBaker.hpp/.cpp` | 空連動 IBL の実行時畳み込み Compute (`BakeSkyLight`。開フレームのコマンドリストへ記録) | `DX11IblBaker` (実行時経路) |
| `DX12HdriBaker.hpp/.cpp` | Editor の HDRI→DDS ベイカー (`IIblBaker` 実装。フレーム非依存の自前コマンドリスト + フェンスで同期実行し 4 DDS + .ibl を出力) | `DX11IblBaker::Bake()` |

依存方向の規約は現行どおり: **上位レイヤーから `Platform/DX12/` への include 禁止**。`DX12Renderer*` へのダウンキャスト禁止。

### 4-2. フレームサイクル

```
BeginFrame:
  フェンス待ち (FRAMES_IN_FLIGHT 前のフレーム完了を保証)
  ├─ そのフレームのコマンドアロケーター Reset
  ├─ アップロードリングの該当領域を再利用可能に
  ├─ 遅延破棄キューから完了分を実解放
  └─ バックバッファを PRESENT → RENDER_TARGET へ遷移、RTV/DSV バインド

(フレーム中) Submit / Dispatch / SetRenderTarget / Clear ...
  └─ すべて直列に 1 本の direct コマンドリストへ記録

EndFrame:
  バックバッファを RENDER_TARGET → PRESENT へ遷移
  ├─ コマンドリスト Close → ExecuteCommandLists
  ├─ フェンス Signal (このフレームの完了マーカー)
  └─ Present (tearing 対応は DX11 と同じ CheckFeatureSupport 判定)
```

- `FRAMES_IN_FLIGHT = 2` から開始 (レイテンシと実装単純さのバランス。3 は計測後に判断)。
- スワップチェーンは `DXGI_SWAP_EFFECT_FLIP_DISCARD` / バックバッファ 3 枚。
- `Resize` / `Shutdown` は**全フェンスを待つ完全フラッシュ**後に実行 (Editor のリサイズ・終了時 Live Object 警告を防ぐ)。

### 4-3. ルートシグネチャ (固定スロットモデルの写像)

`DrawCall` / `ComputeCall` のスロット契約をそのまま写した**汎用ルートシグネチャを Graphics / Compute 各 1 本**だけ作り、全シェーダーで共用する。

Graphics 用 (DWORD 予算: root CBV 14×2 + テーブル 3×1 = 31 / 上限 64):

| Root Param | 内容 | 可視性 |
|---|---|---|
| 0〜13 | root CBV b0〜b13 (**GPU VA 直指定** — CB バージョニングと相性が良く、ディスクリプタ不要) | ALL |
| 14 | SRV テーブル t0〜t31 (テクスチャ) | PIXEL |
| 15 | SRV テーブル t0, t14〜t15 (インスタンス / VS バッファ) | VERTEX |
| 16 | Sampler テーブル s0〜sN | ALL |

Compute 用: root CBV b0〜b13 + SRV テーブル t0〜t31 (t14〜t15 の StructuredBuffer 含む) + UAV テーブル u0〜u7 (テクスチャ UAV と `uavBuffers` の u2〜u3 を同一テーブル内で解決)。

- 毎 Submit で shader-visible リングヒープにディスクリプタを CopyDescriptors し、テーブル先頭ハンドルをセットする (最も単純で確実な方式)。バインドレス化は将来の最適化としてスコープ外。
- SM 5.0 (DXBC) には register space がないため、ルートシグネチャは C++ 側で定義する (シェーダー埋め込み不要)。既存 HLSL は無変更で通る。

### 4-4. 定数バッファのバージョニング (最重要)

既存パターン: `resources.Update(h.objectCB, &objData, ...)` → `renderer.Submit(dc)` を **draw ごとに繰り返す**。

```
DX12ConstantBuffer::Update(data, size):
  UploadArena から 256B アライン済みスライスを割当て memcpy
  └─ m_currentGpuVA = スライスの GPU VA   (バッファ実体は作らない)

DX12Renderer::Submit(call):
  各 constantBuffers[i] の m_currentGpuVA を root CBV i に記録
  └─ コマンドリストへの記録 = その時点の VA が draw に確定 (Submit 時キャプチャ)
```

DX11 の `Map(DISCARD)` がドライバ内部でやっていたリネームを自前のリングで再現する形であり、**上位レイヤーは 1 行も変わらない**。`DX12StructuredBuffer` も同様に、毎フレーム `Update` される用途 (インスタンシング / GPU パーティクル) 向けにフレームインフライト数だけ実体を多重化する。

### 4-5. PSO キャッシュとリソースステート追跡

- **PSO キャッシュ**: キー = (IShader*, `PipelineStateDesc` 3 enum, `PrimitiveTopology`, バインド中 RT のフォーマット構成)。`Submit` 時に未登録なら生成してキャッシュ。既存の `PipelineStateDesc` の組合せ数は高々十数種 × シェーダー数なのでヒッチは初回のみ。`DX11PipelineState.hpp` のコメントどおり、DX12 では `IPipelineState` は desc を保持するだけの記述子となり、実体は PSO キャッシュが持つ。
- **ステート追跡**: `ID3D12Resource*` ごとに現在ステートを保持。`SetRenderTarget` で RTV/DSV 遷移、`Submit`/`Dispatch` の SRV/UAV バインド地点で必要遷移を発行 (遅延・逐次方式)。`RenderGraph` の `ResourceAccess` 宣言を使ったパス境界での一括バリアは**将来最適化**とし、v1 はバインド地点追跡で正しさを優先する。

### 4-6. シェーダー戦略

| 段階 | 内容 |
|---|---|
| DX11 互換経路 | FXC / SM 5.0 / DXBC を `compiled/` へ出力し、従来 GPU の退避経路を維持 |
| DX12 標準経路 | DXC / SM 6.8 / DXIL を `compiled_dx12/` へ出力。`-HV 2021` を共通指定し、`WAVE_INTRINSICS_SUPPORTED 1` を解禁 |
| 実行時再コンパイル | `dxcompiler.dll` を遅延ロードし、期限切れ HLSL を DXC で再生成。DXBC は `D3DReflect`、DXIL は `IDxcUtils::CreateReflection` で同じ descriptor へ正規化 |
| 最新版の扱い | SM 6.9 は preview のため既定にしない。安定版を 6.8 とし、6.9 固有機能 (SER / OMM) は Agility SDK・DXC・GPU 対応を揃えた実験ブランチで評価 |

シェーダーは root / `DemoGame` / `GameHub/Templates` の**三重コピー**があるため、`DX12.hlsli` 追加と bat 変更は必ず全コピーへ同時反映する。

### 4-7. その他サブシステム

- **ImGui**: `imgui_impl_dx12` を採用。フォント + Editor サムネイル (AssetBrowser は数百枚になりうる) 用に**永続 shader-visible SRV ヒープ + フリーリスト**を `DX12ImGuiRenderer` が所有。`GetImTextureID` は GPU ハンドルを返す。
- **GPU プロファイリング**: `ID3D12QueryHeap (TIMESTAMP)` + readback バッファ。既存の 3 フレーム遅延リング (`GPU_QUERY_LATENCY`) 設計をそのまま移植し、周波数は `GetTimestampFrequency` から取得。`GpuProf*` インターフェースは変更なし。
- **キューブマップ RT / 空連動 IBL**: `SetRenderTargetFace` は面+mip ごとの RTV を事前生成してバインド。`BakeSkyLight` / `DX12IblBaker` は既存 Compute シェーダー (DXBC) を流用し、UAV テーブルを面ごとに差し替える。実装完了までは false / nullptr の縮退経路で動く (インターフェース設計済み)。
- **Editor の HDRI→DDS ベイク (`DX12HdriBaker`)**: `CreateIblBaker()` が返す `IIblBaker` 実装。DX11 と同じ 4 種の Compute (Equirect→Cube / Irradiance / Prefilter / BRDF LUT, いずれも既存 DXBC) を、**フレームサイクルから独立した自前のコマンドリスト + フェンスで同期実行**する (DX11 の即時実行と同じ意味論を再現。一度きりの Editor 操作なので GPU 完全待機は許容)。DX12 には `ID3D11DeviceContext::GenerateMips` が無いため、env cubemap の mip 連鎖は GPU で mip0 を焼いた後 **DirectXTex (CPU, 非 WIC box フィルタ) で生成し直して GPU へ再アップロード**し、prefilter の PDF ベース LOD (firefly 低減) を DX11 と一致させる。読み戻し・DDS 保存は DirectXTex の DX12 版 `CaptureTexture` / `SaveToDDSFile` を使う。Compute の Root Signature・static sampler・固定スロット契約 (b0 / t0 / u0) は `DX12PsoCache` の汎用 Compute Root Signature をそのまま共有する。
- **遅延破棄**: `ResourceManager` が unique_ptr を破棄した時点で、各 DX12 リソースのデストラクタが実体を `DX12Context` の破棄キューへ「現在フェンス値付き」で移送する。Editor の再インポートフロー (旧リソース即時破棄 → 再生成) をコード変更なしで安全化する。

---

## 5. 実装フェーズ計画

各フェーズは独立にレビュー・マージ可能な粒度とし、**完了条件 (DoD)** を明記する。

### Phase 0 — 抽象化の仕上げ (DX11 のまま完結)
- Sandbox / EditorLauncher の CMake から死にリンク `d3d11` / `dxgi` を削除
- `RendererBackend::DX12` 追加 + `RendererFactory` に case 追加 (中身は「未実装」ログ + 空バンドル)
- `Application` のバックエンド選択を設定経由にし、Phase 5で既定をDX12へ切替 (`--renderer=dx11`で退避可能)
- `DX12.hlsli` を 3 コピー全部に追加
- **DoD**: 全ターゲットが DX11 で従来どおり動作。`--renderer=dx12` 指定時は明確なエラーログで終了

### Phase 1 — デバイス立ち上げ
- `DX12Context` / スワップチェーン / フェンス / フレームインフライト / デバッグレイヤー (Debug ビルド)
- `BeginFrame` / `EndFrame` / `Clear` / `ClearDepth` / `Resize` / `Shutdown`
- `DX12ImGuiRenderer` (フォントのみ、サムネイルは Phase 3)
- **DoD**: `--renderer=dx12` で Editor が起動し、クリア色の上に ImGui UI が全て描画・操作できる

### Phase 2 — リソースと最小描画
- 汎用ルートシグネチャ / `DX12Shader` (DXBC ロード + reflect) / `DX12Buffer` / `DX12ConstantBuffer` (アップロードリング) / `DX12Texture` / PSO キャッシュ / サンプラーヒープ
- `Submit` の不透明メッシュ経路
- **DoD**: SandboxStandalone で静的メッシュ + テクスチャ + Phong/PBR が描画される

### Phase 3 — フルパイプラインパリティ
- `DX12RenderTarget` (MRT / 深度 SRV 化) / `SetRenderTarget` / ステート追跡バリア
- `Dispatch` + UAV / `DX12StructuredBuffer` (インスタンシング / GPU パーティクル) / VS SRV テーブル
- ImGui サムネイル SRV (AssetBrowser / シーンビューポート)
- **DoD**: Editor の全 RenderPass (Shadow / Deferred / Water / Terrain / Foliage / PostProcess / UI / Debug) が DX12 で DX11 と同等の絵になる

### Phase 4 — 拡張機能
- キューブマップ RT / `SetRenderTargetFace` / `BakeSkyLight` / `DX12IblBaker` (空連動 IBL の実行時畳み込み)
- Editor の HDRI→DDS ベイク `DX12HdriBaker` (`CreateIblBaker()` / IBL ベイクパネル)
- GPU タイムスタンププロファイリング
- **DoD**: 空連動 IBL・Editor IBL ベイク・GPU プロファイラーが DX12 で動作。`IRenderer` の縮退経路 (false / nullptr) に依存する機能が残っていない ✅ 実装完了 (実機 A/B 画像・GPU 時間比較は VS 実行で確認)

### Phase 5 — 品質・最適化 (一部任意)

コード側は実装完了。残りは**実機 (Visual Studio 実行) でしか判定できない検証・計測**と、データが揃ってから判断する**任意最適化**のみ。

- ✅ **デバッグレイヤー / GPU-Based Validation / Live Object チェック** (コード実装済み)
  - Debug ビルドで `ID3D12Debug::EnableDebugLayer` + `ID3D12Debug1::SetEnableGPUBasedValidation(TRUE)` を常時有効化 (`DX12Context::Initialize`)。
  - `ID3D12InfoQueue::SetBreakOnSeverity` で CORRUPTION / ERROR をブレーク (`CreateFactoryAndDevice`)。
  - `Shutdown` で `IDXGIDebug1::ReportLiveObjects(DXGI_DEBUG_D3D12, SUMMARY | IGNORE_INTERNAL)` によるリーク報告。
  - → **実機検証**: 全パス + Editor IBL ベイクを一巡し、警告/Live Object がゼロであることを VS 出力で確認する。
- ✅ **リサイズ・最小化・フルスクリーン遷移の堅牢化** (コード実装済み)
  - 0 サイズ (最小化) は `m_suspended` でフレームを開かず縮退。フレーム記録中のリサイズは `m_resizePending` で次 `BeginFrame` へ遅延し、`ApplyResize` は全フェンス Flush 後に `ResizeBuffers`。
  - Present の `DXGI_STATUS_OCCLUDED` (遮蔽/ロック画面) を検知し、`DXGI_PRESENT_TEST` で復帰するまでフレームを開かない (無駄な記録/Present を回避)。
  - ボーダーレス全画面前提 (`MakeWindowAssociation(NO_ALT_ENTER)` + FLIP_DISCARD、排他全画面は不使用) のため排他遷移のエッジケースが無い。
- 🔵 **DX11/DX12 A/B 比較・GPU 時間比較** (§6。**VS 実機実行が必須** — コード不要)
  - 同一シーン (DemoGame) を `--renderer=dx11` / `--renderer=dx12` で起動し画像比較。PIX / RenderDoc でパス単位照合。
  - GPU 時間は既存 `GpuProf*` (DX12 は `ID3D12QueryHeap(TIMESTAMP)` + readback、実装済み) をパス別に両バックエンドで比較。
- 🔵 **DXC / SM 6.8 / DXIL 化**
  - コード実装済み。`compile_shaders.bat` / `compile_ui_shaders.bat` は引数なしで DX11 と DX12 の両セットを生成する。
  - VS 実機で DXC の全シェーダーエラー、DXIL reflection、PSO 作成、A/B 描画を確認する。
- ⚪ **RenderGraph メタデータによるバリア一括化**
  - A/Bデータに基づいて判断する後続最適化。`--renderer=dx11`による比較・退避を維持する。
- **DoD**: 既定バックエンドの切替可否を判断できる品質データが揃っている (= 上記 🔵 の VS 実機計測で確定する)

---

## 6. 検証方針

| 手段 | 内容 |
|---|---|
| デバッグレイヤー | Debug ビルドで `ID3D12Debug` + GPU-Based Validation を常時有効。警告 = バグとして扱う |
| A/B 比較 | 同一シーン (DemoGame) を `--renderer=dx11` / `dx12` で起動しスクリーンショット比較。PIX / RenderDoc でパス単位のフレームキャプチャ照合 |
| プロファイル比較 | 既存 `GpuProf` 結果 (パス別 GPU ms) を両バックエンドで比較し、劣化パスを特定 |
| 回帰導線 | Editor 起動 → シーンロード → Play → スタンドアロン起動、の既存確認フローを両バックエンドで実施 |

---

## 7. リスクと対策

| リスク | 対策 |
|---|---|
| CB 使い回しの意味論齟齬 (最後の Update が全 draw に効く事故) | §4-4 の Submit 時 VA キャプチャで設計段階から吸収。Phase 2 の DoD に複数オブジェクト描画を含める |
| GPU 使用中リソースの破棄 (Editor 再インポート・シーン切替) | 遅延破棄キューをリソースのデストラクタに組み込み、上位のコードパスを一切変えない |
| フレーム途中のリソース生成 (`CreateNative*` は任意タイミングで呼ばれる) | 生成時は専用アップロードリスト + フェンス待ちの同期転送で開始 (DX11 と同じ意味論)。非同期化は計測後の最適化 |
| DX11 の自動アンバインド挙動への暗黙依存の見落とし | 既知箇所はコピー回避済みだが、GPU-Based Validation と A/B 比較で全パスを網羅検証 (Phase 3 DoD) |
| PSO 生成ヒッチ (初回 draw 時) | 組合せ数が小さいため初回のみ許容。問題になれば起動時ウォームアップ or PSO ライブラリ保存 |
| シェーダー三重コピーの反映漏れ | Phase 0 で 3 コピー同時変更を規約化 (既存の運用ルールに従う) |
| ImGui サムネイル SRV のヒープ枯渇 | 永続ヒープをフリーリスト管理し、上限到達時はログ + 白テクスチャで縮退 |

---

## 8. スコープ外 (明示)

- バインドレスリソース / `SM 6.6 ResourceDescriptorHeap`
- 非同期 Compute キュー / マルチスレッドコマンド記録
- Mesh Shader
- SM 6.9 preview 固有の Shader Execution Reordering / Opacity Micromap
- DX11 バックエンドの削除 (DX12既定化後もA/B比較・互換退避用として両対応を維持)

---

## 9. Phase 6 — DXR 導入計画

Shader Model 6.8 化は DXR の必要条件を整えるが、レイトレーシング機能そのものではない。
DXR は次の順序で追加し、非対応 GPU では既存ラスタライズ経路へ縮退する。

1. **Capability と縮退経路 (実装済み)**
   - `D3D12_FEATURE_SHADER_MODEL` を 6.8 から降順照会する。
   - `D3D12_FEATURE_D3D12_OPTIONS5::RaytracingTier` を保存する。
   - Full DXR は Tier 1.0+、Inline RayQuery は SM 6.5+ かつ Tier 1.1+ のときだけ選択する。
2. **Acceleration Structure**
   - `DX12AccelerationStructure` が BLAS / TLAS と scratch buffer を所有する。
   - 静的 Mesh は BLAS をキャッシュし、Transform 変更時は TLAS のみ更新する。
   - Skinned Mesh は初期版では除外し、更新 BLAS のコスト計測後に追加する。
3. **最小 Inline RayQuery パス**
   - Deferred Lighting の shadow ray から開始し、既存 Shadow Map をフォールバックとして維持する。
   - Compute `cs_6_8` を使い、既存 `IRenderer` に DX12 型を露出しない専用抽象を追加する。
4. **Full DXR pipeline**
   - Ray generation / Miss / Closest hit を `lib_6_8` へコンパイルする。
   - RTPSO、global/local root signature、Shader Table、再帰深度を DX12 バックエンド内部で管理する。
5. **品質・性能検証**
   - GPU-Based Validation、PIX capture、DXR on/off の GPU 時間と画質差を記録する。
   - 対応 GPU でも予算超過時にラスタライズへ戻せる品質設定を用意する。
