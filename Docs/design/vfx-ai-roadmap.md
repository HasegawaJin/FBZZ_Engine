# VFX Editor — AI が AAA 級エフェクトを作るための残課題

最終更新: 2026-07-26

このドキュメントは「AI が単独で AAA 級のエフェクトを作れる状態」を到達点に置き、
そこまでに何が足りていないかを洗い出したもの。実装済みの機能一覧は
`vfx-graph.md`、AI 連携の使い方は `ai-editor-integration.md` を参照。

---

## 現在地 — 推測が観測に置き換わった範囲

AI が「知らないまま決めていた」項目は、下表のとおりほぼ全て観測できるようになった。

| 問い | 答える手段 | 状態 |
|---|---|---|
| この素材はどういう絵か | `vfx_analyze_texture` | ✅ |
| 実際どう描かれるか (.mat の上書き) | `vfx_analyze_material` | ✅ |
| 何を書けるか (シェーダー変数) | `shader_inspect` | ✅ |
| 手持ちで何が作れるか | `vfx_survey_assets` | ✅ |
| グラフとして壊れていないか | `vfx_lint` (+ `fix` / `autoFixable`) | ✅ |
| 今どのノードが動いているか | `vfx_runtime_state` | ✅ |
| どう見えるか | `vfx_preview` (画像) | ✅ |
| **その画が破綻していないか** | `vfx_preview_metrics` | ✅ |
| **時間の形は正しいか** | `vfx_preview_curve` | ✅ |
| **前回より良くなったか** | `vfx_preview_compare` | ✅ |
| **別の角度・距離ではどうか** | `vfx_preview` の `camera` / `vfx_preview_sweep` | ✅ |
| **実際に GPU で回っているか** | `vfx_lint` の `GPU_FALLBACK` / `vfx_runtime_state` の `simulation.effective` | ✅ |
| **実測でいくらかかるか** | `vfx_runtime_state` の `cost` | ✅ |

「大量に出す」を塞いでいた GPU 縮退 (ソート / メッシュパーティクル) も、
絵作りの表現力 (壁 4-c) も、オーサリング UX (壁 5) も解消済み。
**当初のロードマップに挙げた項目は全て埋まっている。**
残っているのは各機能に付いた近似の精度で、そこは「壁 4-c」節に理由付きで明記した。

---

## 壁 1 — AI は「良し悪し」を判定できない ✅ 解決済み

### 何が問題だったか

`vfx_preview` は PNG を返すだけで、判断は全て MCP クライアント側の視覚に委ねられていた。
同じ画を見ても毎回違う結論が出るため「少し暗い」の「少し」に基準が無く、
直す量を決められず反復が振動していた。

### 入れたもの

**1-a. `vfx_preview_metrics`** — 画を「絵」ではなく「数値」として読む。

読み戻しは PNG とは別経路で、HDR 線形値のまま取る
([`IRenderer::CaptureRenderTargetToLinearRGBA`](../../Projects/Engine/include/Engine/Renderer/IRenderer.hpp))。
PNG は 8bit へクランプするので、白飛びしているのか単に明るいのかがエンコードの時点で失われるため。
指標の算出は [`PreviewMetrics.cpp`](../../Projects/Editor/src/Ai/PreviewMetrics.cpp) に集約:

- 露出 — 平均輝度 / 描画画素だけの平均輝度 / P99 / 最大 / クリップ率 / 白飛び率 / log2 輝度ヒストグラム 16 階級
- 画面占有 — coverage / 重心 / バウンディングボックス
- 動き — 直前サンプルとの平均輝度差と変化画素率

さらに、閾値の解釈をクライアント側にぶれさせないよう、**判定そのものをエンジン側に置いた**。
`issues` として `BLOWN_OUT` / `OVEREXPOSED` / `SCREEN_FLOODED` / `TOO_DIM` /
`EMPTY_FRAME` / `STATIC_FRAME` / `OFF_CENTER` を名指しで返す。

