<!-- @file    broadphase-incremental.md -->
<!-- @brief   BroadPhaseの保持BVH・差分refit・比較検証の契約。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-22 -->
# BroadPhaseの差分更新

2026-10-22のユーザー指示により、各substepのBVH再構築を、solverが所有するBVHの保持と変更境界のrefitへ置き換える。PhysicsからEngine/Coreへ依存せず、既存のWorld同期とsleep判定は変更しない。

## 入力と寿命

Colliderの公開形状とUpdateによる直接変更を検出するため、各呼出しで現在の非null ColliderのAABBを一度ずつ読む。保持するのは入力配列の添字、AABB、分割順とノードだけであり、Collider/Body/ColliderInstanceのpointerは保持しない。入力配列の移動、同じ添字での形状差し替えとsolverのコピーを跨いでも、出力の非所有参照は現在の入力配列を指す。

同じsolverの同時実行、layer filterからの同じsolverへの再入と入力変更は行わない。layer、body対応、static、sleep、trigger、materialはキャッシュせず、既存の候補採用処理で現在値を読む。layer filterは対称な衝突行列を想定する。

## 更新

AABBの各成分を比較し、変更したproxyの葉と祖先を重複なくdirtyにする。preorderの逆順で葉から祖先へboundsを更新する。少数変更ではdirty添字だけをソートし、多数変更ではノード配列の逆順を使う。

有効な入力添字の集合が変わった場合は中央値分割を再構築する。旧葉の境界と重ならない移動がmax(8, proxy数/8)を超えた場合も、refit前に再構築する。内部ノードの表面積合計をroot表面積で正規化し、構築直後の1.5倍を超えた場合はrefit後に再構築する。非有限・逆転boundsを含む呼出しは分割せず全候補のAABB判定へ戻し、正常入力に戻ったら再構築する。

全AABBの照合と候補探索は残る。初期段階では追加・削除を局所挿入へ変えず、static/dynamic別の木、接触pair cache、Worldのwhole-sleep early-outの拡張も行わない。

候補の集合と重複なしを守り、出力へ追記する。木を保持するため動いた後の列挙順は毎回再構築した木と異なり得る。候補の順序とPGS結果のbit単位一致は保証せず、物理の既存回帰と接触・CCD・sleepの契約で検証する。

## 検証と計測

BroadPhaseIncrementalTestsは、別の総当たりoracleと現在の候補集合を比較する。移動、直接半径変更、密集、teleport、所属変更、状態変更、配列再配置、solverのコピーと移動、不正boundsからの復帰を通常の公開操作で検証する。PhysicsAutoの既存テストも実行する。

FBZZPhysicsBroadPhaseBenchは画面・GPU・Editorを起動せず、256/5000個体の静止、1個移動、1%移動、全移動、大規模並べ替え、追加削除相当、static、sleep、初回構築と、World同期+Stepを比較する。入力更新と候補検証はBroadPhase単体の計時外に置く。Worldの計時は同期とStepを含み、EngineのTransform同期や描画を含まない。生標本と検証結果をJSONへ保存する。

Release/LTO有効の3プロセスずつの交互比較は[2026-10-22の計測レポート](../benchmarks/2026-10-22-broadphase-incremental/report.md)に保存する。初回構築と所属再構築の費用は定常更新の結果と分ける。

## 参考

- [Godot内蔵物理のBroadPhase](https://github.com/godotengine/godot/blob/master/modules/godot_physics_3d/godot_broad_phase_3d_bvh.cpp)：木を保持し、移動を別操作として扱う境界を参考にした。Godotの実装コードは複製しない。
