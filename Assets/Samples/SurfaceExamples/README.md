<!-- @file    README.md -->
<!-- @brief   Project-authored Water, Grass and Fur の最小導入手順。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-24 -->
# SurfaceExamples — 海・草・Furの拡張

既存の共有描画を使い、プロジェクト側HLSLと `.mat` だけで見た目を編集するサンプルです。外部モデル・有料アセット・スクリプトは不要です。水面は左、草は中央、Hybrid Furは右にあります。シーン名と各オブジェクト名で、通常の制作アセットから区別できます。

## 開く・実行する

1. 現在のSDKで作成した空のManagedStarterプロジェクト、または検証用コピーを使います。配布物の `Assets` を同じ相対パスでプロジェクトへコピーしてください。シナリオもその中に同梱されています。既存の制作シーンへの割当ては不要です。
2. `Assets/Samples/SurfaceExamples/SurfaceExamples.scene` を開きます。標準の `Water/WaterSurface.hlsli`、`Fiber/FiberSurface.hlsli` と、その依存シェーダーを含むSDKが必要です。対応する共有geometry/capability契約を持たない旧SDKは対象外です。
3. 材質プレビューを確認してからPlayします。環境風は1 m/s、水面の波は最大振幅0.12 mと0.04 mです。草はBlade、FurはHybridです。シーンは6オブジェクト、草は上限2,000本、Furは12層・影4層、水面は最大16パッチに抑えています。
4. Playを停止します。自動キャプチャの入口は `Assets/Samples/SurfaceExamples/SurfaceExamples.playtest.json` です。通常のscenario runnerで起動し、1秒・3秒後のGame Viewを保存します。このシナリオはロードとキャプチャのスモーク用であり、画像の正しさや同期の合格判定は行いません。

## 編集→保存→再読込

| 材質 | 実際の入口 | 変更例 |
|---|---|---|
| `Assets/Materials/Samples/SurfaceExamples/CustomWater.mat` | `Assets/Shaders/Samples/SurfaceExamples/CustomWater.hlsl` の `PSMain(WaterPSInput)` | `shallowTint` / `deepTint` (float3)、`lightBands` (int)、`showFoam` (bool)、`albedo` |
| `GrassBlade.mat` | `Assets/Shaders/Samples/SurfaceExamples/Fiber/GrassBladeAppearance.hlsli` の2つの `FiberAppearance*` | `accentColor` (float4)、`patternScale` (float)、`colorBands` (uint)、`enablePattern` (bool)、`albedo` |
| `SoftFur.mat` | `Assets/Shaders/Samples/SurfaceExamples/Fiber/SoftFurAppearance.hlsli` の同じフック | ShellとFin両方へ同じ色・模様を反映 |
| `BaseSurface.mat` | 既存の `Assets/Shaders/Material/Surface/PBR.hlsl` | WaterBottom・草の地面・Furの球の共通ベース色 `albedo` と `roughness` |

3つのMeshRendererには、標準PBRの中立グレーの `BaseSurface.mat` をMaterialComponentで明示的に割り当てています。草とFurの外観は各FiberComponentの材質で編集し、下地はこの共通材質で編集します。ベース材質のシェーダー参照も必要です。空のshaderや材質未設定のメッシュはエラー材質へフォールバックします。未使用の水中Bubble・Sedimentは無効にし、材質参照も空にしているため、これらの追加アセットは不要です。

材質InspectorのProject Appearanceで値を変え、Save .matで保存します。例として水の2つのTintを青から赤、草/Furの `accentColor` を赤から青へ変更してください。シーンも保存し、一度別のシーンを開いてから戻り、材質を再選択して値とプレビューが維持されるか確認します。シェーダーを変更した場合は通常のReload Shadersを行います。元HLSLとincludeが実行時DXCへ渡され、変更した依存が再コンパイルされます。失敗時に旧成功版が残るため、エラー診断も確認してください。材質値だけの変更ではシェーダーソースの再コンパイルは不要です。Play中だけの変更を永続化したつもりにしないでください。

8×8の `Assets/Textures/Samples/SurfaceExamples/Pattern.png` は同梱の外観テクスチャです。`albedo` は既存の正規スロットであり、GPU descriptor番号を `.mat` に書きません。未割当て時の分岐もあります。Fiberの `tex5` はgeometry mask予約なので、外観模様の代わりに使いません。geometry maskを追加するときはRGBAを密度・長さ・毛束・毛先色として[0,1]、sRGB=falseで扱います。

