# User Script の観測 API

2026-09-26。初期実装は Script の列挙・値取得・設定と観測の分離（設計案の 1A / 1B）。
既存の C++20、ScriptProxy、IReflector、ScriptFactory、DLL 再構築方式を維持する。

## 書き方

```cpp
FBZZ_FIELD_RANGE_INT(int, maxHealth, 100, "最大HP", 1, 100000)
FBZZ_OBSERVE(int, currentHealth, m_currentHealth, "現在HP")
FBZZ_OBSERVE(bool, alive, IsAlive(), "生存")
```

`FBZZ_OBSERVE(Type, Key, Expression, Display)` は const の読み取り関数を生成する。
値を同期するためのメンバーは生成しない。Key は JSON の安定したキー、Display は Inspector の表示名。
Expression は副作用がなく、未開始・disabled 状態でも安全で、同期ロードや I/O を行わない式にする。

対応型は int / float / bool / std::string / Vector2 / Vector3 / Vector4 / Quaternion。
他の型はコンパイル時に拒否する。式にカンマがある場合は括弧で囲む。

そのままコンパイルされる例は
[HealthComponent.hpp](../../Projects/Tests/Fixtures/UserScripts/HealthComponent.hpp)。
被弾前・被弾後・死亡後の値を ScriptInspectionTest が検証する。
GreenWare の PlayerHealthComponent は既存キー `debugHealth` を維持し、`m_health` を直接観測する。
最大 HP の所有者は従来どおり PlayerTuning。

## 保存と観測

| 宣言 | 保存・Undo | Inspector | Script API |
|---|---|---|---|
| FBZZ_FIELD など | 保存する | 宣言に従って編集 | 読める |
| FBZZ_FIELD_READ_ONLY | 保存する | 編集不可 | 読める。JSON 編集は拒否 |
| FBZZ_OBSERVE | 保存しない | 読み取り専用 | 読める |
| 通常のメンバー | 保存しない | 非公開 | 非公開 |

IReflector の末尾に `BeginObservation(name, type)` を追加した。
既定値は false。TOML・Undo・JSON の保存用収集は getter を評価しない。
JsonCatalogReflector は型だけを記録して false を返す。
ImGuiReflector と、観測を有効にした JsonReadReflector は true を返す。
値は一度コピーし、既存の型付き Field に読み取り専用として渡す。
旧 FBZZ_COMPUTED は残すが、新規の観測には FBZZ_OBSERVE を使う。

SnapshotComponents は observations 引数を既定 false とし、削除 Undo への観測値混入を防ぐ。
node.components のみ true を渡す。Script の一覧と値は保存用の SnapshotComponents に追加しない。

## 読み取り API

1. `node_get_components({id})` で既存の components と、新しい scripts 配列を得る。
2. scripts の scriptId を `script_inspect({id, scriptId})` に渡す。
3. 型を調べる場合は `script_catalog({type?})` を使う。

Bus 名はそれぞれ node.components / script.inspect / script.catalog。MCP の read 権限で使用できる。
取得処理は Editor の ScriptHandlers.cpp に集約し、Engine に JSON / MCP 依存は持たせない。

Script 一覧は scriptId、index、type、status、enabled、activeInHierarchy、awoken、started を返す。
status は ready / notStarted / disabled / missing / faulted。
index は表示用であり、取得対象の選択には使えない。同型が複数あっても個体を区別する。

取得応答例（説明用）:

```json
{
  "id": "node-guid",
  "scriptId": "entry-uuid/instance-uuid",
  "type": "HealthComponent",
  "status": "ready",
  "enabled": true,
  "activeInHierarchy": true,
  "awoken": true,
  "started": true,
  "frameIndex": 120,
  "samplePhase": "request",
  "fields": { "maxHealth": 100, "currentHealth": 70, "alive": true },
  "schema": [
    {
      "name": "currentHealth", "displayName": "現在HP", "type": "int",
      "role": "observation", "stored": false, "readOnly": true, "writable": false
    }
  ]
}
```

