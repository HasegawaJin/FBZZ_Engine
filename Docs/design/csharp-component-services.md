<!-- @file    csharp-component-services.md -->
<!-- @brief   アニメーション・UI・経路・カメラ・音声・流速場の C# 接続契約。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-06 -->
# C# runtime Component services

ゲーム作者向け C++ Script を提供しない新 SDK では、既存 ScriptProxy があるだけでは C# の接続にならない。ここで扱う11型は実 native row を持つ型付き facade として Get/Add/Remove、RequireComponent、ComponentRef、private cache、Play birth 対応へ同じ kind で接続する。Component を暗黙に追加せず、不在・pending removal・失効 row を診断する。保持済み facade は同型を再追加しても replacement に接続しない。

| C# 型 | native row | kind | service |
| --- | --- | ---: | ---: |
| MotionWarp | MotionWarpComponent | 42 | 307 |
| IKSolver | IKSolverComponent | 43 | 308 |
| Ragdoll | RagdollComponent | 44 | 309 |
| SpringBone | SpringBoneComponent | 45 | 310 |
| UIAnimator | UIAnimator | 46 | 311 |
| NavMeshPatrol | NavMeshPatrolComponent | 47 | 312 |
| SplineFollower | SplineFollowerComponent | 48 | 313 |
| CameraShake | CameraShakeComponent | 49 | 314 |
| VirtualCamera | VirtualCameraComponent | 50 | 315 |
| AudioMixerSend | AudioMixerSendComponent | 51 | 316 |
| FlowField | FlowField | 52 | 317 |

## 実行権限と失敗

query は Edit/Play の同期 callback、Connect、readonly DrawGizmos/Selected。OnValidate と EditorAction は Engine を触らない。Ragdoll の変更は Play の FixedUpdate のみ。MotionWarp、IKSolver、SpringBone、NavMeshPatrol、SplineFollower、CameraShake、AudioMixerSend の変更は Play の live callback。UIAnimator、VirtualCamera、FlowField は ExecuteAlways による見た目構築のため Edit/Play の live callback で変更できる。getter と setter の RunModes を別に catalog 化し、query 権限で mutation を通さない。

入口は exact caller generation/instance/owner/call/thread、Scene lifetime、Component birth を照合する。packet の read flag は selector 分類と一致しなければ拒否する。数値は finite、enum は実値域、integer は int32/uint32 を lossless に転送する。related GameObject は現在 Scene の exact entity に限る。不正 packet と拒否された scope は result と row を変更しない。query 応答の非有限値や不正 enum も既定値へ隠さず診断する。

## アニメーション

MotionWarp の位置と姿勢は world、位置は m、duration は正の s。姿勢は有限かつ非退化な Quaternion を native で正規化する。軸補正は world 各軸の0..1、maxSpeed は非負 m/s、0は上限なし。target は runtime 値なので設定を保存して起動の代用にしない。既存 AnimatorSystem が root motion 適用前に消費する。

IK は Inspector で組んだ targetName の先頭一致チェーンを操作する。未検出は Has/Enabled=false、Weight=0、変更は no-op。SetChainTarget は runtime EntityID を差し替え、null は runtime ID を解除する。保存済み GUID/name の解決規則は既存 IKSystem が保持するため、null を authoring reference の削除とは説明しない。weight は native の0..1 clamp。

Ragdoll の Begin/BeginActive/End、速度・角速度の衝撃、root recipe、profile、接触・地面、blend、muscle、recovery、collapse、standing guard、root anchor は既存 proxy に転送する。world 並進速度は m/s、角速度は rad/s、半径と変位は m、角度は degree、blend/hold は s。開始は pending request、衝撃は pending queue を使い、更新途中の physical rig を facade から直接動かさない。設定倍率の clamp と phase の遷移は native を正本にする。

generic AddComponent/RemoveComponent は他の authored row と同じ Play live 操作で、Ragdoll の fixed-only simulation commandとは分ける。Add は設定 row の追加、Remove は immediate lookup失効と Cleanupでの row破棄を予約する。RagdollRigの剛体はprivate XPBDSolverが借用する単独所有で、SceneのPhysicsWorldへ登録しない。Worldは接触照会と反作用の相手を同期借用するだけ。rig destructorはtransient→constraint→solver body参照→owned bodyの順に解放する。Animatorを無効化/停止する状態は持たず、nativeは毎frameのFKへragdoll poseを重ねる。即時削除へEndのblend予約を挟まず、次の通常Animator更新でFK描画へ戻る。FlowFieldのframe snapshotは設定値を所有copyし、FlowField rowへの参照を保持しない。BakedのVectorField pointerは既存AssetManagerの寿命に従う。

