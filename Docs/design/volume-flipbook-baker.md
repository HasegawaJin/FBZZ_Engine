# Volume Flipbook Baker — ボリュームから焼くフリップブックとモーションベクター

炎・煙のボリュームを GPU でレイマーチし、Flipbook アトラスと Motion Vector (MV) アトラスをエンジン内で焼く。
ステップ 1 は速度が厳密に分かる解析ボリュームを入力にし、流体ソルバー (ステップ 2) はボリュームを書く CS を差し替えるだけで載るようにする。

---

## 1. 現状と課題

- MV 付き Flipbook の再生 (Particle.hlsl) はあるが、MV を作る手段が画像からのブロックマッチング (FlipbookMotionVectors) しか無かった。
- その生成器は **符号も単位もシェーダーと逆** だった。生成器は «+移動量 / 探索半径 [px]» を書き、シェーダーは «−移動量 [Atlas UV]» として読む。strength の既定値 1 では Atlas 全幅ぶり逆方向へずれていた。
- 画像から推定する MV は、明るさが変わる炎で破綻する (同じ模様を追えない)。

## 2. ゴール / 非ゴール

ゴール
- MV の保存値の規約を 1 つに決め、テストで固定する。既存の生成器もそれに従わせる。
- ボリュームの連番 → レイマーチ → 速度場から MV → Atlas + .meta → .mat 適用、の流れをエディターで完結させる。
- DX11 / DX12 の両方で、バックエンドを改修せずに動かす。

非ゴール (ステップ 1)
- 流体ソルバー、6-way ライティング、ループのクロスフェード、専用の発光テクスチャ。

## 3. MV の規約

Particle.hlsl は変更しない。シェーダーが実際に行う計算から逆算した規約:

```
d      = コマ N → N+1 で内容が動いた量。Atlas 全体の UV で軸ごと (+U 右 / +V 下)
保存値 = clamp(-d / S, -1, 1) * 0.5 + 0.5 を RG へ
S      = 全コマ・全画素の max(|d.x|, |d.y|)  → .mat の motion_vector_strength
```

シェーダーは `cur += m·b·S` / `next -= m·(1-b)·S` なので、`m·S = -d` のとき `cur = p - b·d`, `next = p + (1-b)·d` となり、
2 コマとも «コマ N+b で内容がある位置» を指す。

- d が軸ごとの Atlas UV なので、非正方の Atlas でも S はスカラー 1 つで足りる。再生速度にも依存しない (コマ単位)。
- 8bit / BC5 ではゼロが 127.5 を表せず、最大 S/255 の偏りが残る (2048 Atlas で約 0.08px)。許容する。
- MV は Frame Blending が無いと効かない (spriteBlend が 0)。Frame Blending を有効にすると GPU パーティクルは CPU へ縮退する。
- 実装: `FlipbookMotionVectorEncoding.hpp`。テスト: `FlipbookMotionVectorEncodingTests.cpp`。

## 4. 全体像

```
VolumeFill.cs  ──►  媒質 (R 密度 / G 温度)  ─┐
 (解析 puff)        速度 (xyz)             ─┤
                                           ▼
                             VolumeRaymarch (PS, 2·tile × tile の RT 1 枚)
                               左: 事前乗算 HDR 色 / 右: 画面空間速度 + 重み
                                           │  次の editor フレームで読み戻す
                                           ▼
                       CPU: 色 → sRGB 8bit / 速度 × Δt → Atlas UV → Dilate → S → Encode
                                           ▼
                       <name>.png (+.meta Premultiplied) / <name>_mv.png (+.meta BC5)
                                           ▼
                                  .mat へ適用 (Undo 可)
```

## 5. ボリュームソース (解析) とステップ 2 の境界

- noise を纏った puff の和。各 puff は並進 + 定軸回転 + 等方膨張のアフィン変換で運ばれる。
  - 物体座標 `y = R(a)^T (x - c(a)) / (s(a) r0)`。密度は y で評価するので、noise ごと剛体的に運ばれる。
  - 軸が固定なので `R(a+Δ) R(a)^T = R(Δ)` となり、flow map `x' = c1 + (s1/s0) R(Δ)(x - c0)` が閉じた式で出る。
