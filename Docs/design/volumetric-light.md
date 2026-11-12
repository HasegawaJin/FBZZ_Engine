<!-- @file    volumetric-light.md -->
<!-- @brief   明示した PostProcess 設定による体積光の積分・再構成と品質予算。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-03 -->
# VolumetricLight — 半解像度の積分と深度を考慮した再構成

`VolumetricLight` は不透明面までの視線をシャドウマップと同じ雲密度場で遮蔽し、散乱光を HDR へ加算する。重いレイマーチを縦横半分の解像度へ移し、全解像度の深度を使って輪郭を保ちながら戻す。積分ステップや光の設定を下げて速度を得る設計にはしていない。

## 有効条件と品質予算

大気・水中とも、PostProcess の `volumetricLight.enabled` と最初の pass override が明示的に許可した場合だけ実行する。光学水域の存在、カメラの水中判定、Water 材質の光強度では起動しない。強度または距離が非有限・零以下、steps が零以下の場合も処理しない。`RenderSettings::IsVolumetricLightActive()` をパスの判定、実行時の防御、資源確保、Composite の読取、GPU 定数の共通条件にする。

オフ時は VolumetricRaw / VolumetricResult を確保せず、同じ画面寸法でのオン→オフでも解放する。RenderGraph に積分の実行と結果の read を残さず、前フレームの散乱結果を読まない。水の RGB 消散と環境光、水面・界面・物体照明はこの体積光の opt-in と独立に維持する。

水中の積分ステップは材質側の上限と PostProcess の steps (16〜64へclamp) の小さい方、カメラからの積分距離は材質側の上限と PostProcess の maxDist の小さい方を使う。材質の水中光強度へ PostProcess の intensity を一度だけ掛ける。RGB 光学係数・位相は水域の設定を正本に保つ。オン時の品質変更はこの明示した予算に従い、解像度や多数ライトのモードから黙って steps を下げない。

共有水域の積分では、header の保守的な XZ / 垂直外接範囲と ray が交差しない画素を積分前に除く。交差する ray は元の stepDistance・ディザ位相・cell 添字を保ち、水を含まない前後のセルだけを走査から外す。両端は一セル広げ、部分セルを落とさない。完全なジオメトリの影では寄与が零なので、雲と光源側の媒体積分を省く。透過率の小ささによる積分全体の打ち切りは行わない。

## リソースと処理順

`RenderResources` がビューごとに次の 2 枚を持つ。`ViewPipeline` の登録名と `VolumetricLightPass::Setup` の書き込み宣言を一致させる。

| 登録名 | 寸法 | 内容 | 形式 |
|---|---|---|---|
| `VolumetricRaw` | 通常は各軸`ceil(size / 2)`、Many Lightsは`ceil(size / 4)` | RGB = 積分後の散乱光、A = 代表画素の線形深度 | RGBA16F |
| `VolumetricResult` | `width` × `height` | RGB = 再構成した散乱光、A = 1 | RGBA16F |

1. `VolumetricLight.cs.hlsl` が半または1/4解像度で積分する。
2. `VolumetricLightUpsample.cs.hlsl` が全解像度の HDR 深度を読み、半解像度は4 tap、1/4解像度は16 tapで再構成する。
3. 既存の `CopyColor.hlsl` を加算ブレンドで HDR へ描く。

bindless経路の積分と再構成は、一つの同期native operation内で順番にTryDispatchする。RenderThreadへ渡す完了待ちを二回から一回へまとめるが、戻り値は両方が実際に記録された結果であり、非同期受付を成功にしない。初段失敗では後段を呼ばず、どちらかが失敗した場合は水中結果の公開と大気HDR加算を行わない。既に記録した初段はrollbackせず、その出力を合成へ公開しない。各dispatchのUAV/SRV遷移と既存の散乱量・解像度・step予算を維持する。非bindlessの互換経路は従来の二回Dispatchを使う。

不透明描画と Terrain の後、`Water Forward` の前に加算する。レイ終端は **HDR の深度**から取得する。GBuffer の深度には Terrain が含まれないため、そこを読むと地形の奥まで積分した光が手前へ漏れる。水底まで積分した光を透明描画後の Composite で足すと、水面より手前へ乗るため、現在のパス順を保つ。

HDR の深度を SRV として読みながら同じ HDR の DSV を書き込み可能な状態で束縛しない。再構成は RTV / DSV を外す `Dispatch` で実行し、深度を読まない加算描画の前に HDR を束縛し直す。追加の深度コピーとバックエンド固有の読み取り専用 DSV は要らない。既存の全解像度 `VolumetricResult` を再利用するので、増える UAV は半解像度の 1 枚だけ。

