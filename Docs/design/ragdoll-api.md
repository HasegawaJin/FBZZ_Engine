# Ragdollの汎用APIとゲーム側の責務

2026-09-13。RagdollSystemの計算方式は[active-ragdoll.md](active-ragdoll.md)を参照。

## 責務

Engineは骨の選択、物理・関節駆動、姿勢制約、ブレンド、衝撃入力、最終姿勢の同期を担当する。
キャラクターの種類、歩行周期、被弾・死亡の条件、反動の配分はゲーム側に置く。
Mech / Humanoidは選択可能な物理プロファイルであり、Player / Bossの判定には使わない。

## 独立した設定

| API | 変更する設定 |
|---|---|
| SetRoot / SetRoots | 対象部分木と深さ |
| SetExcludedBranches | 指定ボーンとその全子孫の除外 |
| SetProfile | 物理プロファイル |
| SetStandingGuard(enabled, distance, degrees) | 根位置・骨長の保持と目標からの変位・回転制限のみ |
| SetRootAnchor(scale, sag, tiltDegrees) | 自重比の支持倍率、たわみm、傾き度 |
| SetMuscle / SetRecovery | 関節駆動と衝撃後の回復 |
| SetGravity / SetContacts / SetGroundPlane | 重力、接触、補助平面 |
| SetCollapse | ActiveからPassiveへの自動遷移の距離。0は自動遷移無効 |

StandingGuardはプロファイル・支持倍率・重力・接触・駆動モードを上書きしない。
BeginはPassive、BeginActiveはActiveを明示的に選ぶ。Guardを有効にしたPassiveも許可する。
その場合は筋力駆動なしで、姿勢制約だけが目標に対して働く。
回転制限はクォータニオンの最短角0〜180度。ゲーム向けの25度／30度上限は置かない。

## ボーン選択と保存

rootBoneName / extraRootBones / excludedRootBonesはInspectorで編集でき、シーン・Prefabへ保存される。
除外はノード名で解決し、部分文字列による「脚」などの意味判定はしない。
除外は追加の根より優先する。存在しない除外名はリグ構築時に警告する。
除外した子孫は物理を持たず、元の親ローカル姿勢を最終親姿勢へ伝播する。
プロファイル自身の除外規則は引き続き適用される。

## 衝撃入力

- Push / PushAt: ワールド並進速度の加算[m/s]。角速度を暗黙に加算しない。
- PushAngular / PushAngularAt: ワールド角速度の加算[rad/s]。並進速度を変更しない。
- Atは指定原点からの距離で減衰し、radius <= 0は全剛体へ一律。
- 入力の意味はGuardや駆動モードで変えない。方向・配分・演出上限は呼び出し側が決める。

## GreenWareへの移行

`Assets/Scripts/Utils/RagdollPresentation.hpp`が立位設定セットと演出用の並進／回転配分を持つ。
Playerの周期的な揺れと入力タイミングはPlayerComponentに残す。
Boss01の除外名Yaw_FR / Yaw_FL / Yaw_BR / Yaw_BLはBossRigComponentとStage_01.sceneが指定する。

旧preserveLegAnimationとSetLegAnimationPreservedは削除し、該当シーン・スクリプトを移行した。
旧SetStandingGuard(distance, degrees, human)は独立した設定APIへ置き換えた。
今回追加したユーティリティはScript登録マクロを持たず、ScriptCodeGenの登録対象ではない。

## 検証

回帰テストに設定の非干渉、制約の解除、Passive要求の保持、任意名の枝の除外、
追加根より除外を優先すること、リストの保存・復元、並進・回転入力の独立性を追加。
C++ビルド・テスト実行・GreenWareでの見た目確認は未実施。
EngineとScriptsのAPIが変わるため、全体ビルドとEditorの再起動が必要。