- 速度は **コマ間の割線速度** `v = (x' - x)/Δ = M x + m`。回転の大きいコマでも MV が厳密になる。
- 温度は芯で高く、年齢で指数的に冷える。値が場所ごとに変わるだけで移動を伴わないので、MV には影響しない。
- 時間に依存する計算はすべて CPU (`PackVolumeFillConstants`) で済ませ、GPU には puff ごとの行列だけを渡す。
- プリセット: Puff / RisingPlume / Fireball。RisingPlume の Loop は湧く周期を `NΔ / M` に寄せ、puff の見た目を `j mod M` で決めるので、`field(t + NΔ) == field(t)` が厳密に成り立つ。
- 一発もの (Puff / Fireball) は **ベイク全体の長さ D で正規化** する。移動量・膨張・回転・冷却を «D の間にどれだけ進むか» で定義し、Fireball の寿命は D に合わせる。
  - WHY: 秒で定義すると、コマ数や FPS を変えただけで箱からはみ出したり (Fireball は 64 コマで ±4 まで広がっていた)、終盤のコマが空になったりする。
  - RisingPlume は定常状態なので秒のまま (puff が寿命で消えるので、長さを変えても構図は変わらない)。
- **ステップ 2 の境界**: 下流は «u0 に媒質、u1 に速度 [bake 単位/秒]» しか見ていない。ソルバーは同じ 2 枚を書けばよい。

## 6. レイマーチ

- 平行投影、立方体 [-1,1]³ を最大 128 ステップ。影は光源方向へ 16 ステップ。位相は HG。発光は温度 → 4 点ランプ。
- 色: `C += T·(1-τ)·(scatter + emission)`。emission は «不透明な炎の輝度» (Emission × 温度ランプ × 温度²)。
  - 当初は `ρΔ·emission` (密度 × 距離) で積んでいたが、消散係数 k の煙からは実質 emission / k しか出ず、既定の k = 10 で炎がほぼ見えなかった。
  - 輝度が 1 を超えれば RGB > α になるので、Atlas は事前乗算で持つ。
- 速度: 重み `w = T·(1-τ)` (その標本が画素へ寄与する割合) で、画面へ投影した速度を平均する。見えている煙の動きを取る。
- 流速で移流するディテールの倍率は、気体の密度と温度へ同時に掛ける。Fire / Glow は温度が可視光を作るため、密度だけを動かすと発光が静止して見える。液体の等値面には掛けない。

## 7. 状態機械と DX11 / DX12 の読み戻し順序

- 1 editor フレーム = 1 コマ。Tick は «前のフレームで描いたコマを読み戻す → 次のコマを記録する» の順。
- DX12 の読み戻しはコマンドキュー上で行う。同じフレームで描いてすぐ読むと、未提出のリストを飛ばして 1 フレーム前の中身を読む。前のフレームのぶんは提出済みなので、キューの順序だけで正しさが保証される。
- 記録の最後に `SetRenderTarget({})` で RT を切り替える。DX12 はそこで RT をシェーダー読み取り状態へ戻し、読み戻しと ImGui 表示はその状態を前提にする。
- 駆動はパネルの `OnBeforeBegin` (フレーム内、タブが裏でも毎フレーム)。ベイク中はパネルを閉じられない。
- NOTE: ParticlePass の overdraw 読み戻しは同じフレーム内で読んでおり、DX12 では 1 フレーム古い値を返す (今回は未修正)。

## 8. 出力と .mat

- `<project>/Assets/Textures/Generated/VFX/<name>_<preset>_<seed>[_NNN].png` と `_mv.png`。毎回新しい名前にするので再ロードは不要。
- 色: Color (sRGB) / Premultiplied / mip なし / Clamp。exposure を掛けて 8bit へ落とし、.mat の emissiveScale に 1/exposure を入れて戻す。
- MV: Data / BC5 / mip なし / AlphaMode::None / Clamp。
- Apply to Material: albedo, tex5, コマ割り, Frame Blending, Motion Vector, strength = S, emissiveScale, blend_mode = PREMULTIPLIED, 再生モード (Loop なら FPS)。

## 9. Editor / AI

- Tools > Volume Flipbook Baker...。進捗、キャンセル、結果 (S・白飛び率・縁に接触したコマ数)。
- **専用 Viewport は作らない。** ベイクは固定の平行投影で、操作できるのはカメラと光の向きだけなので、
  パネル内のプレビューとギズモで足りる。3D の Viewport が要るのはステップ 2 (発生源や障害物を置く) から。
- **Volume タブ** (焼く前): 再生 / 一時停止・速度、表示 (Color / Alpha / Motion)、背景 (暗 / 明 / チェッカー)、
  大きさ (256 / 384 / 512 / 幅に合わせる)、光の向きの矢印 (逆光 / 順光の表示つき)。
- **構図の警告**: `AnalyzeVolumeFraming` が各コマで «箱の面で切れる» «タイルの縁で切れる» を見積もり、
  該当コマの範囲を文字で出し、プレビューのそのコマを赤枠で囲む。
  puff の «見える半径» (物体座標で 1 + 0.2 × Noise Amplitude、フェードで薄い puff は除く) で判定する。
  厳密な cull 半径で判定すると、既定のプリセットでも警告が鳴り止まない。ベイク時はタイル外周 2px の α を実測して数える。
