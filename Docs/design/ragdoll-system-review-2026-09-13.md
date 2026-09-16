# ラグドールシステムレビュー

## 修正状況（同日追記）

以下の7指摘に対する修正を実装した。後段の指摘と行番号は修正前の記録。
C++のビルド・回帰テスト実行・実機検証は未実施であり、合格を確認した状態ではない。

| 指摘 | 実装した修正 | 回帰テスト |
|---|---|---|
| 1 | 階層の骨長を保つ射影をソルバの速度導出前へ移動。剛体も補正し、混合途中の骨長も保持 | StandingPose / RagdollRig / XPBDSolver |
| 2 | 除外・深さ制限の先の全子孫を最終親姿勢から再構成 | StandingPose |
| 3 | 1/120秒の固定刻みで物理・回復・Blendを統一。過負荷分は蓄積 | RagdollSystem |
| 4 | 自重比の出力0をRigで無効化。有限出力APIで位置・回転を個別に制御し、既存の無制限指定も保持 | RagdollRig / XPBDPoseAnchor |
| 5 | 準備中のBeginを保持。骨リストの構築を未生成のボーンEntity数に依存させない | RagdollSystem |
| 6 | 開始時の重みからBlendOut。再開時のBlendInも現在値から連続させる | RagdollSystem |
| 7 | 最終骨格の書き戻し後に描画境界を更新。Animatorと計算を共有 | RagdollSystem |

追加で、準備中BeginのEndによる取消、一時停止、立位の根移動に伴う偽の衝撃、
オーナースケール変更時のリグ再構築、GPUパレット上限を修正した。
立位は根を固定する演出用制約。足先個別固定による伸縮は撤去したため、足先は許容変位内で動く。

実施済み: git diff --check、ステージ2本のTOML構文、テスト登録、
固定時計の算術確認（15/30/60/120/240fpsで1秒=120刻み）。
残る検証: VS CodeのBuild All / Tests: Build & Run Suite、故障注入、
GreenWare実機での連続被弾・加減速・足滑り・カメラ端・低FPSの確認。
詳細な契約は[active-ragdoll.md](active-ragdoll.md)を参照。

2026-09-13。現在の作業ツリーを静的レビュー。対象はRagdollSystem、RagdollRig、XPBDの拘束・接触・積分、Animatorとの受け渡し、ScriptRagdollProxy、関連テスト。ビルド・実機再現は未実施。以下の症状はコード経路からの判断であり、実機で観測した結果ではない。

## 判断

サーボの数値調整を進める前に、姿勢の受け渡しと状態遷移を修正する必要がある。
剛体・関節・プロファイル・スクリプトプロキシの分離は再利用できるが、最終的な骨格姿勢の整合性を一箇所で保証できていない。
前回追加したstandingGuardは、骨単位の出力制限であり、関節拘束と整合した立位保証にはなっていない。

## 指摘

### 1. [P1] 姿勢維持の補正が関節拘束と物理状態を破る（前回追加分）