カメラが光学水域の外にあるビューでは、水域がシーンにあっても空気中の散乱を計算する。空気の積分は不透明面か最初の可視水面の手前で止め、Water Forward より前に HDR へ加算する。水中の散乱を使う場合は同じ一時テクスチャを再利用して積分と再構成を行い、その結果を Composite が読む。水中カメラでは空気中の積分を省く。Froxel Fog を切っても、`VolumetricLight` と方向光のシャドウマップが有効なら、葉のアルファテストで開いた隙間に光が通る。

## 代表画素と深度 A

半解像度の画素 `(x, y)` は元画像の `(2x, 2y)` から始まる 2×2 区画を担当する。区画内の **最大 Reversed-Z**、つまり最も手前の不透明面を選び、選択した元画素の中心 UV からレイを復元する。深度とレイの UV を別々の画素から取らない。奇数寸法では区画内の座標を元画像の端へクランプし、最後の行と列を落とさない。

raw A は `viewZ / max(farZ, 0.001)`、深度がクリア値のままの空は **-1** とする。カメラ定数は有限の `0 < nearZ < farZ` を前提とする。透視と平行投影の両方で `LinearizeDepth` を使い、再構成時は A に同じ farZ を掛けて viewZ へ戻す。

Reversed-Z を RGBA16F の A へ直接保存すると、遠方の小さな値が 0 へ丸まり空と区別できなくなる。線形深度の正規化は遠方でも値を保ち、-1 は半精度で正確に表現できる。再構成側は `A < 0` で空を判定するため、極端な near / far 比で正の深度が半精度の最小値を下回っても空には化けない。ただしその領域の深度精度は失われる。通常の半精度の相対量子化誤差は後述する 2% の深度幅より十分小さい。

raw A は診断用の深度であり、加算のブレンド係数ではない。`CopyColor` の加算は source alpha を使うため、全解像度の出力 A は必ず 1 にする。強度は積分の最後に一度だけ掛ける。

## 再構成フィルター

元画素の中心を 2×2 区画の格子へ写し、隣接する 4 個の raw 画素を取得する。奇数寸法でも物理的な区画の対応を保ち、raw の UV を単純な寸法比で引き伸ばさない。

空と不透明面は別の集合として扱い、互いの tap を除く。同じ集合の tap にはバイリニアの空間重みと次の深度重みを掛け、残った重みの和で正規化する。

```text
depthWidth = max(targetViewZ * 0.02, 0.001)
depthWeight = exp2(-(abs(sampleViewZ - targetViewZ) / depthWidth)^2)
```

対応する面がなく重みの和が `1e-5` 以下になった場合、同じ集合で最も深度の近い tap のうち、積分終端が `targetViewZ + depthWidth` 以内のものだけを代用する。該当しなければ散乱光を 0 にする。これにより細い手前の輪郭へ背景の長い積分を足すことを抑える。

このフィルターは形状を全解像度で再積分するものではない。2×2 区画で選ばれなかった細い隙間、極端な深度傾斜、手前の面が tap をすべて占める背景画素では散乱光が欠けることがある。半解像度では高周波の影の線も変化する。専用の履歴バッファは追加せず、既存の TAA ジッターと後段の TAA を使う。TAA 無効時は固定ディザであり、時間を変えてノイズを平均する方式ではない。

## 積分と設定

大気の `VolumetricLight` では、正の位相係数 `g` に応じて太陽方向の散乱を最大 2 倍まで強める。倍率は視線と太陽方向の内積の正の部分の二乗に沿って変化する。等方散乱 (`g = 0`) の倍率は 1。水中では材質の位相係数を使う。`Froxel Fog` の `Light Scattering (×)` は、影を適用した太陽・ローカルライトの散乱に掛かる。既定値は 2、基準は 1。密度・背景透過率・環境光・自己発光は個別の設定を使う。

半解像度への移行では `volSteps`、散乱の位相関数、密度、距離、高さによる減衰、色、強度、雲の密度場を保つ。設定は従来どおり 16〜128 ステップへクランプする。ShowcaseCoast が参照する WaterTest の設定は 48 ステップ。

