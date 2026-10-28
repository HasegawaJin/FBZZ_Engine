<!-- @file    many-light-sampling.md -->
<!-- @brief   MegaLightsを参考にした固定評価予算の共通ローカルライト描画。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-09 -->
# 大量ライトの重要度サンプリング

参照は[Epic MegaLights](https://dev.epicgames.com/documentation/en-us/unreal-engine/megalights-in-unreal-engine)。重要な光の確率的選択と固定評価予算を既存のRaster/Clusteredへ適用する。UEのRTによる面光源の可視性、独立した直接光デノイザー、Nanite表現をそのまま提供する実装ではない。現在のBRDF・面光源の形状・Cookie・影アトラス・水域の透過率を維持する。

## 契約

- 新モードはForward+/Deferred+で明示的に有効にする。通常の全数評価と比較できる。Sunは従来の方向光経路を使う。
- ローカルライトの転送上限を1024へ拡張する。既存クラスタの65 uintの枠は維持し、新モードだけ先頭に候補総数とプール長を詰め、32個の候補番号と32個の推定倍率を保持する。
- クラスタと重なる全ライトを走査し、正の色・強度とクラスタAABBへの距離二乗から提案分布を作る。距離をrangeで割った弱い近接度だけでは近い光が遠い同出力の光に埋もれるため、`power / (1 + distanceSquared)`を使う。20%の一様成分を混ぜ、重要度が小さい光も確率0にしない。クラスタ中心で見えないSpotを事前に0にしない。
- 32灯以下は候補をそのまま保持する。それを越えると固定32層の系統的な重要度サンプリングを使い、単純な先着64灯の打ち切りを避ける。選択確率の逆数を光へ掛けて元の総量を推定する。
- 多数ライトの画素は隣接4クラスタのプールを双線形の確率で一度選び、その画素の全標本で同じプールを層化する。静止画素のプール選択から時間位相を除き、プール内の均等な時間列を毎フレームのプール切替で崩さない。標本ごとに別のプールから一様に選ぶと層化が失われ、同じ光の重複評価が増える。SAMPLEDのカリング範囲をXY各1タイルぶん重ね、選ばれたプールにもその画素の全寄与ライトを含める。各プールの推定値は同じ総放射を期待値に持つため、境界を滑らかに混ぜても補正を維持できる。
- 画素はプールを固定予算（既定8、1〜32）の互いに重ならない層に分け、各層で受光点の距離減衰・Spotのコーン・正の色と強度・プール倍率を使ったweighted reservoirを作る。PBR 表面は法線・視線・albedo・metallic・roughness による基本 GGX と拡散反射の輝度も用い、鏡面反射に寄与する光を優先する。非 PBR 表面は法線のコサインだけ、霧・粒子・水中積分は幾何提案を使う。球・管の距離は形状の表面から測り、面光源は中心距離による提案とする。
- 各層の選択は95%の受光点重要度と5%の一様分布の混合とし、選ばれた光へプール倍率を混合確率で割った値を掛ける。非物理材質・追加の clearcoat/sheens・提案と異なる面光源も確率0にしない。層長1なら元のプール推定値をそのまま評価する。候補の基本 BRDF と幾何評価は全層合計で最大32件、完全な材質・Cookie・影・水中の光路は選択された固定予算だけ評価する。
- TAAが実効で有効なビューだけ毎フレームのサンプルを変える。TAA無しでは位相を固定し、無条件の時間ノイズを増やさない。リサイズ・視点切替と履歴棄却は既存のTAA契約を使う。
- クラスタの系統サンプルの回転には、クラスタごとの一様な整数回転に基数2の radical inverse の時間列を加える。毎フレーム独立に偏る候補選択を減らし、時間の接頭辞で候補範囲を覆う。上位24ビットだけ浮動小数へ変換し、長時間実行でも浮動小数の大きなフレーム番号から小数部を失わない。根拠は [PBRT §8.6.1](https://www.pbr-book.org/4ed/Sampling_and_Reconstruction/Halton_Sampler)。
- SAMPLEDのTAAは深度・速度・粒子反応による履歴検証を維持し、現フレームの色範囲への制限を外す。同じクラスタの標本は空間的に一定になり得るため、通常の色制限では有効な時間標本が捨てられる。光や材質の急変は既存の履歴重みで追従する。専用の直接光デノイザーではない。
- SAMPLED と TAA が両方有効なビューだけ Composite に露出済み HDR の第2面を追加する。TAA は HDR を平均してから ACES・sRGB・共有の色調整・LUT・明るさ・フェードを適用し、画面出力は従来どおり LDR とする。非線形変換後に明暗の標本を平均すると強い反射の平均が過小になるため、この順序を使う。
- HDR 履歴は既存の TAA 第2面の GBA に保存する。R は表面で `-1-log2(1+depth)`、空で `-65504` とし、LDR の非負対数深度・空の `-1` と識別する。HDR/LDR の切替や有効性の変化では履歴を混ぜない。旧 LDR 霧・clarity・旧水中補正・画面座標を変える効果は従来の LDR 蓄積へ戻す。通常描画には追加の Composite 面を確保しない。
- Deferred の HDR 再構成では現フレームの深度・法線・albedo・roughness・metallic が一致する近傍だけ再利用する。roughness が 0.4 未満の鏡面は 3×3、粗い面は 5×5 の上限とし、法線が約26度以上違う面を混ぜない。GBuffer の深度が主深度と一致しない前景や GBuffer を持たない経路は空間フィルターを省く。光の強度による棄却は行わず、既定8評価と TAA feedback の値を保つ。これは全 HDR に対する限定的な再構成で、独立した直接光履歴や variance-guided な多段デノイザーではない。
- TAAの出力格子は静止させ、現フレームの色・深度・反応だけを投影ジッターへ対応する座標で読む。ジッターを含まない速度から求めた履歴座標へ、投影ジッターをもう一度引かない。静止した線形画像の正負の投影ジッターをGPU回帰で検査する。
- Forward/Deferred、Water、Particleと体積の共通ライト供給へ接続する。水中のPoint/Spotは従来の強い8灯打ち切りから確率付きの評価へ切替え、影とRGB Beerを保持する。
- 水中体積はサーフェスのLight Samplesを各march cellへそのまま掛けない。セル当たり最大2標本を確率補正し、SAMPLEDでは幅・高さ各1/4の積分を深度に従って再構成する。サーフェスを32へ上げても体積の光源評価は増やさない。
- 体積の標本列はmarch cellの距離でも変え、同じクラスタ内で同じ2灯だけを繰り返さない。1/4解像度の再構成は深度を検証する4×4タップを使い、空と物体の境界を混ぜない。
- 水中の光路は、一定水深の矩形内部に全区間が収まり、他水域や局所濁りが干渉しないことを証明できた場合にBeerの解析積分を使う。除外領域の区間を引き、証明できない波面・岸辺・変化する水深は従来の照会に戻る。
- 影アトラスの16タイルと距離による既存選択は維持する。光源数の拡張を影枚数の無制限拡張と結びつけない。
- ClusterCullは、ライトのview位置・到達半径・距離非依存のpowerを64threadのgroupで一度準備し、24KiBの共有メモリで再利用する。全threadが同期してから範囲外クラスタを返す。クラスタごとの距離・候補順・確率和・二回目の昇順選択は従来と同じで、atomicによる順序変更を加えない。

## Authoringと検証

Project SettingsおよびRenderPipelineAssetへ有効化と画素予算を保存する。通常モードと新モード、少数ライトと多数ライト、ライト順序の入替、色・Cookie・遮蔽・水域、Scene/Gameの異なる視点を検査する。既存SDKプロジェクトへ夜のライト比較Sceneを追加し、通常描画とサンプル描画を同じカメラで撮る。計算量の上限と画質の変化は別に記録する。

2026-10-09の検証では、水域・大量ライト・水中・TAAの実GPU回帰87件、水面とRenderPipelineAssetを含むCPU回帰94件が合格した。1,000灯の光量、重要な末尾ライト、静止位相、不均等層、時間蓄積と深度・粒子の履歴棄却、正負の投影ジッター、奇数寸法の1/4体積、動的テクスチャ転送の順序と寿命を確認した。C#ビルドの24件ではPlay中のディスク削除延期とEdit復帰後の所有範囲を検査した。既存TAAはLDR履歴であり、HDRの直接光再構成やRTの大量影はこの実装に含まれない。

## NightLightsLabの実測

RTX 4070・DX12・Release SDK 0.10.0、Game出力1904×893、EditorのScene/Game両ビューを描くhidden batchで測定した。1,000灯・8サンプル、水中の光の筋を有効にしたまま比較する。FPSはlockstepのdelta時間ではなく、完了したアプリケーションフレームの実経過時間の中央値から換算する。Standalone単独ビューのFPSではない。

| 条件 | 中央値 | p95 | p99 | 最大 | 中央値換算FPS |
|---|---:|---:|---:|---:|---:|
| 改善前（24フレーム） | 168.63ms | 179.15ms | 185.53ms | 185.53ms | 5.93 |
| 改善後1（120フレーム） | 16.33ms | 19.14ms | 23.98ms | 25.11ms | 61.24 |
| 改善後2（120フレーム） | 16.83ms | 19.64ms | 22.32ms | 25.25ms | 59.44 |

改善前のwarmupは8、改善後は64フレームで、測定数も異なる。改善後はTAAを有効・feedback=0.9にしている。個別GPUパスの中央値はComposite約53.8→6.8ms、体積光約44.0→1.6msだった。別の途中計測で見つけた約180〜220msの定期停止は、Play中の旧C#世代のディスク削除を延期した後の両120フレームでは再発しなかった。測定ログは`Scratch/NightPerfBaseline`と`Scratch/NightPerfFinalVolume1/2`、比較は`Scratch/NightFinalComparison.json`に保持する。

ゲーム向けの開始値はMany Lights=ON、Light Samples=8、TAA=ON。16〜32は表面のライト評価が増えるため画質比較用に使い、密集した1,000灯の既定として32を推奨しない。NightLightsLabのプロファイルではVolumetric Lightを有効、空気のdensity=0とし、水中の光の筋を維持する。

### 2026-10-10のソース版追加最適化

GBufferの同値材質共有、抽出内の材質準備、ClusterCullの共有入力、一定水深矩形の補完するComposite shader、波紋の零tail省略、不要なbuffer shadow同期の削減を追加した。解像度、1,000灯・8標本、TAA、水中光を維持する。Scene/Gameを毎フレーム描く同じhidden batchで、最終コードの二回の実測は以下。SDKへの追加反映はユーザー指定で保留し、ソースRelease Editorで確認した。

| 条件 | 中央値 | p95 | p99 | 最大 | 中央値換算FPS |
|---|---:|---:|---:|---:|---:|
| 最終ソース1（120フレーム） | 10.756ms | 11.510ms | 12.503ms | 45.577ms | 92.97 |
| 最終ソース2（120フレーム） | 10.020ms | 10.618ms | 11.116ms | 11.216ms | 99.80 |

warmupは両方64フレーム。120FPSの8.33msには未達で、最初の計測には単発45.58msも残った。途中の最速値だけを最終値として扱わない。個別GPU中央値はComposite約3.15〜3.18ms、体積光約1.67〜1.68ms。描画スレッドの追加が同じ共有ツリーで進行したため、この期間の変化率を一つの最適化の独立効果と解釈しない。測定ログは`Scratch/Night120RasterInputs/report.json`と`Scratch/Night120FinalRepeat/report.json`。

最終描画回帰141件、水域・材質・抽出・RenderPipelineAssetのCPU回帰119件、描画順・特殊化・同期・終了寿命のCPU回帰14件が合格した。ReleaseのEditor/Engine/Graphics/Standaloneテストはエラー・警告0。renderer切離し後のFontAtlas/Material解放が終了済みrendererへ触れないことも検証した。GPU回帰は`build/agent/water-reference-gpu.json`、終了と画面の実測は上記playtestの正常終了で確認する。

### 乾燥領域とBeer評価の追加削減

除外boxが1件の光路ではunion走査を省き、交差前後のwet長を直接計算する。Compositeの証明済みownerでは同じ視線とboxの交差を画素ごとに一度だけ求める。光源側の原点・向きはcellごとに変わるため別に評価する。同じownerで正のwet長が完全一致するcellだけBeer透過・散乱重みを再利用し、波・水底・乾燥領域や浮動小数点の差で長さが変われば再計算する。画質設定と積分格子は維持する。

同じRTX 4070・DX12・ソースRelease、1,000灯・8標本、1904×893、TAAと水中光を有効、Scene/Gameを毎フレーム描くhidden batchで測定した。各回64フレームのwarmup後に120フレームを計測し、完成したアプリケーションフレームの実時間を使う。

| 条件 | 中央値 | p95 | p99 | 最大 | 中央値換算FPS | Composite GPU中央値 |
|---|---:|---:|---:|---:|---:|---:|
| 今回の開始時 | 10.661ms | 11.303ms | 11.673ms | 11.869ms | 93.80 | 3.184ms |
| 追加改善後1 | 9.774ms | 10.869ms | 11.112ms | 11.920ms | 102.31 | 2.859ms |
| 追加改善後2 | 9.989ms | 10.682ms | 10.905ms | 11.366ms | 100.11 | 2.546ms |

水中合成のGPU時間は開始時より約10〜20%減った。Editor全体の120FPSには依然未達で、二回の中央値も8.33msを超える。短い測定区間の最大値を継続的な遅延改善の証明とは扱わない。体積光のupsampleで隣接画素のraw texelを共有する試行は、画素の一致は確認できたもののGPU時間の改善が確認できず採用していない。SDK反映は引き続き保留する。

最終コードの描画回帰142件（Underwater44件を含む）とNightLightsLabの16項目が合格し、各プロセスは正常終了した。新規の回転乾燥box比較は、同じboxを重複登録して一般unionを通す参照と比較し、透視・正射影、box内外のカメラ、波高と環境光を検証する。C++のcompile checkとRelease GraphicsStandalone/GraphicsAutoビルドはエラー・警告0。最終実測は`Scratch/NightImproveFinal1/report.json`、`Scratch/NightImproveFinal2/report.json`、開始時は`Scratch/NightImproveBaseline/report.json`、シーン検証は`Scratch/NightImproveScenario/report.json`、描画回帰は`build/agent/water-reference-gpu.json`。

続くGPU形状描画の比較は[GPU駆動の形状描画](gpu-driven-geometry.md)に記録する。NightLightsLabの最終比較では通常描画105.04FPS、Mesh Shader97.71FPS、GPU間接描画70.14FPSとなり、通常描画を既定に維持した。水中合成や体積光の画質を落とす変更は行っていない。

### 2026-10-10の受光点サンプリング改善

クラスタのpowerだけで作ったプールを画素で一様に間引くと、近い光・こちらを向いたSpot・表側の光の取り逃しによる粒状ノイズが残る。クラスタの距離二乗の提案、画素内で共通のプール、受光点のweighted reservoirと確率補正へ変更した。既定8標本と32候補の上限、影・Cookie・BRDF・Beer、体積の2標本と解像度を維持する。

本番CSとPBRで描いた64×64のHDR画像を全灯評価と比較する。下表はTAAや平滑化を使わないRGBの相対RMS誤差で、任意のゲーム画像全体の改善率ではない。

| 比較条件 | 変更前 | 変更後 |
|---|---:|---:|
| 32灯・8評価、遠い同出力の光と近い光 | 59.6% | 5.3% |
| 32灯・8評価、反対向きのSpotと有効なSpot | 61.6% | 6.8% |

近距離の比較は位相0・7・31で5.3〜5.4%となり、同じ位相の画像は画素単位で再現する。32評価では32灯の参照画像と一致する。1,024灯の近距離候補の保持、表側と裏側の光、全Spotの寄与が零の入力も回帰で検査する。変更前の新規2ケースは失敗し、改善後は合格した（[変更前](../../Scratch/ManyLightsQualityBaseline.json)、[受光点改善](../../Scratch/ManyLightsQualityGuided.json)）。

重要度の根拠は[PBRT §12.6 Light Sampling](https://www.pbr-book.org/4ed/Light_Sources/Light_Sampling)、層化は[§8.4 Stratified Sampler](https://www.pbr-book.org/4ed/Sampling_and_Reconstruction/Stratified_Sampler)、選択と確率は[§A.2 Reservoir Sampling](https://www.pbr-book.org/4ed/Sampling_Algorithms/Reservoir_Sampling)。影を含む完全な寄与で全候補を選別する処理や、専用の直接光デノイザーは追加していない。

最終版のManyLight・Water・WaterMedium・Underwater・GraphicsQualityの実DX12回帰139件が合格した（[GPU結果](../../Scratch/ManyLightsQualityFinalRegression.json)）。C++のcompile check、GraphicsStandaloneのReleaseビルド、全シェーダーの355ジョブのうち更新が必要な130ジョブのコンパイルはエラー・警告0（[シェーダービルド](../../build/agent/build-20261010-225402-55732.log)）。規約検査5ファイルもエラー・警告0。検証の一時ファイルはScratch配下へ置く。初回の一時ディレクトリへの書込失敗は、TEMP/TMPをScratch内へ向けた再実行で解消した。

保存済みNightLightsLabをScratchへコピーし、同じソースRelease EditorとRTX 4070で変更前後を実測した。C#世代の適用を確認し、Scene/Gameの両ビュー、Game出力1904×893、1,000灯・8標本・TAA・水中光を有効にした。64フレームのwarmup後、120フレームのアプリケーション実時間は次のとおり。各条件1回の比較であり、単独ゲームビューの性能保証ではない。

| 条件 | 中央値 | p95 | p99 | 最大 |
|---|---:|---:|---:|---:|
| 変更前 | 16.115ms | 17.424ms | 17.953ms | 18.126ms |
| 最終版 | 16.331ms | 17.598ms | 17.826ms | 18.376ms |

中央値の追加コストは約1.3%。[変更前の実測](../../Scratch/ManyLightsQuality/Baseline/report.json)、[最終版の実測](../../Scratch/ManyLightsQuality/Guided/report.json)、[変更前の画像](../../Scratch/ManyLightsQuality/Baseline/Samples8A.actual.png)、[最終版の画像](../../Scratch/ManyLightsQuality/Guided/Samples8A.actual.png)を保持する。SDKへの公開・配置は行わず、ソースとシェーダー生成物を検証した。

### 光源表示を外した室内比較

[ManyLightsQualityLab](../../Samples/WaterLab/Assets/Scenes/ManyLightsQualityLab.scene)は、LightComponentだけの1,000灯が21個の非発光メッシュを照らす保存シーン。8個の近いPoint、4個の下向きSpot、988個の弱いPointを配置し、Spotの2灯だけ影を投影する。粗い白材質・つやのある白材質・金属を並べ、床・壁の拡散光と物体のハイライトを別々に観察する。水と環境光、SSRなどの追加照明を持たず、露出1.0を固定する。

[比較シナリオ](../../Samples/WaterLab/Tests/Playtests/ManyLightsQualityLab.playtest.json)は全灯評価・8標本・32標本を同じ静止カメラで、TAAなし／ありの両方で撮影する。TAAありはモード変更後96フレーム待つ。8標本では4フレーム離れた追加画像も残す。capture自身の2フレームの待機を含むため、frames=1の追加画像を連続フレームと扱わない。

ソースRelease Editor・DX12・RTX 4070で40手順が合格し、1904×893の7枚を撮影した（[結果](../../Scratch/ManyLightsQuality/RoomFinal/report.json)、[全灯＋TAA](../../Scratch/ManyLightsQuality/RoomFinal/04_Exact1000_Taa.actual.png)、[8標本＋TAA](../../Scratch/ManyLightsQuality/RoomFinal/05_Sampled8_Taa.actual.png)）。シーンの構造とエラーログを検査し、画像の合否閾値は置いていない。8標本＋TAAでも壁の粒状感と金属のハイライトが全灯評価と異なる。撮影成功は画質合格の証明ではない。操作は[WaterLabの手順](../../Samples/WaterLab/README.md)に記載する。

### 2026-10-11の素材提案とHDR再構成

PBR の基本 GGX と拡散反射で候補を選び、Composite の露出済み HDR を時間平均してから画面変換する。Deferred の同じ深度・法線・材質だけ空間再構成し、鏡面は3×3、粗い面は5×5を上限とする。時間の候補列を基数2の radical inverse とし、静止画素が同じ重なりプールを使い続けることで、この列を毎フレームのプール切替で崩さない。1,000灯・8評価・32候補とTAA feedback=0.9を維持した。

同じ室内シーンの各実行の全灯＋TAAを参照とし、8標本＋TAAの1904×893 PNGのRGB相対RMS誤差を比較した。全数評価と標本評価の両方をモード変更後96フレーム待って撮影する。以下は固定領域の静止画各1枚の実測で、標本位相は実行ごとに異なる。任意のシーンや長時間の改善率を保証する値ではない。

| 領域 | 変更前 | 最終版 |
|---|---:|---:|
| 画面全体 | 1.745% | 1.486% |
| 左壁 | 1.441% | 0.666% |
| 右壁 | 1.466% | 0.700% |
| 床 | 0.358% | 0.150% |
| 金属球 | 12.502% | 3.723% |

領域座標とRGBの偏り・残差・4フレーム離れた画像の差は[計測結果](../../Scratch/ManyLightsQuality/RoomQualityMetrics.json)に保存する。壁の4フレーム差のRMSは左0.00679→0.00439、右0.00614→0.00459となった。一方、金属領域は0.01590→0.01895で、明るい細い反射のちらつきは残る。静止画の誤差減少を時間方向の全面的な改善とは扱わない。

64×64のHDR GPUテストでは、金属反射の相対RMSが幾何提案60.8%→素材提案6.8%となる。赤と青の1,000灯を32候補・32位相で平均する検証では、位相ごとのプール切替を外して誤差0.490%→0.068%となり、0.4%の閾値を変更せず合格した。通常LDRの蓄積、HDR/LDRの切替、深度棄却、明るさとフェード、材質・法線・深度境界、追加Composite面の確保と解放もGPUで検査する。

ソースRelease Editorの比較シナリオ40項目が合格し、7枚を撮影した（[最終結果](../../Scratch/ManyLightsQuality/RoomTemporalStable/report.json)、[全灯＋TAA](../../Scratch/ManyLightsQuality/RoomTemporalStable/04_Exact1000_Taa.actual.png)、[8標本＋TAA](../../Scratch/ManyLightsQuality/RoomTemporalStable/05_Sampled8_Taa.actual.png)）。C++ compile check、Editor・GraphicsStandalone・シェーダーのReleaseビルドはエラー・警告0、触った17ファイルの規約検査もエラー・警告0。追加のRGBA16F面はこの解像度で各ビュー約13MiB、PBR提案と空間再構成の追加処理を要する。今回の変更後の速度改善は計測しておらず、前節の性能値をこの版へ流用しない。SDK反映は保留する。

最終コードのManyLight・Water・WaterMedium・Underwater・GraphicsQuality・HybridHistoryの実DX12回帰147件がすべて合格した（[GPU結果](../../Scratch/ManyLightsFinalStableRegression.json)）。時間列の検証は閾値を緩めず修正後に再実行し、最後の全回帰にも含めた。
