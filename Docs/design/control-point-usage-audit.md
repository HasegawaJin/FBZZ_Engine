<!-- @file    control-point-usage-audit.md -->
<!-- @brief   Water/Cloth以外の配置点利用を現行consumerと参照・数値境界から分類する監査。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-08 -->
# TargetPoint追加用途の監査

## 追加用途の実装契約

2026-10-08の「ほかの用途部分もすべて実装」の依頼により、以下を同じ制御点基盤へ統合する。以下の契約に従って実装と回帰検証を進める。

- Camera、Transform制約、VFX端点、NavAgentは既存のEntityRefに永続GUIDとScene世代付きcacheを加える。旧EntityID指定は同じSceneの実体から移行し、GUIDが欠損した参照は保持する。未設定と欠損は別の状態とし、設定済み参照の欠損でfallbackや祖先探索を発動しない。
- IKはtarget/poleのObjectReferenceを正本とする。旧GUIDは欠損時も名前検索へ落とさず、旧name-onlyは全GO作成後に一意な名前だけを一度移行する。新しい空GUIDは明示nullであり、旧名から復活させない。Boneや通常GOも指定できる。
- 巡回はINLINEのworld座標を保持し、POINT_OBJECTSでは安定ID・参照・待機秒数・world速度を一つの行へ持つ。現在行はIDで追跡し、並べ替えで属性や進行を別の点へ移さない。LIVEの再経路は間隔と移動距離で制御し、巡回点をNavAgentの追跡targetへ書き込まない。欠損時は停止して診断する。BAKED_STATICは検証済みのworld数値列を使う。
- Off-Mesh Linkの両端はINLINEのworld数値または二つのTargetPointを明示選択する。非同期ベイクの開始時に数値端点と入力署名を捕捉し、完了時に入力が変わっていれば結果を破棄して再ベイクを要求する。古いmeshへ新しい端点を混ぜない。
- authored LineRendererはINLINE/POINT_OBJECTSとLIVE/BAKED_STATICを持つ。Local/Worldの座標解釈もベイクへ捕捉し、生成線・Trail・GPU頂点をauthoring点の正本へ戻さない。欠損は列全体を無効にし、点を省略しない。
- MotionWarpは永続target参照とCAPTURED/LIVEを持つ。開始時にposeとdurationを一度設定し、CAPTUREDはその後のMarker移動から独立する。LIVEは残り時間をリセットせずposeだけを更新し、欠損時に補正を止める。既存の数値WarpTo/WarpToPoseは数値入力へ明示復帰する。
- 生成位置は公開C#のInstantiateAtでTargetPointのworld poseを発動時に捕捉する。参照は既存のpublic GameObject/配列設定として保存でき、生成後の個体をMarkerへ追従させない。expired/別Sceneの参照は既存SDKの寿命契約で拒否する。
- LODは中心のsourceをOWNER相当のINLINEとPOINT_OBJECTSから選ぶ。参照は距離判定だけを変更し、sizeのscale、Renderer集合、閾値、フェードを保持する。未設定の既定はowner原点、明示targetの欠損は診断し、Rendererを全表示へ戻す。
- Light/FlowFieldは共通のanchor作成・位置/注視Constraint接続を使う。追加の位置正本を持たず、Lightの数値設定とFlowFrameの数値入力を保持する。

追加用途の配置・参照割当・変換・行編集はEditorの共通Opへ接続する。Gizmo candidateとChecked Undoは全ownerの参照・Component寿命・入力を検証する。共有点の解除でGameObjectを破棄しない。Prefabは有効なauthoring参照の閉包を検証し、コピー範囲内だけへ再対応する。

密なLightProbe格子、生成したNavMesh経路、粒子、Trail履歴、renderer集合は引き続き数値/既存参照で扱う。AUTHORING_ONLY exportは今回の追加用途とは独立した後続機能である。