場所: [RagdollSystem.cpp:698](../../Projects/Engine/src/Scene/Systems/RagdollSystem.cpp#L698)、[StandingPose.hpp:27](../../Projects/Engine/include/Engine/Scene/Ragdoll/StandingPose.hpp#L27)

ソルバを解いた後、各骨のワールド位置と回転を独立にクランプする。根と足の位置だけをFKへ固定するため、親子間の長さや関節アンカーの一致は保持されない。
補正後の姿勢は描画・ボーンTransformへ渡るが、通常のクランプでは剛体へ戻さない。内部の接触位置・速度と画面上の体がずれる。

数式上の反例: FKで親Y=0、子Y=1、解が両方+0.3mへ平行移動していた場合、元の長さは1mのまま。根を固定し子の変位を0.12mへ制限すると、親Y=0、子Y=1.12となり長さが12%伸びる。この変位は再捕獲閾値0.96m未満なので、ソルバの姿勢も残る。

修正方向: 支持点・体幹の制約はソルバで解く。演出用の混合は親子のローカル回転と固定長の骨格で行い、最終FKを一度組み立てる。最終的な安全復帰では姿勢・速度・接触履歴を一緒に戻す。

### 2. [P1] 物理対象から除外した子ボーンが、動いた親へ追従しない

場所: [RagdollSystem.cpp:214](../../Projects/Engine/src/Scene/Systems/RagdollSystem.cpp#L214)、[RagdollSystem.cpp:724](../../Projects/Engine/src/Scene/Systems/RagdollSystem.cpp#L724)

maxDepthやprofile.IsExcludedで子孫をruntime.bonesから取り除く一方、書き戻しとスキニング行列の更新もruntime.bonesの範囲だけを対象にする。
RagdollRig::WritePoseの「剛体を持たない骨を親に追従させる」処理は、この配列に含まれている骨にしか効かない。
プロファイルが除外する指・つま先、深さ制限の先にある骨は、親が物理で動いてもアニメーション側の行列が残り、境界でメッシュが伸びたりねじれたりする。

修正方向: 物理へ載せる骨の集合と、最終姿勢を再計算する骨の集合を分ける。物理対象から外れた子孫も、ローカル姿勢を保ったまま最終FKとスキニング行列を更新する。

### 3. [P2] 30fpsを下回ると物理と復帰タイマーの時間がずれる

場所: [RagdollSystem.cpp:627](../../Projects/Engine/src/Scene/Systems/RagdollSystem.cpp#L627)、[RagdollSystem.cpp:681](../../Projects/Engine/src/Scene/Systems/RagdollSystem.cpp#L681)、[RagdollSystem.cpp:712](../../Projects/Engine/src/Scene/Systems/RagdollSystem.cpp#L712)

筋力回復・Blend・Holdは実フレームdtを使う一方、物理Stepは最大1/30秒に制限され、余剰時間を捨てる。
15fpsが続くと1秒の間に物理は0.5秒しか進まず、アニメーションと復帰タイマーだけが1秒進む。負荷が高い場面ほど反動・追従遅れ・復帰の見え方が変わる。

修正方向: 時間を蓄積して固定刻みで進め、目標姿勢の補間と回復・Holdも同じシミュレーション時刻へ揃える。過負荷時の上限と時間破棄は明示的な方針として分離する。

### 4. [P2] 支える力を0にすると、逆に無制限の固定になる

場所: [XPBDPoseAnchor.cpp:10](../../Projects/Physics/src/XPBDPoseAnchor.cpp#L10)、[XPBDPoseAnchor.cpp:36](../../Projects/Physics/src/XPBDPoseAnchor.cpp#L36)、[XPBDConstraint.cpp:98](../../Projects/Physics/src/XPBDConstraint.cpp#L98)

maxForce/maxTorque=0のとき、アンカーはcompliance=0、maxLambda=0を渡す。共通拘束関数はmaxLambda>0の場合しか制限しないため、これは「力なし」ではなく「硬い拘束、上限なし」になる。
rootAnchor=0、重力0、impactSlack=1によってアンカー出力が0へ落ちる経路で、意図と逆に固定が強くなる。関節ドライブも0と無制限の意味を共有しており、境界の契約を整理する必要がある。

修正方向: 無効・有限の出力・無制限を別々に表す。アンカーのゼロ出力はSolveを行わず、線形と角度を個別に扱う。ゼロ重力でも筋力を設定できるよう、筋力の基準と実際の重力を分離する。

### 5. [P2] アセット準備前のBegin要求が失われる

場所: [RagdollSystem.cpp:509](../../Projects/Engine/src/Scene/Systems/RagdollSystem.cpp#L509)、[RagdollSystem.cpp:498](../../Projects/Engine/src/Scene/Systems/RagdollSystem.cpp#L498)

Begin直後にAnimatorやモデル・スケルトンが未準備だと、beginRequestedをfalseにしてreturnする。
phaseはIdleのままで、次のフレームはモデル確認より前のIdle分岐に入り続ける。後からモデルを読み込めても、自動再試行されない。
activateOnStartも開始要求を出した時点で消費するため、この失敗を救済しない。GreenWareのようにOnStartで一度だけ起動する構成に影響する。

修正方向: WaitingForAssets / Ready / Runningなどの準備状態を、BlendIn / Hold / BlendOutから分離する。準備中は開始要求を保持し、永続的なリグエラーと区別する。

### 6. [P2] BlendIn途中のEndで、物理の適用率が急増する

場所: [RagdollSystem.cpp:621](../../Projects/Engine/src/Scene/Systems/RagdollSystem.cpp#L621)、[RagdollSystem.cpp:409](../../Projects/Engine/src/Scene/Systems/RagdollSystem.cpp#L409)

EndはBlendOutへ移る際にphaseTimerを0へ戻すが、開始時点のweightを保存しない。
BlendOutは常にactivationWeightを起点に計算するため、weight=0.2で停止しても次フレームに約0.96へ跳ねる（上限1、blendOut=0.4秒、60fpsの場合）。反応を弱めようとして、逆に一瞬大きくなる。

修正方向: 遷移時のweightを保存して補間の始点にする。Begin→End、End→Begin、同一フレームのBegin→Endを状態遷移テストで押さえる。

### 7. [P2] ラグドール後の姿勢が描画カリング範囲へ反映されない

場所: [AnimatorSystem.cpp:3072](../../Projects/Engine/src/Scene/Systems/AnimatorSystem.cpp#L3072)、[RagdollSystem.cpp:734](../../Projects/Engine/src/Scene/Systems/RagdollSystem.cpp#L734)、[GeometryPassHelpers.cpp:532](../../Projects/Engine/src/Scene/Systems/RenderPasses/Geometry/GeometryPassHelpers.cpp#L532)

カリング用のskinnedBoundsはAnimatorの姿勢で更新される。その後RagdollSystemが骨とスキニング行列を動かすが、範囲を更新しない。
描画側は古い中心・半径を使うため、Passiveで本体原点から離れた死体や、大きく押された部位を誤って画面外と判定する可能性がある。

修正方向: 最終姿勢の確定後に、パレット・ボーンTransform・描画範囲をまとめて更新する。通常カメラの画面端に物理体だけが残るケースを検証する。

## テスト上の不足

StandingPoseTestsは1個の位置・回転を補正する関数だけを検証し、親子の長さ、ソルバとの一致、復帰の連続性を検証していない。
XPBDSolverTestsにはsubstep数を変える検証があるが、RagdollSystem側がフレームdtを切り捨てる経路を通らない。
実描画に依存するResourceManagerが無いとRagdollSystem全体がreturnするため、頭から終わりまでの自動検証を組みにくい構造もある。

追加すべき検証:

- 支持点を固定した2本以上の骨で、連続衝撃後も骨長と関節アンカーが一致する。
- 手・指、足・つま先、深さ制限境界で、除外した子孫も親の最終姿勢へ追従する。
- 15/30/60/120fps、同じシミュレーション時間・入力で、変位と復帰時間を比較する。
- 出力0、微小な出力、通常出力で、支持の強さが逆転しない。
- モデルを遅れて用意しても、最初のBeginが一度だけ成立する。
- Blendの全遷移で、weightが遷移直前の値から連続する。
- 最終骨格の描画範囲が、物理で移動した体を包含する。

## 改修の順序

1. **姿勢の正本を分ける。** アニメーション・IK後の目標姿勢、物理の内部状態、最終出力姿勢を別々に保持する。GameObject Transformを兼用の受け渡し場所にしない。
2. **立位維持を拘束として実装する。** 今回の用途は移動コントローラーを保った演出なので、支持する根・足と、反応させる上体・腕をリグ情報で指定する。世界座標の骨名判定と後処理クランプに依存させない。
3. **最終骨格を一度だけ組み立てる。** ローカル回転の混合→全子孫のFK→骨Transform・パレット・描画範囲の更新を共通化する。
4. **時間と状態遷移を統一する。** 固定刻み、準備待ち、連続するBlend、ゼロ出力の契約を、描画を必要としない更新処理でテストする。
5. **それから演出を調整する。** Bossの重い反動、Playerの柔らかい反動、蛇の局所反応は設定として載せる。まず上記の不変条件を満たしてから強さを詰める。

ここまでが修正前レビューの記録。最新の実装・検証状況は冒頭の追記を参照。
