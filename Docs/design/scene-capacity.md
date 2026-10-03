<!-- @file    scene-capacity.md -->
<!-- @brief   シーンの Entity 容量不足と回復可能な作成失敗の契約。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-02 -->

# シーンの容量と生成失敗

Scene は `ComponentArray::MAX` と同じ 4096 枠を持つ。非アクティブな GameObject、Prefab プールの待機個体、破棄キューで待つ個体も枠を使う。削除処理を実行するとその Entity の枠を再利用できる。上限値と固定のスパース配列はこの変更では拡張しない。

外部入力や実行中の生成は `TryCreateGameObject(name)` を使う。空き枠がなければ警告を記録して `nullptr` を返す。失敗では名前・GUID の生成や GameObject の追加を行わず、既存の Entity の index、generation、空き枠、階層、Component を変更しない。呼び出し側は戻り値を確認して、操作の失敗や無効なスクリプト参照として伝える。

`RemainingEntityCapacity()` は未使用の末尾枠と削除済みの再利用枠を合計する。`CanCreateGameObjects(count)` は必要な個数がこの合計以下かを確認する。0 個は常に受け入れ、極端に大きい個数も加算せず比較するため整数の折り返しを起こさない。これらは枠の予約ではない。Scene の変更は単一スレッドで行い、事前確認と生成の間に別の生成やユーザー callback を挟む場合も各 `TryCreateGameObject` の失敗を扱う。

ファイル読込、階層の複製、Prefab の追加など複数個の操作は、作成対象の個数を先に数えて容量を確認する。既存のシーンを編集する操作では、容量不足だけで既存のオブジェクトを削除したり、一部だけ追加した成功として扱わない。追記する全個体の容量を確認してから生成し、途中で生成が拒否された場合はその操作で追加した個体を除去して失敗を返す。Prefab の差し替えは旧階層を残したまま新階層を構築できる容量が必要になる。

参照を返す `CreateGameObject` は容量が既知の初期構築と、容量を検証済みの内部処理だけの便宜 API として残す。容量不足を返す形式ではないため、この事前条件違反は assert で停止し、Release でも最終防御として abort する。入力次第で上限に届く入口からこの API を呼ばない。

スクリプト側の `ScriptSceneProxy::Create` は nullable pointer を返す。参照を返していた旧 DLL は容量不足を確認できないため、`kScriptProxyApiAbiVersion` を署名に含めて拒否する。Proxy 本体のサイズや vtable の変更がない非仮想 API の変更も、この世代で区別する。

`ComponentArray` の追加は有効な Entity と空き Component 枠を事前条件に持つ。Scene の成功した生成は必ず範囲内の Entity を返し、同じ Entity に同じ Component 型を重複追加しない限り Component の個数も 4096 以下に収まる。無効な参照を照会する `Has` / `GetComponent` / `GetGameObject` は配列へアクセスする前に拒否する。

CPU 回帰テストは上限直前の成功、上限での無変更失敗、非アクティブな子と破棄待ち個体の容量計上、破棄後の枠と世代の再利用、再度の上限到達、Component の最大個数、無効 ID の照会、Scene の Clear と move を確認する。