2026-10-08の現行コードを読み、[共通制御点の設計](control-point-authoring.md)を適用する接続先を整理し、配置点として適切な追加用途へ実装を広げた。共通の参照接続Inspector、巡回/リンク/Lineの入力adapter、生成時のpose捕捉、MotionWarpのCAPTURED/LIVE、LOD中心を統合した。以下の実装はReleaseビルドと回帰テストで検証する。

TargetPointは著者が配置する少数の目標をGameObject/Transformで表す。すでに任意GameObjectを参照するconsumerは、その参照先としてMarkerを使える。曲線、巡回経路、ベイク用端点の数値配列は入力adapterを足す対象であり、生成した経路や粒子・Mesh頂点までGameObjectへ変換しない。

## 分類と優先順位

| 分類 | 判断 |
|---|---|
| 参照で足りる | 既存のGameObject参照にTargetPointを割り当てる。新しい位置adapterを重ねない。保存・欠損・cloneの共通参照契約は別途確認する。 |
| adapterが必要 | 永続authoringがVector3/点列であり、位置の正本を参照へ切り替える入力adapter、明示的source mode、変換Opが必要。 |
| 数値を維持 | 計算結果、密な標本、GPU入力、状態値、renderer参照等。配置点に置き換えない。 |

P0/P1/P2は元の実装順であり、今回の依頼で配置点に適切な範囲すべてを統合した。密な計算結果は数値のまま保つ。新たな性能の合格値は実測が必要である。

| 用途 | 現行入力とconsumer | 分類 | 順序 |
|---|---|---|---|
| 汎用Splineの制御点 | `SplineComponent.points/pointBindings`、`ResolveSplinePoints`、`SplineSystem::Update` | adapter実装済み。INLINE/POINT_OBJECTSとLIVE/BAKED_STATICを共通評価へ接続 | P0実装済み |
| Camera follow/look-at | `CameraFollowComponent.target`、`CameraRigSystem::Update` | 参照で足りる | P0の利用例 |
| Camera path | Camera上の`SplineFollowerComponent`とSpline | 汎用Spline adapterを再利用。専用Camera点列を増やさない | P0の利用例 |
| Transform Aim/LookAt/Position | `TransformConstraintComponent.target`、`ConstraintSystem::Update` | 参照で足りる | P0の利用例 |
| IK target/pole | `IKChain.targetEntity/poleEntity`、`IKSystem` | 参照で足りる。既存GUID/名前補助の保存経路を監査 | P0の利用例 |
| VFX Beam/Lineの端点 | `fromEntity/toEntity`、`VFXBeamSystem`、`VFXLineSystem` | 参照で足りる。fallback座標の意味を維持 | P0の利用例 |
| AIの単一移動目標 | `NavMeshAgentComponent.target`、`NavigationSystem` | 参照で足りる。既存再経路条件を維持 | P0の利用例 |
| AI巡回点列 | `NavMeshPatrolComponent.waypointBindings`、`NavMeshPatrolSystem` | 安定ID・待機時間・速度とLIVE/BAKED_STATICのworld点列を実装 | P1実装 |
| NavMesh Off-Mesh Linkの両端 | `startTarget/endTarget`、`NavMeshBakeSystem` | 二つのMarker入力と非同期結果の署名・寿命検証を実装 | P1実装 |
| VFX/Prefab spawn位置 | `Script/ScriptScene.InstantiateAt` | public GameObject参照とlocal offsetから、発動時のworld poseを捕捉 | P2実装 |
| authored LineRendererの点列 | `LineRendererComponent.pointBindings` | Local/World、LIVE/BAKED_STATICと明示変換を実装 | P2実装 |
| Motion Warpの目標 | `targetReference`、`SetTargetPoint/WarpToTarget` | CAPTURED/LIVEを明示し、残時間と速度制限を保持 | P2実装 |
| Sink/Vortex等の位置 | `FlowField` GameObjectと`FlowFieldSettings` | 既存Transform/Parent/Constraintで足りる。評価入力は数値を維持 | P0の利用例 |
| Lightの位置/向き | `LightComponent`所有GOのTransform | 既存Parent/Constraintで足りる | P0の利用例 |
| LOD renderer集合/中心/閾値 | `LODGroupComponent.centerTarget`、`LODSystem` | 任意中心を距離評価へ接続。renderer参照と閾値は維持 | P2実装 |
| LightProbe格子、NavMesh経路、粒子、Trail履歴 | ベイク/solver/履歴の生成値 | 数値を維持 | 対象外 |

