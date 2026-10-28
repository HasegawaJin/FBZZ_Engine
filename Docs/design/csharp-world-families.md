<!-- @file    csharp-world-families.md -->
<!-- @brief   Terrain/Water/VFX/Sequenceの実row、world照会、callback権限とbirth容量。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-06 -->
# C# Terrain / Water / VFX / Sequence

公開facadeは実TerrainComponent / WaterComponent / VFXComponent / SequencePlayerComponentの行を持ち、GetComponentから取得する。native kindはTerrain32、Water33、VFX30、Sequence31。コンポーネントを暗黙に追加せず、RequireComponent・保存ComponentRef・private cache・reload・Play採用が同じ型とbirthを使う。削除予約は即時に不在、再追加は新birth。保持済みfacade/ComponentRefは再接続しない。

pending removalはcanonical NativeComponentKind.COUNT由来のbitset、Play birth tableも同じextent。COUNTは排他的上限でComponentではない。uint32へのshiftとkind32境界を持たず、後続Joint34を含む現行COUNT35を一か所で定義する。Scene実体4096の制約、caller Span容量、設定collection長とは別である。

## Terrain

local高さはTransform適用前のY[m]、localXZも[m]。GetHeightLocalはnativeと同じ対角2三角の補間で、範囲外をclampし、穴でも基盤の高さを返す。空の未構築gridは高さ0、法線UP。GetNormalLocalはlocal単位法線。

TrySampleWorld(worldX,worldZ,out sample)はworldXZの無限鉛直線と実地形三角形の**最上交点**を返す。穴・範囲外・非可逆scale・無効形状ではfalseとdefault sample。worldY入力からlocalXZを推測しない。回転・非一様/負scaleを含む実world TRSを用い、Physics colliderの初期化を要求しない。native GetHeightWorldは同じ照会を使い、未交差時だけ既存の入力Y fallbackを返す。native Scene terrain照会も最上の有効な地形を選ぶ。

法線はlocal平滑法線へworld TRSの逆転置を適用する。単なる回転やscale乗算は非一様scaleで接線と直交しない。根拠は[PBRT4 3.10.3 Normals](https://pbr-book.org/4ed/Geometry_and_Transformations/Applying_Transformations#Normals)で、実装直近にも記す。native GetNormalWorldはworldPosをsigned inverseでlocalへ写して標本化し、同じ法線変換を使う。鉛直面の位置と法線を組で必要とする場合はTrySampleWorldを用いる。

通常水平gridのworld照会はO(1)。傾斜gridはlocalXZへ写した鉛直線のcellを順番に訪問し、Math::Ray::IntersectTriangleを再利用する。全heightData走査・mesh作成・JSON encode・heap allocationをせず、訪問cell数は通過columns+rowsに比例する。正逆2本の重なるrayで無限鉛直線を表すため、surfaceが基準Y=0でも交差を落とさない。

SetHeightAtGridとSetHoleCellはPlayのFixedUpdate限定。高さはnativeのindex/height範囲clampを使い、描画とcollider rebuildを一緒に要求する。穴は値が変わったときだけrebuildする。PaintLayerAtGridとMaterialAssetRefによる層割当はlive Edit/Play callbackで使用でき、nativeのsplat正準化とdirty処理を再利用する。共有Materialの内容を書き換えない。

## Water

波の正本は既存MaterialとWaterSystem。WaveAmplitudeScaleは有限な個体倍率で、負値はnative同様0へclampする。CurrentはworldXZ流速[m/s]。GetSurfaceHeightWorldはnativeと同じworld phaseを評価し、基準worldYを加えた高さ[m]。GetSurfaceDisplacementLocalはlocalXZをworld phaseへ写した基準面からの変位[m]で、絶対local高さではない。AddRipple/SplashはPlay限定で、既存EmitWaterRipple/QueueWaterSplashを使う。新しい波の式を複製しない。

## VFX / Sequence

VFX.Play(restart=true)は既存Restart、falseはResume。Stopはtime0とrestart要求、Triggerは次更新のpending名。Durationは明示duration>0、なければ既存descendant-derived resolvedDuration。Playback commandsはPlayのlive callbackだけ。

SequencePlayerのPlay/Stop/Seekはpending requestを積み、開始snapshotと停止復帰をSequenceSystemが処理する。直接playing/timeを書き換えない。Time/Seekはnative double精度を維持し、packetではround-trip文字列を使う。 Durationは現authoring sequenceまたは取得済みasset cacheだけを読み、import/pinを開始しない。割当済みで未ロードなら診断、未割当は0。Bind(key,target)は現在Sceneのexact object tokenを検証し、nullはそのキーを明示clearする。既存Inspectorで割当済みsequenceを使い、未実装の汎用path loaderを公開しない。

## 読み取りと描画

明示ApiContractのquery aliasだけがlive/Connect/readonly DrawGizmosを許す。既存read aliasのcatalog権限を広げない。描画scopeのnative入口もread flagだけを受け、playback・地形更新・rippleを拒否する。OnValidate/EditorActionはEngineを触らず、これらのqueryも使用不可。Get/Add/Remove・callback・typed referenceのnative/managed境界はsame-generation/instance/owner/call/threadおよびexact scene/row birthを検証する。

## 検証境界

回転・非一様/負scaleで接線から独立に期待法線を得るテスト、可変傾斜でsigned inverse座標を検証、折れた地形の複数鉛直交点から最高点を選ぶテスト、穴/範囲外/特異姿勢のout未変更を持つ。COUNT32超のremove/re-add、world-family kernel、実CoreCLR facade/FixedUpdate/Gizmo mutation拒否、catalog/analyzer/ref spoolの回帰も同じwaveで確認する。source checkpoint時点は未ビルドで、root統合gateの結果を別途記録する。