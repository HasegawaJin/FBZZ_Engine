# Bindless (ResourceDescriptorHeap)

- 状態: 実装済み (段 A / B / C 完了)。**ビルドと実機描画の確認は未了**

DirectX 11 撤去 (`dx11-removal.md`) で SM 6.x がエンジンの床になった。その対価を回収する最初の一手。

**32 スロットのディスクリプタテーブルを全廃した。SRV / UAV はすべて `ResourceDescriptorHeap` から直接引く。**

---

## なぜやるか

性能そのものより **構造**が目的。現在の描画は 1 ドローごとに次を払っている。

```
DX12Renderer::Submit
  → AllocatePixelSrvTable()          動的リングから 32 枠
  → CopyDescriptors(32 枚)            テクスチャの SRV を毎回コピー
  → SetGraphicsRootDescriptorTable
```

`DrawCall` は CB 14 枠 + テクスチャ 32 枠 + instance + vsBuffers + psBuffers を持つ
数百バイトの構造体で、CPU がドローごとに組み直している。bindless にすると:

- 毎ドローの `CopyDescriptors` (最大 32 枚) が消える
- `DrawCall` から `textures[32]` が消せる (テクスチャは添字で CB に載る)
- **「テクスチャは 32 スロットまで」という上限が外れる**

3 つ目が一番大きい。スロット枯渇はレンダーパスを足すたびに効いてくる制約で、
現に `kPsBufferBaseSlot = 29` のように空き枠を数えて割り当てている。

> **性能目当てではない。** 最大シーン (Stage_02) の描画は約 330。この規模で CPU の
> ディスクリプタコピーはボトルネックではない。狙いは上限を外して設計を素直にすること。

---

## 要件 — bindless は必須

`ResourceDescriptorHeap` は 2 つ揃って初めて使える。

| 要件 | 判定 |
|---|---|
| Shader Model 6.6 以上 | `D3D12_FEATURE_SHADER_MODEL` |
| Resource Binding Tier 3 | `D3D12_FEATURE_D3D12_OPTIONS.ResourceBindingTier` |

`DX12Context::SupportsBindless()` が両方を見て、**満たさなければ起動を拒否する**。
SM 6.8 の要求と同じ扱い。

### なぜ実行時フォールバックを持たないか

移行後のシェーダーは `FBZZ_TEX2D` マクロ経由で `ResourceDescriptorHeap` を**無条件に**引く。
ここを「非対応でも起動はできる」ことにすると、ルートシグネチャのフラグだけが落ちて、
描画時に「何も貼られていない」絵が出る。原因が見えない形で壊れるくらいなら、
起動時に何が足りないかを名指しして止める。

```
[ERROR] DX12Context: このエンジンは bindless (Resource Binding Tier 3) を要求します。
        この GPU / ドライバーは Tier 2 までです
```

対応ハードを実質狭めていないことが前提にある。エンジンは既に **SM 6.8 未満で起動を拒否**
しており、SM 6.8 が動く世代 (Maxwell+ / GCN 1.2+ / Gen9+) は実質すべて Tier 3。
ここで弾かれる機械は事実上存在しない。

フォールバックを持たない代わりに、シェーダー側に `#if` が 1 つも入らない。

---

## ディスクリプタヒープの区画割り

`m_resourceSrvHeap` (shader-visible, CBV_SRV_UAV) を 3 つに割る。
正本は `DX12Context.hpp` の `BINDLESS_HEAP_BASE` / `DYNAMIC_HEAP_BASE`。

```
[0,    56)                                   null ディスクリプタ
[56,   56 + 16384)                           永続 bindless      ← 新設
[16440, + FRAME_COUNT * 65536)               フレームごとの動的リング (既存)
```

### なぜ動的リングと分けるか

動的リングは**フレームごとに巻き戻る**前提で、同じ添字が翌フレームには別のリソースを指す。
bindless の添字はテクスチャの**識別子**として定数バッファやマテリアルに載るので、
リソースが生きている限り不変でなければならない。

### 枠の寿命

- 確保は bump (`m_bindlessNextSlot`)、解放はフリーリスト経由の再利用
- **解放は即時に戻さない。** `FreeBindlessSlot` は現在のフェンス値と一緒に保留列へ積み、
  `CollectDeferredReleases` が通過を確認してからフリーリストへ返す。即時に戻すと、
  まだそのテクスチャを読む記録済みコマンドが、再利用された別テクスチャを読む
