<!-- @file    foot-ik-tuning.md -->
<!-- @brief   足 IK の腰補正・値調整・診断と互換既定値。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-11-03 -->
# 足 IK の値調整

FootPlace はアニメーション後の FK に接地補正を重ねる。

同じ個体に有効な Ragdoll と構築済みの物理形状があり、UseRagdollFootShape=true の場合、足首の手動基準高の代わりにその形状を使う。形状の +Y を足裏法線へ合わせ、形状中心・ボーンからのオフセット・半辺長または半径から、接地面に底面が乗る足首高を求める。物理足では DeadZone を使わず、平滑化と補正上限を通した目標を後段の RagdollSystem が消費する。無効・未構築・欠損部位では SurfaceOffset へ戻る。UsesPhysicsShape で実際の経路を確認できる。平地で足首と基準高が一致する場合は腰を下げない。TwoBone の maxExtension / softness から腰の沈み込みを生成しない。膝を意図して曲げる場合だけ hipBendReserve を m 単位で指定する。足が届かない場合は脚長で目標を制限し、腰の上限を破らない。

Active の物理足に世界支持がある場合は、同じ物理接触から支持形状を特定し、そのコライダーへの物理レイで面の高さ・法線を求める。SAT の近似法線を地形の面法線として使用しない。接地相の水平位置を物理の足へ戻し、同じ目標で腰と2-Boneの膝・足首を解く。FK の末端だけで静摩擦面を引っ張らない。支持面の高さは足首の XZ へ平面変換するため、角の接触点を足首直下の高さと混同しない。水平変位は MaxCorrection で制限し、SmoothTime で平滑化してから Ragdoll の weight に応じて混合する。足裏法線も SmoothTime で平滑化し、地形の三角形境界で向きが跳ばないようにする。高さは平滑化した向きの形状を実際の面法線へ投影して求める。離地後も接地重みが残る間は水平・高さ・足裏回転の補正を減衰させ、FK へ戻す。地面を見失っても直前の法線を使って回転を戻し、Grounded は false のままにする。支持オフセットは接地重みが数値許容以下になってから消去し、weight=0・Passive では即座に破棄する。

## 調整と保存

Inspector、Scene/Prefab の chain、C# IKSolver.SetFootSettings が同じ値を扱う。チェーンの識別は従来どおり targetName の先頭一致で、FootPlace 以外は false。SDK の設定変更は Play の同期 live callback で行い、Scene の保存値は変更しない。TryGetFootSettings で読み戻せる。

| Native / C# | 単位・範囲 | 既定 |
|---|---|---|
| useRagdollFootShape / UseRagdollFootShape | 同一個体の物理足形状を優先 | true |
| footSurfaceOffset / SurfaceOffset | 足首ピボットの地面からの高さ、world m、0..0.5 | 0.05 |
| correctionDeadZone / DeadZone | m、0..0.25 | 0.025 |
| maxCorrection / MaxCorrection | 上下補正・水平追従それぞれの上限、m、0..1 | 0.12 |
| footPlantDistance / PlantDistance | FK の足裏から地面までの距離と、左右の FK 足裏高の差に対する接地重みの上限距離、m、0..1 | 0.06 |
| smoothTime / SmoothTime | 指数応答の時定数、s、0..1、0 は即時 | 0.10 |
| hipWeight / HipWeight | 腰の追従率、0..1、0 は腰を維持 | 1 |
| maxHipLowering / MaxHipLowering | 腰を下げる上限、m、0..1、0 は下降禁止 | 0.10 |
| maxHipRaising / MaxHipRaising | 腰を上げる上限、m、0..1、0 は上昇禁止 | 0 |
| hipBendReserve / BendReserve | 意図的な追加の膝曲げ余裕、m、0..0.25 | 0 |
| rayUpRatio / RayUpRatio | 脚長に対する比、0..2 | 0.5 |
| rayDownRatio / RayDownRatio | レイ全長の脚長比、0..4 | 1.2 |

接地重みは FK の足裏と地面の距離、および左右の FK 足裏高の差で判定する。既定では各距離が 2 cm 以内で全重み、6 cm に達するまで滑らかに 0 へ減衰する。片足が支持されていて左右の FK 足裏高が近ければ、もう片足は段差の低い面へ追従できる。地面が FK の足裏より上に来た場合は上向き補正でめり込みを解消する。

SurfaceOffset は靴底の厚みではなく足首ボーンの高さ。モデルごとに調整する。足を上下させる値と腰の上限を同時に大きくしない。腰の上限はチェーンの有効 weight に応じて縮め、HipWeight は目標の強さに一度だけ掛ける。足の位置も weight を二重に掛けない。

