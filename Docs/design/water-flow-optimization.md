# 水面評価と速度場 PNG

## 波の解決

WaterSystem は cellSize の確定後に WaterComponent::PrepareWaveCache を呼ぶ。
4 波 × 2 方向の k・ω・方向・縦横振幅、逆写像の収縮率を保存し、位相と群の包絡だけを座標・時刻ごとに評価する。
縦と横の振幅は別々に作り、従来の乗算順を保つ。公開された waves / cellSize / waveSpread / waveGrouping の直接編集も比較して検出する。
保存対象には含めない。読み取りを並列化する場合は、入力変更とキャッシュ準備を読み取り開始前に完了する。

## 高さの帯

上限は解決済み振幅 × 群の最大倍率、全 surfaceFlow の絶対変位上限、全波紋の振幅 × 1.58 の和。
丸め誤差ぶん外側へ広げる。FluidVolume は上面だけでなく depthLimit 側も帯判定する。
不明な上限は infinity として正確な callback に戻す。完全に沈んだプローブも高さ評価を省略できる。
着水・離水で前回から帯を飛び越えた体は正確な高さを求めてイベントを維持する。
水中の濁りに必要な depth は正確な高さを使うため、CompositePass の早期省略は空中だけ。

## 流れの共有とカリング

FlowFieldFrame.fields は shared_ptr<const vector<ActiveFlowField>>。
収集のたびに別の配列を作り、物理 callback と描画は同じ配列を共有する。
含まれる VectorFieldAsset のポインタは非所有のままなので、アセットのホットリロードをまたいだ利用は禁止。

CPU 粒子はスポーン後・移動前の全位置を含む球で場を選ぶ。GPU は実スポーン位置と速度から球を作り、
重力・合成流速の上限で毎フレーム拡張する。最後の粒子が寿命を迎えるとリセットする。
連続放出が続くと球は広がり続けるが、古い粒子を落とすよりも保守性を優先する。
速度カーブの上限をまだ求めないため、その場合の GPU カリングは無効。
無限の場は残し、Baked は回転した箱との交差を見る。Baked の範囲外にも働く既存のゼロ流速への抵抗は、
省略した全 Baked 場を1本のゼロ Uniform にまとめて維持する。

## 速度場 PNG

新規作成と FluidBaker の出力は PNG。FluidBaker は `<stem>_Velocity.png` を出力する。
速度場の保存・読み込みは PNG + .meta に統一する。.fga は外部ツールからのインポートとして受け付ける。

- RGB = XYZ の速度。復元は `(byte / 255 * 2 - 1) * max_magnitude`。
- 幅 = sizeX、高さ = sizeY × sizeZ。Z スライスを上から順に並べ、x が最速で変わる。
- 保存前に従来のインポートと同じ NormalizeVectorField を適用し、32³・RGBA8 に揃える。PNG の32³は読み込み時に再正規化しない。
- `.meta` の `[vector_field]` に version = 1、size、bounds_min、bounds_max、max_magnitude を保存する。
- `[texture]` は data / srgb=false / compression=None / mipmaps=false。速度場の読み込みは原本のバイトを直接読む。
- 書き戻しでは既存の GUID を保持する。通常のテクスチャ設定保存も `[vector_field]` を保持する。
- `.png` または `.png.meta` の更新で速度場キャッシュと GPU アトラスを更新する。

PNG は容器を画像形式へ統一するもので、色のフリップブックや2次元の Motion Vector とはデータの役割が異なる。
速度場は3次元の流速を保存するため、それらの画像と同じ1枚へ兼用はしない。