**1-b. `vfx_preview_curve`** — 全区間をサンプルして指標の時系列を返す。
`peakNormalized` (ピーク位置を全長で正規化) と `tailRatio` (消え際の残り) を添えるので、
「爆発なのにピークが t=0.62 にある」「再生終了時点でまだピークの半分が残っている」を直接指摘できる。

**1-c. `vfx_preview_compare`** — 2 つの `.vfx` を同じ時刻列で測り、指標の差を符号付きで返す。
「emitRate を上げたら coverage が 1.8 倍になり輝度はほぼ変わらなかった = コストだけ増えた」が言える。

---

## 壁 2 — AI はカメラを動かせない ✅ 解決済み

`vfx.preview` に `camera` を追加した。注視点まわりの球面座標
(`preset` / `target` / `distance` / `yaw` / `pitch` / `fovY`) で指定する。
自由なカメラ行列を組ませると同じ「斜め上から」を再現できず評価が揺れるため、意図的に自由度を絞ってある。

視点の組み立ては [`VFXPreviewCamera.hpp`](../../Projects/Editor/include/Editor/Ai/VFXPreviewCamera.hpp) に 1 か所化した。
埋め込み版 (`EditorApp`) と独立版 (`VFXEditorApp`) の両方が同じ規則を使うので、
起動方法で絵が変わることはない。

併せて `vfx_preview_sweep` で距離を振った連続評価を返す。
LOD の切り替わりと「ゲーム内距離で読めるか」の確認が 1 コマンドで済む。

> 同時に、独立版の AI capture が `view` (overdraw / gizmos) を無視していた不整合も直した。
> 埋め込み版とは違う絵が返っており、AI の判断が起動方法に依存していた。

---

## 壁 3 — 性能の真実が見えない ✅ 解決済み

### 3-a. GPU シミュレーションの無言の縮退

判定を [`ParticleGpuSimulation.hpp`](../../Projects/Engine/include/Engine/Scene/Components/ParticleGpuSimulation.hpp) へ集約し、
`bool` ではなく**縮退理由**を返すようにした。同じ判定を 3 経路が共有する:

- `vfx_lint` — `GPU_FALLBACK` 警告。どの設定が原因かを名指しし、GPU に載せる代替案まで返す
- `vfx_runtime_state` — `simulation.requested` と `simulation.effective` を分けて返す
- Editor — Inspector の Simulation コンボ直下 / グラフノードのバッジ / タイムライン下部の 3 か所

グラフノードの `GPU` バッジは「要求」ではなく「実際に GPU で回るか」で出るようになった。
縮退したノードには `GPU→CPU (sortMode)` のように原因が並ぶ。

### 3-b. 実測コストが AI から見えない

`vfx_runtime_state` に `cost` を追加した。

- `particlePassGpuMs` — Particle パスの実測 GPU 時間 (Editor が表示しているのと同じ値)
- `overdraw` — 重なり枚数の集計。`meanLayers` / `maxLayers` / `heavyRatio` / `overdrawFactor`

overdraw の数値化は、ヒートマップ用の計数 RT を CPU へ読み戻して集計する
([`ParticleOverdrawStats`](../../Projects/Engine/include/Engine/Scene/Systems/ParticleOverdrawStats.hpp))。
読み戻しは GPU 同期でフレームを止めるため常時は行わず、
`vfx_preview` を `view="overdraw"` で実行したときだけ計測する
(担当者が UI から overdraw を見ているときは計測しない — 実時間性が要るため)。

### 3-c. `vfx_optimize_budget` が粒子数しか触らない

`strategy` を追加した。

| strategy | 何をするか |
|---|---|
| `particles` (既定・従来の挙動) | 発生数・上限を比例で下げる |
| `fillRate` | 粒を大きくして枚数を減らす + 寄与の大きい層を遠距離で間引く |
| `both` | 両方 |

