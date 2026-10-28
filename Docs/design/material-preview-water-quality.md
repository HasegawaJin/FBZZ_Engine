<!-- @file    material-preview-water-quality.md -->
<!-- @brief   水・粒子の材質プレビューと水中集光の画質契約。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-09 -->
# 材質プレビューと水中の集光

AssetBrowserと材質Previewは既存MaterialPreviewCoreを共有する。shader未指定のparticle材質は、本番と同じCPU Particle shaderで描き、材質の色・発光倍率・照明・透明度を維持する。メッシュ用の紫の代替shaderへParticleVertexを流さない。独自shaderが明示されているときは指定を維持する。

Water材質は本番のResolveWaterWavesを使い、材質の波長・振幅・向き・群・方向広がりを反映する。プレビューの水面を32m四方として実寸の波を表示し、時刻の倍率を掛けない。水を平らに指定した材質へ架空の波を加えない。シーンの水深・地形・光・流れは材質だけでは決まらないため、プレビューの観察リグと区別する。

水中の集光は追加の放射輝度としてHDRへ加算し、暗い模様としてScene色を乗算しない。通常の幾何影・集光・水中散乱を別々に切って同一視点で比較する。屈折と集光は水面形状に追従し、自己遮蔽の誤差や床の奥行きの丸めでまだらな影を増やさない。

実シーンの比較では、まだら模様は集光を無効にすると消え、通常の物体影は残った。既定の密な正弦模様を、周期的なセルの境界から作る細い光の網へ変更する。強さは波の圧縮と水面高さの曲率へ結び、平らで圧縮のない面の模様を消す。曲率と法線は4 texel幅で評価し、捕捉メッシュの三角形単位の階段を均す。受け面の影には本番の通常照明と同じ上限6倍の傾斜バイアスを使い、水中の体積位置の影バイアスは維持する。

受け面の法線をworld Yの符号で上向きにすると、垂直な壁の微小な復元誤差で表裏が反転して集光が点滅する。表示された不透明面はカメラへ向く法線を選び、床・壁・天井のいずれも同じ向きの契約にする。射影の微分は分岐前に計算し、集光テクスチャへ明示的な画素範囲を渡す。

参照は[UE Single Layer Water](https://dev.epicgames.com/documentation/en-us/unreal-engine/single-layer-water-shading-model-in-unreal-engine)、[GPU Gemsの水中集光](https://developer.nvidia.com/gpugems/gpugems/part-i-natural-effects/chapter-2-rendering-water-caustics)。画素の比較と実GPUの出力で確認する。

GUIDで指定したshaderの参照を、LoadShader・ReloadShader・ReloadAllShadersの共通経路で実ファイルへ解決してからバックエンドへ渡す。GUIDを残したままキャッシュ名へ書き換えると、パス解決が再び元のHLSLを指すことがある。DX12の保存側も.cso以外を拒否し、ソースへの誤保存を防ぐ。GPU回帰では、明示GUIDと既定のParticleが同じ画像になり、読込・コンパイルで元のHLSLが変わらないことを検査する。

## 検証

2026-10-09: Releaseの変更ファイル検査、GraphicsStandaloneとSDKビルドはエラー・警告0。GUID修正のログは`build/agent/build-20261009-180251-17892.log`、壁の法線を含む最終SDK更新は`build/agent/build-20261009-181018-66988.log`。水中描画31件と材質プレビュー2件、合計33件の実DX12検査が合格した（`build/agent/water-reference-gpu.json`）。GUID明示の追加検査は修正前に元HLSLへの誤保存を検出し、復旧・共通参照解決・保存先制約の修正後に合格している。変更したC++/HLSL/CMakeの全体AgentLintは10ファイルでエラー・警告0。

指定ゲームのWaterLabの106手順が最終Release SDKで合格し、比較画像13枚を更新した。集光・通常影・GTAOを分けた診断も45手順が合格し、4枚の比較を保存した。報告と画像はゲームの`Library/Playtests/WaterLab`と`Library/Playtests/WaterShadowProbe`。サムネイルの実GPU出力は`Scratch/MaterialPreviewGpu`。性能比較はこの検査の対象外。