SetRoots と SetExcludedBranches は全名前を事前検証し、同期 call の begin/stage/commit/abort で未公開候補を作る。名前数・名前の全長に packet の上限を流用しない。UTF16 の不正 surrogate、空名、NUL は変更前に拒否する。chunk は codepoint 境界で最大32768 UTF8 bytes、同時 transfer handle は32件という資源契約。各 chunk は root index と累積 UTF8 byte offset を検証し、順序違い・重複・不正 UTF8 では候補を廃棄する。

transfer は exact owner/generation/instance/call/Scene/entity/row birth と対象 recipe snapshot を保持する。commit まで旧 recipe は未変更。途中の SetRoot/SetRoots/Excluded 変更、row の除去・再追加、終了した call は以前の recipe を巻き戻さない。最後の allocation を済ませて root/extra/excluded の文字列だけを移し、非コピーの稼働 rig を複製しない。EndCall、owner 解放、世代退役は未 commit 候補を回収する。メモリは旧 recipe snapshot と候補の名前総量に比例する。空 roots は骨格 root 全体、空 exclusions は除外なし。

SpringBone の EnsureChain は既存 row 上で rootBoneName を引き、欠損チェーンだけを追加する。新チェーンは weight と force が0で、depth 変更は次 native 更新の組み直しを要求する。weight/stiffness/damping は native0..1 clamp、角度は degree、radius は m、world force は方向と m/s²。ResetAll は既存 teleport 復帰と同じ慣性破棄経路。未検出チェーンの変更は no-op。

## UI・経路・カメラ・音声・流速場

UIAnimator は color/position/scale/rotation/fill/material の全 Tween を既存 row の Play* に渡す。color は linear RGBA、position は UI local、scale は倍率、rotation は Z の degree、fill は UIImage の値、material は要素ごとの override。duration/delay は非負 s。easing、loop、pingPong は一度の packet で渡し、Stop(channel) は指定 Tween だけを停止する。delay は Play より先に設定する。

NavMeshPatrol の waypoint は world m。個別 wait は負で共通 wait、speed は非正で Agent の既定値を使う。未存在 index の TryGet は false と default position、Set は false と旧 row。SplineFollower は実 Spline row を持つ GameObject を同じ Scene で Bind し、null で解除する。速度は native の概算長で進捗へ変換する m/s、進捗は0..1、Restart は Reverse に応じて0/1から始める。WrapMode は Once/Loop/PingPong。

CameraShake は native deterministic seed を int32 のまま扱い、開始で elapsed を0へ戻す。位置振幅は m、回転振幅は degree、frequency は1/s。Stop/Disabled の以前の pose offset は既存 CameraRigSystem が次更新で戻す。VirtualCamera の priority は exact int32、FOV は1..179 degree、blend は非負 s。active は CameraRigSystem が決める readonly 値。

AudioMixerSend は補助 bus と sendLevel を同時に変更し、0..1へ native clamp する。主 AudioSource の出力・音量を変えない。未知 bus を実際に送るかは既存 AudioSystem の契約に従う。

FlowField は既存 proxy と同じ先頭 force を操作する。空 force list は診断し暗黙に追加しない。channels の uint32 書込みだけは全 force へ配る。方向は Component local direction で native が world rotation を適用する。speed は通常 m/s、Baked は既存速度場の無次元倍率。radius<=0 は距離減衰なし、falloff は最低0.01。type query は LegacyDrag/Baked をそのまま返し、setter は Uniform/Sink/Source/Vortex/Curl のみ。読み込み専用 LegacyDrag や asset を要する Baked を Uniform として偽装しない。

## 検証

ManagedComponentServicesTests は11 family の実 row 変更、degenerate pose の未変更、Ragdoll fixed-only、related Scene、UI 独立停止、waypoint miss、uint32 の上位 bit、Baked query identity、130 roots の非公開 staging と recipe revision 拒否を検証する。Registry/Factory/NativeName/ComponentAccess と実 CoreCLR facade の統合は root の共通 gate で確認する。native kernel 単独の成功を SDK の実行確認とは扱わない。

RagdollSystemTestsのmanaged除去回帰は、Worldとのactive contactsを持つprivate rigをCleanupで破棄し、SceneのWorld query/stepとAnimator rowを維持してから、再追加が新birth・未起動のfresh rigになることを検証する。