`fillRate` の粒サイズ補正は `k^(1/3)` 倍。`√k` にすると総塗り面積が変わらず
fill rate 対策にならないため、意図的に √ より弱くしてある (総塗り面積は `k^(1/3)` 分の 1 へ)。
遠距離での間引きは、各ノードの塗り寄与 (枚数 × 面積) 上位 1/3 だけを対象にする
— 一律に間引くと遠景で「芯だけ残って形が判る」状態まで壊れるため。

どの strategy を使うかは `cost.overdraw` と `cost.particlePassGpuMs` を見て決める、という判断基準を
`vfx_guide` の `budget` カテゴリへ書き足した。

---

## 壁 4 — 表現そのものの上限

ここは「AI が作れない」ではなく「エンジンが出せない」領域。
**「大量に出す」を塞いでいた 2 つは解消済み。残りは絵作りの表現力の話。**

### 4-a. GPU ソート ✅ 解決済み

`sortMode != None` でも GPU シミュレーションのまま走る。CPU 縮退しない。

並べ替えるのは `(キー, 粒子 index)` の対だけで、粒子プールそのものは動かさない
— プールはスポーン用のリングバッファなので、要素の位置が変わると `gpuWriteHead` が
指す場所が意味を失い、スポーンとシミュレーションが壊れる。

実装は bitonic sort の 3 段構成 (`.cs.hlsl` はエントリ 1 本のため 3 ファイル):

| シェーダー | 役割 |
|---|---|
| `ParticleGpuSortKeys.cs.hlsl` | カメラ距離からキーを作る。死亡粒子と 2 のべき乗への詰め物は `0xFFFFFFFF` で必ず末尾へ落とす |
| `ParticleGpuSortStep.cs.hlsl` | 比較距離がグループ幅を超える段。グローバルメモリを往復する |
| `ParticleGpuSortLocal.cs.hlsl` | 比較距離がグループ内へ収まった以降の全段を LDS で一気に回す |

LDS 段が無いと 10 万粒子で 150 回超のディスパッチになり、ソート自体が重くなって本末転倒になる。
描画側は `ParticleRenderCB::gpuSortEnabled` を見て、ソート済み index を経由して粒子を引く。

### 4-b. メッシュパーティクルの GPU 化 ✅ 解決済み

`meshParticlePath` があっても GPU シミュレーションのまま走る。

CPU 経路は粒子 1 個につき DrawCall 1 本 (`MeshTrailRenderPass` が `emitter->particles` を舐める) で、
GPU では CPU 側に粒子配列が無いため描きようがなかった。
`ParticleGpuMesh.hlsl` の VS が `SV_InstanceID` で `StructuredBuffer<GpuParticle>` を引くようにして、
`maxParticles` 個のインスタンス描画 1 本へ畳んだ。ソートも同じ経路で効く。

> 併せて DX11 の `Submit` を「`instanceCount` が 2 以上なら `instanceBuffer` 無しでもインスタンス描画」へ緩めた。
> per-instance データを t0 ではなく t14 から引くため (同じ .hlsl に t0 の albedo テクスチャがある)。

### 4-c. 絵作りの表現力 ✅ 解決済み

| 項目 | 入れたもの |
|---|---|
| **連続リボン** | `particle.trailRibbon` — 履歴点をポリラインとみなし、Trail ノードと同じマイター接合で 1 枚の帯を張る。幅は `trailRibbonWidth` (0 で粒子サイズ)。剣閃・魔法の軌跡・リボン状の炎が作れる |
| **パーティクルの自己影** | `particle.selfShadowStrength` — 光源側で密度 (Σα と Σα·深度) を 1 枚の RT へ積み、Beer-Lambert 則で減衰。厚みのある煙・雲が立体に見える |
| **ボリューム影 / 光の柱** | ボリュメトリックのレイマーチ各ステップで自己影の密度を引く。粒子 1 個の球内部だけでなく雲全体の遮蔽が効き、雲を貫く光の筋が内部に現れる |
| **Distortion の重なり順** | 背景の退避を「歪みエミッターの描画直前」に取り直す。歪みを重ねたとき後ろの歪みが手前へ伝わり、歪みより前に描いた炎・煙も屈折へ入る |
| **デカールの投影品質** | 受け面の法線を深度のスクリーン空間微分から復元し、角度フェード (`angleFadeStrength` / `angleFadeDegrees`) を追加。法線マップの相対ライティングも投影面ではなく受け面基準へ直した |

