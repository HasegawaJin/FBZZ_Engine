# 非同期コンピュートキュー

画面空間の Compute (AO・Bloom・SSR) を描画キューと別のキューへ出し、ジオメトリ描画と重ねて走らせる。

## 1. 今の形

DX12 のキューは 2 本ある (`DX12Context.hpp:208, :239)`。

| キュー | 用途 |
|-------|------|
| `m_commandQueue` (DIRECT) | **このフレームの描画・Compute のすべて** |
| `m_copyQueue` (COPY) | テクスチャの非同期転送だけ |

描画は 1 フレーム 1 本のコマンドリストに直列記録し、`EndFrame` でまとめて 1 回投入する (`DX12Context.cpp:411-412`)。SSAO / GTAO / Bloom / SSR / MotionBlur / ContactShadows / VolumetricLight はすべて CS だが、全部この 1 本に並ぶので**ジオメトリ描画と重ならない**。

## 2. 重ねるには列を割る必要がある

キュー間の同期は「投入した順」でしか効かない (`ID3D12CommandQueue::Wait`)。描画を最後に 1 回投入する今の形のままコンピュートキューを足しても、

- コンピュートを先に投入 → 描画がその完了を待つ → **直列のまま**
- 描画を先に投入 → コンピュートは描画の完了を待つ → **直列のまま**

にしかならない。重ねるには、描画の列を **[生産者] | [消費者]** に割り、その間でコンピュートを走らせる。

```
描画キュー   : [ Shadow → GBuffer ]──Signal(F)────────────[ Lighting → ... ]
                                          ↘ Wait(F)  Signal(G) ↗ Wait(G)
コンピュート :                            [ AO → AOBlur ]
```

`SplitGraphicsList()` がこの「閉じて投入し、同じアロケーターで開き直す」を行う。アロケーター自体は
フレーム末まで Reset しないので、投入済みのリストが参照している記録メモリは生きたまま残る。

### 列を割ると消えるもの

`ID3D12GraphicsCommandList::Reset` はパイプライン状態を全部落とす。**描画先・ビューポート・シザー・
ルートシグネチャ・PSO・ディスクリプタヒープが既定へ戻る**。`DX12Renderer` は「直前に何を束縛したか」を
覚えて冗長な設定を省いているので、割った直後にその記憶を捨てないと、束縛していない状態を「設定済み」と
誤認して描画が壊れる。`BeginFrame` が既に同じ後始末 (`InvalidateRootCbvCache`) を持っているので、
割った後も同じ経路を通す。

## 3. キューをまたぐリソースの状態

COMPUTE キューが扱える状態は限られている。`PIXEL_SHADER_RESOURCE` / `RENDER_TARGET` /
`DEPTH_WRITE` はコンピュートキューでは**遷移も使用もできない**。

@see https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12 «Multi-engine synchronization»

この制約を守る方法は 2 つある。

| 方法 | 内容 | 採否 |
|------|------|------|
| 宣言する | 非同期区間が触るリソースを呼び出し側が全部並べ、描画キュー側で先に移しておく | **採らない** — 宣言漏れが «たまに壊れる» 形で出る。しかも漏れているかどうかは絵から分からない |
| 全部 COMMON へ落とす | 区間へ入る前に、追跡中のリソースを描画キュー側でまとめて `COMMON` へ移す | **採る** |

`COMMON` はどのキューでも合法で、コンピュートキューはそこから `UNORDERED_ACCESS` /
`NON_PIXEL_SHADER_RESOURCE` へ自由に移せる。区間を抜けた後は、次に束縛する側が `COMMON` から必要な
状態へ移し直す — `DX12StateTracker` は状態を 1 か所で持っているので、これは何も足さずに成り立つ。

代償はバリアが増えること。区間 1 つにつき「追跡中で COMMON でないリソースの本数」ぶんのバリアが出る。
宣言方式より遅いが、**宣言漏れによる «たまに壊れる» が構造的に起きない**。速さが問題になったら、
そのとき測って宣言方式へ移せばよい。

## 4. どの区間を非同期にするか

**まだ決めない。** 重ねて得をするのは「区間の GPU 時間」と「その間に描画キューが持っている仕事の量」の
小さい方までで、今のパス順ではそれが分からない。

- まず `IRenderer::GpuProfGetResults()` (パス単位の GPU 時間) を GreenWare の 3 ステージで取る
- 区間の候補は「入力が確定していて、消費者が十分後ろにあるもの」。今の順では AO の消費者
  (`DeferredLighting`) がすぐ後ろにいるので、**重なる窓は Sky と DepthCopy しかない**
- 窓が足りなければ、区間を動かすのではなく**パス順の方**を見直す (AO を Shadow と重ねる等)

最初に載せるのは AO (`SSAO` / `GTAO` とそのブラー)。入出力が曖昧さなく、CS だけで閉じているため、
**仕組みが正しく動くかを確かめる対象**として適している。得になるかは §5 の測定で決める。

## 5. 既定は無効

`RenderSettings::asyncCompute` は既定 false。環境変数 `FBZZ_ASYNC_COMPUTE=0` でも落とせる。

- 重ねても**絵は 1 ピクセルも変わらない**こと (Playtest の基準画像が on / off で一致する) が先
- 速くなったかは on / off の GPU 時間で比べる。既定で入れるのはその数字が出てから
- 検証レイヤー (`FBZZ_GPU_VALIDATION`) を有効にして、キューをまたぐ状態遷移の警告が 0 であること

## 6. やらないこと

- **コマンドリストの並列記録**。ここで足すのは「列を割る」までで、割った各列は今までどおり 1 スレッドが
  順に記録する。並列記録は `IRenderer` を `ICommandContext` ベースへ作り替える別の話で、40 ある描画パス
  すべての API が変わる
- **コンピュートキューの GPU 計測**。タイムスタンプはキューごとに別のクエリヒープと周波数が要る。
  区間の得失は当面「フレーム全体の GPU 時間」で見る

## @see

- `Docs/design/render-graph.md` — パスの依存申告。どの区間が独立かはここが正本
- `Docs/design/gpu-instancing.md` — 同じ «測ってから既定にする» 手順
- [Multi-engine synchronization](https://learn.microsoft.com/en-us/windows/win32/direct3d12/user-mode-heap-synchronization) — キュー間のフェンスと状態の契約