## 共有処理を保つ範囲

水は `water_surface_v1` + `water_v1` を宣言し、`WaterSurface.hlsli` のVSを変更しません。`WaterSurfaceExcluded`、footprint/edge clipも維持しています。波を変える場合は `.mat` の `wave*`、WaterComponent、WaterWavesAssetの対応する設定を使います。時間・波・flow・rippleはエンジンの共有サービスが正本です。任意の独自頂点HLSLをCPUの浮力へ自動同期する機能はありません。このバンド色サンプルは標準optics/SSRの全表現を再現しません。水中volumeはエンジン側です。mesh設定時も既存のWater indexed fallbackの範囲です。

草はstatic Bladeで、chunkedPlacement、既存ページとカリングを使います。`bladeSegments=3` は既定6より曲線近似が粗い設定です。接写・強風の品質が必要なら6へ戻してください。`shadowBladeSegments=0` は本描画を継承します。全パスで同じ共有風・接触変形・被覆を使います。

FurはShellとFinのstatic/skinned各5パス、計20wrapperを同梱します。Shellのみも使用できます。Hybridの影は既存仕様どおりShellのみです。このシーンにskinnedモデルはありません。草5wrapperとFur20wrapperは、同じ `AppearanceFields.hlsli` を使い、engine-ownedの先頭128 bytesを変更しません。色とGBuffer以外も含めて、共有変形・LOD dither・被覆を保持します。

外観フックへ独自頂点変形・alpha clipを追加しないでください。共有契約の宣言は作者の約束です。独自変形には対応する全パス・保守的bounds・必要なCPU samplerが別途必要です。既存の最大長と曲げ、個体上書きを使う間は共有bounds計算を利用します。shader/family不足やモード不一致は診断付きのbuilt-in fallbackとなり、見た目が出ることだけでは独自familyの成功を証明できません。

## 事前コンパイルと再コンパイル

このシーンは `farOceanEnabled=false` の小さな水面です。水平線まで続く遠景海面の形状・LOD・水中volumeはエンジン側の機能であり、外観HLSLの変更だけでは追加されません。遠景を試す場合も共有Waterの設定と契約を使ってください。

エンジンの `Assets/Shaders/compile_shaders.ps1` は `Assets/Shaders` 配下の全 `.hlsl` を再帰収集します。この配置の26入口も対象です。SDKのAssets再帰コピーとは別の処理です。ソースの事前展開を行う専用ShaderImporterは不要で、既存の実行時loaderも元HLSLをDXCへ渡します。

25本のFiber wrapperには `#if 0` 内のliteral includeがあります。これはオフラインスクリプトのliteral依存走査へ外観/フィールドincludeを知らせる宣言で、コンパイル時には無効です。有効な `FBZZ_FIBER_MATERIAL_FIELDS` / `FBZZ_FIBER_APPEARANCE` macro hookで各フックを一度だけ読み込みます。この宣言を除くと、外観/フィールドだけを更新した場合にオフラインの既存CSOを再生成し損ねる可能性があります。実行時のShaderDependencyTrackerは既にmacro includeへ対応しており、別の経路です。

通常の利用者はReload Shadersで再コンパイルし、材質を保存→別シーン→再読込で確認します。エンジン側で事前CSOを用意する場合は通常のシェーダーcompileタスクを使ってください。無効ブロックも含めた依存ファイルの更新後に、26入口の生成結果と診断を確認します。任意フォルダーへ単純移動すると最寄り `Shaders` がinclude rootになるため、共有 `Common/...` / `Fiber/...` 等の解決が崩れる場合があります。GUIDだけ直して移動を完了したと判断しないでください。

## 検証状態

2026-10-24の通常Releaseで、26入口のshader/capability検証、3材質の編集・保存・再読込・プレビュー、シーンの保存・再読込をGPU回帰3件で確認しました。隔離プロジェクトの非表示Playtestは9手順を完走し、1秒・3秒の実画像で水・草・Hybrid Furと通常ベース材質を確認しています。実ログに材質エラーやbuilt-in fallbackはありません。これは小さなstaticサンプルの機能確認で、性能比較、実GUI操作、skinnedモデルを使った実シーン検証の代わりではありません。

対応契約の詳細: `Docs/design/water-appearance-authoring.md`、`Docs/design/fiber-project-appearance.md`、`Docs/design/shader-capabilities.md`。
