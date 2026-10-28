<!-- @file    water-surface-quality.md -->
<!-- @brief   白波・遠景海面・Terrain 水深情報の描画契約。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-07 -->

白波は Gerstner の正の変位を振幅上限で正規化した高さ帯から発生させ、水平微分の圧縮で増幅する。`crestFoamThreshold` と `crestFoamFade` は高さ比、`crestFoamStrength` は被覆率の倍率、`crestFoamLifetime` は秒単位の減衰時間である。後者が 0 なら現在の波頭だけを評価し、正なら同じ位置で過去の波を 3 回評価して指数減衰した最大被覆率を残す。これは描画上の履歴近似であり、泡を流す物理シミュレーションではない。

波の変位・透過光の高さ比と泡の被覆率を別々に保持する。泡は `foamColor` の線形アルベドと `foamRoughness` による拡散光・空反射・鏡面光を受ける。暗所で自己発光させない。岸・着水・流れの泡は既存 `foamStrength` を使い、白波の強度と独立に調整できる。波と解析微分の根拠は [GPU Gems, Chapter 1](https://developer.nvidia.com/gpugems/gpugems/part-i-natural-effects/chapter-1-effective-water-simulation-physical-models) を参照する。泡の閾値と履歴は本エンジンの見た目の近似である。

`farOceanEnabled` が有効な合成形状を持たない Rectangle は Horizon 水域となり、水平の所属をカメラに依存せず全 XZ へ拡張する。水面と水中アトラスの生成範囲は同じ view の far clip とカメラ周辺の光路を包む。保存済みの格子設定を維持しつつ共有パッチを使う。有限面の境界 leaf に外向き strip を接続し、4 つの corner と合わせて外周を覆う。全境界 leaf を保つ要求は汎用 `SurfacePatchView::includeCulledPatches` で表し、視錐台による細分化の優先順位は維持する。接続辺の頂点位置と波の帯域を一致させる。外周の strip 同士も同じ外向き分割数と共有セル幅を使う。

`farOceanCoverageScale` はカメラの視錐台を覆う余裕、`farOceanRippleFadeDistance` は水域の外へ出る局所波紋が消える世界距離 [m]。海面有効時は有限面の外周透明化を止め、局所の岸マスクと波紋を遠景へ反復しない。浮力・水中・水域照会は同じ Horizon 所属を使う。`buoyancyDepth` と除外領域は維持し、Finite へ戻すと保存した Extent X/Z の所属を使う。Spline と WaterZone の合成形状は Horizon 指定があっても有限輪郭のままとする。Inspector にその契約を表示する。遠景を設ける考え方は [Unreal Engine Water Mesh](https://dev.epicgames.com/documentation/en-us/unreal-engine/water-meshing-system-and-surface-rendering-in-unreal-engine) を参考にするが、FBZZ の幾何と材質は同じ共有パッチで接続する。

Horizon の細かな描画範囲は Water owner の Transform と Extent に固定される。カメラ追従はその有限矩形内のパッチ LOD を選び直すが、矩形自体を追従させない。外側は有限面の境界から外周まで延長した低密度 strip/corner であり、船の近くまでカメラを移しても自動では細かなメッシュにならない。Horizon は共有パッチ経路を使うため、旧 `cameraFocusedGrid` を有効にしてもこの外側を細分化しない。

CPU の高さ照会・浮力は描画セル幅に依存しない実効波を使い、外周描画はセル幅で波の変位を帯域制限する。そのため Horizon の遠景では同じ波設定・時刻でも見えている高さと CPU 照会の一致を保証しない。船や水面接触など高さの一致が必要な interaction 範囲は有限の細かな描画範囲に収める。既存設定で広い行動範囲を扱う場合は Extent にその範囲と波の水平変位の余裕を含め、近景距離・セル幅・パッチ予算を確保する。外周の実誤差を近景の同期成功へ含めず、検証では使用した far/finite 経路と実セル幅を分けて記録する。

地形情報は Scene 内の有効な全 Terrain の三角形面を world 座標で照会する。親・回転・符号付き非一様 scale・穴を含む。重なる Terrain は最上面を選び、水面より上なら乾燥領域とする。底が無い場合は明示的な無効フラグと泡 0 を返す。Terrain の `surfaceRevision` と形状・Transform の署名で派生データを失効させ、高さ配列の有限値検査と bounds 計算は変更時だけ行う。

GPU 情報は水域 UV のテクセル中心で焼く RGBA8: R=岸泡、G/B=符号付き基準水深の上位/下位 byte、A=bit0 底あり・bit1 乾燥・bit2 深度範囲外。深度は `[-depthRange,+depthRange]` を UNORM16 へ写す。範囲外の底は画面上の接触水深へフォールバックし、深い底を浅瀬として扱わない。byte をフィルターしてから結合せず、4 テクセルを Load・復号した後に有効な底の水深を補間する。水平変位後の world XZ を水域基準 UV へ戻し、水面の波高を加えた実際の垂直水深から岸泡を評価する。光の吸収・屈折に使う画面上の水層距離とは独立である。地形データで波高を減衰させる処理は、CPU/GPU の同じ波を保つ契約が必要なため含めない。

GPU リソースは ResourceManager/Scene/Entity の既存 cache に所属させる。地形・水面・岸泡設定が変わらないフレームでは情報テクスチャを再生成しない。描画 view が複数でも情報は共有し、遠景パッチの選択と範囲だけは view ごとに計算する。CPU で全境界の位置・帯域・カリング・保存・地形編集の失効を、GPU 読み戻しで白波の照明・局所波紋・接続 coverage を検証する。

Terrain の内容検証 cache は既存 Component lifetime と `surfaceRevision` を使い、同じ Entity へ外した Terrain を再追加した場合も区別する。Terrain の借用 pointer は抽出の呼び出し内だけ保持する。構造が同じ frame 内で変わっても、次の抽出で現在の配列から取り直す。合法な `buoyancyDepth=0` は情報の無効化だけに使い、水面自体の描画を消さない。

## 抽出候補と水面材質の共通準備 (2026-10-16)

Terrain と Water の描画抽出は、各段の開始時に当該 Component の世代付き Entity 列を複製し、現在の Component と Transform を Entity から取得する。Component から Entity を探し直さず、元の Component 列順を維持する。Sky と Sun/Moon は型付き候補の和集合を元の GameObjects 順へ整列してから読むため、同じ型を遅れて追加しても最初の有効な設定を変えない。候補の作業領域は再入の深さごとに分け、終了時は Entity 値を捨てて容量だけ再利用する。

Weather も毎回型付き候補を Scene 所有順へ並べ、最初の activeInHierarchy かつ enabled の現在値を使う。callback のない選択中だけ候補を保持し、追加・削除・世代再利用・Scene.Clear 後の候補や値を次の呼び出しへ持ち越さない。

水面準備は Scene/Game のビュー間でも毎回現在の入力を確認する。共有するのは所有する不変 MaterialAsset publication だけから導く矩形 current・4 本の基底波・windResponse・grouping・spread で、WaterSystem.cpp 内の上限 32 件の作業領域へ保持する。現在の AssetHandle から publication を取り直し、完全に同じ publication の場合だけ値を再利用する。戻り値は数値のコピーとし、Scene・Component・Transform・可動 slot の借用 pointer を保持しない。材質の編集・置換・再ロードは同じ frame 内でも別の publication として扱い、保持上限を越えた入力も通常の導出と循環置換で処理する。個体の波倍率・有効設定・環境風は毎回適用し、可変な MaterialAssetAuthoring を直接渡す経路は cache しない。

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

Horizon では有限水域が視錐台から外れても、水面・水中アトラス・界面パスが同じ `ResolveWaterHorizonRectangle` の範囲へ外周パッチを提出する。アトラスの bounds と候補 table をこの範囲から作り、除外・plume も拡張後の範囲で抽出する。吸収・散乱の矩形光路クリップは Horizon で水平スラブを省き、有限水深と除外区間を保つ。GPU table の矩形種別 w=0 は非矩形、1 は Finite、2 は Horizon とし、配列サイズを変えない。資源・入力が不正な場合の fail-closed は維持する。

2026-10-11 の水面だけの検証は、水中領域まで同期する要件を満たしていなかったため、上記の共通契約へ修正した。保存キー `farOceanEnabled` と Extent は変更せず、古い Horizon データも読み込める。旧データの Horizon は新しい水平所属契約で動き、Finite データの挙動は維持する。