- **枯渇 (16384 枠) は実害のある失敗。** bindless が必須になった以上、`INVALID` を返しても
  逃げ場はなく、そのテクスチャは貼られない。`FBZZ_LOG_ERROR` で一度だけ報告する。
  容量はそれが起きない規模に取ってあり、超えるなら容量を上げるのが正しい対処

### 容量の根拠

最大シーン Stage_02 の実測でテクスチャは 3 桁に収まる。Editor のプレビューと
フォントアトラスを足しても 1 桁余る規模として 16384 を取った。

---

## 発行は遅延

`DX12Texture::GetBindlessIndex()` が初回呼び出しで枠を確保し、自前の CPU SRV を
永続レンジへ `CopyDescriptorsSimple` する。

**なぜ Init 時に発行しないか。** `DX12Texture` の Init 経路は 8 本あり
(`Init` / `InitFromData` / `InitFromData3D` / `InitFromResource` / `InitCubeFromResource` /
`InitForCompute` / `InitForCompute3D` / `InitDynamic`)、全てに発行を足すと
**足し忘れた経路のテクスチャだけが黙って bindless から消える**。原因が「真っ黒に描画される」
としてしか現れないので、見つけるのが難しい。加えて、実際に bindless で参照された
テクスチャだけが枠を消費するので容量も節約できる。

GPU 実行中に shader-visible ヒープへ書き込むことになるが、書くのは
**まだ誰も参照していない新しい枠**なので競合しない。

---

## ルートシグネチャ

既存のディスクリプタテーブルは**そのまま残す**。このフラグは「ヒープを直接引ける」という
許可を足すだけで、テーブル経由の束縛を無効化しない。

```cpp
D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
```

これが効いて、**144 本のシェーダーを一斉に書き換えずに済む**。bindless へ移したものから
順に切り替えられ、移していないシェーダーは 1 行も変えずに動き続ける。

描画 (`Submit`) とコンピュート (`Dispatch`) のどちらも `m_resourceSrvHeap` を
`SetDescriptorHeaps` しているので、両方から同じ添字で引ける。

---

## シェーダー側

`Common/BindlessIndices.hlsli`。`ResourceDescriptorHeap[i]` を直書きせず必ずマクロを通す。

```hlsl
#include "Common/BindlessIndices.hlsli"

FBZZ_TEX2D(texAlbedo, TEX_ALBEDO_SLOT);   // 宣言はこの 1 行だけ
...
float4 c = texAlbedo.Sample(sampDefault, uv);   // 参照側は移行前と同じ
```

`static Texture2D texAlbedo = ResourceDescriptorHeap[...]` へ展開される。**この形にしたのは、
284 箇所ある参照を 1 つも書き換えずに移行するため。** 添字は `b14` の添字ブロックから引く
(`FbzzPixelSlot`)。スロット番号 `TEX_*_SLOT` は `Common/Binding.hlsli` の `t0`〜`t31` 定義と
1:1 の数値版で、食い違うと「コンパイルは通り、別のテクスチャが貼られる」形で壊れる。

`NonUniformResourceIndex` で包んである。同一 wave 内のレーンが別々の添字を持つ場合
(1 ドローに複数マテリアルが混ざる GPU 駆動経路) に包み忘れると、先頭レーンのリソースを
全レーンが読む。単一マテリアルなら冗長だが、ドライバが均一性を証明できれば最適化で消えるので、
**付け忘れる方の損が大きい**。

---

## 移行の段取り

32 スロットを全廃する方針。ただし**一度に全部は動かさない**。シェーダー 144 本と
Submit 経路を同時に切り替えると、失敗したときに「画面が真っ黒」としか分からず、
どのパスが原因か切り分けられなくなる。ビルドが通る 3 段に割る。

### 段 A — 機構の追加 (完了)

**旧テーブルを残したまま**、添字を配る経路を並走させる。描画の挙動は 1 ピクセルも変わらない。

