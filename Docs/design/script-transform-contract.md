<!-- @file    script-transform-contract.md -->
<!-- @brief   Script Transform の即時同期とワールド位置編集の契約。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-02 -->
# Script Transform の座標契約

`ScriptTransformProxy` の `position` / `rotation` / `scale` はローカル値であり、姿勢の正本である。これらの代入と `Translate` / `Rotate` / `LookAt` は、戻る前に対象と全子孫のワールド値を更新する。非アクティブな子孫も含み、同じコールバックから更新後の `worldPosition` / `worldRotation` / `forward` を読める。

`worldPosition` への代入は、親の位置・回転・非均一スケールを使ってローカル位置を逆算する。ローカル回転とスケールは変更せず、子孫も同期する。ルートでは `position` と `worldPosition` が同じ位置になる。次の `FlushWorldTransforms` や PrePhysics でも、指定した位置がローカル値から再現される。位置を置くために二つのプロパティへ同じ値を代入する必要はない。

親のワールドスケールが絶対値 `1e-6` 以下の軸では逆変換できない。その軸のローカル位置はゼロとし、到達できるワールド位置を返す。たとえば回転のない親の位置が `(10,20,30)`、スケールが `(0,2,3)` なら、子へ `(99,24,36)` を指定した結果は `(10,24,36)` になる。無限大や NaN を姿勢へ混ぜず、回転とローカルスケールは保つ。

## 更新の範囲と既存の経路

即時同期は祖先をローカル値から順に計算し、対象の部分木だけを更新する。同じフレームに親のローカル姿勢を変えた場合や、親の world 値が描画補間中の場合でも、そのキャッシュをローカル位置の逆変換に使わない。他のルートと祖先の別の子孫は更新しない。

ワールド値の読み取りだけでは再計算しない。描画補間された姿勢を読む既存の経路と、物理の確定姿勢を分離したままにする。物理シミュレーションがその後に動かした位置や、LateUpdate の描画補間は、各システムの契約に従う。

この即時同期は Script のプロキシ操作に適用する。`GameObject::transform` の公開メンバーへ直接書く Engine・Editor のバッチ処理は、従来どおり必要なタイミングで `FlushWorldTransforms` を呼ぶ。`SetWorldPose` は親のワールド姿勢が確定済みであることを前提とする低水準の書き込みであり、子孫の同期は呼び出し側が行う。

C# の `Objects.TranslateWorld` / `TrySetWorldPositions` は、入力順でこの即時同期を反復する。親子や重複参照の順序を変えない。事前検証の shadow と実適用は、`Transform` と親の姿勢を受ける共通の `UpdateWorldTransform` / `SetWorldPose` 数式を利用する。後半の移動や子孫同期で非有限値になる要求も Scene と出力を変更せず拒否する。`TryReadWorldPositions` は通常の読み取りと同じ world キャッシュを返す。

Script のサイズ、プロキシのデータメンバー、仮想関数の並びは変えない。`worldRotation` は引き続き読み取り専用である。

C# の `LocalRotation` / `LocalScale`、`Rotate(axis, degrees)`、`LookAt(worldTarget)` は同じ local 正本と即時同期へ接続する。書き込みは作業用 Transform で祖先・子孫を事前計算し、不正な回転や非有限値、子孫での overflow を Scene の変更前に拒否する。負・零スケールは許す。`Rotate` の零・微小軸は完全な no-op とし描画 world キャッシュも同期しない。読み取りの `WorldRotation` / `Forward` / `Right` / `Up` も描画キャッシュを再計算しない。実体のある enabled な物理 body の書き込みは Physics 専用 API が接続されるまで拒否する。

## 全体同期の実行範囲

Gameplay の姿勢システムは有効な操作が local から確定した world 入力を必要とする段階で同期し、その段階が local 姿勢を書いた後に同期する。attachment、constraint、camera の各段階と個体順は変えない。対象がない、無効、参照が解決できない段階は LateUpdate の描画補間された world を上書きしない。

LateUpdate の物理描画補間は各個体の最終 world を親から子の順で直接計算するため、その直前の同じ全体 local-to-world 走査は行わない。public Transform フィールドの直接変更と明示 Flush の契約は維持し、dirty 判定は導入しない。

Animator、IK、SpringBone、Ragdoll は local の直接変更と骨以外の子孫も持つため、有効な producer がある場合は制約システムの前に一度の保守的な全体伝播を残す。ResourceManager を必要とする producer はその実行可能性も判定し、対象がない静止シーンの全体走査を避ける。

## 検証

既存の `ScriptTransformProxyTests` で未接続時の既定値とルートの即時同期を検証する。`ScriptTransformHierarchyTests` で回転・負の非均一スケール付きの親、同フレームの祖先変更、非アクティブな子孫、非可逆軸、PrePhysics 後の再現、読み取り時の描画姿勢維持を検証する。

## CPU同期の作業リストと検証済み算術の再利用

全体Flushと描画補間は、その呼出し時点のGameObjectsとGetParentから親優先の作業リストを作る。flat格納順が親子順と逆でも祖先を先に処理する。非アクティブな子も同期する。Component参照・階層・Scene寿命の永続キャッシュは作らず、callbackを呼ばない区間で容量だけを再利用し、終了時にGameObject参照を消す。部分木Flushは祖先解決と子リンク走査を維持する。

GameObjectを通る通常のworld算術では、既存のAI姿勢サービスが有限値・正規化可能性を検証して記録した結果を参照できる。local position/rotation/scale、親の有無、親world position/rotation/scaleがビット単位で一致する場合だけworld三値を復元する。Sceneや個体の識別情報を算術の入力とはしない。参照を保持しない値だけのmemoなので、SceneのClear・移動・slot再利用も現在の数値入力で判定する。符号付きゼロの差を許容誤差で潰さず、SSE丸め・denormal・例外maskの制御変更で証明を無効化する。未検証入力をこのmemoへ登録しない。通常の算術は従来のUpdateWorldTransformを使用する。

更新頻度、物理履歴、補間alpha、AnimatorのLODとCPU/GPU姿勢経路は変更しない。localの直接変更とworldの直接変更に対するFlushの同期契約も維持する。

AI姿勢ステージングでは、親worldRotationとlocalRotationのQuaternion積およびその長さを検証と正規化で共有する。Quaternion::Normalizedと同じfloatの逆数・乗算順序でworldRotationを生成し、有限性・正規化可能性・出力検証を省かない。失敗時は従来どおりshadowを公開せず、出力とSceneを変更しない。通常のUpdateWorldTransformを基準として、非単位ローカル回転・非均一/負/ゼロscaleを含む96階層のビット単位一致を検証する。