## 汎用SplineとCamera

[SplineComponents.hpp](../../Projects/Engine/include/Engine/Scene/Components/SplineComponents.hpp)の`SplineComponent`はINLINEのlocal点列とPOINT_OBJECTSの順序付きBindingを明示的に選び、`SplineFollowerComponent.spline`は曲線を持つGOを参照する。[ControlPointAdapters.hpp](../../Projects/Engine/include/Engine/Scene/ControlPointAdapters.hpp)の`ResolveSplinePoints`がowner-localの数値点列と開閉状態を返し、[GameplayComponentSystems.cpp](../../Projects/Engine/src/Scene/Systems/GameplayComponentSystems.cpp)の`SplineSystem::Update`が同じSplineを読むFollower間でその更新内の解決結果を共有する。LIVEは現在の点を読み、BAKED_STATICは数値点と`bakedClosed`を使う。追従位置や各フレームの補間標本は数値のまま保つ。

Camera pathはCamera GOへ既存Followerを付けて曲線を参照すれば構成できる。[SplineCurve.hpp](../../Projects/Engine/include/Engine/Scene/SplineCurve.hpp)の`EvaluateSplineCurve`と`EvaluateSplineCurveTangent`をFollowerが使い、[ViewportControlPoints.cpp](../../Projects/Editor/src/Panels/Viewport/ViewportControlPoints.cpp)の候補曲線表示も同じ位置評価を使う。Camera専用の点リスト、補間器、参照形式を追加せず、通常Splineの実装を利用する。静的ベイクを使う場合、authoring側の編集表示とruntime側の保持した曲線は明示したmodeに従う。

以前のlocal長さによる進行とscaleを省いた接線は修正済みである。[SplineCurve.cpp](../../Projects/Engine/src/Scene/SplineCurve.cpp)の`EstimateSplineCurveLength`は32区間の線分へsigned worldScaleを適用してworld長さを見積もる。`SplineSystem::Update`はその長さで速度進行を計算し、位置と接線の両方へ同じsigned scaleとworld回転を適用する。INLINEとPOINT_OBJECTSは共通の数値点列からこの経路を通るため、鏡像や非一様scaleでも位置と姿勢のTRS解釈が一致する。進行は推定総長を使う正規化parameter方式であり、局所速度には曲線parameter化の影響が残る。

[CameraRigComponents.hpp](../../Projects/Engine/include/Engine/Scene/Components/CameraRigComponents.hpp)の`CameraFollowComponent.target`は任意GOへの参照で、`CameraRigSystem::Update`がworld位置と必要なworld回転を読む。`offset`は`useTargetRotation`時だけtargetの回転で回し、target scaleで乗算しない。Markerを追従/look-at目標へ割り当てる既存経路はすでにある。`CameraBlendComponent.fromCamera/toCamera`はカメラ役の入力なので、単なるMarkerをCameraの代替として自動挿入しない。

受入は、Markerへのfollow/look-at、Spline path上のCamera、Marker欠損、二つのPrefab個体の独立、親TRS下の移動を扱う。点入力adapterの変更でCamera固有のdamping、FOV、blend時間を変更しない。

## IKとTransform制約

[ConstraintComponents.hpp](../../Projects/Engine/include/Engine/Scene/Components/ConstraintComponents.hpp)と`ConstraintSystem::Update`は位置、回転、scale、Aim/LookAtを任意GOのTransformから解決する。Markerはこのtargetへ直接割り当てられる。姿勢やscaleを採用するかは制約modeの意味であり、Markerの表示サイズではない。

