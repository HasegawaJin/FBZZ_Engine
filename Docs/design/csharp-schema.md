<!-- @file    csharp-schema.md -->
<!-- @brief   C# 設定を保存形式から分離する共通 recursive schema と snapshot。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-05 -->

# C# 共通 field schema

`ManagedFieldSchema` は CLR 型、保存メンバー、安定キー、表示 metadata、null 契約、範囲、列挙候補、作成時の値を定義する。Inspector、AI、DataAsset、保存 codec と候補検証はこの情報を共有する。編集 transaction の受入条件は [csharp-settings-transactions.md](csharp-settings-transactions.md) に置く。

## 値型と保存

値は bool、各幅の整数、float/double、decimal、char、string、すべての整数基底 enum、Vector2/3/4、Quaternion、GameObject、ComponentRef<T>、DataAssetRef<T>、Material/Texture のアセット参照、対応する一次元配列・List と Serializable class/struct を扱う。数値の wire 表現は次節の幅別契約に従う。すべての保存 float/double 成分は有限値を要求する。Flags は宣言されたビットの任意の組み合わせを許可し、未知のビットを拒否する。候補は名前と実際の整数を対で渡す。

node の `valueKind` は編集用の意味、`type` は CLR 宣言型、`mayBeNull` は保存可能な形を示す。RequiredReference は実行前の生存検証であり、編集中の null 許可を変更しない。strings、配列・List、struct は nonnull。`visible=false` でも値を保存・検証する。

各保存メンバーは `name`、`previousKeys`、`label/help/group/unit/visible`、`minimum/maximum`、`requiredReference/requiredAt` と再帰 `value` を持つ。class/struct の `fields`、collection の `element` も同じ node 形を使う。再帰型は `recursive=true` と CLR 型名で循環を止める。値の循環・多態 object・独自 IEnumerable は扱わない。

`defaultValue` は null、空配列、空 string、scalar の有効な型既定値。Serializable class/struct の `createValue` は型と保存メンバーだけから作る constructor-free な JSON で、範囲外の数値は有効な範囲既定値を使う。nullable な再帰 field は null とする。metadata の発見・保存・snapshot 読取では constructor と property getter を呼ばない。隠れたメンバーも createValue に含める。既存 Script の宣言 initializer は instance の snapshot に保持する。実 constructor の初期値を得る場合は明示的な作成 authoring 操作を用い、metadata の発見へ混ぜない。

Native の ManagedValueKind と IReflector は既存 TOML 保存の境界に閉じる。Vector2/4・uint とその collection はこの境界では STRUCTURED envelope を使う。Inspector はその wire kind を widget 選択に使わず、schema と JSON snapshot を直接描画する。Roslyn の ScriptFieldContract は同じ保存型集合と属性制約を診断し、compiler の無関係な警告を抑制しない。

### Primitive の幅と正確な表現

sbyte/byte、short/ushort、int/uint、long/ulong、有限 float/double、decimal、char を scalar と一次元配列・List・nested 値で同じ codec と schema に従って扱う。任意の CLR 型を暗黙に string 化して対応したことにはしない。

schema は整数の bit 幅と signedness、実際の float 幅を保持する。int32 以下と uint32 は通常の整数を使い、int64・uint64 は schema 宣言に対応する canonical decimal string / tagged value を JSON・JavaScript・保存・snapshot・編集・Undo の全経路で保持する。uint64 を double に通さず、bool を数値として受けない。Inspector も対応幅の整数 control を使い、int32/float32 の通常操作を維持する。

double は有限の binary64 として roundtrip し、float32 に狭めない。decimal は不変文化の十進表現で正確に保持し、binary float へ変換しない。列挙候補・Flags bitmask・範囲 metadata も宣言された整数幅または decimal に対応する表現を使う。`ValueRangeAttribute` は float / double / int / uint / long / ulong の境界 overload を持ち、decimal 境界は CLR attribute 引数に decimal を使えないため invariant string を受ける。string 境界は不変文化で解析し、field の宣言型と整数幅に対して検証する。

char は UInt16 の UTF-16 code unit として0..65535を正確に保持し、surrogate code unit 自体も許可する。Unicode 正規化や置換を行わず、単独 char を通常の JSON string として処理しない。string の不正 UTF-16 拒否とは別の契約である。範囲外の narrowing、整数幅に合わない enum/Flags、非有限 float は具体的な診断で拒否する。

## Snapshot と実体参照

