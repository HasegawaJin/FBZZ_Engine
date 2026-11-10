<!-- @file    water-wave-optics-morph.md -->
<!-- @brief   波アセット・浅水減衰・汎用LODモーフ・水の光学係数の契約。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-07 -->

今回の対象は、既存矩形水域のまま海面の波・浅瀬・格子遷移・光学を拡張すること。水域スプライン、Zone共通メッシュ、地形編集、船体浮力点、局所浅水流体、専用GBuffer/tile SSRは別の段階とする。既存の物理と描画の分離、座標、far ocean、Terrain情報は [water-foundation.md](water-foundation.md) と [water-surface-quality.md](water-surface-quality.md) を維持する。

## 波アセット

`WaterWavesAsset` は組込み `DataAsset` として `.fzdata` へ保存し、既存のCreate・型指定Picker・反射Inspector・Undo・再読込を使う。Waterは期待型を持つ `wavesAssetRef` で参照する。参照が空なら従来の `.mat` にある4基波、設定済みなら専用Assetを波の入力とする。光学・流れ・環境風は引き続き既存の材質とSceneから解決する。

FluidはMath/STLだけに依存し、最大16基波の決定的生成と数値契約を持つ。乱数は指定した整数ハッシュと変換で定義し、同じalgorithmVersion/seed/設定で再生成できる。Assetの初期波数は8、生成器自身が方向分布を持つため方向伴走成分の初期値は0。Waterの実行時基波配列は16、active countの既定は4、従来の方向広がりで最大32成分を評価する。描画LODで物理の波数・振幅を変えない。

位相は `k dot(D,worldXZ) - omega seconds + phaseOffsetRadians`。包絡の位相にも同じoffsetを加える。従来4基波はoffset=0、包絡比の `.09/.125/.16/.195` を保持し、追加基波ではこの4つを繰り返す。CPUの順写像・逆写像・高さ・法線・bounds・cacheと、GPUの変位・履歴泡・残差法線が同じ位相とactive countを使う。

生成した波は個体振幅と風を適用した後に、包絡と方向分散を含む水平Jacobianの保守上限を1未満へ制限する。従来の材質4波の意味は変更しない。数値失敗ではout未変更。設定済みAssetの不正編集では同じ参照のlast-goodだけを保持し、新参照に有効値が無ければ静水と診断にする。NaNを含めた安定した設定署名で診断の反復を抑える。生成は設定変更時だけで、照会中に動的確保を行わない。

GPUは16基波のdir/params、4個のpacked phase float4、active count/明示入力flagを持つfloat4を使う。既存CBを手で作る検証のflag=0は4基波互換、実抽出のflag=1はcount=0も明示的な静水とする。CBのC++/HLSL両ミラー・Preview・選択を同時に更新する。

## 浅水減衰

`waveAttenuationDepth` [world m] は0で無効。Terrain情報から作る不変の数値snapshotをWaterへ持ち、Physicsのcallbackは同じsnapshotを借用する。snapshotにScene・TerrainへのpointerやGPUリソースを残さない。高さ・穴・Component lifetime・Transform・水域・深度設定の変更で失効させる。

底なしは係数1、乾燥した底は0、深さが設定値以上なら1。基準水深から各texelのsmoothstep係数を作り、その係数と勾配を補間する。外周はclampした境界値から有限幅で1へsmoothstepし、係数が突然戻って逆写像に別の枝ができないようにする。幅は `max(world texel幅X,world texel幅Z,1m)`、外周勾配も収縮上限へ含める。符号付きGB16bitのrange外も明示的に扱い、範囲端へ丸めた深い底を浅瀬と誤認しない。GPUとCPUは同じ標本・flags・補間を使う。描画専用depth-bufferの視線距離を物理へ混ぜない。

減衰はGerstnerの垂直・水平変位に適用し、逆写像にも含める。法線と逆写像収束には係数の空間勾配を含める。流れの凹みと局所波紋の意味は維持する。GPU履歴泡・残差法線も同じ減衰を使う。通常のsystem更新とPhysics snapshot準備で最新データを用意し、描画を行わない物理でも機能する。