**残る近似と、その理由:**

- **自己影** — 密度の柱を「総量と平均深度」の 2 値で代表する。前後に離れた 2 つの塊が
  同じ平均を持つケースは区別できない。厳密解はライト方向のスライス分割
  (half-angle slice rendering) が要り、パス数が重なり数に比例して増えるため採らない。
- **自己影の順序依存** — あるエミッターが受ける自己影は「自分自身 + 先に処理された
  エミッター」までしか含まない。単一エミッターの煙・雲 (自己影が最も効く形) では完全に正しい。
  完全にするには全エミッターの形を同時に保持する別バッファが必要になる。
- **自己影は CPU 限定** — 密度パスが CPU 頂点バッファを光源視点で描き直す方式のため、
  `selfShadowStrength > 0` は GPU 縮退する。lint の `GPU_FALLBACK` が名指しする。
- **Distortion の粒子単位順序** — 同一エミッター内で重なる粒子は 1 DrawCall なので
  同じ背景を共有する。1 パス方式の原理的な限界で、粒子ごとに DrawCall を分けると
  「大量に出す」が成立しない。歪みを重ねたいなら別エミッターへ分ける。
- **Decal の法線復元** — 深度の不連続 (シルエット境界) では微分が跳ねる。
  そこは角度フェードで薄くなる側に倒れるだけなので、破綻としては安全側。

いずれも「目的は表現であって物理的正しさではない」という判断で選んだ近似で、
各シェーダーの WHY コメントに同じ内容を書いてある。
評価の基盤 (壁 1〜3) が揃っているので、これらの効果は
`vfx_preview_compare` と `vfx_runtime_state` の `cost` で定量的に確認できる。

### 残る GPU 縮退条件

`sortMode` と `meshParticlePath` が外れ、`selfShadowStrength` が加わった結果、
CPU へ落ちるのは以下になった。

`simulationSpace = Local` / per-particle Trail / SubEmitter (birth・death・collision) /
`prewarm` / `flipbookFrameBlending` / `motionVectorFlipbook` /
`selfShadowStrength > 0` / Depth 以外の collision

**壁 3-a が入っているので、どれに当たっても lint と実行状態が名指しする。**
気づかないまま縮退している状態はもう起きない。

---

## 壁 5 — オーサリング UX ✅ 解決済み

| 項目 | 入れたもの |
|---|---|
| **SubGraph のドリルダウン** | ダブルクリック / 右クリックメニューで中へ入り、Canvas 上部のパン屑で任意の段へ戻る。多段の入れ子も辿れる |
| **Reroute ノード** | `VFXNodeType::Reroute`。実体も時間も持たない配線の中継点。Canvas ではピンだけの最小サイズで描き、Timeline にも帯を出さない |
| **ノードのサムネイル** | Particle / Trail / Decal / Mesh ノードへ素材のサムネイルを出す。`.mat` は albedo を代表画にする。ホバーで 192px へ拡大 |
| **逆参照検索** | Asset Browser の「Find References...」を GUID 参照へ対応させた |

**SubGraph ドリルダウン** は親グラフを dirty のまま置き去りにしないよう、中へ入る前に必ず保存する
(保存に失敗したら移動しない)。戻ってきたときにディスクと編集内容のどちらが正か決められなくなるため。
実際の読み込みは `requestedAssetPath` 経由で Panel が次フレーム先頭に行う
— ここで `LoadGraph` を直接呼ぶと `document.path` と `ctx.selectedAssetPath` がずれ、
Panel が「別のアセットが選ばれた」と誤認して親グラフへ即座に戻してしまう。

