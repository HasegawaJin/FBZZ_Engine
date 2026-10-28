<!-- @file    water-gameplay-visuals.md -->
<!-- @brief   水中物体・境界・潜水ライト・集光・接触演出・除外領域の共有契約。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-09 -->

# 水中ゲームの共通描画と物理

この段階は、水中物体に届く照明の消散、画素単位の波と水域境界、Point/Spotの体積光、実波に連動する投影集光、泳ぎと接触の泡・砂濁り、3Dの乾燥領域を接続する。既存のWaterSurfaceQuery、Simulation snapshot、汎用LOD、光学材質を正本にする。GraphicsはSceneとPhysicsを参照しない。

## 水域情報

各有効ownerの実効メッシュとWaterの共通頂点評価を上向き正射影で描き、ビュー近傍のworldXZ atlasへ基準水位からの波高・局所水深・実波の圧縮・被覆を保存する。数値はRGBA32F。静的な輪郭、Spline/Zoneの高さと流速、Gerstner、浅水減衰、波紋と同じ入力を使い、画素や各体積サンプルから数千三角形を走査しない。照会領域は視界、光の到達距離、水中積分距離を覆い、有限の解像度は描画近似として扱う。物理は不変の数値snapshotを直接照会する。

矩形の光源経路は一様水深のときだけ解析的なslabを使い、地形から水深を読むときはatlasの底へ切り替える。短い水面までの光路を境界探索で先に分け、最大積分距離の粗いsampleが浅い水を飛び越さないようにする。乾燥boxはray上の交差区間の和集合を除き、局所濁りも残ったwet区間だけを積分する。

材質係数・水域ID・atlas写像・除外形状・局所的な濁りは数値表で共有する。PS補助StructuredBuffer t14とTexture2D t15を時分割し、VS粒子t14/t15とcluster t29/t30を奪わない。資料が無効・未記録なら前フレームのatlasを読まない。所有はRenderViewResourcesとScene側の不変入力に閉じ、世代変更と破棄で返却する。

別の画面上の水面深度・owner maskは、色・選択と同じ変位とLODで最初の可視界面を求める。Compositeと体積光はカメラから界面または不透明受光点までの媒体だけを積分する。Water表面が担当する界面から水底の光路を重複して吸収・散乱させない。湖・川の横境界と、カメラが水面付近にある混在画面はatlasと界面maskから決める。

Compositeは光路が全水域のXZ外接範囲に交差しない画素を媒体探索前に省く。交差する画素の視線積分ステップと界面の終端は変更しない。環境光・散乱重み・水中光強度のいずれかが零なら環境光源の経路探索だけを省き、背景のRGB消散と当該フレームの方向光は保持する。

濁り球なしの真上への光源経路は、実際の原点でatlasの水柱を一度解決し、水面までの距離と上限距離の小さい方から乾燥boxの交差区間の和集合を除いてBeer則を評価する。波高・地形水深・非矩形の輪郭も同じXZの水柱から読む。複数ownerがある場合も、同じXZの各水柱を一度調べ、点照会と同じ優先度・実水面高さ・stable indexで最初のownerが光路全体を保持することを証明できればこの経路を使う。最初の水面を越える光路には、低い優先度の別水域も消散を与えうるため、その区間に別の水柱があれば証明しない。斜め光源、光路内で所有権が変わる水柱、局所濁りは既存の一般積分へ戻す。

矩形・一様水深・濁りなしのownerは、描画入力の保守的な波高境界から必ず水が存在する内部区間をGPUへ渡す。body row0.wは0が未証明、1以上が「最大上下変位+1」で、既存の手製表は一般経路を使う。別ownerのXZ外接範囲がrayと交差する場合も一般経路を使う。証明した内部では光源のRGB透過をBeer則と乾燥boxの正確な区間和で計算し、波境界・地形水深・Spline・濁りの一般探索は省略しない。