| 追加したもの | 場所 |
|---|---|
| 添字ブロックのレイアウト | `Engine/Renderer/BindlessIndices.hpp` ↔ `Common/BindlessIndices.hlsli` |
| ルートパラメーター 16 = `b14` (root CBV) | `DX12PsoCache::CreateRootSignature` / `CreateComputeRootSignature` |
| ドローごとの添字書き込み | `DX12Renderer::Submit` / `Dispatch` |
| バッファ側の添字 | `IStructuredBuffer::GetBindlessIndex()` + DX12 実装 |
| 数値スロット定数 (`TEX_*_SLOT`) | `Common/Binding.hlsli` |
| 宣言マクロ (`FBZZ_TEX2D` 等) | `Common/BindlessIndices.hlsli` |
| 移行の実例 | `Material/Surface/PBR.hlsl` |

**ルートパラメーターは末尾 (16) へ足した。** 既存の 14 / 15 を動かすと
`SetGraphicsRootDescriptorTable(14/15)` が黙って別の引数を差すことになる。

**添字ブロックはドローごとに `Reset()` から組み直す。** 前のドローの添字が残ると、
束縛していないテクスチャを読む。アリーナ枯渇時はゼロ埋めではなく
**全枠 INVALID の専用ブロック**を差す (添字 0 はヒープ先頭の「有効な」ディスクリプタなので、
ゼロ埋めだと全スロットが無関係なリソースを指す)。

### 段 B — シェーダーの移行 (完了)

宣言 1 行の置き換えで済む形にしたので、**Sample 呼び出し 284 箇所は 1 文字も変えていない**。

```hlsl
Texture2D texAlbedo : register(TEX_ALBEDO);   // 旧
FBZZ_TEX2D(texAlbedo, TEX_ALBEDO_SLOT);       // 新
```

実績: エンジン側 **126 ファイル / 324 宣言**、GreenWare 固有 **13 ファイル / 26 宣言**を機械変換。
スロット定数 (`TEX_*_SLOT` 47 個 / `UAV_*_SLOT` 15 個) は `Binding.hlsli` の `t0`〜`t31` / `u0`〜`u7`
定義から生成した数値版。

**シェーダーツリーは 4 本ある** (エンジン / GreenWare / GameHub テンプレート 2 種)。エンジン側を直して
同期しただけでは、**GreenWare 固有のシェーダー (36 本) が取り残される**。移行や一括置換のときは
「エンジンを直して配る」ではなく、各ツリーを個別に走査して未変換が 0 件であることを確かめること。

#### 手で直した例外

`ParticleMaterial.hlsli` の `gParticles` / `gSortedParticles` (t14 / t15) だけは
`FBZZ_VS_SBUFFER` を使う。**旧モデルでは VS の t14/t15 は「頂点 SRV テーブル」の
vsBuffers[0]/[1] を指しており、同じ t14/t15 でもコンピュート側 (`ParticleGpuSim.cs`) の
srvBuffers とは別物だった。** 平坦な添字空間では、ここを取り違えるとコンピュート用の
ディスクリプタを VS が読んで「粒子が全部原点に出る」形で壊れる。

### マテリアルのテクスチャ枠 — リフレクションの作り直し

**bindless で最初に踏んだ落とし穴。** `ResourceDescriptorHeap` から引くテクスチャは
**DXIL に束縛情報を一切残さない**。実測 (`dxc -Fc`) でも Resource Bindings に cbuffer 9 本が
並ぶ一方、texture は 0 件だった。

`DX12Shader::BuildDescriptor` はマテリアルのテクスチャ枠を `D3D_SIT_TEXTURE` かつ
`BindPoint < 5` から作っていたため、移行後は **全シェーダーで枠が 0 件**になる。
Inspector には 5 枠のフォールバックがあるので «全部消える» とはならないが:

- albedo しか使わないシェーダーに差せない枠が 4 つ並ぶ
- `t5`〜`t7` の汎用枠を使うカスタムシェーダーは枠を失う
- AI バス・スクリプトプロキシ・マテリアルプレビューはテクスチャを 1 枚も返さなくなる

#### 直し方: 枠の正本を cbuffer へ移す

cbuffer の変数はリフレクションに残る。そこで **`MaterialConstants` の添字フィールドを
枠の宣言そのもの**にした。

