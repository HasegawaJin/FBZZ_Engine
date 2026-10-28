<!-- @file    water-spline-surface.md -->
<!-- @brief   スプライン水域の形状・基準高さ・水深・流速を描画と物理へ共有する契約。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-07 -->

Waterの形状は既存のRectangle、閉じたSpline Lake、開いたSpline Riverから選ぶ。湖の制御点は同じlocal Y、川は点ごとにlocal XYZ・全幅・水深・符号付き流速を持つ。複数水域のWater Zoneと河口接続は [water-zone.md](water-zone.md) に従い、海の岸線反転・島は後続とする。[Epic Water Body Actors](https://dev.epicgames.com/documentation/en-us/unreal-engine/water-body-actors-in-unreal-engine) の湖と川の役割を参照する。

正本はWaterComponentの制御点であり、生成Meshを正本へ置き換えない。各点には1〜INT32_MAXの一意なIDを保存する。位置と幅はlocal単位、水深と流速はworld m・m/sとする。yaw・親TRS・符号付き非一様scaleに対応し、従来と同じ水平TRS契約を使う。流向は変換した曲線接線をworld XZへ投影して正規化し、符号付き流速を掛ける。scaleで流速や水深を変えない。Splineの点流速が基準流れを定め、Rectangleの材質currentSpeedと入れ替わる。外部FlowFieldによる影響は引き続き加算する。

Catmull-Romの曲線を有限個の標本へ分け、湖は単純閉多角形、川は幅を持つ帯として三角形化する。輪郭は湖2048頂点・川4096頂点までとし、点数256・分割数64の個別上限を満たしても積が予算を超える場合は生成しない。水面内部は共有中点で細分化し、元の平面・水深・流速の線形補間を維持する。32768頂点・65536三角形または座標精度の限界では、直前の完全な段階を保ち、qualityLimitedと実際の最大辺長を返す。不変snapshotはlocal/world頂点、頂点ごとの基準高さ・水深・world流速、三角形の添字と高さ勾配、bounds、BVH、署名を持つ。描画のVB/IBとCPUの所属・重心座標補間は同じ三角形を使う。幅・深さ・流速の標本間補間を両経路で独自に作らない。

設定やTransformの変更時だけsnapshotを作り直す。無効な点・重複ID・非有限値・退化・自己交差・重なる川の帯・予算超過では数値生成のoutを変更しない。選択中の不正Splineは無効な水域として扱い、Rectangleへ暗黙に戻さない。SceneやGPUへのpointerをsnapshotへ残さず、Physics substepにはsnapshotとTransformの値を渡す。Edit/Playや複製は同じ不変snapshotを共有でき、編集後の新snapshotはそのWaterだけへ差し替える。

水面の照会はGerstnerの水平逆写像を解いてからsnapshotを標本化する。川の基準world Yへ波・流れの形・波紋を足し、法線には基準三角形の高さ勾配を含める。生成頂点の水深をbuoyancyDepthで制限してから、三角形内で補間する。水中判定と浮力の各標本は同じ底面を使い、深い洞窟へ浮力を延ばさない。描画の吸収距離も水柱深さで上限を合わせる。

Splineの描画はsnapshotの形をそのまま使い、Rectangle用の頂点集中・四分木LOD・far oceanを有効経路に混ぜない。保存したRectangleの設定は保持する。Splineの細かさは曲線の分割数と水面内部の目標辺長で指定する。波のGPU格子標本化は従来と同じ近似であり、描画の分割数で物理波を変えない。色と選択マスクは同じ頂点経路を使う。

Terrain情報と波紋のUV領域は、entity原点を中心としたSplineの対称包絡矩形を共有する。これは情報格納の領域であり、水域所属は三角形で決める。Terrainの水深はtexelごとのSpline基準高さから水底を引き、形状署名を再ベイク条件に含める。Splineの指定水深を仮想の底とし、地形がさらに浅い場合は浅い側を水深情報に使う。矩形を前提とする旧CausticsマスクはSplineに適用しない。

Inspectorで形状・分割数・点の位置・幅・水深・流速を編集できる。点の追加・削除・移動でも残るIDは保持する。Scene保存は制御点だけを保存し、snapshotとGPU資源は保存しない。旧SceneはRectangleで読み込む。SDKのliveな高さ照会は個別snapshotを準備し、同じcallback内でTransformを動かした直後も反映する。

SplineのTerrain光学情報は、変位前のパラメータUVで標本化して基準高さと対応させる。指定水深は共有属性として一致するが、実Terrain底面はGerstnerの水平変位とともに標本位置が移動する近似である。

検証は曲がる川・凹形の湖・異なる高さ/幅/水深/逆流・負非一様scale・無効形状・共有辺・Scene保存往復・複製後の編集を扱う。物理のContains/Applyと描画用頂点を比較し、色/選択の形状と水深・流速のGPU入力も確認する。

2026-10-07に実装と独立レビューを完了し、CPU回帰とSpline用GPU回帰3件を追加した。2026-10-08にRelease構成のEngineAutoをエラー・警告0でコンパイル・リンクした ([ログ](../../build/agent/build-20261008-074939-60180.log))。EditorLauncherとGraphicsStandaloneもエラー・警告0でビルドした ([ログ](../../build/agent/build-20261008-075546-40156.log))。Shaderの `point` 予約語衝突は修正済みで、Shaderのビルド対象は最新状態である。CPU/GPUテスト本体の実行とシーンの見た目は未確認である。
