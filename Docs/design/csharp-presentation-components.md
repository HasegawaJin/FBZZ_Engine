<!-- @file    csharp-presentation-components.md -->
<!-- @brief   Line / Sprite の実描画、入力 snapshot と Material の authoring・runtime 上書き契約。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-06 -->
# C# Line / Sprite と native 描画

Line と Sprite は既存 native Component を操作し、PresentationSystem が生成する標準 MeshRenderer と MaterialComponent を使う。Component を取得しただけで追加しない。GameObject の有効化、Renderer の有効化、Line / Sprite 自体の有効化、Material の有効化と slot の可視性は独立する。PresentationSystem は renderer.enabled と material.enabled を変更せず、Line / Sprite が無効なら描画用 mesh pointer を公開しない。

## Line の色・空間・cache

startColor / endColor の linear RGBA を全経路の距離比で補間し、標準 Vertex の color に載せる。帯と筒の両方で終端 alpha まで渡す。閉路は最後の区間でも距離を進め、最初の位置に終端色の頂点を別に置くため、始点色と終端色が異なる閉路には明示した色の継ぎ目ができる。重複した隣接点の区間は描画しない。

Local の点と幅は local の m。World の点と幅は world の m で断面を生成してから world TRS の逆変換で renderer 用の local 頂点へ戻す。回転、非一様 scale、負 scale があっても指定した world 点と幅を保持する。World またはカメラ向きの帯に必要な inverse が特異なら空の geometry とし、古い mesh を表示しない。ゼロ長経路、非有限値、負の幅も同じ扱い。

変更検出は hash の一致に依存しない。点列を順序付きで全成分保持し、両端の幅・色・空間・loop・billboard・shape・radialSegments、および vertex に必要な world position / rotation / scale / camera position を float snapshot として比較する。Tube は camera を含めず、camera が動いただけでは再生成しない。Local の camera-facing Ribbon は camera を local へ戻すため owner の world TRS も比較する。Sprite は UV・出力 size/pivot・flip・drawMode・元画像の寸法・border・pixelsPerUnit を比較し、metadata の編集を取りこぼさない。

## Sprite の Sliced と照明

Sliced は ResolveSpriteReference が返す atlas UV と border L/T/R/B を用いて最大 nine-slice の quad を生成する。border の local 寸法は pixel / pixelsPerUnit。出力が両 border の合計より狭い場合は両方を同比率で縮め、中央を 0 にする。flip は UV だけでなく左右・上下の asymmetric border も入れ替える。source 未解決や有効な寸法がない場合は通常 quad を作り、次の source size / metadata 解決で再生成する。

SpriteFallback / Line の既定 VFXMeshFallback が共有する標準 Unlit shader は Vertex COLOR を読み、頂点 RGBA × effective albedo × texture を返す。Line の Presentation albedo は白なので、startColor をもう一度掛けない。

receiveLighting は Presentation の receiveLighting numeric default を 0 / 1 にし、標準 Unlit shader が 1 のときだけ既存 Lambert、方向光、点 / spot / area の既存走査、shadow と screen contact shadow を評価する。光は RGB だけに作用し alpha を変更しない。独自材質は shader が同じ numeric receiveLighting を宣言・評価する契約とする。独自 shader を強制的に別 shader へ交換しない。Line の独自 shader は COLOR semantic を宣言して色を PS へ渡す。

照明へ渡す colour albedo texture は標準 PBR と同じ sRGB 入力として Common/Color.hlsli の SRGBToLinear を通し、その後に線形の材質 tint と頂点 colour を掛ける。alpha は変換しない。通常の Unlit 表示は従来の sampling を保つ。SkinnedUnlit も材質 × texture の full RGBA と同じ optional receiveLighting を持つが、SkinnedVertex の配置を保つため COLOR semantic を追加しない。ボーン変形後の normal / world position を既存照明へ渡す。

## Material の優先順位

共有 .mat の値 → Presentation authoring defaults → 明示した Material runtime overrides の順に重ねる。Sprite の color / texture、Line の白 tint、両面と sorting 由来 queue、receiveLighting は独立した presentation defaults に入れ、user の paramOverrides / integerParamOverrides / textureOverrides と描画 state override を書き換えない。ClearOverride は明示 override だけを消し、次の描画から最新の authoring default が現れる。

Material の共有割当を明示したときは hasPresentationMaterialOverride を slot に保持する。同じ path の再指定と空参照による解除も明示割当なので、以後の Presentation update が authoring material path で戻さない。異なる .mat が authoring から変わった場合は GPU / reflection cache を破棄して再解決し、runtime numeric / texture overrides は保持する。別の MaterialComponent にある explicit authoring material は初回 projection でも保持する。

sortingLayer * 1000 + orderInLayer と祖先 SortingGroup の和は int64 で求め、3000 を足した effective queue を int32 の範囲へ clamp する。明示 Material.SetRenderQueue はこの default より優先する。

ProceduralMesh の default material path も Material.Enabled を変更せず、hasPresentationMaterialOverride がある slot の割当を戻さない。default path の変更には SetMaterialPath を使って shader / reflection cache を破棄する。二枚の GPU mesh は各 slot の cpuIndices と現在の builder.Indices の全内容が一致する場合だけ index upload を省略し、同じ indexCount の別 topology を同一視しない。全更新 A → 同件数の全更新 B → 頂点だけの更新でも、交互の slot に B の index を揃える。

## 検証

PresentationGeometryTest は線の距離比 RGBA / 終端 alpha、閉じた筒、重複点、回転・非一様・負 scale の world 座標と幅、特異姿勢、点の順序と完全な TRS cache、atlas nine-slice の原寸 border、flip、小さい出力、metadata 変更、および全種の explicit Material override の保持と clear 後の default を検証する。C++ compile / suite / shader build は統合 gate の記録で確認し、source の変更だけを GPU 検証済みとは扱わない。

2026-10-06 の offline shader 検証では vendored DXC を用い、Unlit / SkinnedUnlit の VSMain(vs_6_8) / PSMain(ps_6_8)、HV2021 と実 DX12 define で Release(-O3) / Debug(-Zi -Qembed_debug -Od) の 8 組をコンパイルした。全組が診断なし・exit 0。reflection は静的 Vertex の COLOR offset44 / stride60、skinned の BLENDINDICES offset44 / BLENDWEIGHT offset60 / stride76 と一致し、receiveLighting は両 PS で MaterialConstants b2 の uint offset24。出力は Scratch/UnlitShaderCheck_20261006_2329。これは GPU pixel readback や generated SDK / staged Editor assets の同期の検証を含まない。
