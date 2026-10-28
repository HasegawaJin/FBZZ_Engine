<!-- @file    graphics-quality.md -->
<!-- @brief   反射、地形、時間履歴、環境光とプローブ可視性の共通画質契約。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-09 -->
# 描画方式を跨ぐ画質の共通契約

## 不透明の鏡面反射

Raster の SSR は受光側 Fresnel を掛けた反射から、同じ IBL 評価式の環境鏡面を差し引く。RGB は符号付きの鏡面差分で、confidence は材質の反射率と独立している。SSRResolve は ADDITIVE で不透明 HDR へ一度だけ差分を合成し、直接光・拡散 GI・発光を保持する。透明メッシュ・水面と Bloom の前で解決し、Composite で HDR 全体を反射色へ lerp しない。

Hybrid の SSR 出力は既存の Fresnel 適用済み RGB と confidence を維持する。Hybrid は DeferredLighting の provider resolver が所有し、SSRResolve を重ねない。単一鏡面方向が表せない粗いローブは prefilter 済み IBL へ戻す。

SSR の生産者と解決パスはそれぞれ HDR / SSRResult の依存を宣言する。Dispatch を記録していないフレームの永続テクスチャは採用しない。カスタム材質の深度・法線能力不足による縮退は既存の ResolvedRenderPlan が担当する。

## 地形と環境光

Forward / GBuffer は同じ TerrainSurface、材質の法線強度・roughness、天候と Specular AA を使う。描画方式による法線増幅・roughness 下限を持たない。法線は world の逆転置で変換し、非一様 scale と shear でも表面に垂直に保つ。描画資源は ResourceManager の RenderSharedResources が所有し、複数 Manager の交互描画と Reset で共有しない。Forward は IBL 有効時に Deferred と同じ PBR 環境光の供給元を使い、水中の光路による減衰も保持する。

拡散と鏡面 IBL の色を一律に無彩色へ寄せない。ルックの彩度は既存の Post Process Profile が調整する。遮蔽・金属度を無視した定数の間接光下限を加えない。Raster と ray hit は同じ拡散間接光の式を使う。

Forward のスキンド表面も静的表面と同じ IBL の照度・prefilter・BRDF LUT を束縛する。粒子のシェーダーと頂点貸出プール、雲の深度コピー、カスタム合成 PSO、BRDF LUT の焼き済み状態は ResourceManager の共有状態へ閉じ、別 Manager の数値ハンドルを流用しない。

DX12 の描画先も Manager の instanceId と RT ハンドルを組にして解決する。Clear / ClearDepth は最後に生成された Active Manager へ依存せず、実際に束縛した所有者を使う。内部の生存台帳は所有者の終了時に参照を除去し、破棄済み Manager や同じ数値ハンドルを持つ別 Manager へ戻らない。描画先の切り替え・診断プレビューの復元でも所有者を保持する。

## TAA の履歴

ビュー別の ping-pong RT は2面のカラーを持つ。第0面は完成色 (alpha=1)、第1面の R は log2(1+viewDepth)、空は -1。確率照明の HDR 履歴では R を負の対数深度へ符号化し、GBA は露出済み HDR 色を保持する。同じ描画提出で両方を更新し、リサイズ・カメラカット・provider 変更・TAA 停止の既存の履歴失効境界を共有する。

再投影先の深度が一致しない画素や空と形状が入れ替わる画素は履歴を棄却する。Velocity の A へ同じ頂点・骨・繊維の前フレーム NDC 深度を渡し、動く物体を現在の world 位置で再投影して比較しない。前回 VP と同じ near / far / 射影種別を b8 の予約領域へ保存する。被覆は本描画と同じジッター、速度はジッターなしの UV 差分を保持する。輪郭では3x3の最前景の深度・速度・履歴深度を同じ標本へ揃える。再投影の近傍標本の位置差を相殺し、静止時の履歴座標を出力画素中心に保つ。

静止画素は Halton 8 相に対応する raw 色を別の8枚の RGBA16F RT へ保持する。現在相の raw 色を第3 MRT へ出し、完成色・深度と同じ描画提出で更新する。raw RGB は LDR または露出済み HDR、A は直前から連続する有効標本数 [1,8]。深度・色空間・provider・連続フレームの検証が通り、移動が0.001 pixel以下、ParticleReactiveが0.0001以下の画素だけを有限窓へ入れる。