**Reroute** は `startOffset` / `duration` を常に 0 として扱う。ここで時間を持たせられると
「線を整理しただけ」のはずの操作でエフェクトのタイミングが変わる。
Inspector も時間欄を出さない (設定できるのに効かない項目を作らない)。

**逆参照検索** は以前ファイル名と「拡張子を除いた stem」で本文を検索していた。
これには 2 つ問題があった: ディスク上の参照は保存時に `guid:<32hex>` へ変換されているため
`.scene` からは 1 件も見つからず、一方 stem 単独の一致は "Fire" のような短い名前が
無関係なファイルへ大量に当たって結果一覧が使い物にならなかった。
GUID 参照とパス参照の両方を探し、stem 単独の一致は採らないようにした。

---

## 評価できない、で終わらせないための注意

指標は**視覚判断の置き換えではない**。輝度ヒストグラムが良くても「炎に見えない」ことはある。
目的は判断の全自動化ではなく、

- 機械的に判る破綻 (白飛び、覆いすぎ、動いていない、コスト超過) を**先に潰す**
- 残った「らしさ」の判断だけを画像と `vfx_guide` の規約に委ねる

という切り分けにある。これは `vfx_analyze_texture` が
「blendMode の推測」を消して「どんな絵にしたいか」だけを残したのと同じ構図で、
AI 連携の設計方針として一貫している。

`vfx_preview_metrics` が `issues` を空で返すのは
「機械的に判る破綻は無い」という意味であって、「良い絵」という意味ではない。
この区別は tool description と応答の `hint` の両方に明記してある。

---

## 次に着手するなら

ロードマップの項目は埋まったので、次は**精度を上げるか、新しい表現を足すか**になる。
どちらも「今のままで何が困るか」から始めること。近似を厳密解へ置き換える作業は
コストが跳ね上がるうえ、`vfx_preview_compare` で差が出ないなら価値も無い。

精度を上げる候補 (いずれも重い):

- 自己影のライト方向スライス分割 — 前後に離れた塊を区別できるようになる。
  パス数が重なり数に比例して増える
- 自己影の GPU 対応 — 密度パスを GPU 粒子バッファから直接描く経路を足す。
  そうすれば `selfShadowStrength` の GPU 縮退が消える
- Distortion の粒子単位順序 — 歪みだけ別 RT へ深度付きで貯めて後段で合成する

**評価の基盤 (壁 1〜3) が揃っているので、これらに手を入れた効果は定量的に確認できる。**
以前は「速くなった気がする」「良くなった気がする」しか言えなかった。

## 生成・評価・採択・知識化の閉ループ

個別の編集・観測機能が揃っていても、元アセットを直接上書きして会話内だけで採否を決めると、
失敗試行を比較できず、成功理由も次回へ残らない。AI 制作は次の固定手順を使う。

1. `vfx_knowledge_catalog` — 組み込みとプロジェクト固有の成功 Template を検索する。
2. `vfx_candidate_fork` — 最も近い `.vfx` から独立候補を作る。
3. schema 駆動編集 — 候補だけを局所変更し、元アセットは触らない。
4. `vfx_candidate_evaluate` — 同じ objective で lint、全区間の露出・占有・ピーク・tail を評価する。
5. 意味評価 — 機械条件を通った候補だけ画像で「狙った演出に見えるか」を採点する。
6. `vfx_candidate_accept` — `accept` の候補だけ本番へ Undo 可能に採択する。
7. `vfx_knowledge_promote` — 成功条件を Graph 名へ残し、Project Template へ昇格する。

機械指標だけで AAA 品質を承認してはならない。`semanticAssessment` が無い候補は
`needs_semantic_review` に留める。これにより白飛びなどの客観的破綻と、
炎らしさ・重量感・ゲーム内での読みやすさという意味判断を混同しない。

知識は会話ログではなく再実行可能な `.vfx` として `Assets/VFX/Templates` に残す。
次回はその成功例を fork するため、制作回数が増えるほどゼロから組む割合が下がる。