腰の移動は子孫へ一度だけ適用する。足首の子孫は腰移動後の位置から残りの足補正へ追従し、後続の FABRIK / AimAt も補正済みの姿勢を入力にする。TwoBone / HandPlace / FootPlace は末端目標を weight で混合してから固定した上下節長で解く。到達不能な目標は到達可能な距離へ制限し、最終の節方向と回転を一致させる。FullBody の反復でも骨長を縮めない。

反対向きの Pole へも脚軸まわりの角度で曲げ方向を補間し、中間 weight で方向が消えないようにする。FullBody の AimAt は同一フレーム初回の基準姿勢と前フレームの履歴から評価し、反復回数や早期収束で weight・角度上限・応答時間を変えない。

FootPlace の AutoPole は FK の曲げ方向の信頼度と preferred 方向との一致度を使う。一致度の正側を三次の重みで混合し、半球の境界で重みと傾きを 0 にする。後方折れを preferred 側へ戻す際に、わずかな FK の変化だけで膝の解を切り替えない。十分に曲がって preferred と整列した FK はそのまま保持する。

SDK は非有限値と範囲外の packet を全体変更前に拒否する。Inspector の直接入力や旧データは実行時に同じ範囲へ収める。上限を縮めた場合は平滑化の途中でも新しい上限を即座に守る。AdjustHip=false と HipWeight=0 は腰補正を即座に停止する。チェーン、solver の無効化または weight=0 で履歴を破棄し、再有効化で古い沈み込みを持ち越さない。

足の上下補正と物理足への水平追従も、平滑化後の履歴を現在の MaxCorrection へ制限する。Play の Pause 中は IK の骨姿勢と平滑化履歴を保持し、再開時に更新する。編集モードの評価は継続する。

追加キーのない旧 Scene/Prefab は表の既定値を使用する。人工的な沈み込みの廃止により、以前の平地姿勢とは腰の高さが変わる。意図したしゃがみは BendReserve で明示する。

## 観測と検証

GetFootState は前回の IK 更新の結果を返す。Left/RightCorrection は平滑化した足の目標補正、HipTarget は制限後の腰目標、HipOffset は実際に腰へ適用した world Y 変位で、下降が負。HipLimited は希望量が上下限で切られた場合に立つ。Left/RightGrounded はレイ接地候補の判定で、剛体の接触判定とは異なる。FixedUpdate での照会は前回の LateUpdate の結果になる。

Inspector も足の左右補正、腰の目標と適用量、上限到達、接地を表示する。Animation Priority ボタンは腰追従 0.5、下降上限 0.05 m、上昇・追加曲げ 0 に設定する。保存対象に診断値や平滑化履歴は含めない。

FootIKTest は平地 FK 維持、上下限、追加曲げ、半 weight、実行中の上限縮小、無効化、欠損骨、地面なし、非有限の保存値、保存往復を IKSystem の出力骨姿勢で検証する。ManagedComponentServicesTest と FootIKApiTests は設定 packet の原子性、寿命、公開 metadata を検証する。

EngineTestProject の EngineTestController は男女別の足首基準高と共通の腰・足設定を公開する。applyFootIKTuning=true の場合に Controller の値を適用する。Prefab の値を使う場合は Play 前に false にする。Play 中に false へ切り替えた場合は現在値を保持して以後の適用を止め、Inspector から直接調整できる。Ragdoll を統合した個体は物理形状を使い、独立した ragdollFootHipWeight=1、ragdollFootMaxHipLowering=0.12 m を適用する。物理足の追従時間は maleRagdollFootSmoothTime=0.12 s、femaleRagdollFootSmoothTime=0.20 s を別に持ち、男女切り替え時に適用する。Prefab の物理用 chain も同じ腰・追従時間を保存する。独立 PlayerIK の標準比較値は足首 0.10 m、腰追従 0.5、下降上限 0.05 m、上昇 0、追加曲げ 0、追従時間 0.12 s。EngineTestFootIKTuningMale/Female は Play 中に設定を変更し、半 weight の歩行、ジャンプ時の解除、下降上限、下降禁止、IK 無効化、腰補正無効化、調整元の切替を検証する。

2026-11-03 の通常 SDK 検証では男女とも標準設定の安定後の腰補正はほぼ 0 m。半 weight 歩行のサンプルは男性 -0.00316 m、女性 -0.00135 m。足を約 0.10 m 下へ補正する設定でも腰は指定上限 -0.025 m で止まった。測定は EngineTestProject/Library/Playtests/FootIKTuningMeasurements.json と各シナリオの report.json に出力する。
