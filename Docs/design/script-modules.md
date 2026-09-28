# C++ Script の内部モジュール

2026-09-28。ゲーム処理を分割してもアタッチとライフサイクル中継を増やさないための改善。

## 選択と範囲

- 通常のクラス: 計算・状態・ヘルパーを分割する既存の Utility Header を使う。登録もアタッチも不要。
- Script 基底へ子の一覧と更新順の管理を追加: 全スクリプトへ管理コストを持ち込むため今回は採用しない。
- **明示的な ScriptModules 登録**: 既存 Script をメンバーとして所有したまま、共通の管理クラスが通知を中継する方式を採用する。

利点はアタッチを増やさず既存の proxy / 反射 / 障害処理を再利用できること。
制約は親が各フェーズから一度ずつ管理クラスを呼ぶ必要があること。ゲーム固有の依存関係や更新順は明示する。
依存を名前や型だけで推測して自動注入しない。C# ランタイムの導入と実行性能の比較は別の判断とする。

## 契約

- 親とモジュールは管理クラスより長く生存し、登録後は移動しない。登録は構築時の一度だけ。所有権は親のメンバーにある。
- 開始・反射・終了は登録順。Update / LateUpdate / FixedUpdate はそれぞれの指定順。同順位は登録順。
- Context は通知前に親から再取得する。開始時に Awake / Enable / Start を通す。二重 Start / Destroy は無視する。
- AdoptContext は非所有の親 Script 参照も保持する。isActiveAndEnabled とイベント配信が親の enabled を見落とさないため。Script のサイズが変わるので Engine と Script DLL を同じ SDK で再ビルドする。仮想関数は変更せず、既存の sizeofScript ABI 検査で旧 DLL を拒否する。
- 親の無効化は Disable から通知する。子の enabled 変更は各更新フェーズで同期する。無効化は開始状態を失わず、再有効化で Start を繰り返さない。
- Invoke / FrameDelay / Coroutine は Update で一回進める。Late / Fixed では進めない。更新フェーズを持たない子もタイマーは使用できる。
- 実行は ExecuteCallback を通す。破棄では OnDestroy とタイマー・購読の解放を行う。エンジン本体と同じく、破棄だけを理由に OnDisable は送らない。
- 描画用 Gizmo は実行前でも通知できる。衝突・アニメーションイベントなどの引数付き通知は親が必要な相手へ明示的に渡す。
- Get<T>() はこのグループ内の具象型・基底型・インターフェースを探す。同じ型が複数該当する場合と未登録は nullptr。シーン内検索へ暗黙に混ぜない。
- FBZZ_REFLECT_MODULES は親自身の FBZZ_FIELD 宣言と子の Reflect を両方実行する。既存 Player の保存キーを維持するため子の項目は平坦に並べる。モジュール間で永続キーを重複させない。
- Ref<T> の解決は Script 派生だけでなく FBZZ_SCRIPT_INTERFACE にも対応させ、GetScript<T>() と同じ型判定を使う。

既存の TryBases は MSVC の実行テストで基底・インターフェース一致を返せなかったため、
副作用付き fold 式を、各基底の結果を順番に返す処理へ変更した。
複数継承で補正されたポインターと、参照先の削除検出をテストする。
ゲーム側の IDamageable 等が持つ既存の登録簿は、この変更では移行しない。

## 利用例

```cpp
Movement movement;
Health health;
ScriptModules modules{*this, {
    ScriptModule{movement}.Update(0).FixedUpdate(),
    ScriptModule{health}.Update(1),
}};

void OnStart() override { modules.Start(); }
void OnUpdate() override { modules.Update(time.DeltaTime()); }
void OnFixedUpdate() override { modules.FixedUpdate(); }
void OnLateUpdate() override { modules.LateUpdate(); }
void OnEnable() override { modules.Enable(); }
void OnDisable() override { modules.Disable(); }
void OnDestroy() override { modules.Destroy(); }
```

クラス外の FBZZ_REFLECT_MODULES(MyScript, modules) で反射も登録一覧へ集約する。
通常のアタッチされた相手には FBZZ_REF(TargetType, target, "Target") を使い、利用時は未設定・破棄済みの確認を行う。
内部モジュールの参照は modules.Get<T>() または親のメンバー参照で取得できる。親と同じ寿命の参照をシーン越しに保存しない。

## 検証

開始・終了の一回性、フェーズ順序、親と子の無効化・再有効化、タイマーと破棄後の購読解放、型付き参照の削除検出を単体テストする。
Player は terrainRecovery と内部モジュールの設定の反射往復を回帰テストする。
内部 Controller にも届く停止通知の契約は、記録用モジュールで検証する。実音の停止は Play での確認対象。
既存のプレイヤー入力・戦闘・LateUpdate の相対順を維持し、ゲーム側スクリプトもコンパイルする。

2026-09-28 の Development 構成では、追加の 10 件と既存の関連テストを合わせて 64 件が成功した。
親自身の反射を一時的に外すと Player の保存テストが失敗し、戻すと成功することも確認した。
実際のゲーム画面・入力・音を通した Play 確認は別途行う。