`GET_SETTINGS_SNAPSHOT` は callback を実行せず、一度の読取で `settings`、`observations`、`diagnostics` と `references` を返す。`references` は root の保存キーからの RFC 6901 path をキーに、参照種別と宣言型、GUID、objectId、sceneLifetime を保持する。native Component は kind と row の birth、Script は savedScriptId と instanceId・generation・birth も照合する。opaque な identity は decimal string として返し、JavaScript や JSON float の丸めを通さない。GUID の再解決は行わない。

`generation/instanceId` と `revision` が対象を照合する。revision は settings とその exact reference identity の決定的な内容 hash。観測だけの変化は設定の revision を変えない。別の変更と競合する UI draft は確定を拒否して診断し、古い値で上書きしない。表示 page の失効と未確定 draft の処理を分ける。

`GET_SETTINGS_REVISION` は Host 経由の確定・復元に伴う軽量な counter を返す。Inspector は exact instance・世代・counter と schema を照合し、表示に必要な bounded live page を取得する。表示だけのために全 snapshot を作らず、collection は 64 要素ごとに描画する。非表示ページの値は切り捨てない。live page の sample は要求単位で、複数ページの同時点を保証しない。編集開始時に revision を取り、commit 時は counter だけに依存せず live snapshot の内容 hash を照合する。

collection の移動は値と path 別 token の双方へ適用する。明示的な再接続と移動を区別し、同じ GUID の失効 token を新しい実体へ再解決しない。transaction の `moves: [{fromPath,toPath}]` は snapshot の元 leaf と新 leaf を明示する。Inspector は複数回の移動・削除でも元 path を保持し、AI の remove/reorder 操作も同じ token 移動へ変換する。

## 候補 transaction

`PREPARE_SETTINGS` は `targets:[{instanceId,expectedRevision,fields,rebind,moves,initial}]` を受け取り、数値 token だけ返す。`GET_PREPARED_SETTINGS` は token の変更前後 snapshot、変更 path、参照 table を読み取る。JSON 出力の容量不足で prepare callback を再実行しない。既存の候補は Script と保存メンバーの graph を constructor-free に複製し、非設定の instance field は CLR 既定値となる。`Reset` と `OnValidate` は設定・定数・純粋な補助演算だけに依存する。Engine scope は明示的に解除し、FBZZ2010 は直接の runtime cache、可変 static、Engine API、I/O 依存を診断する。任意の .NET helper による副作用を隔離する sandbox ではない。

`COMMIT_SETTINGS` は全対象の snapshot と最初の counter を再照合した後、一度だけ補正結果を代入する。`RESTORE_SETTINGS` は補正が変更した root field とその exact token だけを照合・復元し、無関係の後続編集は維持する。補正結果と Undo snapshot の可変 graph は生個体と共有しない。Undo/Redo では `Reset/OnValidate` を再実行しない。追加の redo は通常の再追加として新 instance を構築し、同じ owner/type/scene と新候補 revision を照合して保持済み補正結果を移す。

準備済み transaction token は256件、受信中の大きな編集 session は32件を保持資源の契約とする。設定値全体に合計8 MiBや単一 JSON の大きさを対応上限として適用しない。大きな不変 snapshot と preview は所有する退避領域へ移し、wire 応答は必要容量を診断してページ単位で読み、編集入力は未公開 session へチャンク単位で受信する。一回の packet、常駐メモリ・退避容量、保持 handle と入れ子深さは設定の総件数と別の契約である。明示 `DISCARD_SETTINGS`、plan の破棄、既存個体の削除、世代廃棄で解放する。追加 undo 後は値の receipt だけを保持し、破棄済み instance を検索可能な状態で保持しない。Play 調整・採用と DataAsset の設定も [transaction の受入条件](csharp-settings-transactions.md) に従う。

`ComponentRef<T>` は GameObject GUID に component/script の保存 identity と宣言型を加え、instance token と保存 identity を分ける。`TryGet` は保持した個体だけを照合し、明示 `Resolve` と authoring の再選択で再接続する。`DataAssetRef<T>` は GUID と宣言型を使い、DataAsset の作成・編集・保存・Undo は共通 schema/snapshot と候補 transaction を通る。portable な DataAsset の内部には Scene 参照を保存しない。

## 検証

`InspectorSchemaTests` は sparse/negative enum、Flags の未命名組み合わせと未知ビット、metadata、nullable class/nonnullable struct、list の順序保存、Vector2/4、uint 全域、same-GUID の異なる token と revision を確認する。UI と AI が同じ schema/value を使うこと、作成・編集・保存・Undo が同じ検証を通ることを後続 transaction の統合検証で確認する。