影は shader 内の `VolumetricGeometryVisibility` で評価する。カスケードの選択・境界ブレンドは地表と同じ helper を使い、比較は固定 4 tap の hardware linear PCF とする。atlas の各 tap を tile の内側へクランプし、線形比較の footprint が別のカスケードを読まないようにする。カスケード境界のブレンド領域では次の tile の 4 tap も評価する。設定が radius 3 の地表は 49 回の比較を保ち、体積光だけを 4 回へ減らす。

散乱点には面法線がないため、上向きの仮の法線を使った slope bias を除き、`shadowBias` / `CascadeBiasAt` の固定 bias を使う。錐台外は可視として扱う。`shadowStrength` は地表の影の濃さの設定であり、体積光は `ShadowPass.cpp` の契約どおり生のジオメトリ遮蔽を読む。このため地表の影の濃さが 0 でも caster を省かない既存の条件と一致する。

地表用の 2D `SampleCloudShadow` は重ねない。これを雲の 3D 密度場に基づく `FBZZCloudShaftTransmittance` と同時に掛けると、形の異なる 2 種類の雲影が二重に光を減らす。3D の透過率、weather / shape の取得、48 ステップの積分は維持する。変更前に比べ雲の二重減衰が消え、体積光が明るくなる場合がある。固定 4 tap では地表の大きな PCF kernel と同じ影の柔らかさは保証しないため、雲の光芒に加えて細い caster の影とカスケード境界も画像で確認する。

透過率の小ささを理由とした積分の早期終了は追加しない。打ち切り後に残る光の上限を示せない変更は、画面全体の明るさを変える可能性がある。

## ピクセル数とメモリ

| 出力寸法 | 積分する raw 寸法 | raw ピクセル数 | 元の積分ピクセル数 | 追加 raw メモリ |
|---|---|---:|---:|---:|
| 1548×871 | 774×436 | 337,464 | 1,348,308 | 2,699,712 bytes / 2.57 MiB |
| 1920×1080 | 960×540 | 518,400 | 2,073,600 | 4,147,200 bytes / 3.96 MiB |
| 3840×2160 | 1920×1080 | 2,073,600 | 8,294,400 | 16,588,800 bytes / 15.82 MiB |

偶数寸法の ray 数は元の 1/4。1548×871 では 25.0287% となり、48 ステップの積分サンプル数は 64,718,784 から 16,198,272 へ減る。再構成と加算は全解像度なので、GPU 時間が厳密に 1/4 になるという保証ではない。メモリ表は各ビューの raw texture だけで、既存の全解像度出力やリソースの配置・アラインメントは含めない。

## GPU 計測と画像確認

1. 同じネイティブ GPU、出力サイズ、シーン、カメラ、設定、固定 dt と撮影フレームで変更前後を測る。シェーダーの最適化レベルも揃える。DX12 の Debug runtime compile は `-Od`、Release は `-O3` のため、事前コンパイル CSO と runtime compile の混在ではパスの変更以外の差が生じる。WARP は GPU 性能比較に使わない。C++ は `Tools/AgentBuild.ps1` で検証し、ビルドを同時に走らせない。
2. [AI 検証ループ](ai-verification-loop.md) の Playtest から `profiler.snapshot` を複数回取得する。起動・シェーダーの初回準備・リサイズ直後を除き、ウォームアップを揃える。
3. `gpuRenderPasses` の `name == "VolumetricLight"` かつ `available == true` の `elapsedMs` を読む。GPU timestamp は非同期に完了するため、`physicalFrameSerial` が同じ標本を重複して数えない。`source.viewId`、`width`、`height`、`sceneGeneration`、`planGeneration`、`resourceEpoch` を保存し、対象ビューを揃える。`cpuRenderPasses` や lockstep の `frameMs` を GPU 時間の代用にしない。
4. このパスの 2 回の Compute と加算描画は同じ `VolumetricLight` の timestamp 内で実行する。再構成のコストを計測から外さない。複数標本の中央値と範囲、シナリオのレポートを保存する。
5. 同フレームの画像で地形の輪郭、島と空の境界、細い隙間、水面の重なりを比較する。必要なら [Render Pass Viewer](render-pass-viewer.md) の `VolumetricRaw` / `VolumetricResult` と HDR を確認する。キャプチャの診断描画は性能計測と分ける。