header row2.wが1の場合、row4.zwは波高と姿勢を含む全ownerの保守的な垂直外接範囲を保持する。範囲外の空気中の受光点や、範囲に交差しない視線はatlasを読まずに省く。旧表はmarker0で従来の探索を使う。row4.xyのatlas寸法は維持する。

Compositeは、証明済み矩形・一様水深・濁りなしで視線全体に競合ownerが無い場合だけ、最初のownerと光学係数を再利用する。波高・水底と水面の境界は各march cellで読む。完全に同じownerの内部ではhardware bilinearを使い、混在被覆・穴・波面と水底の近傍はownership-awareな四点評価へ戻す。真上への環境光源の距離はownerのmaxDistanceを守り、乾燥boxの和集合を差し引く。固定XZのslabは一度だけ求め、元の積分格子へ各cellで交差させる。除外件数0が確定したownerだけ区間長を直接使う。全ownerの保守的なXYZ範囲外のcellは省略できるが、格子の開始位置や幅を詰め直さない。

Deferredの受光点は同じ水域照会とheaderを共有し、Sunと各ローカルライトの光源経路へ渡す。光強度または色が零のSunには光源経路を求めない。発光、RGB消散、環境光、ローカルライトの受光・影の契約は維持する。

除外boxが1件の場合は、その交差区間の前後のwet長を直接求め、複数boxのunion走査を省く。不正なtable rowは従来と同じくwet長0にする。Compositeの証明済みownerでは、視線とそのboxの交差を画素ごとに一度求め、波でclipした各cellと交差させる。環境光源の原点はcellごとに変わるため、光源側のbox交差を視線の区間で代用しない。複数boxは一般unionを維持する。

同じ証明済みownerのBeer透過と散乱重みは、直前の正のwet長と完全一致するときだけ再利用する。cellの浮動小数点差分、波、水底、乾燥領域で長さが変われば元の関数で再評価する。非証明ownerの係数や濁りは共有せず、ステップ数・格子・界面終端・光源距離上限は変更しない。

当該ビューに有効な一定水深矩形があれば、Compositeを二つの補完するvariantで描く。初期ownerが証明済みの矩形・一様水深・濁りなしで、視線全体に競合ownerが無い画素を専用shaderが描き、条件外をdiscardする。通常shaderはshared mode 3でまったく同じ判定の画素をdiscardし、残りを元の積分で描く。両方が元のHDRを読み、色の効果を二重に重ねない。波・岸辺・水底の各cell、界面終端、積分格子、乾燥boxの正確な区間和は共通コードを使う。複数水域が同じ画面にあっても孤立した矩形画素を特殊化でき、地形水深・水域外のカメラ・濁り・競合する水域は通常経路を維持する。

## 物体照明・集光・潜水ライト

