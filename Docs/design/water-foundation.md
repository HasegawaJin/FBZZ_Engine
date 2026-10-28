<!-- @file    water-foundation.md -->
<!-- @brief   Water の座標・水域照会・物理・描画品質の分離契約。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-07 -->
# Water の共通基盤

## 初版の責務

WaterComponent は既存の有限矩形・材質・実効波を正本として保持する。WaterSurfaceQuery が水域への所属、水面高さ、水没深さ、法線、流速を world 単位で評価する。水中効果、着水、浮力、既存公開高さ API は同じ契約へ接続する。Graphics は Scene を知らず、解決済みの値と GPU handle を受け取る。

初版は水平な高さ場である。親 Transform、yaw、正負を含む非一様 scale を扱う。各 world scale の絶対値は 1e-4 以上とする。pitch/roll による傾斜面は、現在の world Y 波変位と法線の契約では表せないため supported=false とし、描画・照会・物理を同じ条件で除外する。

## 座標と照会

水面の基準高さは worldPosition.y、波の変位と流速は world m と m/s。Gerstner の水平逆写像を解いてから、yaw と signed scale の逆変換で元の水域矩形への所属を判定する。矩形外の高さ外挿は既存 API との互換のため維持し、insideSurface と分離する。

水中の領域は表面から buoyancyDepth までの有限深さとする。初版は既存の保存名を維持して水中表示と浮力領域に共有し、Inspector の Water Depth は浮力を無効にしても編集できる。照会失敗では out を変更しない。高さと所属だけが必要な経路では、法線差分や流速場の標本化を行わない。Physics は値の snapshot と不変の flow field を受け取り、substep 中に Scene を参照しない。

## 波と描画品質

物理・水域照会は、材質または波アセットと個体倍率から解決した波と波紋を評価し、resolution や描画 LOD で振幅を変えない。任意設定の浅水減衰は物理と描画で同じ不変の深度snapshotを使い、波の逆写像にも反映する。波アセットと減衰の契約は [water-wave-optics-morph.md](water-wave-optics-morph.md) に従う。描画だけが格子の実際のセル幅で変位を帯域制限し、残差を法線と粗さへ渡す。遠景の粗い描画と物理の差は LOD の近似として扱う。

新しい共有パッチの LOD は [surface-patch-lod.md](surface-patch-lod.md) に従う。旧格子の保存項目は維持する。色・選択マスクで同じ頂点評価を使い、平行投影でも水面の view Z から深度差を求める。

## 資源と今後の接続

GPU cache のキーには Scene generation と Entity generation を含め、Edit と Play の別 Scene や Entity 再利用を区別する。ResourceManager の単調な instance ID と Reset 世代で、管理者のアドレス再利用も区別する。Scene/Entity の返却入口からその所有分だけを解放し、共有パッチの handle を非所有の描画候補から解放しない。波紋の時刻は WaterSystem だけが進める。

白波・地形水深情報・遠景海面は [water-surface-quality.md](water-surface-quality.md) に従う。湖と川のスプライン形状・水深・流速の共有は [water-spline-surface.md](water-spline-surface.md)、接続水域の和集合・優先順位・単独浮力と河口補間は [water-zone.md](water-zone.md) に従う。海の岸線・島、Zoneのビュー別タイルLODと個別材質/波の遷移、地形の非破壊編集、局所 interaction atlas、Water motion 履歴は後続として扱う。

光学mode1の水中HDR消散・方向光の散乱・水面裏側の透過と全反射は [water-underwater-lighting.md](water-underwater-lighting.md) に従う。水中照会の正確な所属と、描画のカメラ列に基づく光路近似を区別する。

## 検証

親・yaw・負非一様 scale、矩形境界、波の逆写像、有限水没深さ、無効入力、旧高さ外挿、物理の領域、保存往復、描画品質変更で変わらない波高を検証する。色と選択・平行投影は実シェーダー描画の出力で確認する。

2026-10-07 の Development 構成で、関連 CPU 回帰 136 件がすべて合格した ([ログ](../../build/agent/test-20261007-155745-40540.log))。変更 C++ 19 単位と保存経路 2 単位のコンパイル検査はエラー・警告 0 ([本体](../../build/agent/check-20261007-153510-51884.log)、[保存](../../build/agent/check-20261007-155518-21268.log))。

RTX 4070 / DX12 / Agility 1.619.6 で WaterShaderTest の 5 件が合格した。平行投影の水層距離、集中格子の選択、旧格子と共有パッチの一致、混在 LOD の穴と選択 coverage、実選択パスの負スケールを GPU 読み戻しで確認した。旧 clip w の水深式へ一時的に戻すと専用テストが失敗し、原本復帰後に全 5 件が再合格した。これは描画契約の検証で、GPU 性能の改善率や一般シーンの品質保証ではない。
