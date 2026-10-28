<!-- @file    surface-patch-lod.md -->
<!-- @brief   連続面の共有グリッド・有限四分木 LOD・境界標本化の契約。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-07 -->
# 連続面の汎用パッチ LOD

## 責務

Graphics の SurfacePatchLod は、水面・地形・高さ場に共通する有限矩形の分割と可視パッチの選択を担当する。Scene、材質、水の波、GPU API、ファイル形式を知らない。任意の三角メッシュの簡略化は別の問題として扱う。

入力は local XZ の矩形、world 行列、高さの範囲、水平変位の上限、ビューの位置・投影係数・視錐台、セル密度と予算である。結果は安定した depth/x/z、矩形、セル幅、隣接辺の stitch mask を持つ。選択結果はビュー所有で、共有 Scene の入力や物理へ書き戻さない。
同じ frame/view の描画パス間では `SurfacePatchSelectionCache` を使って選択と辺・角の接続結果を再利用できる。key は surface の変位境界、world 行列、全 LOD/morph 設定、camera 位置・前方・投影条件・viewport 高さ、includeCulled と視錐台 6 平面を値で比較する。frustum ポインターや water input のアドレスだけでは一致とみなさない。Water は cache 用の view を includeCulled=true に揃え、均衡化済みの全 leaf を共有する。各パスは元の view 条件で可視 leaf を絞ってから統計と描画を行うため、far ocean の許可差が選択 cache を分けない。far mesh 自体はこの cache の外で各パスの許可条件から生成する。別 camera の媒体 atlas は独立した frustum を持つので main view の選択を再利用しない。cache は RenderPassContext が所有し、最大 8 選択・合計 16384 patches まで保持する。枠または予算を越える場合は最古を追い出し、単独で予算を越す選択は保存しない。

## 選択と予算

四分木を投影セル幅と近景のセル幅から分割する。これは格子の標本化幅であり、地形の高さ誤差を保証する metric ではない。maxDepth と maxPatches に達した場合は粗い葉を保持し、qualityLimited を返す。予算不足を理由に面を欠落させない。

隣接葉は 2:1 までの解像度差へ均衡化する。境界の評価に必要な葉を保った後、ビューの視錐台で出力を絞る。入力の高さ・水平変位は local 単位で、変位込みの箱を world へ写す。Water の world m の波は軸のスケールで local 上限へ戻してから渡し、縮小で安全域を過少評価しない。

## GPU と境界

固定分割の unit grid VB と、粗い隣接辺の組合せ 16 種の IB を共有する。粗い辺に接する奇数頂点を偶数頂点へ寄せた index を使い、同じ境界の頂点列で接続する。GPU 実体を所有する cache と、非所有の描画候補を分ける。

変位も境界で一致させる。内部は自身、辺は隣接葉との最大、角は接する全葉の最大セル幅で波を帯域制限する。CPU helper と HLSL が同じ規則を使う。角の影響を異なるパッチ長の辺へ独自に補間しない。任意設定のLODモーフは親fanへ一致する階層写像を使い、詳細は [water-wave-optics-morph.md](water-wave-optics-morph.md) に従う。予算で強制したLOD変更の時間履歴は後続とする。

## Water の最初の利用

adaptivePatchLod を保存する opt-in とし、旧 resolution/chunkCount/cameraFocusedGrid の意味を維持する。新経路は unit grid をパッチの local 矩形へ展開し、泡・波紋の UV は水域全体の矩形から求める。旧カメラ集中の非線形格子写像は同時に適用しない。

水面の色と選択マスクは同じパッチ選択・頂点変位を使う。物理水面は描画 LOD から独立した波を評価する。Terrain の既存 LOD は維持し、本基盤へ接続する adapter は別途実装する。

標準Waterの色・媒体atlas・界面・選択マスクは、同じVB/IBとindex数を使う連続パッチをインスタンス描画する。元のパッチ順を保ち、有限面と遠景の境界で束を分ける。矩形、セル幅、辺・角の帯域、morphの深さと軸を192 byteのVS slot 0へ載せ、body/viewの行列・波・媒体定数はWaterCBで共有する。patchCellSize.z=2だけがこのlayoutを読む。custom shader、旧格子、単一パッチ、確保失敗は従来のCB描画を維持する。DynamicStructuredBufferPoolは束・パス・ビューごとに借り直し、未完了GPU描画のデータを上書きしない。

## 検証

有限矩形の被覆、可視選択、予算時の被覆維持、2:1 均衡、全 stitch mask の面積と index、共有頂点の位置・帯域一致、ビュー別独立性、非有限入力の無変更失敗を CPU の出力境界で確認する。Water は保存往復と、実際のシェーダーを使う GPU 描画で接続を確認する。

2026-10-07、SurfacePatchLodTest 13 件は [関連回帰](../../build/agent/test-20261007-155745-40540.log) 内ですべて合格した。小さい汎用 scale、導出 overflow、不可視 coarse 隣接と可視 fine の帯域保持も含む。Water の共有パッチと混在 LOD は WaterShaderTest の実 GPU 描画で確認した。

## 参照

- [GPU Gems 2 Chapter 2: Terrain Rendering Using GPU-Based Geometry Clipmaps](https://developer.nvidia.com/gpugems/gpugems2/part-i-geometric-complexity/chapter-2-terrain-rendering-using-gpu-based-geometry): 共有グリッドと境界の連続性。
- [Epic: Water Meshing System and Surface Rendering](https://dev.epicgames.com/documentation/en-us/unreal-engine/water-meshing-system-and-surface-rendering-in-unreal-engine): 水域とタイル LOD、遷移、遠景の役割。
