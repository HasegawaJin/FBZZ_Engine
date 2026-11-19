<!-- @file    water-spline-surface.md -->
<!-- @brief   スプライン水域の形状・基準高さ・水深・流速を描画と物理へ共有する契約。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-07 -->

Waterの形状は既存のRectangle、閉じたSpline Lake、開いたSpline Riverから選ぶ。湖の制御点は同じlocal Y、川は点ごとにlocal XYZ・全幅・水深・符号付き流速を持つ。複数水域のWater Zoneと河口接続は [water-zone.md](water-zone.md) に従い、海の岸線反転・島は後続とする。[Epic Water Body Actors](https://dev.epicgames.com/documentation/en-us/unreal-engine/water-body-actors-in-unreal-engine) の湖と川の役割を参照する。

## シェーダーの編集箇所と見分け方

[WaterSurface.hlsli](../../Assets/Shaders/Water/WaterSurface.hlsli) は描画パス間で共有する入力と頂点評価を提供する。[WaterStandardAppearance.hlsli](../../Assets/Shaders/Water/WaterStandardAppearance.hlsli) は標準材質の入力と `PSMain` を持ち、河川の見た目を `EvaluateWaterRiverAppearance` から受け取って屈折・照明・泡を合成する。プロジェクト材質はこの二つの入口から必要な処理を使う。[Water appearance authoring](water-appearance-authoring.md)

| 見え方・調べたい点 | 編集箇所 | 最初に切り分けること |
| --- | --- | --- |
| 浅瀬で波頭が太い、段差と峰が離れる | [WaterRiverWaves.hlsli](../../Assets/Shaders/Water/WaterRiverWaves.hlsli) の `WaterRiverStandingPhase` と `WaterRiverStandingSection` | ワイヤー表示で峰の位置と断面を見る。形の不足はここで直し、斜め視点でだけ面の傾きが読めない場合は `WaterRiverStandingOptics` を見る。 |
| 波を越える泡と反射の切れ目がずれる | [WaterRiverAppearance.hlsli](../../Assets/Shaders/Water/WaterRiverAppearance.hlsli) の `WaterRiverStandingStreak` | 同じ泡の塊を上り斜面から下流まで追う。切れ目は共有する `split`、泡の芯と反射の幅は同じ高さから求める閾値を比べる。 |
| 泡が手前で消える、急に途切れる | [WaterRiverFlow.hlsli](../../Assets/Shaders/Water/WaterRiverFlow.hlsli) の `WaterRiverStandingFoam` と [WaterRiverWaves.hlsli](../../Assets/Shaders/Water/WaterRiverWaves.hlsli) の `WaterRiverStandingWave` | `crestFoamLifetime` による時間減衰、頂点で拾える上流の範囲、地形深度の欠損の順に確認する。岸泡と沖の泡の被覆は [WaterSurfaceFoam.hlsli](../../Assets/Shaders/Water/WaterSurfaceFoam.hlsli) を見る。 |
| 浅くなる位置と波の減衰がずれる | [WaterSurfaceTerrain.hlsli](../../Assets/Shaders/Water/WaterSurfaceTerrain.hlsli) の `WaterTerrainUvAtWorldXZ` と `WaterDepthState` | 変位前の世界座標が正しい地形テクセルを読むか確かめる。水深の被覆と、波の高さを減衰させる係数をそれぞれ確認する。 |
| 岩の周りの泡が進まず、岩ごとの渦が読めない | [WaterRiverObstacles.hlsli](../../Assets/Shaders/Water/WaterRiverObstacles.hlsli) の `WaterRiverFlowAroundExclusions` と `WaterRiverWakeAt` | `Water Exclusion` の位置と大きさを確認し、旋回速度と後流の減衰を調べる。Flow Field が作る水面の変位は [WaterSurfaceFlow.hlsli](../../Assets/Shaders/Water/WaterSurfaceFlow.hlsli) にある。 |
| 水底が引きずられる、岸だけ反射がちらつく | [WaterAppearanceOptics.hlsli](../../Assets/Shaders/Water/WaterAppearanceOptics.hlsli) の `WaterTraceSplineRefraction` と `PSMain` の `TraceWaterSSR` 呼び出し | `refractionStrength` を下げた比較で水底の歪みを切り分ける。映り込みは画面内の反射と空の反射の切り替えを確認する。 |
| 遠景で細かな反射がちらつく | [WaterSurfaceDetail.hlsli](../../Assets/Shaders/Water/WaterSurfaceDetail.hlsli) の `WaterResidualWaveSlope` と `PSMain` の `roughnessAA` | `normalStrength` と `detailStrength` で寄与を確認し、画素より細くなった勾配を粗さに渡す経路を見る。大きな波の形は [WaterSurfaceWaves.hlsli](../../Assets/Shaders/Water/WaterSurfaceWaves.hlsli) にある。 |

同じ浅瀬を固定カメラで時間を進めて比べると、波頭の位置、泡が切れる位置、反射のちらつきを順に確認できる。泡の寄与は材質の `foamStrength` で切り分ける。GPU側の調査箇所は、画面内反射のレイマーチと `Water Exclusion` の画素ごとの走査。段差泡の探索は頂点側にあり、色パスを基準に実測して優先順位を決める。

## 水面と流れの契約

正本はWaterComponentの制御点であり、生成Meshを正本へ置き換えない。各点には1〜INT32_MAXの一意なIDを保存する。位置と幅はlocal単位、水深と流速はworld m・m/sとする。yaw・親TRS・符号付き非一様scaleに対応し、従来と同じ水平TRS契約を使う。流向は変換した曲線接線をworld XZへ投影して正規化し、符号付き流速を掛ける。scaleで流速や水深を変えない。Splineの点流速が基準流れを定め、Rectangleの材質currentSpeedと入れ替わる。外部FlowFieldによる影響は引き続き加算する。

標準材質のさざ波と泡は、頂点から補間したworld XZの設定流速と局所Flow Field速度の合計を基準に移流する。有効な地形水深がある浅瀬では、描画上の移流速度を基準の58〜100%に調整して深場との差を出す。急瀬の判定には基準の流速を使う。経過時間が大きくなっても曲がり角の移動量が伸び続けないよう、短い位相を二つ使い、各位相の切り替わりで寄与をゼロにする。Ripple Directionを手動指定した場合はさざ波に指定方向を使い、泡は流速に従う。

浅く速い流れでは、基準水深と合計速度からフルード数を局所的に近似してさざ波を強め、移流する模様が強い場所に泡を出す。基準水深は制御点から補間し、有効な地形の水深が浅ければ地形を優先する。地形情報がない場所では制御点の水深を使う。乾いた水際と低速の流れでは急瀬の効果を弱める。泡の量は材質の`foamStrength`で調整する。この近似は描画のみに適用する。[HEC, Subdivision Froude Number (式1)](https://www.hec.usace.army.mil/publications/TechnicalPapers/TP-110.pdf)を参照する。

細かいさざ波に、流れに沿って伸びる長い筋を重ねる。ワールド XZ と流速の画面微分から、流れに沿った速度の変化 `a = (v · ∇)v` を求める。短い逆追跡を `−v Δt + a Δt² / 2` とし、急な折れや遠景で変位が暴れないように速度と加速度を制限する。長い筋は細かいさざ波と異なる周期を持ち、位置によって二つの位相の切り替えをずらす。画素が覆う幅は流れを横切る成分で評価し、斜めから見ても幅が読める筋を残す。減衰に使う幅の下限を画素全体の幅の42%にして遠景で消す。[MIT Marine Hydrodynamics, Lagrangian Acceleration](https://web.mit.edu/fluids-modules/www/potential_flows/LecturesHTML/lec02/lecture2.html) と [Vlachos, Water Flow in Left 4 Dead 2](https://cdn.cloudflare.steamstatic.com/apps/valve/2010/siggraph2010_vlachos_waterflow.pdf) を参照する。

逆追跡の勾配は各画素の加速度を局所的に固定し、折り返し前の位相と流速から求める。ノイズのフィルタ幅と法線の勾配を同じ変形に合わせることで、曲がりや岩の周りで筋の形と反射の動きをそろえる。筋の先端には流れ方向の傾きを残す。同じノイズの高さを使って筋の反射を少し鮮明にし、失われた傾きによる粗さ補正を加える。[Mikkelsen, Surface Gradient–Based Bump Mapping Framework](https://jcgt.org/published/0009/03/04/) を参照する。

頂点段階は波変位の微分を正規化せず渡す。標準材質は基準面と頂点波紋の高さ勾配をその微分の Y 成分へ加えて光学法線を作る。表裏の判定はラスタライズされた面で行う。太陽の鏡面反射は、流れの筋が解像できる場所で筋を横切る軸に粗さを配る。筋が薄くなると異方性も減る。流れの異方性と等方性には同じ分母の下限を使い、軸が切り替わるときに反射のピークを跳ねさせない。Ripple Direction を指定した水面は指定方向を使う。[GPU Gems, Gerstner Waves](https://developer.nvidia.com/gpugems/gpugems/part-i-natural-effects/chapter-1-effective-water-simulation-physical-models) と [PBRT, Anisotropic Microfacet Distribution](https://pbr-book.org/4ed/Reflection_Models/Roughness_Using_Microfacet_Theory) を参照する。

頂点密度で描けなかった波の法線には、既存の位相を使った二次高調波を重ねる。波頭の両斜面を立て、谷を広げる。急瀬や流れに運ばれる泡の筋では、描画上の振幅比を少し増やして波頭の形を変える。二次高調波は基本波より先に遠景で減衰させ、画素で解像できなくなった傾きの分散を反射の粗さへ回す。[FHWA HEC-25, Fig. 5.4](https://www.fhwa.dot.gov/engineering/hydraulics/pubs/hif19059.pdf) を参照する。

河川の描画専用の波は、流速の大きさ `|U|` に有限水深での重力波速 `sqrt(g tanh(kh) / k)` を足して進む。`k` は基本波 6 m、短波 2 m の代表波長から求める。各帯域の符号付き逆位相速度を中心線に沿って積分した移動時間 `T` を頂点へ渡し、`(T - time) / period` を位相にする。流れが反転する場所では進行方向も反転し、静水付近では振幅を消す。基本波の周期は水域のセル幅と中心線の平均位相速度で決め、短波は 1.15 秒とする。浅い地形の上では、設定水深との差から局所的な位相遅れを加える。遅れは時間に依存せず、横方向に水深が異なる浅瀬では波面を曲げる。頂点が解像できる帯域は高さへ、残りはピクセルの法線へ分ける。色・選択・水面アトラスは同じ頂点評価を使う。追加の高さは上下 0.15 m の範囲で浮力照会から独立した描画効果とする。[Akylas and Mei, Dispersion of Surface Water Waves, 式 (1.21)](https://web.mit.edu/fluids-modules/waves/www/material/chap-3.pdf) を参照する。

水域が持つ除外ボックスのうち、範囲内にある直立した小型ボックスを最大8件まで頂点用の円柱として同じ収集ループで詰める。岩の周りでは位相を曲げて振幅を上げる。ピクセルの短い波は切り抜きと回り込みの判定結果から得た曲がりも使うため、メッシュより細かな岩の周りでも形を変えられる。[MIT Marine Hydrodynamics, Flow Around a Circular Cylinder](https://web.mit.edu/fluids-modules/www/potential_flows/Lectures/lec10/lecture10.pdf) を参照する。

急瀬の段差では、変位前の位置で現在と上流2点の地形水深を比べる。間隔は水面メッシュの辺長と流れに沿った水深テクセルの間隔から決め、0.55–6 m に収める。高さは局所水深とフルード数で求め、進行波の振幅とは独立させる。中間水深を挟む区間を水深2点の追加照会で絞り、一様な傾斜を除く。波頭は段差の下流寄りに固定し、上流側を長く、下流面を短くした断面を使う。進行波と合わせた変位は上下 0.15 m に収める。ピクセル段階は補間した断面位相から傾きを計算する。水深テクセルが有効な手前の浅瀬だけ、ピクセルでも同じ地形断面を再評価する。遠景は頂点から補間した位相を使い、画素より細い傾きは反射の粗さへ回す。[Magirl et al. (2009), Colorado River rapids, §3.6](https://agupubs.onlinelibrary.wiley.com/doi/10.1029/2009WR007731) と [Mikkelsen, Surface Gradient–Based Bump Mapping Framework](https://jcgt.org/published/0009/03/04/) を参照する。

固定波を越える泡と反射は、同じ逆追跡で得た移流模様の高さと流れに沿った勾配を共有する。上り斜面では筋に流れ方向の傾きを加え、波頭では反射を細く絞る。厚い泡は鋭い反射を弱める。下流では反射を筋の両側へ広げ、泡が細い芯だけに残ったところで水面の反射が見える。流れ方向の勾配が大きい部分では、泡と反射の筋を同じ位置で分断して反射の粗さを上げる。断面や模様が画素より細くなると変化を弱める。泡と反射の合成は波頭の頂点変位から独立しており、既存の移流用テクスチャ参照を共有する。

段差の泡は波頭から供給する。色パスの頂点は同じ3点の水深比較を上流へずらし、追加の水深サンプルを最大4点まで読む。サンプル間で中間水深を補間し、実際の波頭からの流下距離を見積もる。干出・欠損で探索を打ち切り、泡が無効な材質と選択・界面・媒体のパスではこの追加探索を省く。ピクセルは流れと渦を含む速度とその変化から流下時間を求め、`crestFoamLifetime` に応じて泡を減衰させる。固定波が砕ける泡にも `crestFoamStrength` を適用する。泡は反射と共有する移流模様の広い部分から細い芯へ移行する。[Callaghan et al. (2012), Whitecap Foam Decay](https://agupubs.onlinelibrary.wiley.com/doi/10.1029/2012JC008147) を参照する。

上流の浅瀬と岸で生まれた泡は、逆追跡で得た地形水深から供給量を求める。岸由来の泡は、水深が増す流れに沿って岸から離れるときに引き伸ばす。岸の帯も同じ移流模様で途切れさせる。上流の地形サンプルは手前の画素に絞り、地形が欠ける場所では現在地点の泡を使う。Ripple Direction を手動指定した場合、法線は指定方向のさざ波を使い、泡は流速に従う。

河川の白波は水深に対する波高と局所フルード数を使い、浅瀬で解像できる波頭の前面に発生させる。波頭は流れに乗る泡を追い越す。各帯域の位相が流れに乗った点を通過する速度から砕波後の経過時間を見積もり、残留量が大きいほうの発生地点へ逆追跡する。上流の地形サンプルはその時刻で共有し、残留泡の寄与が小さい画素では通常の1.35秒前を調べる。泡には `crestFoamLifetime` に応じた指数減衰と終端フェードを掛ける。砕けたばかりの波頭には密な泡を残し、時間とともに既存の移流模様で細い筋へ分ける。薄い泡は拡散色と粗さを下げ、泡の隙間から水面の反射を戻す。[FHWA HEC-25, Wave Transformation and Breaking](https://www.fhwa.dot.gov/engineering/hydraulics/pubs/hif19059.pdf) と [Callaghan et al. (2012), Whitecap Foam Decay](https://agupubs.onlinelibrary.wiley.com/doi/10.1029/2012JC008147) を参照する。

岩や橋脚の近くでは、水面の接平面を流れの前後へ短く延ばし、不透明深度から復元したワールド位置がその水面より上にあり、横方向にも近いときに接触とみなす。上流側の狭い接触泡と、下流側の既存の移流模様で途切れる泡の尾を描く。水没面、可視範囲外、静水、解像できない遠景では効果を弱める。泡の照明と強度は既存の `foamStrength` の経路に合成する。[GDC 2008, Shore and Foam](https://media.gdcvault.com/gdc08/slides/S6172i1.pdf) の深度を使った接触表現を参照する。

岩や橋脚に Water Exclusion を合わせた場合は、同じボックスからワールド空間の回り込みも評価する。平均水面をまたぐ直立ボックスのうち、水平半幅が両軸とも 0.12〜3 m で、長辺が短辺の 2 倍以内のものを、断面積が等しい円柱で近似する。流速補正は円柱の外側へ滑らかに減衰し、複数のボックスでは補正が重なった量を制限する。補正後の速度をさざ波・反射の筋・泡に使い、下流側の両縁に揺れる泡の尾を出す。下流の泡の起点はビューの深度から独立する。この近似は標準材質の描画に適用する。[MIT Marine Hydrodynamics, Flow Around a Circular Cylinder](https://web.mit.edu/fluids-modules/www/potential_flows/Lectures/lec10/lecture10.pdf) を参照する。

標準スプライン材質は所有者の除外ボックス列を一度だけ走査し、変位後の水面の切り抜きと平均水面の回り込みを同じレコードで評価する。ボックスの Y 軸方向で両方の対象から外れた場合は X・Z 軸の行を読まない。1 ピクセルあたりの走査は所有者の候補数に対して線形で、上限は 64 件である。泡の筋は岩の両脇の弧から始まり、裏側へ向かって内側に曲がる。左右の筋の位置と強さを逆位相で揺らし、流下時間に応じて減衰させる。揺れの位相を運ぶ速度は基流の半分とし、周期は円柱近似の直径と流速からストローハル数 0.20 で決める。[NASA TM-110339, Introduction](https://ntrs.nasa.gov/api/citations/19970017057/downloads/19970017057.pdf#page=3) を参照する。

同じ後流位相から、岩の裏で左右交互に強まる逆向きの渦を作る。流れ関数の勾配を90度回転して旋回速度を得る。渦は岩の半径の3.4倍以内で滑らかに減衰し、重なった旋回速度は基流の40%以内に抑える。さざ波と泡はこの速度を共有し、長い筋と泡の逆追跡は流れに沿った速度変化に渦の時間変化も加える。後流の評価は既存の除外ボックス走査内で完結する。[Roshko (1954), On the Development of Turbulent Wakes from Vortex Streets](https://ntrs.nasa.gov/api/citations/19930092207/downloads/19930092207.pdf) と [Bridson et al. (2007), Curl-Noise for Procedural Fluid Flow, §2.1](https://www.cs.ubc.ca/~rbridson/docs/bridson-siggraph2007-curlnoise.pdf) を参照する。

Catmull-Romの曲線を有限個の標本へ分け、湖は単純閉多角形、川は幅を持つ帯として三角形化する。輪郭は湖2048頂点・川4096頂点までとし、点数256・分割数64の個別上限を満たしても積が予算を超える場合は生成しない。水面内部は共有中点で細分化し、元の平面・水深・流速の線形補間を維持する。32768頂点・65536三角形または座標精度の限界では、直前の完全な段階を保ち、qualityLimitedと実際の最大辺長を返す。不変snapshotはlocal/world頂点、頂点ごとの基準高さ・水深・world流速、三角形の添字と高さ勾配、bounds、BVH、署名を持つ。描画のVB/IBとCPUの所属・重心座標補間は同じ三角形を使う。幅・深さ・流速の標本間補間を両経路で独自に作らない。

内部の細分化は長い辺を優先し、隣り合う三角形で同じ中点を使う。曲線の標本数は岸の形を、メッシュの目標最大辺長は内部の三角形密度を調整する。Water Zoneでは接続全体の密度をMesh Cell Sizeで調整する。

湖・河川の輪郭検査は、広がりの大きい軸で辺を並べ、許容誤差込みで XZ の範囲が重なる辺だけを交差判定する。走査軸への投影が集中する場合は二次の候補走査が残る。湖の耳切りでは頂点を除去したときに隣接する二つの三角形を再検査し、他の候補は除去頂点によるブロックだけを解除する。入力順の三角形を維持しながら、最悪計算量を三次から二次に抑える。

設定やTransformの変更時だけsnapshotを作り直す。無効な点・重複ID・非有限値・退化・自己交差・重なる川の帯・予算超過では数値生成のoutを変更しない。選択中の不正Splineは無効な水域として扱い、Rectangleへ暗黙に戻さない。SceneやGPUへのpointerをsnapshotへ残さず、Physics substepにはsnapshotとTransformの値を渡す。Edit/Playや複製は同じ不変snapshotを共有でき、編集後の新snapshotはそのWaterだけへ差し替える。

水面の照会はGerstnerの水平逆写像を解いてからsnapshotを標本化する。川の基準world Yへ波・流れの形・波紋を足し、法線には基準三角形の高さ勾配を含める。生成頂点の水深をbuoyancyDepthで制限してから、三角形内で補間する。水中判定と浮力の各標本は同じ底面を使い、深い洞窟へ浮力を延ばさない。描画の吸収距離も水柱深さで上限を合わせる。

Splineの描画はsnapshotを入力にし、Rectangle用の頂点集中・四分木LOD・far oceanは独立した描画経路で使う。Splineの最高密度は曲線の分割数と水面内部の目標辺長で指定する。`mesh`では、岸の形を保ったまま[画面誤差でクラスタLODを選ぶ](gpu-driven-geometry.md#スプライン水域のクラスタlod)。色と選択マスクは同じ頂点評価とLODを使う。物理波はsnapshotの標本密度を使う。

Terrain情報と波紋のUV領域は、entity原点を中心としたSplineの対称包絡矩形を共有する。これは情報格納の領域であり、水域所属は三角形で決める。Terrainの水深はtexelごとのSpline基準高さから水底を引き、形状署名を再ベイク条件に含める。Splineの指定水深を仮想の底とし、地形がさらに浅い場合は浅い側を水深情報に使う。矩形を前提とする旧CausticsマスクはSplineに適用しない。

Inspectorで形状・分割数・点の位置・幅・水深・流速を編集できる。点の追加・削除・移動でも残るIDは保持する。Scene保存は制御点だけを保存し、snapshotとGPU資源は保存しない。旧SceneはRectangleで読み込む。SDKのliveな高さ照会は個別snapshotを準備し、同じcallback内でTransformを動かした直後も反映する。

SplineのTerrain光学情報は、変位前のパラメータUVで標本化して基準高さと対応させる。描画時の水柱は地形の底と制御点の仮想底の浅い側から、波を含む現在水位までとする。不透明な物体が手前にある場合は画面深度から得た水柱で制限する。平均水面に対する[スネルの法則](https://www.pbr-book.org/4ed/Reflection_Models/Specular_Reflection_and_Transmission)から、屈折先が確定しない場合の光路長を求める。岸泡の範囲は光路長と独立した鉛直水深で決める。薄い水際で消え、移流する模様で途切れる。

水上から見た屈折色は、波を含む水面法線で曲げた光線を水柱の底まで延ばし、投影した位置の色を使う。浅瀬ではさざ波による細かい揺れを残し、深くなるにつれてうねりを優先する。画面上の移動量は `refractionStrength` と透過率で抑える。屈折先は色のバイリニア補間が使う4点の深度を一度に調べ、前景・空・深度の段差では元の画素へ戻す。[PBRT, Snell transmission](https://www.pbr-book.org/4ed/Reflection_Models/Specular_Reflection_and_Transmission) と [Microsoft HLSL GatherRed](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/texture2d-gatherred) を参照する。

採用した屈折先の深度は吸収と散乱の光路長にも使う。画面端や移動量の制限が強いときは平均面の光路へ近づける。水上の反射は水の屈折率からフレネル曲線を求め、材質の垂直反射率と反射倍率を適用する。浅い視線で波の傾きが画素内に混ざる場合は、傾きの分散で反射を平均化する。係数光学では、太陽が水に入る向きと水中で弱まる量を、観測方向の減衰と合わせて散乱に反映する。[PBRT, Homogeneous-medium transmittance](https://www.pbr-book.org/4ed/Volume_Scattering/Transmittance) と [Bruneton et al. (2010), §5.2](https://morpho.inrialpes.fr/Publications/2010/BNH10/article.pdf) を参照する。

標準の `River.mat` は `opticalMode=1` で RGB 吸収・散乱を使う。吸収 `[0.37, 0.14, 0.07]`、散乱 `[0.025, 0.045, 0.045]` はゲーム内の澄んだ川向けの調整値 [1/m]。川底の色を浅瀬に残し、光路が長い淵ほど赤を吸収する。光学係数の役割は [Epic Single Layer Water](https://dev.epicgames.com/documentation/en-us/unreal-engine/single-layer-water-shading-model-in-unreal-engine) を参照する。

検証は曲がる川・凹形の湖・異なる高さ/幅/水深/逆流・負非一様scale・無効形状・共有辺・Scene保存往復・複製後の編集を扱う。物理のContains/Applyと描画用頂点を比較し、色/選択の形状と水深・流速のGPU入力も確認する。

2026-10-07に実装と独立レビューを完了し、CPU回帰とSpline用GPU回帰3件を追加した。2026-10-08にRelease構成のEngineAutoをエラー・警告0でコンパイル・リンクした ([ログ](../../build/agent/build-20261008-074939-60180.log))。EditorLauncherとGraphicsStandaloneもエラー・警告0でビルドした ([ログ](../../build/agent/build-20261008-075546-40156.log))。Shaderの `point` 予約語衝突は修正済みで、Shaderのビルド対象は最新状態である。CPU/GPUテスト本体の実行とシーンの見た目は未確認である。
