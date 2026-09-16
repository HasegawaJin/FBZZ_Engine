# Player完成版のControllerと旧シーン撤去

2026-09-14。完成モーションをPlayerとして実際に出力し、旧Playerの全階層をシーンから削除する依頼に対応する。

## データの切り替え

- `Assets/Models/Player` とそのフォルダ `.meta` を `Player_BackUp` へ移動する。
- 旧 `Assets/Animation/Player` も `.meta` ごと `Models/Player_BackUp/Animation` へ保存する。
- 新しいPlayerとControllerには新規GUIDを割り当てる。旧GUIDを新骨格へ使い回さない。
- Stage_01、Stage_02、Stage_03、Resultの旧Player本体・子孫・そのPlayerにアタッチされた武器を削除する。シーン全体は再生成せず該当する `[[gameobjects]]` ブロックだけを除去する。
- シーン変更前の完全なファイルとSHA-256を `Docs/Art/MiniBotC/ExportDelivery` に保持する。
- 新Playerのシーン配置やゲームスクリプトの一刀対応は今回のController作成には含まれない。

## 再生構成

承認済みの両手剣モーションは腰・脚・武器の連動を含むので、現在のControllerはBaseの全身再生で構成する。
制作前の構想にあった上半身攻撃や加算被弾は有効化しない。UpperBody / Armsマスクは将来の個別合成用に用意する。

- `Locomotion`: Idle / SwordWalk / Run_F の1D BlendTree。Speedはm/s、閾値0 / 2 / 6、平滑化0.08秒。速度は初期調整値。
- `JumpStart → FallLoop → Land → Locomotion`。崖からの落下は `IsGrounded=false` と `VerticalSpeed < -0.1`。
- DodgeとRunStopは単発。終了後Locomotionへ復帰する。
- 攻撃6本は個別Triggerで起動し、終了後に接地状態に応じてLocomotionまたはFallLoopへ戻る。
- `IsBlocking` で `ToBlocking → Blocking → BlockingToIdle`。GuardHitはBlockingから割り込む。
- Hit、Death、Victory、Defeatは明示的なTrigger。Death・Victory・DefeatIdleはResetまで保持し、通常移動や被弾で勝手に戻らない。
- 全StateのIK重みは0。Blenderで評価済みの両手拘束や接地姿勢へ、未設定のゲームIKを重ねない。
- マスクはBase=`Root`配下、UpperBody=`Spine`配下、Arms=左右`Shoulder`配下。武器Socketは右手配下なので全マスクに含まれる。

## 参照とインポート

Controllerは原本FBXを直接モーションとして読まない。現行 `AnimatorSystem` は取り込み済み `.anim` を必要とする。
各FBXの新規GUIDから `AssetDatabase::DeriveGuid(sourceGuid, "anims/<take>.anim")` と同じFNV-1a二系統で
サブアセットGUIDを導出し、`guid:<derived>|Library/Baked/<sourceGuid>/anims/<take>.anim` を参照する。
この参照はFBZZ EditorでFBXをインポートした時点で解決される。Libraryの生成物は手で偽造しない。

`.fbx.meta` はBlender、倍率1、OpenGL法線、ループ7本を設定する。Root Motionは配置時のAnimatorで無効にする。
FBXとControllerの構造検証は実施するが、FBZZでのインポート・実機再生は別の確認項目として記録する。

配置後に自動生成されたLibraryキャッシュは検出したが、その処理が取り込み設定をAuto / DirectX / clips空へ戻していた。
原本のGUIDを保持したままBlender / OpenGL / 各クリップのLoop設定を復元済み。設定反映後の再インポートが必要。

## 配置済みPlayerのゲーム接続（追加依頼）

Stage_01にユーザーが配置したPlayerのインスタンスと位置を維持する。新規複製はせず、
Animator・動的RigidBody・CapsuleCollider・CharacterController・PlayerComponentを接続する。
一刀モードでは右手の`Socket_Weapon_R`だけを使い、抜納刀と旧パリィ・登攀を起動しない。
攻撃は既存BladeComponentの判定と入力を使用し、全身OverrideのSlotで完成クリップを再生する。
被弾も全身Overrideとし、加算合成は使わない。右クリックの既存入力名は維持してガードの保持へ接続し、
ガード可能な攻撃を受けた際にGuardHit、死亡時にDeathを一度だけ再生する。
旧武器のアセット参照は新クリップと新しい一本の剣へ置き換える。全身の骨格は変更しない。