- **Flipbook タブ** (焼いた後): 焼いた Atlas を GPU に載せ直し、**左 = MV なし / 右 = MV あり** で並べて再生する
  (`FlipbookCompare.hlsl`。warp は Particle.hlsl と同じ式)。再生 FPS (焼いた FPS へ戻すボタンつき)、
  コマのスクラブ、MV の強さの倍率 (1 が規約どおり、2 で行き過ぎ) を持つ。焼き終えると自動でこのタブへ切り替わる。
- **Preview as VFX**: 選んだ .mat を使うエミッター 1 つだけの `.vfx` を `Assets/VFX/Generated/<mat>_Preview.vfx` へ書き、
  `asset.open` で Prefab 編集モードとして開く。実際の ParticlePass で再生される姿を確かめる最終確認。
  - VFX Editor の別プロセスと VFXPreview パネルは既に廃止されている (vfx-prefab.md §8)。独立したパーティクルの
    プレビュー世界は無いので、既存の Prefab 編集モードを使う。
  - .vfx は PrefabSerializer ではなくテキストで書く。PrefabSerializer は実在の Scene から保存するため、
    使うと編集中のシーンへ一時的な GameObject を足して消すことになり、Undo と «変更あり» 表示を汚す。
- `vfx.generateMotionVectors` (AI) は `materialPath` を受け取ると、生成した MV と S を .mat へ書き込む (Undo は .mat の書き換えだけを戻す)。S は生成結果でしか分からないため。

## 10. 既定値の根拠

GPU の計算 (VolumeFill / VolumeRaymarch) を CPU に写したシミュレーター (40³ 格子、64px タイル、5 コマ抽出) で詰めた。
指標は «箱からのはみ出し»・«タイル縁への接触»・«露出後の明るさの分位点と白飛び率»・«発光だけの明るさ»・«1 コマの最大移動量»。

| 項目 | 調整前 | 調整後 | 結果 |
|---|---|---|---|
| RisingPlume の上端 | y = 1.24 (箱の外で頭が切れる) | 下端 -0.78 / 上昇 0.42 / 寿命 2.2 | y = 0.87 |
| Fireball の広がり | ±4 (タイル縁の 33% に接触) | D で正規化、寿命 = D | ±0.92、縁の接触 0 |
| Puff / Fireball のコマ数依存 | コマ数で構図が変わる | D で正規化 | 32 / 64 / 128 コマで同じ範囲 |
| 発光の式 | ρΔ·E (発光だけの最大 0.03) | (1-τ)·E | 炎の芯が見える |
| Plume の冷却 | 0.5 秒 (発光の最大 0.22) | 0.8 秒 | 発光の最大 0.59 |
| Exposure | 0.5 (最明部 0.57〜0.70、階調の 3〜4 割が未使用) | 0.8 | 煙の最明部 0.93、白飛び 0〜0.36% (炎の芯のみ) |
| マテリアルの motionVectorStrength 既定 | 1.0 (新しい規約では Atlas 全幅ぶりずれる) | 0 (warp しない) | 生成器が S を入れる |

1 コマの最大移動量 (256px タイル換算): Plume 約 6.4px / Fireball 約 4.7px / Puff 約 2.3px。MV の効果が見える範囲に収まっている。

## 11. 実装順

1. MV の規約 (Encoding) とテスト
2. 既存生成器の修正と呼び出し側 (Inspector / AI bus / MCP)
3. PNG / .meta 書き出しの共通化 (FlipbookImageIO)
4. 解析ボリュームとテスト
5. シェーダー (VolumeFill.cs / VolumeRaymarch)
6. Baker とパネル
7. 流体ソルバー (ステップ 2)

## 12. 採らなかった案

- **MRT + 添字付きの読み戻し**: バックエンド 2 つの改修が要る。横並びの RT 1 枚で足りる。
- **RT 2 枚**: GPU 待ちが 1 コマあたり 2 回になる。
- **CPU レイマーチ**: テストはしやすいが、ステップ 2 の GPU ソルバーと繋ぐには 3D テクスチャの読み戻し (DX12 の状態管理の改修) が要る。
- **16bit PNG の MV**: BC5 で再び量子化されるので意味がない。
- **光の透過率ボリュームを先に CS で作る**: 影マーチの方が単純で、ベイクは速度を要求しない。
- **シェーダー側を旧生成器に合わせる**: シェーダーのコピーが 5 つのアセットツリーにあり、既存マテリアルの strength の意味も変わる。

## 13. ステップ 2 へ

- Stable Fluids (移流・浮力・圧力投影・渦度閉じ込め・温度/燃料) を VolumeFill.cs の位置に置く。媒質と速度の 2 枚を書くだけで、以降はそのまま流れる。
- 解析ボリュームは、ソルバーが出す MV の妥当性を比べる基準として残す。