Terrain のある Horizon では水平変位に上記の減衰を使い、鉛直変位は基準水位の岸で沖の振幅の 65% を残す。地形が基準水位より高い側では、`0.35 × waveAttenuationDepth` を 0.25〜2 m と深度の符号化範囲で制限した高さまで滑らかに減衰させる。波の山と谷で地形と水面の交点が動くため、緩い砂浜では長く、急な岸では短く移動する。描画、岸泡、水中アトラスと CPU の水面照会は同じ水位変化を使う。この包絡は波の遡上を表現する幾何の近似であり、[Stockdon et al. 2006](https://pubs.usgs.gov/publication/70030520) の遡上高推定式や浅水流体の時間積分は含まない。

## 汎用LODモーフ

Surface層はScene・水・RHIを知らず、元のdyadic格子、world/view、密度から座標と連続セル幅を返す。従来の無効設定は選択・16種のcenter-fan IBを維持する。nearDistanceの硬い切替は有効時だけ連続にする。

center-fanの親位相を維持する。奇数辺頂点を直前の偶数へ寄せ、中心 `(2a+1,2b+1)` は `a+b` 偶数で `(2a,2b)`、奇数で `(2a+2,2b)` へ寄せる。各階層の写像は元の格子頂点で重みを評価し、fan三角形内へ線形延長してfineからcoarseへ合成する。共有するcoarse頂点はfiner階層の保持点なので、細かい側・粗い側・角で同じ最終座標になる。

far stripは接線方向だけを親fanに合わせて粗くし、外向き座標を保持する。near内周と同じ接線格子を使い、cornerの座標は固定してstripとの辺を保つ。帯域は同じ共有点で同じ値とする。モーフ後の保守boundsでカリングし、色と選択を同じview-local設定で描く。

通常の密度選択では、省くfine/stitch頂点が親の位置へ到達してからcoarseへ切り替える。予算で強制された選択はqualityLimitedを維持し、被覆と共有辺を優先する。予算変更まで完全なpop-freeを保証するには別途view所有の遷移履歴が必要であり、この制約を隠さない。

## 光学係数

材質の `opticalMode=0` は従来、1はRGB `absorptionCoefficients` / `scatteringCoefficients` [1/m]と `phaseG` [-.95,.95]を使う。非有限値・負係数をGPUへ渡さない。従来のSSR・IBL・屈折・表面Fresnel・泡の照明を維持する。

`sigmaT=sigmaA+sigmaS`、`T=exp(-sigmaT pathLength)`、積分散乱重みは `sigmaS/sigmaT*(1-T)`。0係数は透過率1・散乱0とし、極小係数も安定に評価する。透視投影では視空間Z差を視線のZ成分で割って経路長とし、平行投影では一定の観測方向とZ差を使う。屈折後の実際の底面までの経路は近似である。

位相関数は正規化したHenyey–Greensteinとし、光の進行方向から観測方向へのcosを使う。水面から光源へ向くL、観測者へ向くVに対して `cos=dot(-L,V)`。太陽のradiance単位と1/(4pi)をそろえ、Lambertを重ねて掛けない。空は一様半球からの入射として近似する。光学モード1では従来の色由来murk/SSS/rimの加算を重ねず、消散を二重にしない。当初の係数presetは旧光学モードのまま追加した。Oceanをモード1へ切り替える描画検証と設定は[water-surface-quality.md](water-surface-quality.md)に従う。

## 検証

波生成はseed・範囲・位相・active count・不正値・Asset保存再読込・収束上限を確認する。浅水は底なし・乾燥・符号scale・穴・再追加・変更後の物理snapshotとGPU一致を確認する。モーフは面積だけでなく親triangle集合、非一様重み、複数階層、coarse辺と角、far接続、予算、負scale、小さいscale、途中段階の色/選択coverageを確認する。光学は0消散・経路長による減衰・RGB比・位相符号・完全暗所と旧モードをGPU出力で確認する。

2026-10-07に実装と静的レビューを完了し、波数・位相・光学・減衰・モーフのGPU回帰を追加した。2026-10-08にRelease構成のEngineAutoをエラー・警告0でビルドした ([ログ](../../build/agent/build-20261008-074939-60180.log))。FluidAutoとPhysicsAutoもエラー・警告0でビルドした ([ログ](../../build/agent/build-20261008-075115-43616.log))。GraphicsAuto/GraphicsStandaloneとEditorLauncherもエラー・警告0でビルドし、Shaderのビルド対象は最新状態である ([ログ](../../build/agent/build-20261008-075546-40156.log))。CPU/GPUテスト本体の実行は未実施であり、ビルド結果や以前の基盤テストの合格を、この拡張の動作回帰の合格として扱わない。

- [Epic Water Waves Asset](https://dev.epicgames.com/documentation/en-us/unreal-engine/simulating-waves-using-the-water-waves-asset-in-unreal-engine): 波生成器のauthoring項目。
- [Epic Water Bodies](https://dev.epicgames.com/documentation/en-us/unreal-engine/water-body-actors-in-unreal-engine): 水深による波の減衰。
- [GPU Gems 2 Chapter 2](https://developer.nvidia.com/gpugems/gpugems2/part-i-geometric-complexity/chapter-2-terrain-rendering-using-gpu-based-geometry): 格子モーフと親境界。
- [Epic Single Layer Water](https://dev.epicgames.com/documentation/en-us/unreal-engine/single-layer-water-shading-model-in-unreal-engine): 吸収・散乱・位相と水面合成。
- [PBRT 4: Phase Functions](https://pbr-book.org/4ed/Volume_Scattering/Phase_Functions): 正規化されたHenyey–Greenstein。