RuntimeComponentsComponentとManagedComponentServicesRuntimeTestsは実CoreCLRから11型のgetter/setterとParticle設定をABIへ通し、native rowの観測結果を照合する。RagdollのUpdate拒否とFixedUpdate受付、130 rootsの全件転送、relatedのcurrent Scene照合、foreign GUIDをnullへ変換せずIK targetと親を保持する拒否、FlowFieldのheld facade/ComponentRefが再追加birthへ接続しないことを検証する。未解決のnonnull GameObjectはObjectId0として保持されるため、nullable relatedやparentのwire0へ送る前に診断する。

## ParticleSystem の追加接続

既存 ParticleSystem kind28/service189 の selector0..12は維持し、13..73を追加する。発生位置/速度/ばらつき、重力、開始/終了色とサイズ、最大粒子数、playback、Point/Sphere/Cone/Box/MeshSurface、sort、requested simulation mode/space、render mode/stretch、collision、rate-over-distance、prewarm、sub-emitter、flow coupling、angular velocity、local Curl noise、受ける流速場と mask、typed material、GPU eligible/fallback reason/field/description を実 row/proxyへ接続する。

追加 query は Edit/Play の同期 read/Connect/readonly draw、変更は Play の live callbackのみ。既存0..12の read権限は変更しない。公開 enum は nativeの値と一致し、不正値は native と facade の両方で拒否する。GpuFallbackReasonの ReservedVectorField=11 は予約のまま、Lights=12を末尾に保持する。shape/simulation/collision等の enum応答も不正値を診断する。scalar/vector/複数設定は全入力のfinite検証を終えてから変更し、atomicな組を部分更新しない。

emitPositionとemitVelocityは emitter local、位置はm、初速/ばらつきはm/s。Gravityは world m/s²で、Local simulationでも nativeが worldで積分してlocalへ戻す。Angular velocityはrad/s、rate-over-distanceはparticles/m、FlowCouplingは1/s。SetVelocityDampingは旧名称のまま同じflow couplingへ転送し、別のdrag式を追加しない。サイズ・形状寸法等の非負clampはnativeを再利用する。MaxParticlesの変更はgpuInitialized/gpuCapacityを失効させ、SimulationMode変更はGPU clearを要求する。Local simulation spaceとPhysics collisionは既存proxyどおりrequested modeをCPUへ変更する。

ModelAssetRefのSetMeshShapeは実GUIDの種類をSceneAssetReferencesで分類し、実際のconsumerと同じasset::Modelをloadする。選択したmeshが存在することを確認後に既存proxyでshapeを変更し、頂点/三角形cacheを失効させる。負のmesh indexは全mesh、scaleはnative最低0.0001、signed normalVelocityはm/s、followSkinnedAnimationは同じownerのAnimatorを使う。空Modelはsourceを明示解除し、shapeはMeshSurfaceのまま。Model不正時は設定もcacheも未変更。NormalVelocity単体の変更はcacheを組み直さない。

MaterialAssetRefは実GUIDのMaterial分類とload成功を確認後にsettings.materialPathへ割り当てる。空値は既存ParticleFallback素材へ戻す。getterは登録済みGUIDだけを返し、metadataを新規発行しない。Blend/Flipbook/SoftParticle等の見た目は.mat [particle]が正本なのでEmitter固有の別設定へ複製しない。GPU判定は既存ParticlePassと同じsettingsとruntime.materialを使う。素材の最初の描画解決前、または素材変更直後の再解決前は前回のmaterial状態に基づくため、現在のbackend初期化を保証するqueryとは説明しない。

SubEmitterの3名前は既存VFX subtree scopeを維持する。空値は該当linkを解除し、burst countはnative最低1。VFXが設定したscope内で名前を解決し、手置きEmitterだけは従来のScene解決を使う。SetSubEmittersが別のVFX instanceを指す全体lookupへ書き換えない。Noise queryはCurlを暗黙に追加せず、未設定ならstrength0、frequency0.5、speed1の公開既定値。SetNoiseは既存Find/EnsureLocalForceを使い、同じtypeの先頭Curlを更新する。

ManagedParticleServicesTestsはcapacity変更時のGPU失効、requested/actual mode分離、Local/Physics縮退、Prewarm reset、Curl再利用、VFX scope保持、mask上位bit、typed asset失敗時cache保持と明示解除を検証する。ParticleServicesApiTestsはservice/selector/payload、typed GUID、非有限/不正enumのdispatch前拒否、stale birth、catalogのread/write runModesとreserved reasonを検証する。