Forward、Deferred、Terrainの受光点が水中なら、Sun・Point/Spotの光源から受光点へのRGB透過をBRDFへ入る照明へ適用する。環境光は水面から受光点への代表的な光路で減衰する。カメラへの消散は別の合成段が一度だけ適用し、自己発光を入射光として誤って暗くしない。[PBRT Transmittance](https://www.pbr-book.org/4ed/Volume_Scattering/Transmittance)を参照する。

投影集光は同じ波の法線・圧縮と屈折した太陽光から計算し、同じ輪郭と除外mask、RGB透過、生の影を使う。波の平均周波数だけで独立した模様を流す既存近似から共有実波へ接続する。光子追跡を追加する設計ではない。

潜水ライトは既存のCluster/Legacy/Linear入力、距離、Spot cone、Cookie、Point/Spot影を再利用する。体積点には面法線が無いため法線不要の強度と固定biasの生の遮蔽を使う。太陽が無効な夜も局所光を積分する。clusterの選択は各サンプルのviewZと画素座標で行い、全Sceneライトの別走査を増やさない。

局所ライトは体積点で有効な寄与を比較し、最大8灯へ光源透過・影・Cookieの評価を絞る。これは多数の光が重なるときの描画品質の上限。水域のatlasはビュー当たり1024平方のRGBA32F、各ownerのtileは64〜512平方、最大256ownerとする。除外boxと濁り球はビューと光の到達範囲に交差するものだけを詰め、その後owner当たり64件の上限を適用する。超過・無効なownerは水面と媒体を描かず、変更時だけ診断を出す。乾燥領域の入力を途中で捨てて水没させない。

## 泡と局所濁り

波紋の描画テクスチャは断面の数式を変えず、波紋・水域姿勢・範囲の署名で共有する。同じ入力をScene/Gameの各ビューで焼き直さない。常駐する動的テクスチャへ更新し、DX12のフレーム中の転送は描画と同じリストへ順番に記録する。転送バッファはフレームの完了フェンスまで保持し、CPUからGPU完了を毎回待たない。フレーム外や非対応経路は既存の同期方式へ戻す。

既存の剛体着水・離水・航跡の巡回から、owner単位の数値イベントを供給する。深い水中で泳ぐ移動は気泡、表面近くの移動は泡、検証した床との接触は砂濁りとする。浮力用水深を床として扱わない。作者の明示イベントも同じ入口で検証する。

pendingとactiveはowner当たり各32件、泡・砂の再利用emitterは各64粒を一回の上限とする。期限・局所流速・減衰を管理し、単一のeventごとにGameObjectを生成しない。粒子を追加・破棄する処理はCleanupのserialな構造変更境界に置く。Pauseでは年齢とスポーンを進めず、削除・無効化されたownerの粒子返却は進める。Scene切替、owner返却、Edit/Play複製、保存ではruntimeと生成個体を持ち越さない。InspectorのCopy・Paste・Reset・Undoは作者設定だけを移し、適用先の実行状態を保持する。明示イベントの寿命は数値の濁りとCPU粒子の両方へ反映する。GPUへは寿命付きの濁りの数値だけを渡す。

## 乾燥領域

WaterExclusionComponentはCollider不要の3D oriented box。enabled、local size、全水域または対象参照を保存し、親、回転、符号付き非一様scaleを扱う。不変snapshotはworld位置・逆回転・逆scale・半径を保持し、LIVE_QUERYとSIMULATIONの境界を守る。

水面そのものがbox内なら、その点の水面描画を除外する。元の水域のXZ輪郭と水深は保持し、boxより下には水が残る。水面より下の空洞は上の水面を残し、box内の点だけ水中表示・浮力のcenter/probe・接触演出・物体照明・体積光を除外する。高さの外挿APIは維持する。物理はSceneを参照せず、snapshotのpoint maskを使う。参考は[UE Water Exclusion Volume](https://dev.epicgames.com/documentation/en-us/unreal-engine/water-body-actors-in-unreal-engine#waterbodyexclusionvolume)。

## 検証

保存・複製・無効化、親と回転、3Dの上下境界、複数水域とZone、実効波と静止水、界面前後の二重消散回避、物体照明の光源側RGB減衰、夜のPoint/Spot、Cookieと生の影、Particle供給、PauseとScene世代、濁りの期限、無効/返却済みGPU資源をCPUと実GPUの出力で検証する。性能の実測はユーザーの指定に従い後回しとし、処理本数・容量・更新境界を明示して実装する。

## 作者の設定

水中の物体・視界・体積光はWater材質のOptical Volumeを有効にし、RGB吸収・散乱、水中光の強度・距離で調整する。Oceanと配布テンプレートは有効が既定。Point/Spotは既存のLight設定を使い、Spotの角度、Cookie、Shadowをそのまま潜水ライトへ反映する。

方向光・Point/Spot の視線方向の体積散乱は PostProcess の VolumetricLight の明示 opt-in が必要。Optical Volume の既定が有効でも体積光パスを自動で動かさない。PostProcess の steps / maxDist は媒体側の上限と小さい方を使い、intensity は散乱光へ掛ける。無効時も背景の消散・環境光と物体へ届く照明は維持する。詳細は [VolumetricLight](volumetric-light.md) に従う。

Water InspectorのWater Interactionで水面の泡、泳ぎの気泡、接触した床の砂を個別に切り替える。速度のしきい値・発生間隔・寿命・材質を調整できる。自動演出は剛体の移動と検証した静的な床接触を使い、ゲームの独自泳ぎ処理はC#の`Water.EmitInteraction(WaterInteractionKind, worldPosition, worldVelocity, radius, strength, lifetime)`で同じ入口へ供給する。

空のGameObjectへWater Exclusionを追加し、Transformで位置・向き、sizeでローカルの箱を設定する。全水域を対象にする既定値のほか、excludeAllを外してwatersへWater参照を登録できる。選択時はScene Viewに箱を表示する。海中基地の室内・通路と同じ寸法へ合わせると、その内部の水中判定と浮力、表示、入射光、体積光を共通で除外する。

## 2026-10-09の検証結果

Compositeの探索削減後、DevelopmentのGraphicsStandaloneビルドはエラー・警告0。水域透過17件と水中合成30件のGPU回帰が合格した ([最終GPU結果](../../Scratch/CompositeOptimization/FinalGpuResults.json))。波高と重なる乾燥box、水域外の画素、光強度零でのRGB消散を追加で検証した。水面32件も合格した ([水面結果](../../Scratch/CompositeOptimization/WaterSurfaceResults.json))。同時に回して旧契約の期待で失敗したSSR関連2件は、後続のテスト更新で[現在の反射合成](graphics-quality.md)に合わせた。Compositeの二重合成防止、RasterのFresnel適用済みRGBと材質非依存の信頼度、Hybridの交差棄却を検証し、DeferredEmission全8件と符号付き差分・透明背景保持の2件が実GPUで合格した ([SSR再検証](../../Scratch/CompositeOptimization/SsrResults.json))。テストのDevelopmentコンパイルと再リンクはエラー・警告0 ([ビルド](../../build/agent/build-20261009-171624-31280.log))。実シーンのComposite GPU時間の前後比較は未確認。

ReleaseのEngine・Editor・各テストの全体ビルドはエラー・警告0 ([全体ビルド](../../build/agent/build-20261009-121040-13480.log))。最終差分を含むEditor・GPUテスト・Launcherの再リンクもエラー・警告0 ([最終ビルド](../../build/agent/build-20261009-125822-53628.log))。コンパイラのメモリ不足へ対応するため、同じReleaseツリーの並列コンパイル上限を2へ下げて検証した。

関連CPU回帰595件が合格 ([CPU結果](../../build/agent/test-20261009-131221-19452.log))。水域・部分浮力・制御点・粒子・保存・Prefab・Play複製・Pause退場・InspectorのCopy/Paste/Reset/Undoを検証した。DX12実GPU回帰75件が合格 ([GPU結果](../../build/agent/water-gpu-gameplay-20261009.json))。Water32件、WaterMedium15件、Underwater28件で、実波のatlasと界面・selection被覆、RGB受光と自己発光、可視界面での二重消散回避、夜のPoint/Spot、Cookie・生の影、地形水深、薄い乾燥boxで分断された濁り、湖・川の境界と集光を確認した。

比較画像は`Scratch/WaterUnderwater/UnderwaterNightWithoutSpot.png`と`UnderwaterNightSpot.png`。同じ床・RGB立体・屋根・カメラへ潜水Spotの体積光を加えた制御されたfixtureであり、実ゲームシーンの撮影やOcean既定値の美観比較ではない。実波と接続した投影集光、有限解像度の媒体atlas、最大8灯の局所散乱は描画近似である。UEの完全互換や光子追跡として扱わず、性能の実測は指定どおり後回しにする。