有効標本数を n とすると出力は直近 n 相の等重み平均、n は8を上限とする。8相が揃えば `y[t] = (c[t] + ... + c[t-7]) / 8` であり、静止した8相周期入力に対する出力は相に依存しない。現在色も保存面と同じ FP16 へ丸め、物理スロット0..7の固定順で FP32 加算する。差分 running sum は使わず、長時間の丸め誤差蓄積を避ける。[Moving Average Filters](https://www.dspguide.com/ch15.htm) を数式の根拠とする。

静止画素の有限窓を単相の近傍色で clip すると、高周波の被覆を変化と誤認して周期的な失効が再発するため、この分岐では clip しない。照明・材質の時間変化は常に最新8フレームへ入れ替わり、色を凍結しない。静止した面の色変更が窓全体へ反映されるまで最大8フレームかかる。深度不一致・新規露出・カメラカット・provider変更では有効長を1へ戻す。移動または反応マスクのある画素は従来の EMA、YCoCg の近傍平均・分散と min/max の交差による clip へ戻し、raw の有効長も1へ戻す。変形物の速度供給や透明物の多層履歴は別の制約として残る。

細い形状がある相で深度の3x3範囲から完全に消える場合、単一の深度だけではジッター被覆と物体の消失を区別できない。非ジッターのカメラ行列と不透明面の幾何・被覆入力が前回から完全に一致すると確認できたビューだけは、履歴 UV を出力画素中心へ固定し、被覆に起因する深度・空判定の切替を許可する。未対応入力、カメラ移動、幾何変更、Scene世代変更では通常の棄却条件へ戻す。色空間・provider・有限値の検証は免除しない。

GTAO の回転・ステップ雑音も TAA のピクセルジッターに同期する。TAA 有効時の同じ相・同じ深度・法線からは、時間に依存せず同じ AO を生成する。TAA 無効時は従来の時間変化を維持する。形状の周期被覆を平均する有限窓へ、独立した時間雑音を持ち込まない。

追加 raw 履歴は1920x1080で約126.6 MiB/view、3840x2160で約506.3 MiB/view。TAAが有効なビューだけ確保し、リサイズ・TAA無効化で解放する。確保失敗時は部分的な8相履歴を使わず、既存の2面 TAA へ戻して de-jitter を維持する。同じ寸法では確保を毎フレーム再試行せず、リサイズまたはTAA再有効化で再試行する。

RGBA16F の2面目による追加容量は内部 1920x1080 で約31.6 MiB/view (ping-pong 合計)。履歴は TAA が実効で有効なときだけ確保し、FXAA・AA無効・TAAパス停止時は両方を退役させる。再有効化では未初期化の履歴を棄却する。対数深度を通常の出力 alpha へ埋めず、透明合成やカスタムポストへ深度値を渡さない。

TAA の実効判定はジッター生成・履歴確保・パス登録で共通にする。Velocity の永続 RT は前フレームの内容を持ち得るため、当該ビューでクリア・更新した場合だけ TAA・MotionBlur・カスタム処理へ束縛する。停止したパスの古い速度を現在の移動量として扱わない。

Scene View のグリッド・NavMesh・深度付き Script Gizmo は TAA 前の HDR 深度へ描くため、モデルと同じ `MakeJitteredViewProjection` を使う。非ジッターの補助線をジッター済みモデル深度と比較しない。TAA 後の Script Gizmo Overlay は出力画素中心へ描き、非ジッターの投影を保持する。

標準 Unlit は `.hlsl.meta` に標準剛体頂点・Unlit 表面・不透明被覆を宣言する。PS が alpha を返しても discard や深度変更を行わないため、Opaque 材質で使ったときは不透明深度の静止入力を確認できる。Unlit を PBR / GBuffer 互換とは扱わず、AlphaBlend 材質の透明色履歴へ保証を広げない。

自動露出の順応はビュー別に保持し、設定または pass override で停止したフレームは準備段階で失効させる。無効なパスの Execute は呼ばれないため、失効をパス本体だけに置かない。再開フレームはその時点の輝度へ即時に合わせる。

## Light Probe の可視性

GI の捕捉は全サブメッシュを走査し、各 slot の可視性・材質・不透明性を評価する。LightProbeProject は同じ6面の捕捉深度を 8x8 octahedral タイルへ投影する。3x3の小さな方向近傍の radial distance / captureFar の1次・2次モーメントを RG、captureFar [m] を B、裏面率から求めた元プローブの有効度を A に保存する。タイルは Texture3D の XY に格子と共に並べ、Z はプローブの Z 座標とする。SH の膨張で有効度や可視性を別プローブから移さない。

引く側は格子の8近傍を個別に評価し、距離モーメントの Chebyshev 上界の3乗、法線方向、三線形重みを掛けて正規化する。全近傍が遮蔽されたときはボリューム内の黒を返す。IBL へ戻すと閉じた部屋へ空の光を再導入するためである。可視性資源を持たない入力は既存の SH 補間へ縮退する。

既定の 8x4x8 格子の可視性容量は128 KiB、最大 64x32x64 で64 MiB。8x8の角度分解能のため細い遮蔽物や疎な格子に近似誤差は残る。RT による DDGI そのものではなく、既存 Raster 捕捉へ可視性を加えた実装である。

AdvancedGraphicsCB は464 bytesを維持し、末尾の予約領域に2つの bindless SRV 添字を置く。DrawCall / ComputeCall の indirectReadTextures へ実体ハンドルを渡し、UAV から PS / CS の読み取り状態への遷移を backend が追跡する。SH、可視性、捕捉面は Scene の寿命で破棄する。

## Reflection Probe の描画項目

DynamicScene は各物体の全 RenderMeshItem を走査し、visible / slotVisible と材質・幾何の有効性を守る。不透明を先に描き、透明は renderQueue と捕捉面の奥行きで奥から描く。同点は Entity ID・材質 slot と元の項目順で安定させる。主カメラのソートを流用しない。スキンド・粒子・UI の被覆制約は維持する。

## 検証

GraphicsQualityShaderTest は実 DX12 で TAA の深度不一致・空の履歴・正常蓄積・粒子反応、全サブメッシュの速度、地形の材質値、環境鏡面の色、遮蔽時の定数光の除去、薄壁のプローブ可視性、全近傍遮蔽、SSR の符号付き差分と透明背景の alpha を読み戻す。地形・SSR・SH射影の本番シェーダーもコンパイルする。ReflectionProbeCaptureTest は複数サブメッシュ・非表示材質 slot・不透明と透明の順・捕捉面の奥行き順を出力で検証する。

2026-10-09 の最終 Development ビルドは EditorLauncher・GraphicsAuto・GraphicsStandalone の3対象でエラー・警告0 ([ビルドログ](../../build/agent/build-20261009-101703-72788.log))。CPU の描画計画・履歴・プローブ・パッチなど206件が合格した ([CPU結果](../../build/agent/test-20261009-101835-12768.log))。実 DX12 の画質・反射プローブ21件と水中光28件の合計49件が合格した ([GPU結果](../../Scratch/GraphicsQuality/FinalGpuResults.json))。最終合成の従来水中入力の当該フレームの光を保持し、共有水域の準備失敗へ流用しない条件も CPU 回帰で確認した。

SSRResolve の一時コピーだけで加算 alpha を0へ変えると、差分の3色が反映されず専用テスト1件が失敗した。元のシェーダーを使う同じケースは再合格した。本番アセットは検出力の確認で変更しない ([破壊時](../../Scratch/GraphicsQuality/MutationResults.json)、[通常時](../../Scratch/GraphicsQuality/RestoredResults.json))。

追加5項目の修正後は、GI 捕捉の後続・非表示 slot、複数 ResourceManager と Reset を跨ぐ地形描画、非一様・負の scale の地形法線、奥行き方向へ動く物体の TAA、TAA 停止・再開時の資源寿命を実 DX12 で検証した。EditorLauncher・GraphicsAuto・GraphicsStandalone の Development ビルドはエラー・警告0 ([追加ビルドログ](../../build/agent/build-20261009-131942-9696.log))。CPU 206件が合格し ([追加CPU結果](../../build/agent/test-20261009-131742-23568.log))、画質・プローブ26件と水中光28件の GPU 合計54件も合格した ([追加GPU結果](../../Scratch/GraphicsQuality/AdditionalGpuResults.json))。

GPU 時間と一般シーンでの前後比較は別途実測する。単体 fixture の合格をゲーム全体の画質・速度の保証へ換算しない。

さらに追加した5項目では、Forward の実スキンド PBR の環境鏡面、TAA / MotionBlur の古い速度棄却、Velocity 停止時のビューグラフ実行、自動露出の設定・pass override 停止からの再開、複数 Manager と Reset を跨ぐ BRDF ベイク・加算合成、所有者終了後の Clear を出力で検証した。GPU 合計61件が合格し ([追加GPU最終結果](../../Scratch/GraphicsQuality/RemainingGpuResults.json))、CPU 206件も合格した ([追加CPU最終結果](../../build/agent/test-20261009-142856-13048.log))。EditorLauncher・GraphicsAuto・GraphicsStandalone の最終 Development ビルドはエラー・警告0 ([最終ビルドログ](../../build/agent/build-20261009-143212-26196.log))。

TAA の当該ビューの速度更新判定を一時的に外すと、古い速度によって履歴が棄却され、専用テスト1件が失敗した。判定を修正版へ戻して再ビルドした同じケースは合格した ([速度判定の破壊時](../../Scratch/GraphicsQuality/RemainingVelocityMutationResults.json)、[復元後](../../Scratch/GraphicsQuality/RemainingVelocityRestoredResults.json))。

2026-10-27 は、静止輪郭11点の2周期、相によって完全に消える細線、HDR/LDR切替、深度・移動・反応マスクによる履歴リセット、GTAO雑音の位相同期を含む実DX12のGPU47件が合格した。通常ReleaseとLTO有効Releaseの両方で確認した ([通常GPU結果](../../Scratch/TAASilhouette20261026/StaticProofGpuTests.xml)、[最適化GPU結果](../../Scratch/TAASilhouette20261026/LtoStaticProofGpuTests.xml)、[最適化ビルド](../../build/agent/build-20261027-013320-52292.log))。描画計画・RenderGraph・描画キューの既存回帰57件も合格した ([回帰結果](../../build/agent/test-20261027-014224-50040.log))。

実シーン `TaaJunkPolishAllPhases` は通常の静止証明とGTAO同期を使い、最適化SDKで42手順が成功した ([シナリオ結果](../../Scratch/TAASilhouette20261026/CycleFinal/Batch.report.json))。ゲームを一時停止しカメラを固定したままシェーダー時間を進め、1144×644の画面を5フレーム間隔で16枚取得し、8相すべてを2巡した。輪郭27,764画素の変動幅は平均3.00×10^-8、95百分位0、最大8.34×10^-4、0.1超の比率0だった ([測定値](../../Scratch/TAASilhouette20261026/CycleFinal/FrameMetrics.json))。この測定は当該静止場面の時間変動を対象とし、基準画像との同等性や他シーンの速度を示さない。

Scene View の補助描画を含めた追加GPU回帰48件が通常Releaseで合格した ([GPU結果](../../Scratch/OverlapFlicker20261027/NormalGpuTests.xml))。本番の GridDebugPass と深度付き ScriptGizmo を、モデルと同じ投影で描いた参照画像と透視・平行投影の8相で比較し、差分0を確認した。遮蔽平面・交差斜面と、TAA後の非ジッターOverlayも検査する。独立したテスト用DLLだけを旧版へ替えると差分を検出し、修正版を復元すると合格した ([旧版](../../Scratch/OverlapFlicker20261027/OldGridRegression.xml)、[修正版](../../Scratch/OverlapFlicker20261027/FixedGridRegression.xml))。この回帰は投影・遮蔽の整合性を対象とし、任意の共面メッシュの競合を解消する保証ではない。

最新の作業台では、標準 Unlit の未宣言1件が静止証明を失敗させ、薄い形状の被覆切替で履歴が毎周期失効していた。Unlit の正当な宣言だけを補い、既存の棄却条件を維持した最終実シーン検証は、1251×666の Scene View を5フレーム間隔で16枚、通常描画・陰影なしで各8相2巡取得し、両方の全画素変動幅が0だった ([最終測定](../../Scratch/OverlapFlicker20261027/JunkFinal/Metrics.json)、[シナリオ](../../Scratch/OverlapFlicker20261027/JunkFinal/report.json))。別名でも宣言から認識し、未宣言の同名shaderを認定しないこと、alpha 0でもOpaque材質でRGBと深度が書かれること、標準GBuffer頂点経路と8相の被覆が一致することを追加GPU回帰で検証した。最終49件がLTO有効Releaseで合格した ([最終GPU結果](../../Scratch/OverlapFlicker20261027/FinalGpuTests.xml))。

正式SDKとprojectへの反映後は、実Editorの静止した Scene View 1904×893 / Game View 1144×644を各16枚、3フレーム間隔で8相2巡取得し、両方とも変動したRGB画素数0を確認した ([実Editor測定](../../Scratch/OverlapFlicker20261027/LiveFinal/LiveMetrics.json))。編集中の未保存Sceneは保持し、シーンもアセットも未変更の重複Editorだけを終了して、単一の接続先で測定した。

## @see

- [Temporal AA and the Quest for the Holy Trail](https://www.elopezr.com/temporal-aa-and-the-quest-for-the-holy-trail/) — 履歴棄却・速度・近傍 clipping。
- [Dynamic Diffuse Global Illumination with Ray-Traced Irradiance Fields](https://jcgt.org/published/0008/02/01/paper-lowres.pdf) §5.2 — 可視性を用いたプローブ補間。
- [Real Shading in Unreal Engine 4](https://cdn2.unrealengine.com/Resources/files/2013SiggraphPresentationsNotes-26915738.pdf) — split-sum IBL。