schema はフィールド配列。例では一部省略した。実体の schema には default を返さず、
カタログの設定値にだけ default を返す。観測値に default は作らない。
readOnly は宣言、writable はこの API からの変更可否。初期版の Script API は全フィールド writable=false。
入れ子の fields / elementFields でも親のキーと表示名を保持する。

catalog の応答は types 配列。各要素に type、fields、requiredComponents、optionalComponents を返す。
既存 factory で型の初期値を構築するが、ライフサイクルと観測 getter は実行しない。
constructor と独自 Reflect は未アタッチ状態でも安全である必要がある。

## 識別子と状態

scriptId は Entry UUID と Script 実体 UUID を組み合わせた不透明な値。
別の sceneEpoch 引数を要求せず、同じ目的を個体の生存期間で保証する。
Entry の移動・vector 再確保・並べ替えでは維持する。
削除・複製・Scene 復元・同じ Entry への実体の再生成では変わる。
保存しない。テストシナリオへ固定値として埋め込まず、その都度一覧から取得する。

Missing の場合は live fields を返さず、保持中の設定を
`savedValues: {format: "toml", data: "..."}` で返す。0 や空の現在値へ置換しない。
disabled でも getter を評価できる。faulted の実体、DLL 再読込・Scene 復元中には getter を呼ばない。

| エラー | 意味 |
|---|---|
| NO_SCENE / NODE_NOT_FOUND | Scene または Node が無い |
| INVALID_ARGUMENT | id / scriptId が無い |
| STALE_SCRIPT_ID | 対象個体が無い。一覧を再取得する |
| SCRIPT_BUSY | 再読込・復元中。完了後に再取得する |
| SCRIPT_FAULTED / SCRIPT_SCHEMA_FAILED | 実体または型の反射処理に失敗した |
| SCRIPT_TYPE_NOT_FOUND | 型が登録されていない |
| DUPLICATE_SCRIPT_FIELD | 公開キーが衝突している |
| NON_FINITE_SCRIPT_VALUE | NaN / Infinity がある |

重複キーや非有限値は応答全体をエラーにして、不正な部分スナップショットを正常値として渡さない。
実行中のカスタム反射がアクセス違反を起こした場合は、既存の Script 障害処理で停止し Console に記録する。

## 参照と非対応型

EntityID / EntityRef / FBZZ_REF は `{id, expectedType, status}` を返す。
id は Node GUID または null。EntityID の index / generation は Script API に出さない。
状態は none / resolved / missing / typeMismatch / ambiguous。
型付き参照は Component の有無、Script の継承・インターフェース一致を検査する。
同型 Script が複数ある参照は ambiguous とする。参照が複数個体のどれを指すかは保証しない。
型付き参照リストにも同じ判定を使う。

KeyCode は整数として公開する。アセットは既存の GUID / 解決パスを返し、ロードは要求しない。
ParticleCurve / ParticleGradient は初期版では `{status: "unsupported", type: "..."}`、
schema では supported=false と明示する。

## ABI・適用・今後の段階

Reflection ABI 世代を 4 から 5 に上げた。Script のサイズに加え、
vector 本体から見えない ScriptEntry の sizeof / alignof も ABI 署名へ含める。
新しい Editor / Engine と、同じ SDK で再ビルドした Script DLL を組み合わせる必要がある。
古い DLL を混在させたまま新しい観測 API は使えない。

初期版は既存のメインスレッド上の要求処理で値をコピーする。
frameIndex は要求処理時点のフレーム番号であり、フレーム完了を意味しない。
Playtest の完了フレームに合わせた assert 接続（1C）、Edit モードの Script 設定変更と Undo（2）、
複合 ScriptModule の依存関係の自動注入（3）、リロードを越える実行状態の保持は別段階。
観測 getter に自動 setter を作って実行状態を復元することはしない。

2026-09-28 に、明示的な登録一覧から内部 Script の通知と反射を中継する
[ScriptModules](script-modules.md) を追加した。通常のヘルパークラスは引き続き登録・アタッチ不要。
ゲーム固有の依存関係と更新順は親が指定する。

検証は ScriptInspectionTest、EditorBusDispatcherTest、BusContractParityTest と EditorMcp のテストを使う。