[IKSolverComponent.hpp](../../Projects/Engine/include/Engine/Scene/Components/IKSolverComponent.hpp)の`IKChain`はtarget/poleのEntityIDに加えてGUIDと名前を持つ。[IKSystem.cpp](../../Projects/Engine/src/Scene/Systems/IKSystem.cpp)のTwoBone処理はtarget world位置と`targetOffset`、pole world位置を数値の解法へ渡す。`SolveAimAt`もtargetのworld位置を読む。新しい位置adapterは不要で、骨長、softness、角度制限、autoPole、足の接地を共通Markerへ移さない。

ここで「参照で足りる」は、古いEntityID/GUID/名前の複合保存が新しい欠損GUID契約を自動的に満たすという意味ではない。共通参照再対応を導入するとき、IKの専用SerializerとPrefabReferenceRemapのcustom component参照も対象に含める。名前だけによる別個体への付け直しを新しいBindingの正常復旧として扱わない。

## AIの巡回とNavMesh Link

[NavMeshAgentComponent.hpp](../../Projects/Engine/include/Engine/Scene/Components/NavMeshAgentComponent.hpp)の単一`target`追従は既存経路を使える。`repathInterval`等は動く目標へのA*再計算を制御するため、Markerを導入しただけで毎フレーム`SetDestination`へ置き換えない。

[NavMeshPatrolComponent.hpp](../../Projects/Engine/include/Engine/Scene/Components/NavMeshPatrolComponent.hpp)はINLINEのworld点列、または安定ID・target・wait seconds・speed world m/sを持つ行を保存する。[NavMeshPatrolSystem.cpp](../../Projects/Engine/src/Scene/Systems/NavMeshPatrolSystem.cpp)は開始・待機終了・到達とLIVEの間隔/移動距離条件で数値目的地を更新する。並べ替えでも現在行のIDと属性を保持し、現在行の削除・欠損で旧経路の移動を停止する。

設定済みの`agent.target`は欠損時も巡回を抑止する。[BehaviorTreeSystem.cpp](../../Projects/Engine/src/Scene/Systems/BehaviorTreeSystem.cpp)の`ExecutePatrol`は明示的に追跡targetを解除する。静止したLIVE点へ同じ目的地を繰り返し設定して到達をリセットしない。Actor自身の子孫をLIVE巡回点にする接続は拒否する。

[NavMeshOffMeshLinkComponent.hpp](../../Projects/Engine/include/Engine/Scene/Components/NavMeshOffMeshLinkComponent.hpp)はworld数値または二つのtargetを選ぶ。[NavMeshBakeSystem.cpp](../../Projects/Engine/src/Scene/Systems/NavMeshBakeSystem.cpp)は捕捉した端点・設定と地形穴を含む署名で非同期結果を検証し、同じ数値から近傍polygonへ`OffMeshConnection`を作る。同じpolygon内の不要な自己リンクは作らない。

非同期ベイク開始時の入力と、完了時の接続処理が読む入力は同じ署名の数値を使う。ベイク中にMarkerが動いた場合、新しいtargetを古いmeshへ黙って接続せず、現在入力と結果の署名を検証して再ベイク/再接続を要求する。A*、Funnel、生成したpath点とpolygonは数値のまま保持する。Sensorが検出するゲーム上の対象も単なる配置Markerに置き換えない。

## VFX、Line、Motion Warp

[VFXBeamComponent.hpp](../../Projects/Engine/include/Engine/Scene/Components/VFXBeamComponent.hpp)の`fromEntity/toEntity`は既に任意GOを参照する。[VFXBeamSystem.cpp](../../Projects/Engine/src/Scene/Systems/VFXBeamSystem.cpp)の`ResolveEndpoint`はworld位置へworld offsetを加え、未設定端点はworld fallbackを使う。Markerに新しい位置adapterは不要である。

