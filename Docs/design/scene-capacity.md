<!-- @file    scene-capacity.md -->
<!-- @brief   動的な Entity・Component 保存と回復可能な生成失敗の契約。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-02 -->

# シーンの動的保存と生成失敗

Scene に制作上の個体数上限は設けない。Entity の世代と GameObject の非所有索引は必要な分だけ動的に拡張する。非アクティブ個体、Prefab プールの待機個体、破棄キューで待つ個体も Entity を使い、削除処理の完了後にその index を再利用できる。`EntityID::INVALID_INDEX` は uint32 index の無効値であり、固定確保量ではない。

外部入力や実行中の生成は `TryCreateGameObject(name)` を使う。EntityID で表現できる未使用 index がなくなった場合は警告と nullptr を返す。失敗では名前・GUID の生成や GameObject の追加を行わず、既存の index、generation、階層、Component を変更しない。必要なメモリは実行環境が用意する。回復可能な生成失敗の確認と、メモリ不足に対するプロセス全体の方針を混同しない。

`RemainingEntityCapacity()` は EntityID で表現できる未使用末尾 index と削除済みの再利用 index を合計する。`CanCreateGameObjects(count)` はその表現可能性を加算せず比較し、整数の折り返しを防ぐ。これらはメモリや枠の予約ではない。`EntitySlotCount()` は現在までに割り当てた index 領域の大きさであり、bridge の作業配列と照会の境界はこの実数を使う。数値表現限界の値で作業配列を確保しない。

ファイル読込、階層の複製、Prefab の追加など複数個の操作は、作成対象の個数を先に数えて容量を確認する。既存のシーンを編集する操作では、容量不足だけで既存のオブジェクトを削除したり、一部だけ追加した成功として扱わない。追記する全個体の容量を確認してから生成し、途中で生成が拒否された場合はその操作で追加した個体を除去して失敗を返す。Prefab の差し替えは旧階層を残したまま新階層を構築できる容量が必要になる。

参照を返す `CreateGameObject` は容量が既知の初期構築と、容量を検証済みの内部処理だけの便宜 API として残す。容量不足を返す形式ではないため、この事前条件違反は assert で停止し、Release でも最終防御として abort する。入力次第で上限に届く入口からこの API を呼ばない。

スクリプト側の `ScriptSceneProxy::Create` は nullable pointer を返す。参照を返していた旧 DLL は容量不足を確認できないため、`kScriptProxyApiAbiVersion` を署名に含めて拒否する。Proxy 本体のサイズや vtable の変更がない非仮想 API の変更も、この世代で区別する。

`ComponentArray` は必要な型だけ安定アドレスの分割 dense 領域を確保し、sparse 索引を到達した Entity index に応じて拡張する。分割の個数に固定上限はなく、分割サイズは確保の粒度だけを表す。追加で既存 Component の参照と寿命 token を失効させない。削除は既存の末尾 swap 契約を維持する。Data は連続 span ではなく軽量 view、Entities は連続 span であり、後者は追加による再確保を跨いで保持しない。無効参照は配列アクセス前に拒否する。

`ForEachChunk` は同じ dense 範囲の EntityID と Component を等長の span で渡す。末尾は実要素だけを含み、型別の連続領域を一度引いて走査できる。callback 中の追加・削除・Clear・move は禁止する。既存の Data の逐次走査も chunk の境界だけで保存領域を引き直す。

公開 GameObject の Transform 配置は維持する。一括 world 位置操作は、適用対象が同じ Scene の重複しない子なしルートだけの場合、再利用する dense 作業配列で姿勢を事前計算し、全対象の有限性確認後にまとめて確定する。translate の最初の読み取りは現在の world キャッシュとし、確定後は local を正本に world を即時同期する。重複・親子関係がある場合は既存の入力順 shadow 計算へ戻し、欠損 slot・active 条件・物理個体の拒否・失敗時の Scene と出力の不変性を共通に保つ。

CPU 回帰テストは旧4096境界を超える生成・Component追加・保存復元・Prefab操作、分割を跨ぐ削除、追加後の参照安定性、破棄後の世代再利用、無効な高 index の照会、Clear と move を確認する。C# の生成・検索・プールにも固定4096上限はなく、転送の byte 数と CLR/ABI の数値表現を検証する。batch packet の予算は論理件数の上限ではない。