```hlsl
cbuffer MaterialConstants : register(CB_MATERIAL) {
    float4 albedo;  ...
    uint texAlbedoIndex;   // ← これが «albedo 枠がある» の宣言
    uint texNormalIndex;
};
FBZZ_MATERIAL_TEX(texAlbedo, texAlbedoIndex);
```

- 並びの正本は `ShaderDescriptor.hpp` の `kMaterialTextureSlots` (フィールド名 ↔ `.mat` のキー)。
  以前は HLSL の変数名・Inspector の `kCanonicalSlots`・`.mat` のキーが 3 か所に散っており、
  ずれても «別のテクスチャが貼られる» としてしか現れなかった
- `BuildDescriptor` は添字フィールドを `vars` (編集可能変数) から除いて `textures` へ振り替える。
  除かないと Inspector の数値欄に `texAlbedoIndex` が編集可能な uint として並ぶ
- `Material::Upload` が `constantOffset` へ実際の bindless 添字を毎フレーム書く。
  焼き込まないのは、添字がテクスチャの生存に紐づくため (差し替え・再読み込みで変わる)
- **宣言した枠だけが Inspector に出る。** 使わない枠を宣言すると «差したのに効かない» になる

#### t0〜t4 は 2 つの別物だった

移行対象は «自前の `MaterialConstants` を持つ» シェーダーだけ (エンジン 33 + GreenWare 固有 6)。
地形のスプラットマップ・ライト Cookie・雲アップスケール・SSR・IBL・流体なども `t0`〜`t4` を
使うが、これらは **エンジンが `DrawCall::textures` へ直接差すパス固有のもの**で、
`.mat` から差すものではない。b14 の添字ブロック経由のままが正しい。
### 段 C — 旧テーブルの撤去 (完了)

- ルートシグネチャ: param 14 / 15 (SRV / UAV テーブル) を削除。**ディスクリプタテーブルが 1 つも無い**
  ルートシグネチャになり、param 14 が bindless 添字ブロック (b14) になった
- `DX12Renderer::Submit` / `Dispatch` からテーブル構築と `CopyDescriptors` を削除。
  **状態遷移 (`QueueTransition`) は残す** — 添字が同じでも、間に挟まった別パスでリソースの状態は変わる
- `m_pixelTableCache` / `MakePixelTableKey` / `PixelTableKeyHash` を削除。
  1 ドローあたりのコピーが無くなったので「同じ束縛なら再利用する」最適化ごと不要になった
- `DX12Context` から `Allocate*SrvTable` / `AllocateUavTable` / `GetNull*` を削除
- **null ディスクリプタ 56 枚と、フレームごとの動的リング (65536 × 2) を撤去。**
  「束縛されていない」は添字 `INVALID_BINDLESS_INDEX` で表せるので null ビューが要らない。
  共有 SRV ヒープは 131,128 枠から 16,384 枠へ縮んだ
- `DrawCall` / `ComputeCall` の**ハンドルは残した**。エンジンはハンドルで動いており、
  消えたのはバックエンド側のテーブル構築だけ

#### ベイカー 2 つの扱いが分かれた理由

| | 束縛するヒープ | 添字の作り方 |
|---|---|---|
| `DX12IblBaker` | 共有ヒープ (`m_resourceSrvHeap`) | 面ごと UAV を `PublishBindlessDescriptor` で永続レンジへ発行し、ベイク後に返す |
| `DX12HdriBaker` | **自前のヒープ** | 自前ヒープ内の添字をそのまま渡す (複製が要らない) |

`ResourceDescriptorHeap[]` は「そのとき束縛されているヒープ」を引く。HdriBaker は
自分のヒープを `SetDescriptorHeaps` しているので、共有ヒープへ複製する必要がない。

IblBaker の一時枠はスコープガードで返す。失敗経路が 3 つあり、どれかで返し忘れると
ベイクのたびに枠が減り続けて、最後は枯渇して IBL が焼けなくなる。

### その先

- `ComputeCall::uavOutputs` の 8 枠 (撤去済み DX11 SM 5.0 の CS UAV 上限の名残) を解く。
  UAV も `ResourceDescriptorHeap` から引けるので、段 C の直後が自然
- 定数バッファも bindless 化するかは未決。root CBV 14 本で足りている間は動かさない