[VFXLineComponent.hpp](../../Projects/Engine/include/Engine/Scene/Components/VFXLineComponent.hpp)と[VFXLineSystem.cpp](../../Projects/Engine/src/Scene/Systems/VFXLineSystem.cpp)は同じ実体参照を持つが、未設定fromは自分の位置、未設定toはowner-local `toPoint`をTRS変換する。Beamのworld fallbackと混同しない。未設定と「設定済みtargetの欠損」は区別し、`disableWhenEndpointMissing`を保持する。欠損GUIDを空へ落とすとこの区別も失われるため、参照基盤の回帰対象にする。

[PresentationComponents.hpp](../../Projects/Engine/include/Engine/Scene/Components/PresentationComponents.hpp)のLineRendererは数値列とTargetPoint列を明示選択し、Local/World解釈もベイクへ保存する。Rendererへの入力とEditor候補は同じ解決済み点順序を使う。Beam、Lightning、Trailの生成線やGPU頂点は引き続き数値である。

spawn用途は`InstantiateAt`へMarkerを渡す。永続参照はScriptのpublic GameObject/配列設定を使い、発動時に有限poseと同Sceneの寿命を検証する。local offsetは符号付きworld scaleとrotation、rotation offsetはworld rotationとの積を使う。offsetが0ならscaleの逆変換や階層列挙を必要としない。生成後の個体は後から動かしたMarkerへ追従しない。

[MotionWarpComponent.hpp](../../Projects/Engine/include/Engine/Scene/Components/MotionWarpComponent.hpp)は設定済みtargetとCAPTURED/LIVEを保存する。`WarpToTarget`は開始時にpose/durationを一度捕捉し、LIVEはposeだけを更新する。自己・子孫へのLIVE接続や欠損は拒否/中断し、開始時の拒否では既存補正を変更しない。数値`WarpTo/WarpToPose`はNUMERICへ復帰する。

## FlowField、Light、LOD

[FlowField.hpp](../../Projects/Engine/include/Engine/Scene/Fields/FlowField.hpp)の`Sink`が旧Attractの役割を持つ。シーン配置のFlowFieldは自分のGOで中心/向きを持ち、[FlowFieldEval.cpp](../../Projects/Engine/src/Scene/Fields/FlowFieldEval.cpp)がworld Transformを数値へ写し、[FlowFieldSystem.cpp](../../Projects/Engine/src/Scene/Fields/FlowFieldSystem.cpp)が`Scene::FlowFrame`を物理前に準備する。既存のParentまたはTransform ConstraintでMarkerに追従させられる。`FlowFieldSettings`、ParticleEmitterの内蔵`localForces`、速度場の格子は数値のままにし、粒子ごとにEntity参照を解決しない。

[LightComponent.hpp](../../Projects/Engine/include/Engine/Scene/Components/LightComponent.hpp)の位置/方向は所有GOのTransformを読み、[RenderLightExtractor.cpp](../../Projects/Engine/src/Scene/Systems/RenderLightExtractor.cpp)の`ExtractRenderLights`が数値GPU入力へ渡す。Markerを配置anchorにする場合、Parent/Position制約、SpotのAim/LookAt制約を使えばよい。ライト位置の第二のTargetPoint正本を無条件に追加しない。色・光度・範囲・影・flickerはLight固有の数値である。

[LODGroupComponent.hpp](../../Projects/Engine/include/Engine/Scene/Components/LODGroupComponent.hpp)は中心だけをowner原点または`centerTarget`から選ぶ。[LODSystem.cpp](../../Projects/Engine/src/Scene/Systems/LODSystem.cpp)はそのworld位置とCameraの距離を評価し、sizeには従来のowner scaleを使う。Markerのscaleはsizeへ混ぜない。欠損時は一度診断し、Rendererを全表示へ戻す。renderer集合と閾値・fadeは従来の契約を保つ。

[LightProbeVolumeComponent.hpp](../../Projects/Engine/include/Engine/Scene/Components/LightProbeVolumeComponent.hpp)のbox、格子数、SH値、ベイク結果は数値であり、各probeをTargetPointへ変換しない。volume ownerの配置に共通Transformを使うことと、密なprobeをScene実体にすることは別の操作である。