| 段階 | 条件 | VolumetricLight GPU 中央値 | 標本・画像・レポート |
|---|---|---|---|
| 変更前 | 既存 CSO、最適化フラグ未記録 | 14.455 ms | 12 標本、14.054〜19.607 ms、`Scratch/WaterLightingBaseline/report.json` |
| 半解像度＋深度再構成 | Debug runtime compile、`-Od` | 10.644 ms | 12 標本、10.583〜11.714 ms、`Scratch/WaterLightingHalf/report.json` |
| 半解像度＋固定 4 tap の体積光専用影 | Debug runtime compile、`-Od` | 0.877 ms | 12 標本、0.863〜1.141 ms、`Scratch/WaterLightingFinal/report.json` |
| 保存したシナリオで再確認 | 上記と同じ CSO、`-Od` | 0.829 ms | 12 標本、0.819〜0.884 ms、`Scratch/WaterLightingRegression/report.json` |
| 遠景の波・地形影の修正後 | Debug runtime compile、`-Od` | 0.889 ms | 12 標本、0.833〜1.369 ms、`Scratch/WaterFarFinal/report.json` |

2026-10-03、DX12 / NVIDIA GeForce RTX 4070、GameView 1548×871、viewId 1、sceneGeneration 7、planGeneration 1。
ShowcaseCoast の保存カメラ `[0,220,-1200]` を使い、60 フレームのウォームアップ後、10 フレーム間隔で取得した。
全標本は `gpuProfiler.available/complete` と pass の `available` が true。計測区間に画像読み戻しを入れていない。
変更前 CSO の最適化フラグは保存していないため、その行との比率を厳密な同一コンパイル条件の高速化率として扱わない。
半解像度段階と固定 4 tap 段階は同じ `-Od` であり、影の分離による差を比較できる。
最終パスの値には全解像度の再構成と加算合成を含む。画像は近景、時間経過、旋回、移動、岸辺を撮影した。
`GreenWare/Tests/Playtests/WaterLighting.playtest.json` の 63 ステップを完走し、遠景を含む 11 枚を撮影、シェーダー診断 0 件を確認した。

## 2026-10-10 の有効条件の検証

- C++ compile check と Development の Graphics / GraphicsStandalone / EngineAuto ビルドはエラー・警告0。関連CPU回帰84件と実DX12のUnderwaterShaderTest36件が合格した。
- 共有水域の外接範囲を緩くした入力と厳密な入力で、元の積分格子を保つ区間削減の画素結果が一致した。透視・平行投影と完全な水域外の視線を検証した。
- `NightLightsLab` の1,000灯を隔離コピーで実行し、コピー側だけPostProcessのVolumetricLightを明示的にオフにした。Release SDKPreview / RTX 4070 / Game view 1904×893、全数評価と32サンプル評価で61手順が合格した。
- 各モード60フレームのウォームアップ後、10フレーム間隔で12回ずつ採ったGPU snapshotは24件すべてavailable / complete。804件の描画パス記録にVolumetricLightは0件で、水面・媒体・Compositeは継続していた。元入力と検証コピーのScene / profileの署名も保持した。
- 不在のパスへ仮の0msを付けず、GPU実行の不在として記録する。オン時の同条件の高速化率はこの検証から主張しない。

記録: [CPU結果](../../build/agent/test-20261009-235306-66448.log)、[実GPU結果](../../Scratch/VolumetricLightFix/GpuResults.json)、[NightLightsLab結果](../../Scratch/VolumetricLightFixCheck-01a120b5/NightVolumetricOff.report.json)、[GPU記録の検査](../../Scratch/VolumetricLightFixCheck-01a120b5/VerificationSummary.json)、[検証SDKビルド](../../build/agent/build-20261010-000907-56448.log)。

## 2026-10-10 の同期統合の検証

積分と再構成の実記録確認を一回の同期操作へまとめ、ThreadedRendererのpending commandも同期operationと同じpacketへ載せる。待機中のpumpが追加したcommandはFlushのdrainループで完了まで回収する。散乱・解像度・step・Light Samplesは変更していない。

Release / DX12 / RTX 4070、隔離したNightLightsLabの1,000灯、sampled-8、CPU geometry、Scene/Game各1904×893、96フレームのwarmupと600フレームの測定。同じCore/Engine DLLとshaderを固定し、同期だけを従来方式へ切り替えた比較版を測ってから最終実装へ復帰した。

| 同期方式 | frame中央値 | p99 | 最大 | VolumetricLight GPU中央値 | VolumetricLight CPU中央値 |
|---|---:|---:|---:|---:|---:|
| 分離した同期の比較版 | 15.213 ms | 22.436 ms | 27.169 ms | 1.614 ms | 0.276 ms |
| 統合後の最終実装 | 14.502 ms | 22.881 ms | 40.345 ms | 1.614 ms | 0.197 ms |

