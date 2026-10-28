<!-- @file    csharp-render-components.md -->
<!-- @brief   C#の線・Sprite・Trail・Mesh・Particleと可変長転送の契約。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-06 -->
# C# rendering components

LineRenderer35、SpriteRenderer36、SortingGroup37、TrailRenderer38、MeshTrail39、MeshRenderer40、SkinnedMeshRenderer41は実rowのbirthを保持する。Get/Add/Remove、RequireComponent、ComponentRef、private cache、reload、Play採用はcanonical COUNT53に従う。失効したfacadeはreplacementを変更しない。public APIはEngine実装型やGPU pointerを公開しない。

線・Sprite・Meshの見た目設定はlive Edit/Play callbackで変更し、Connectとreadonly DrawGizmosではqueryだけを認める。Trailの記録・ClearとParticleの再生・設定変更はPlayに限る。単位は位置/幅m、duration/samplingはs、色はlinear RGBA。enum、不正Unicode、非有限数、負の幅/時間、範囲外indexは変更前に拒否する。素材は型付きGUIDから既存asset resolverへ渡し、共有assetを書き換えない。SpriteのsubassetはTextureAssetRefと名前/IDを既存ResolveSpriteReferenceへ渡す。

Line.SetPositionsはReadOnlySpan<Vector3>を一度の同期操作として扱う。内部begin/stage/commit/abortはcallerの世代・個体・同期call・Scene・object・Component birthと一致する要求だけを受ける。空/一点も有効で描画を止める。転送packetの容量は点列全体の上限ではない。MeshのApply/UpdateVerticesも全数値・triangle index・受信順・宣言した全量を検査し、未公開候補から一度だけcommitする。失敗は旧CPU/GPU形状を保持する。未commit候補はabort、同期call終了、owner解放、世代退役で回収する。同時候補32件は資源上限であり、点/vertexの固定総件数上限ではない。

MeshVertexはlocal position/normal/tangent/UV/linear RGBAの15float。indexはuint32で全bitを保ち、floatへ丸めない。native meshのuint32件数、CLR Span/int件数と利用可能メモリが表現上限。三角形はDirectXの既定clockwise。UpdateVerticesは同数の頂点を置換して既存topologyを保持する。GPU uploadは既存RuntimeMeshSystemが担当する。readのcaller容量不足はRequired/Written=0、失敗時と成功時の余りは未変更。

Lineの点順・色・形状・空間・billboard・必要なworld TRS/カメラ位置を更新判定に含める。端点色は頂点色で補間し、Materialのpresentation既定値と明示overrideを分離する。明示Material変更は次のPresentation更新で剥がさない。SpriteのSlicedは元画像borderとpixelsPerUnitからnine-sliceを生成する。

検証は実native row、除去/再追加のbirth、packetを超える点/vertex/index、不正途中packet、同期call終了、点順/World姿勢、RGBA、nine-sliceの出力、実CoreCLR facadeとcatalog/保存の対応を対象にする。ビルド・受入結果は実行後に別途記録する。

## 2026-10-07 の検証

- DevelopmentのEditorライブラリ、Engine/Editor自動テストはbuild-20261007-001743-31160.logでエラー0・警告0。Editor起動用exeはbuild-20261007-002241-47200.logで同じDLL・managed host・assetsへ同期した。shader 304 jobsの変更4 jobsが成功し、残る300 jobsは同じcompiler signatureで既に最新。
- managed-test-20261007-002109-28264.logの実Host/Roslyn回帰142項目が合格。Compiler/Analyzer/SDK documentationのエラー・警告0。
- test-20261007-002425-23548.logの関連native回帰150件が合格。線/meshの分割転送、全18facade、native通知、実CoreCLR、保存再読込、除去/再追加、foreign GUID拒否、Ragdoll solver破棄、Material override、shader capabilitiesとEditorのSDK接続を含む。
- Unlit/SkinnedUnlitのVS/PSをvendored DXCのSM6.8/HV2021でDebug/Release計8組コンパイルし、診断0。静的vertexのCOLOR offset44/stride60、skinned stride76、receiveLighting uint offset24をreflectionで確認した。出力はScratch/UnlitShaderCheck_20261006_2329にある。
- 初回は旧テストexeと更新済みDLLを混在させ、Editorテストが0xc0000139で起動失敗した。同じDevelopment構成でテスト本体と依存DLLを再リンク・配置後、Editorの関連80件を含む上記150件が起動・終了とも成功した。

検証ログはすべてbuild/agent/配下。geometryと実uploadされたindex bytesを検証したが、GPU pixel readbackと製品SDK全構成の再公開はこの受入には含めない。