## 統合した操作とAPI

Inspectorの共通参照欄で接続先の選択、解除、選択中の点への移動、TargetPointの作成と接続を行う。`target_point.bind_reference/select_reference/create_and_bind/configure_anchor`が同じ登録簿を使い、作成と接続は一件のChecked Undoとなる。Light/FlowFieldのanchorは既存Transform Constraintへ接続する。Actor自身が追う点は独立した兄弟/rootへ作り、自己・子孫への追従を拒否する。

巡回、Line、NavLinkは`control_points`操作のkindを`patrol/line/nav_link`へ指定する。巡回とLineには追加・並べ替え・参照解除・変換・静的ベイクを提供し、巡回行の待機秒数と速度は安定IDへ付ける。NavLinkは固定端点1/2を作成・接続する。world点列はidentityのrootコンテナ、Local Lineはownerの子へ配置する。接続解除や数値入力への復帰でMarkerを破棄しない。

公開C#は`Script/ScriptScene.InstantiateAt(prefab, anchor[, localOffset[, rotationOffset]])`、`MotionWarp.SetTargetPoint/WarpToTarget`、`IKSolver.SetChainTarget/SetChainPole`を使う。生成とCAPTURED warpは発動時に数値poseを保持する。LIVE warpは残時間を再初期化しない。古いLine/Patrolの数値APIも保持し、point-object入力の読み取りは実際の点列を返す。数値の書き込みはINLINE/LIVEへ明示復帰する。

Gizmoの候補とUndoは全consumer、Script設定、選択点の子孫、Component寿命、参照の同位置差し替えを追跡する。新用途でもSceneの正本はリリース時に一度だけ変更する。旧Sceneの読み込みでMarkerを自動生成しない。

## 受入と監査対象

- 任意GOを受け付けていたconsumerへMarkerを割り当てても、Boneや通常GOの既存参照を排除しない。
- 同じMarkerをCamera/IK/VFXで共有し、移動・Hierarchy複製・Prefab生成・欠損後の保存で各参照の意味を維持する。
- 未設定fallbackと欠損GUIDを区別し、VFXの自動非表示を保存往復で維持する。
- Spline path上のCameraと、editor表示が同じ点順序・座標を使う。Marker導入でFollowerの速度単位や姿勢契約を変えない。
- Patrolの行の並べ替えでwait/speedが別の点へ移らない。Path、Funnel、A*結果へScene参照を混ぜない。
- NavLinkのMarker移動をベイク署名へ含め、非同期結果に別時点の端点を混ぜない。
- spawn/MotionWarpの数値PoseはMarkerを後から動かしても変わらない。LIVE追従は明示モードだけで行う。
- Markerはeditor overlayに表示し、Light/粒子/probeの密な数値を増殖させず、game draw callを点ごとに増やさない。

追加用途はコードと実System/公開SDKの回帰で検証する。実画面の画像比較と追加用途全体のGPU・UI性能測定は別途の確認であり、未計測の時間を合格値として扱わない。

## 検証結果

2026-10-08、ReleaseでEngine/Editorの自動テスト実行ファイル、Editor Launcher、FBZZTestBench、C# SDK/実行ホスト/fixtureをビルドしてエラー・警告0件。今回の追加用途と既存の関連回帰359件がすべて合格した。Editor99件、Engine216件、Physics44件を含む。

実SystemのCamera/Constraint/VFX/LOD、公開C#の生成/MotionWarp/IK Pole、接続と共有点のUndo、Sceneの保存/追加読み込み、Prefab二個体の参照独立、ベイク中の端点/地形穴変更を検証した。実画面の画像比較は未実施。検証ログは`build/agent/build-20261008-170137-62852.log`、`build/agent/build-20261008-171956-52524.log`、`build/agent/test-20261008-172823-54708.log`。
