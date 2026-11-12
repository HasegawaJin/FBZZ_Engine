<!-- @file    water-surface-quality.md -->
<!-- @brief   白波・遠景海面・Terrain 水深情報の描画契約。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-07 -->

白波は Gerstner の正の変位を振幅上限で正規化した高さ帯から発生させ、水平微分の圧縮で増幅する。`crestFoamThreshold` と `crestFoamFade` は高さ比、`crestFoamStrength` は被覆率の倍率、`crestFoamLifetime` は秒単位の減衰時間である。後者が 0 なら現在の波頭だけを評価し、正なら同じ位置で過去の波を 3 回評価して指数減衰した最大被覆率を残す。これは描画上の履歴近似であり、泡を流す物理シミュレーションではない。

波の変位・透過光の高さ比と泡の被覆率を別々に保持する。泡は `foamColor` の線形アルベドと `foamRoughness` による拡散光・空反射・鏡面光を受ける。暗所で自己発光させない。岸・着水・流れの泡は既存 `foamStrength` を使い、白波の強度と独立に調整できる。波と解析微分の根拠は [GPU Gems, Chapter 1](https://developer.nvidia.com/gpugems/gpugems/part-i-natural-effects/chapter-1-effective-water-simulation-physical-models) を参照する。泡の閾値と履歴は本エンジンの見た目の近似である。

`farOceanEnabled` が有効な合成形状を持たない Rectangle は Horizon 水域となり、水平の所属をカメラに依存せず全 XZ へ拡張する。ビューの far clip とカメラ周辺の光路を包む矩形を海面と水中アトラスで共有する。海面の LOD ルートはこの矩形を含む 2 の累乗幅の格子とし、各軸の位置を幅の 1/4 単位で揃える。ルートが移動しても細かい階層の頂点格子はワールドに揃う。海面・選択・界面・水中アトラスはメインカメラからパッチ LOD を選び、各パスの視錐台で描画範囲を絞る。

`farOceanCoverageScale` はカメラの視錐台を覆う余裕、`farOceanRippleFadeDistance` は水域の外へ出る局所波紋が消える世界距離 [m]。海面有効時は有限面の外周透明化を止め、局所の岸マスクと波紋を遠景へ反復しない。浮力・水中・水域照会は同じ Horizon 所属を使う。`buoyancyDepth` と除外領域は維持し、Finite へ戻すと保存した Extent X/Z の所属を使う。Spline と WaterZone の合成形状は Horizon 指定があっても有限輪郭のままとする。Inspector にその契約を表示する。遠景を設ける考え方は [Unreal Engine Water Mesh](https://dev.epicgames.com/documentation/en-us/unreal-engine/water-meshing-system-and-surface-rendering-in-unreal-engine) を参考にするが、FBZZ の幾何と材質は同じ共有パッチで接続する。

Horizon では沖合も同じ共有パッチで細分化する。Inspector の `Surface Detail` はパッチの解像度、細分割の段数、パッチ数の上限をまとめて設定する。

| Surface Detail | Patch Resolution | Detail Levels | Patch Budget |
|---|---:|---:|---:|
| Low | 8 | 8 | 512 |
| Balanced | 16 | 9 | 1024 |
| High | 32 | 10 | 1024 |

三つの保存値が表のいずれかと一致する場合はその名前を表示し、個別設定の組み合わせは `Custom` と表示する。プリセットを選んだときだけ三つの値をまとめて変更する。`Advanced LOD` では各値と、描画密度を決める `Screen Cell Width`、`Near Detail Range`、`Near Detail Size` などを個別に調整できる。保存済みの Extent は局所波紋と水面材質の UV の基準に使う。

CPU の高さ照会・浮力は描画セル幅に依存しない実効波を使う。GPU はセル幅で表現できない変位を落とし、画面で表現できる分を残差法線へ渡す。近景の接触精度は実セル幅で評価する。遠景やパッチ予算で粗くなった箇所では CPU の波との高さに差が出る。

地形情報は Scene 内の有効な全 Terrain の三角形面を world 座標で照会する。親・回転・符号付き非一様 scale・穴を含む。重なる Terrain は最上面を選び、水面より上なら乾燥領域とする。底が無い場合は明示的な無効フラグと泡 0 を返す。Terrain の `surfaceRevision` と形状・Transform の署名で派生データを失効させ、高さ配列の有限値検査と bounds 計算は変更時だけ行う。

GPU 情報はテクセル中心で焼く RGBA8 で渡す。R は岸泡、G/B は符号付き基準水深の上位・下位 byte、A は底あり・乾燥・深度範囲外のフラグを持つ。水深を復号してから補間し、範囲内の有効な底を使う。Horizon の水深テクスチャは Terrain の範囲を覆い、カメラと保存済みの水域 Extent から独立する。浅水減衰は変位前の world XZ、岸泡と水中アトラスの底は水平変位後の world XZ を使う。波の高さを足した鉛直水深で岸泡を評価し、地形情報が欠ける場所は画面上の接触深度を使う。画面上の水柱は引き続き吸収・屈折に使う。

GPU リソースは ResourceManager/Scene/Entity の既存 cache に所属させる。地形・水面・岸泡設定が変わらないフレームでは情報テクスチャを再生成しない。描画 view が複数でも情報は共有し、遠景パッチの選択と範囲だけは view ごとに計算する。CPU で全境界の位置・帯域・カリング・保存・地形編集の失効を、GPU 読み戻しで白波の照明・局所波紋・接続 coverage を検証する。

Terrain のある Horizon は 513×513 を使用する。GPU では RGBA8 約 1 MiB を水域ごとに保持する。広域の岸線の細部はテクセル密度に依存する。Terrain の高さ・穴・Transform の変更は深度データを失効させる。CPU の水中照会と浮力も同じ底を読み、現在の波高を加えた深さを `buoyancyDepth` で制限する。浅水減衰が無効でも Terrain の底は使う。

## Horizon の屈折と岸泡

Horizon が一つだけあり、除外形状を持たないとき、基準海面から 1 m 以内にある Spline River の端を河口候補とする。標準水面材質は、その端から川幅に応じた範囲で波を乗せる前の高さを使い、海面へ色の被覆をフェードする。描画ビューでは受け側の海を先に描き、海が描かれたときにフェードを適用する。水中判定と浮力は各 Water の形状を使う。

係数光学の Horizon は、屈折率 1.333 の界面から反射率と屈折方向を同時に解く。非偏光 Fresnel の S・P 成分を平均し、水中側の全反射では透過方向をゼロにする。Horizon の鏡面 F0 も同じ屈折率から求める。材質の `reflectivity` は反射量の倍率で、Horizon の係数光学では `fresnelBias` / `fresnelPower` は使わない。[PBRT §9.3.4–5](https://www.pbr-book.org/4ed/Reflection_Models/Specular_Reflection_and_Transmission) を参照する。

直射の海中散乱は、解像できるうねりの法線を水平面の法線に半分近づけた入射面で太陽光を屈折させる。細かいさざ波は入射面に含めず、太陽が水平線に近づくと入射面を水平へ戻す。入射時の透過率とその面を通過する光束から、海中の光線に垂直な放射照度を求める。光路に沿った散乱点の深さを視線と太陽光の鉛直成分の比で表し、太陽から散乱点、散乱点から水面の二つの Beer 減衰を閉形式で積分する。位相関数と散乱積分は屈折候補・元の画素それぞれの光路で評価する。空と局所光は既存の近似を使う。深い海での多重散乱を含む考察は [Bruneton, Neyret and Holzschuch 2010 §5.3](https://morpho.inrialpes.fr/Publications/2010/BNH10/article.pdf)、Beer 減衰は [PBRT §11.2](https://www.pbr-book.org/4ed/Volume_Scattering/Transmittance) を参照する。

係数光学を使う海面の上側は、水の屈折率 1.333 で視線を曲げる。水面直前の不透明深度を起点とし、画面への投影と深度の再照会を最大 2 回行う。屈折先の色と光路長を同じ候補から取得し、深度差が大きい候補や前景、画面外を退ける。画面端と探索距離の端では元の視線の放射輝度へ連続的に戻す。`refractionStrength` は画面 UV で探索半径を制御し、0 で屈折探索を無効にする。底の深度が空なら透過する海底色が無い深い媒体として扱う。屈折の方向は [PBRT §9.3](https://www.pbr-book.org/4ed/Reflection_Models/Specular_Reflection_and_Transmission)、光路長と吸収は [PBRT §11.2](https://www.pbr-book.org/4ed/Volume_Scattering/Transmittance)、シーン色と深度の入力は [Epic Single Layer Water](https://dev.epicgames.com/documentation/en-us/unreal-engine/single-layer-water-shading-model-in-unreal-engine) を参照する。

屈折候補の色を線形補間する前に、対応する 4 テクセルの不透明深度を確認する。奥行きが連続する場所では線形補間し、空・水面より手前の物体・大きな深度差が混じる場所では検証済みの候補画素の色を使う。

Terrain のある岸は、波で移動する水際に薄い残り泡を描き、浅瀬で波頭が盛り上がり圧縮される場所に砕波の泡を重ねる。岸の水深と正の波高から砕波の位置を近似し、湿潤境界の幅を画素フットプリントに合わせる。`foamThreshold` と `foamFade` は世界鉛直水深で帯の範囲を決める。水深の利用は [GPU Gems §1.3.1](https://developer.nvidia.com/gpugems/gpugems/part-i-natural-effects/chapter-1-effective-water-simulation-physical-models)、波面圧縮の指標は [Dupuy and Bruneton 2012](https://liris.cnrs.fr/Documents/Liris-5812.pdf) を参照する。

係数光学を使う岸の接触泡は、近傍の不透明深度を鉛直水深に換算して Terrain 情報を補う。視線が浅い場合、深度が空の場合、視線の交点が水平に離れる場合は Terrain の水深を使う。残り泡の幅と密度は変位前のワールド XZ に固定した低周波ノイズで変え、動く細かい泡模様と重ねる。水面から不透明面までの深度の利用は [GDC 2008 Shore and Foam](https://media.gdcvault.com/gdc08/slides/S6172i1.pdf) を参照する。

Horizon の岸泡は、白波の履歴と同じ 3 時点の符号付き波高から、過去の砕波と水柱の深さを推定する。今の水位がその砕波より下がった浅瀬で、引き波に残る泡を現在の泡に重ねる。古いサンプルほど被覆率が指数減衰し、現在も水が覆っている範囲にだけ描く。寿命は既存の `crestFoamLifetime` [s] を共有し、0 なら岸泡も現在時刻だけを使う。沖の白波を切ったときは、地形水深で絞った頂点だけ履歴を評価する。泡の残留と減衰は [Callaghan et al. 2012](https://doi.org/10.1029/2012JC008147) を参照する。

浅瀬の泡模様は、基準水深の画面微分をワールド XZ に戻して求めた地形の傾きに沿って、波高が上がると陸側へ、下がると沖側へ動かす。等深線を横切る変位の一次近似を使い、平坦な地形は正則化し、移動幅は 2 m までに抑える。水深とアトラスの有効率で沖の模様へなだらかに戻す。地形の傾きと遡上の関係は [Stockdon et al. 2006](https://pubs.usgs.gov/publication/70030520) を参照する。

水平線付近で画素が広い水面を覆うときは、補間した頂点法線とさざ波から、波長ごとに画素内で解像できる傾きを評価した法線へ移行する。透視投影の遠景では視線と基準水面の交点を微分し、反射の画素フットプリントを求める。実際の交点が高い波頂にあるときは変位面の微分を使い、基準水面に近づくにつれて交点の微分へ連続的に移行する。画素の広さを `detailScale` のセル数で評価し、0.22 セルから 0.95 セルまで連続的に切り替える。解像できない傾きの分散は GGX の粗さへ移す。[Bruneton, Neyret and Holzschuch 2010](https://doi.org/10.1111/j.1467-8659.2009.01618.x) の幾何から BRDF への遷移を参照する。

Horizon の外側では、入射余弦が画素内で急変する浅い視角に限り、解像できない波の傾きと画素内の角度分散から環境反射のフレネルを平均化する。屈折光線、水中の全反射、水面の被覆率には元の界面計算を使う。[Bruneton et al. 2010](https://doi.org/10.1111/j.1467-8659.2009.01618.x) §5.2 の平均フレネルを参照する。

さざ波のタイルは、うねりの傾きでずらした座標の微分を使って mip を選ぶ。画素内の帯域制限にはずらす前のフットプリントも下限として使う。着水波紋は接空間の傾きへ戻し、さざ波と残差波の傾きに加えてから法線を求める。平坦な着水波紋は傾き 0 なので、さざ波の法線を弱めない。[Mikkelsen 2020](https://jcgt.org/published/0009/03/04/) の surface gradient による合成を参照する。

係数光学の Horizon は、透視カメラの位置と同じ water owner のアトラス水位から入射側を決める。波高の上限から水面の外にいることが明らかな場合は先に判定し、アトラスが無効の場合と平行投影はワールド XZ の符号付き面積を使う。浅い視角では光学法線を視線側へ連続的に補正する。

沖の白波は、水平方向に圧縮された波頭の前面に濃い泡を出す。残り泡の被覆率は基流と局所流の上流で過去の砕波を評価して求め、寿命に応じて指数減衰させる。波頭に出る泡と残り泡は別に合成する。波頭泡の模様は変位前のワールド XZ で標本化し、残り泡の模様は基流に沿って伸ばす。[Dupuy and Bruneton 2012](https://liris.cnrs.fr/Documents/Liris-5812.pdf) の変形を用いた白波を参照する。`OffshoreCrossSwell.fzdata` は `wavesAssetRef` 用で、9 基波の方向と位相を分散させる。

波頭の前面に出る泡は、頂点から補間した泡量が 0 から増えるにつれて連続的に現れる。既定の Horizon 水面は、近景の強い交差砕波に空中の飛沫を出す。水面描画後に半透明の小粒子を描き、水面と不透明物の深度で遮蔽する。world XZ の 4 m 格子と時刻から候補を再現し、発生時点の共有波評価で波高・水平圧縮・交差による持ち上がりを確認する。候補はビューと水域ごとに最大 21×21、距離は 39 m で消え、寿命は約 0.4〜0.6 秒。発生点から初速と重力で飛ばし、個体や履歴バッファは保持しない。[Chentanez and Müller](https://matthias-research.github.io/pages/publications/tallCells.pdf) の空中飛沫と水面の分離を参照する。

Terrain の内容検証 cache は既存 Component lifetime と `surfaceRevision` を使い、同じ Entity へ外した Terrain を再追加した場合も区別する。Terrain の借用 pointer は抽出の呼び出し内だけ保持する。構造が同じ frame 内で変わっても、次の抽出で現在の配列から取り直す。合法な `buoyancyDepth=0` は情報の無効化だけに使い、水面自体の描画を消さない。

## 抽出候補と水面材質の共通準備 (2026-10-16)

Terrain と Water の描画抽出は、各段の開始時に当該 Component の世代付き Entity 列を複製し、現在の Component と Transform を Entity から取得する。Component から Entity を探し直さず、元の Component 列順を維持する。Sky と Sun/Moon は型付き候補の和集合を元の GameObjects 順へ整列してから読むため、同じ型を遅れて追加しても最初の有効な設定を変えない。候補の作業領域は再入の深さごとに分け、終了時は Entity 値を捨てて容量だけ再利用する。

Weather も毎回型付き候補を Scene 所有順へ並べ、最初の activeInHierarchy かつ enabled の現在値を使う。callback のない選択中だけ候補を保持し、追加・削除・世代再利用・Scene.Clear 後の候補や値を次の呼び出しへ持ち越さない。

水面準備は Scene/Game のビュー間でも毎回現在の入力を確認する。共有するのは所有する不変 MaterialAsset publication だけから導く矩形 current・4 本の基底波・grouping・spread で、WaterSystem.cpp 内の上限 32 件の作業領域へ保持する。現在の AssetHandle から publication を取り直し、完全に同じ publication の場合だけ値を再利用する。戻り値は数値のコピーとし、Scene・Component・Transform・可動 slot の借用 pointer を保持しない。材質の編集・置換・再ロードは同じ frame 内でも別の publication として扱い、保持上限を越えた入力も通常の導出と循環置換で処理する。個体の波倍率と有効設定は毎回適用し、可変な MaterialAssetAuthoring を直接渡す経路は cache しない。

Spline・Zone・除外領域の準備、Zone member の波紋消去と flow count の初期化、現在の current の代入は毎回行う。公開 field・Transform の直接変更を frameStamp だけで省略しない。LIVE_QUERY の準備は SIMULATION の公開済み snapshot を上書きせず、カメラの水中判定・Horizon 範囲・パッチ LOD・選択・culling は各ビューで計算する。不変 publication の参照を持つだけでは、Scene 全体の準備を省略できる根拠にはならない。

2026-10-07 Development の関連 CPU 回帰は 156 件すべて合格した ([検証ログ](../../build/agent/test-20261007-172053-64932.log))。最終変更 8 C++ 単位のコンパイル検査はエラー・警告 0 ([検査ログ](../../build/agent/check-20261007-171211-62996.log))。RTX 4070 / DX12 / Agility 1.619.6 の WaterShaderTest は既存 5 件を含む 11 件が合格し、白波の高さ帯・谷の保持・減衰した過去の泡・移動・暗所・強度の独立、遠景の色と選択 coverage・波紋抑制、地形深度の桁上がり・底なし・乾燥・範囲外を GPU 読み戻しで確認した。比較画像は `Scratch/WaterWhitecaps/SmallWaveBefore.png` と `SmallWaveAfter.png` に保存し、単一波の検証画像として確認した。

深度範囲外フラグの除外だけを一時的に外すと専用 GPU テストが失敗し、画像の最大差は 0.76763916 となった。ソースを byte 単位で原本へ戻した後に GPU 11 件すべてが再合格した。この検証は回帰を検出する確認であり、一般シーンの見た目や GPU 負荷の改善率を保証する測定ではない。

## 海の材質と編集負荷の改善 (2026-10-08)

Ocean材質と配布テンプレートの同名材質は、RGB吸収・散乱係数を使う`opticalMode=1`へ切り替える。吸収`[0.18,0.055,0.025]`、散乱`[0.009,0.025,0.04]`はworld m^-1、位相の平均cosは0.45。波の振幅・方向・波長・個体倍率は変更しない。細波強度は0.65、smoothnessは0.9、白波の高さ閾値0.5・fade0.2・強度0.6・粗さ0.78として、面全体の白濁と過大な微細法線を減らす。Lake/River/Flatの材質は既存の光学設定を維持する。

Rectangleの深度クリア画素では空を水底として透過しない。透過項だけを背景判定で除外し、体積内散乱と空反射は保持する。Splineは著者が与える有限水深の透過を維持する。光の係数モデルを参考にした公式資料は[Epic Single Layer Water](https://dev.epicgames.com/documentation/en-us/unreal-engine/single-layer-water-shading-model-in-unreal-engine)。専用GBuffer/tile SSRを持つUEと同じレンダーパイプラインを実装したという意味ではない。

細波は既存world-space勾配タイルを使い、0.5 cycle/pixelでのNyquist cutoffを保って減衰をsmoothstepにする。完全に除いた後続octaveの傾き分散は有限幾何級数でまとめ、見えない傾きのエネルギーを粗さへ引き継ぐ。SSRの鋭い反射はfilter済み粗さに応じて粗いIBLへ混合する。Terrain深度が岸泡を代替する場合は使わないfoam maskを読み込まない。大波と材質法線で細部を分担する考え方は[Epic Water Waves](https://dev.epicgames.com/documentation/en-us/unreal-engine/simulating-waves-using-the-water-waves-asset-in-unreal-engine)を参考にする。

汎用パッチのモーフ余白は可視性を保証するculling boundsに残し、投影密度・近景距離は元タイルに物理変位の余裕だけを足したboundsから測る。海のroot-cell由来の巨大な余白だけで遠景を最高LODへ分割しない。出力AABB、2:1均衡、stitch、予算、モーフ写像の契約は維持する。Editorの制御点HUDは表示のための全Scene Transform同期と常時の水域解析を行わず、軽量な点数・modeを表示する。詳細診断は「状態を確認」の明示操作またはInspectorで行い、HUDには確認時点の結果として表示する。

実測性能の比較はユーザーの指定で後回しにした。削減した処理やパッチ数の検証をCPU/GPU時間の改善率へ換算しない。見た目の比較は実シーンの再生成ではなく、同じカメラ・光・波・本番生成ノイズ・幾何の512px GPU fixtureで旧光学材質と新光学材質を描く。表示用PNGだけ既存Compositeの露出1・ACES・sRGBを通し、HDR readbackによる数値検証は維持する。画像は同じ改良Shader上の材質比較であり、旧Shaderとの前後比較ではない。

最終Releaseビルドはエラー・警告0 ([ビルドログ](../../build/agent/build-20261008-215459-36560.log))。Water・SurfacePatch・ControlPoint・TargetPointのCPU回帰317件が合格した ([テストログ](../../build/agent/test-20261008-215711-40564.log))。DX12のWaterShaderTestは26件が合格し、底なしの透過、Terrain岸泡、SSRの粗さ混合、Nyquist付近の連続性、モーフの色・選択coverageを確認した ([GPU結果](../../build/agent/water-gpu-final.json))。表示用画像は`Scratch/WaterVisual/final/LegacyOcean.png`と`CoefficientOcean.png`に保存して確認した。実際のShowcaseCoastの描画比較は未実施である。

## 設定変更と生成済みメッシュ (2026-10-11)

Horizon は矩形の水面と水中領域を延長する。CPU の所属と Physics の水平 broadphase は全 XZ、GPU は共通のカメラ範囲を使う。描画入力を Adaptive にする場合は共有パッチ VB / IB が必要で、有限格子のキャッシュをそのまま渡してはいけない。

水面メッシュのキャッシュは surface shape と spline signature だけでなく、実際のメッシュ方式・範囲・有限解像度・chunk 数・patch 解像度・spline の水深上限を値で比較する。dirty フラグを経由しない変更でも不一致なら旧 GPU 資源を返して再構築する。Adaptive が既に有効なら Horizon の切替だけで同じ共有格子を作り直す必要はない。外周は既存の view ごとの far mesh 生成・カリングで更新する。

Horizon では `ResolveWaterHorizonRectangle` からビューのアトラス bounds と候補 table を作り、除外・plume もこの範囲で抽出する。吸収・散乱の矩形光路クリップは Horizon で水平スラブを省き、有限水深と除外区間を保つ。GPU table の矩形種別は w=0 が非矩形、1 が Finite、2 が Horizon。資源・入力が不正な場合は fail-closed とする。

2026-10-11 の水面だけの検証は、水中領域まで同期する要件を満たしていなかったため、上記の共通契約へ修正した。保存キー `farOceanEnabled` と Extent は変更せず、古い Horizon データも読み込める。旧データの Horizon は新しい水平所属契約で動き、Finite データの挙動は維持する。
