<!-- @file    water-reference-tuning.md -->
<!-- @brief   UEの水・体積光・Niagaraと実描画出力に基づくWaterLabの改善契約。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-09 -->

# 水中表現の参照と検証

WaterLabの水中光、泡・砂、乾燥領域の比較を、既存の共有水域・描画・物理境界を保って改善する。係数を増やす前に、本番の数値入力がGPUの媒体照会と実際の粒子描画へ届くことを検証する。

## 参照

- [UE Single Layer Water](https://dev.epicgames.com/documentation/en-us/unreal-engine/single-layer-water-shading-model-in-unreal-engine): RGBの吸収・散乱、位相関数、反射・屈折と水面下の色。
- [UE Volumetric Fog](https://dev.epicgames.com/documentation/en-us/unreal-engine/volumetric-fog-in-unreal-engine): 光源の散乱寄与、影、局所的な密度と視線方向による見え方。
- [UE Niagara Renderers](https://dev.epicgames.com/documentation/en-us/unreal-engine/niagara-renderers?application_version=4.27): 粒子に割り当てる材質・サイズ・向き・色・透明度を独立に扱う。
- [PBRT Transmittance](https://www.pbr-book.org/4ed/Volume_Scattering/Transmittance)と[Phase Functions](https://www.pbr-book.org/4ed/Volume_Scattering/Phase_Functions): 光路長に対するRGB透過とHG散乱の方向規約。
- [PBRT Distant Lights](https://pbr-book.org/4ed/Light_Sources/Distant_Lights): 光源色と強度が入射放射輝度を構成する。

FBZZの描画手法・単位へ合わせて参照する。UEのパラメータ数値をそのまま移植しない。水面のBRDF、視線の透過、光源から物体へ届く光、自己発光を分け、同じ水中経路を重ねて吸収しない。

## 泡と砂の確定した問題

既存Particle shaderの`emissive_scale`は最終RGBの出力倍率としても使われている。WaterBubblesとWaterSedimentを0にすると、照明で得たRGBまで消える。既存の炎材質との互換を保ち、この倍率を1にし、six-wayの発光色・発光量を0へ明示して受光と自己発光を分ける。

ParticleLightingのDirectional項は光源色と強度を別々に受け取る。強度を掛けないと太陽の強度0でも直接光が残るため、非負の強度を一度だけ適用する。環境光・局所光・発光・最終出力倍率へ重ねて乗算しない。

## 本番入力を使う検証

既存の水域GPU検査は数値の`medium`を直接設定するものが中心だった。Engineの`ExtractRenderWater`が返す実効材質、波、水域形状、除外、局所濁りを、そのまま媒体テーブル・atlas生成・GPU照会へ渡す統合検査を加える。

照明を有効にした粒子が非ゼロ、光源色を保って強度0にした直接光がゼロ、発光だけを有効にした粒子が暗所でも残ることを本番ParticleパスのHDR出力で検証する。明るさ倍率と発光の名前を変えて既存材質を一括変更する範囲にはしない。

実シーンでは光ON/OFF、演出の前後と寿命終了、乾燥領域ON/OFFを同じ視点で撮り比べる。撮影成功だけを効果の視認性の証拠にしない。見た目と処理負荷は別に記録し、性能計測はユーザーの指定どおり後回しにする。

## 復旧後の水質と集光

水域テーブルの候補列開始位置を、全グリッドセル見出しの後へ直す。以前はセル見出しを水域番号として読んでおり、多くの照会で水域が消えていた。本番抽出を使うGPU回帰で、水面の実捕捉、候補探索、水中照会、除外、局所濁り、RGB透過まで検査する。

復旧後は、診断用に強めていたWaterLabの吸収・散乱を澄んだ水へ調整し、近景の立体と泡が見える視程を残す。RGBの総消散は約0.055〜0.13 /mとし、1/eの距離は約8〜18m。深部の赤の減衰を保ち、散乱の明るさで一様な青を足しすぎない。

既定の集光テクスチャは1段だけだったため、細かい模様が遠景と斜めの面でちらつく。既存の共通RGBA8縮小処理で1×1までのmip列を作り、通常のシェーダーサンプリングへ渡す。[PBRT Image Texture §10.4.2](https://www.pbr-book.org/4ed/Textures_and_Materials/Image_Texture)の画素範囲に応じたフィルタリングを参照する。波に追従する屈折・水域の境界は維持し、サンプル側の集光の倍率・模様の密度も下げる。

## 配置先

参照用Sceneとアセットの正本は`Samples/WaterLab`。指定された`FBZZCTestProject`にはScene、独自材質、粒子材質、C#制御、Playtestを追加し、既存Scene・SDK選択・C#プロジェクトを維持する。入力設定がなかった場合はEngineの既定操作も残してWaterLab専用入力を追加する。

## 検証記録

2026-10-09: Release SDK 0.10.0を更新・公開し、nativeの変更ファイル検査とSDKビルドはエラー・警告0。指定ゲームの既存C#プロジェクトも公開Release SDKでエラー・警告0。水面抽出・資源のCPU回帰18件、実DX12の水域・照明・粒子回帰17件が合格した。変更した7つのC++/HLSLファイルは全体AgentLintがエラー・警告0。

指定ゲームのWaterLabは最終水質とmip付きの集光で106手順が合格し、13枚を撮影した。同じ視点の画像をHUDの下で比較した平均RGB差（8bit、0〜255）は、Spot ON/OFFが約11.5、乾燥領域ON/OFFが約43.6、泡・砂の前後が約10.1だった。画像差は波の時間変化も含むため、単独機能の正しさをこの値だけで判定しない。照明・媒体の契約はGPU回帰で別に検査している。

記録は`build/agent/build-20261009-162352-4000.log`、`build/agent/test-20261009-160051-61520.log`、`build/agent/water-reference-gpu.json`、指定ゲームの`Library/ManagedBuild/agent/managed-build-20261009-155855-65720.log`と`Library/Playtests/WaterLab/report.json`。
