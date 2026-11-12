<!-- @file    water-underwater-lighting.md -->
<!-- @brief   水中ゲーム向けの水の光学係数共有と影付き散乱光の合成契約。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-09 -->

# 水中の光と視界

この文書の検証ログは先行段階の記録。画素単位の水域境界、物体への入射光、局所ライト、実波の集光、接触演出、乾燥領域へ拡張した現在の契約は[水中ゲームの共通描画と物理](water-gameplay-visuals.md)を参照する。

水域の所属・水面高さ・有限水深は既存のSimulation snapshotとWaterSurfaceQueryで決める。光学モードの水域では、水面のRGB吸収・散乱係数とphaseGを水中でも共有する。旧光学モードの水中補正は従来の表示を維持する。Graphicsへは解決した値を渡し、Sceneや材質ファイルへ依存させない。

水中の色はHDRで背景放射輝度にBeer透過率を掛け、環境光の均質散乱と方向光の影付き単一散乱を加えた後、既存の露出・ACES・sRGB変換を受ける。固定青色のLDR霧と彩度低下を光学モードに重ねない。光のない環境では散乱は自己発光しない。方向光は空気から水への屈折と界面のFresnel透過、光源側のRGB消散、視線側のRGB消散を別々に扱う。[PBRT Transmittance](https://www.pbr-book.org/4ed/Volume_Scattering/Transmittance)、[Phase Functions](https://www.pbr-book.org/4ed/Volume_Scattering/Phase_Functions)、[UE Single Layer Water](https://dev.epicgames.com/documentation/en-us/unreal-engine/single-layer-water-shading-model-in-unreal-engine)を参照する。

共有水域の合成modeはシーン全体に適用するが、空気のFogを抑止する判定にはカメラの解決済み水深を使う。Compositeの`underwaterDepth`は光学水中なら実水深、陸上・水域外・乾燥領域なら0とする。水域が存在しても乾いたカメラでは通常FogとFroxel Fogを維持し、不透明物体と最初の可視水面のうち近い方まで適用する。空だけの画素には従来どおり通常Fogを適用せず、Froxel Fogはグリッド終端まで適用する。

方向光の積分は既存VolumetricRaw/VolumetricResultの半解像度と深度対応upsampleを再利用する。水中での結果はHDRへ先に加算せず、Compositeで透過後の背景へ一度だけ加える。記録成功をビューごとに保持し、無効化・失敗・Path表示では前フレームの散乱結果を読まない。上空の体積光は従来の水面前加算を維持する。新たな履歴・体積グリッド・全解像度レイマーチは増やさない。

体積光の積分は PostProcess の VolumetricLight が明示的に有効な場合だけ行う。Water 材質の Optical Volume と Underwater Light は媒体と光の修飾値であり、PostProcess を自動で有効にしない。水中も PostProcess のステップ数と距離を上限にし、強度を掛ける。体積光オフでも背景の RGB 消散と環境光は維持する。共通条件は [VolumetricLight の契約](volumetric-light.md) を参照する。

共有GPU媒体を束縛しない単体シェーダー経路では、カメラで照会した水面と水深のslabと矩形で視線を切る。現在の本番経路は実波のatlas、最初の可視界面、3Dの乾燥領域を照会する。透視・平行投影ではそれぞれ適切なレイ原点を使う。

材質の水中設定は`underwaterLightStrength`、`underwaterMaxDistance`、`underwaterLightSteps`、`underwaterTransitionDepth`、`underwaterDistortion`とする。既定は1、120m、48、0.15m、0.001。積分ステップは16〜64。大波の集約周波数・振幅・速度で界面の集光模様を動かし、深さでコントラストを弱める。これは光束を再配分する描画近似であり、波面の光子追跡や保存された流速場の変更ではない。

投影Causticsは太陽の色・強度・水中への屈折方向・RGB消散・ジオメトリの影・受光面の向きに結びつける。夜や完全な影に青い模様を加えない。共有GPU媒体を使う経路ではSpline/Zoneの輪郭と実波へ接続する。

光学水面の裏側は水から空気へのFresnel透過と臨界角の全反射を使う。表面から外側の背景放射輝度を読み、カメラから表面までの媒体はCompositeが担当する。水面裏側で水底向け散乱・水深の減衰を再加算しない。現在は通常の物体・Skinned・TerrainのBRDFへ入る照明にも入射側の消散を適用する。

GPU検証は距離別RGB消散、無光、方向光と影、上向き視線の水面終端、平行投影、設定無効化を対象にする。性能測定は前回のユーザー指定に従い後回しとし、画像と回帰テストで正しさを確認する。

## 設定と最終検証

Ocean材質と配布テンプレートは既定で光学mode1。既存の湖・川などは材質InspectorのOptical Volumeを有効にして共有係数を設定する。Underwater Lightで強度・距離・水面付近の切り替わり幅・歪み・積分ステップを調整できる。水中の領域自体はWater DepthとSplineの局所水深で決まり、水中の光の距離とは独立である。

方向光の視線消散と単一散乱の重みは均質なcellごとに解析積分し、影・光源側の消散・界面の集光はディザ点で評価する。濃い媒体で`σS * stepDistance`の矩形積分がエネルギーを過大評価することを避ける。集光と雲の評価位置は屈折した光の水面への入射位置とする。影を装飾設定で薄くしても、生のジオメトリ遮蔽は水中の光と投影集光へ残す。

2026-10-09のRelease全体ビルドはエラー・警告0 ([ビルドログ](../../build/agent/build-20261009-080458-76720.log))。関連CPU回帰367件が合格し、抽出・LOD・制御点・RenderGraph・Froxelと、光の無効化時に未生成の結果を読まない本番グラフ経路を確認した ([CPU結果](../../build/agent/test-20261009-081240-34856.log))。DX12のWaterShaderTest28件とUnderwaterShaderTest15件、計43件が合格した ([GPU結果](../../build/agent/water-gpu-underwater-20261009.json))。水面裏側の透過と全反射、臨界角付近の連続した変化、距離別RGB透過、平行投影、水面・横境界での光路終了、無光、位相、影、無効化後の結果除外、濃い媒体の散乱上限を検証した。

画像は`Scratch/WaterUnderwater/UnderwaterAmbient.png`と`UnderwaterSunshafts.png`。同じ床・屋根・RGB立体・カメラ・設定で方向光の体積散乱だけを切り替え、既存Compositeの露出1・ACES・sRGB出力を確認した。これは調整した材質と制御された影による描画fixtureであり、実ゲームシーンの撮影やOcean既定値の比較ではない。