frame分布は600フレーム。CPU pass値は測定後の16個の詳細snapshotにおけるScene/Game合計、GPU値はGameの8標本であり、frame最大値と同一フレームの内訳ではない。同期全体のCPU中央値は3.957→2.607 ms、packet提出回数の中央値は82→50。Syncと内側のSubmitPacketはinclusive値が重なるため加算しない。

中央値は約4.7%改善したが、p99と最大の改善は確認できなかった。別の統合後2回も中央値14.581 / 14.629 msだった。以前のEditor DLLでは中央値12.615 msの記録もあり、共有Core/Engine等の更新を含むため今回の同期変更だけの比較に使わない。RenderThreadが40 msのスパイク原因であるとは断定できず、120 FPS達成もこの結果からは主張しない。

最終ソースへbyte一致で復帰し、EditorLauncher / GraphicsAuto / GraphicsStandaloneのReleaseビルドがerrors=0 / warnings=0。同期・水中pass lifecycleのCPU32件、ThreadedRendering / WaterMedium / Underwaterの実GPU67件が合格した。SDK反映は保留。

記録: `build/agent/build-20261010-175526-40180.log`、`build/agent/test-20261010-175737-26112.log`、`build/agent/water-reference-gpu.json`、`Scratch/VolumetricLightFixCheck-01a120b5/VolumeLegacyCurrentCore/NightVolumetricOn600.report.json`、`Scratch/VolumetricLightFixCheck-01a120b5/VolumeCoalescedMatchedFinal/NightVolumetricOn600.report.json`。

## 2026-10-10 の再開後のスパイク調査

600フレームの無問い合わせ性能窓とは別に、300フレームの詳細snapshotを採取した。遅い診断フレームではLightRangeDebugの自己時間5～6 ms、Scene/GameのMaterial抽出、同期のpacket完了待ちが目立った。VolumetricLightのCPU scopeは中央値0.199 ms、最大0.673 msだった。snapshot問い合わせとJSON生成の負荷が診断区間へ入るため、性能窓の35 ms前後のフレーム原因へ直接対応付けない。GPU timestampにも2フレームのageがあり、同一フレームのGPU原因とは断定しない。

ライト範囲の円周係数と隣接頂点を再利用し、全ライト・24分割・形・色を維持した。[デバッグ描画の設計と計測](debug-draw-performance.md)を参照。共有ソース固定で旧方式へ一時復帰した比較では、frame中央値13.710→12.635 / 12.776 ms、LightRangeDebug CPU中央値2.563→1.618 / 1.471 ms。VolumetricLight GPU中央値は約1.61～1.62 msを維持した。再確認で最大39.572 msが残るためスパイク解消と120 FPS達成は主張しない。

本変更はC++のデバッグ円生成だけで、水面・水中のshaderや品質予算は変更していない。ライトのpixel不変計算を共有するshader候補はScratchで準備したが、実GPU A/B未実行で本番へ採用していない。CPU35件とRelease Editor/GraphicsAutoビルドが合格。別チャットの共有GPU fixtureの検証状況は、本変更の検証結果と区別する。

## 実装と根拠

- 所有・登録: `Projects/Graphics/include/Graphics/Pipeline/RenderResources.hpp`、`Projects/Graphics/src/Pipeline/RenderResources.cpp`、`ViewPipeline.cpp`。
- パス: `Projects/Graphics/src/Passes/PostProcess/VolumetricLightPass.cpp`。
- 積分・再構成: `Assets/Shaders/PostProcess/Lighting/VolumetricLight.cs.hlsl`、`VolumetricLightUpsample.cs.hlsl`。GreenWare 側にも同じコピーを置く。
- [NVIDIA — Fast, Flexible, Physically-Based Volumetric Light Scattering](https://developer.download.nvidia.com/assets/gameworks/papers/Fast_Flexible_Physically-Based_Volumetric_Light_Scattering.pdf)、Apply Lighting / Composite Results: 低解像度の照明を bilateral upsample し、加算合成する構成。
- [NVIDIA — Depth Precision Visualized](https://developer.nvidia.com/content/depth-precision-visualized): Reversed-Z と深度の精度。視空間深度の復元は既存 `Common/Space.hlsli` を使う。
- [Microsoft — SampleCmp](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-to-samplecmp): 比較 sampler の線形フィルターは深度そのものではなく比較結果を混ぜる。既存 shadow sampler の設定を使う。

参照 URL は対応する C++ / HLSL の実装直近にも `@see` として残す。
