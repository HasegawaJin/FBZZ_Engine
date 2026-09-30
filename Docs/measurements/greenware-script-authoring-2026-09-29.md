# GreenWare：最新 Script API の適用と記述量

2026-09-29。ゲームで使用している実スクリプトを変更し、変更前の Git 履歴と比較した。

本記録は観測APIへの移行完了時点を固定している。同日後続の初期化順序・必須設定・音声APIの変更は含めない。JSONの afterSnapshot に示す7ファイルは、本記録の元のSHA-256と一致する内容を保存したもの。

## 今回の移行：状態の表示を FBZZ_OBSERVE へ

基準は `534b075e`。体力・スタミナ・戦果・ボス状態を扱う主要7スクリプトを対象に、実状態のコピーだけを持つ表示項目を移行した。
`FBZZ_FIELD_READ_ONLY` 自体は有効な API であり、保存が必要な読み取り専用設定まで一律に置き換えるものではない。

| 指標 | 変更前 | 変更後 |
|---|---:|---:|
| 対象スクリプト | 7 | 7 |
| 表示用に複製していたメンバー | 21 | 0 |
| 上記メンバーへ同期する代入文 | 30 | 0 |
| 表示への同期だけを行う OnUpdate | 2 | 0 |
| 7ファイルの非空・非コメント行 | 1,013 | 977 |

非空・非コメント行は36行、約3.6%減った。テスト・共通APIの実装はこの値に含めない。
代入文は同じ行に書いたものも1文ずつ数えるため、削除行数とは一致しない。

| ファイル | 移行した項目 | 削除した同期代入 |
|---|---:|---:|
| Combat/EnemyHealthComponent.hpp | 1 | 2 |
| Player/PlayerHealthComponent.hpp | 1 | 1 |
| Player/PlayerBreathComponent.hpp | 2 | 4 |
| Game/CombatManagerComponent.hpp | 10 | 15 |
| Combat/BossCoreComponent.hpp | 1 | 2 |
| Combat/SerpentBossComponent.hpp | 3 | 3 |
| Combat/Boss03BossComponent.hpp | 3 | 3 |

### 書く手順がどう変わるか

`PlayerBreathComponent` の `debugBreath` は、以前は宣言・ResetBreath・OnUpdate の3箇所を対応させていた。
移行後は次の1宣言が実状態を直接読み、SpendDodge / DrainGuard / Refill に表示同期を追加する必要がない。

```cpp
FBZZ_OBSERVE(float, debugBreath, m_breath, "息")
```

従来は `Refill()` で実状態を変えても表示用コピーは次の `OnUpdate()` まで変わらなかった。
移行後は、無効状態でも観測時点の値を返す。既存の表示キーを維持し、観測値は保存・復元の対象から外す。
旧シーンに残る `debugHealth` などの値は読み飛ばし、`maxHealth` などの設定値は従来どおり読む。
未開始で依存先がないボスの節数・翼数は0を返す（以前の未更新の既定値28/6とは異なる）。

## 既存の Player モジュール移行も実履歴で集計

こちらは今回新たに実装した変更ではない。`75edd80d` → `ae3d1ef6` に含まれる変更を集計した。

| PlayerComponent 内の個別中継 | 変更前の呼び出し数 |
|---|---:|
| Reflect | 16 |
| AdoptContext | 16 |
| OnStart | 16 |
| Update | 13 |
| LateUpdate | 3 |
| FixedUpdate | 1 |
| OnDestroy | 8 |
| OnDrawGizmos | 1 |
| 合計 | 74 |

移行後は16件の `ScriptModule{...}` 登録に集約。モジュールを個別に列挙する記述は74→16（約78.4%減）。
別に、各フェーズから共通管理を呼ぶ9箇所と `FBZZ_REFLECT_MODULES` 1宣言が残る。
これらも含めた中継・登録の記述数は74→26。エンジン側の共通管理コードを含む総コード量の削減率ではない。
PlayerComponent 全体の非空・非コメント行は412→351（61行、約14.8%減）。

更新・保存・開始・終了が必要な内部モジュールを追加する場合、以前は各中継へ追記していた。
移行後はメンバー宣言と登録一覧への追加で対象フェーズを指定する。ゲーム固有の依存注入と更新順の設計は引き続き必要。
親自身のフィールドは `FBZZ_FIELD` の宣言から反射されるため、手書き Reflect への追加漏れも防げる。

## 検証

- Development の `FBZZTestsEngineAuto` をビルドし、変更対象を含む実ヘッダーをコンパイル。エラー・警告0。
- 新規 `GreenWareScriptObservationTest` 7件と、既存の ScriptModules / Player / Boss03Counter / BossBreak / ParryRush を合わせた32件が成功。
- 未開始・無効状態、被弾とリセット、スタミナ消費・息切れ・回復、戦果カウンター、依存先のないボス、旧保存値の読み飛ばし、Player内部モジュールの保存と観測を確認。
- `AgentLint --changed` はエラー・警告0。
- GreenWare の Scripts.dll の再構築、実プレイ・描画・音声、他の開発者の作業時間は今回測定していない。

実行入口：`Tools/AgentBuild.ps1 check` → `build FBZZTestsEngineAuto` → `test -Filter 'GreenWareScriptObservationTest|PlayerScriptModulesTest|ScriptModulesTest|Boss03CounterTest|BossBreakTest|ParryRushTest'`。
最終テストログ：`build/agent/test-20260929-124847-37380.log`。

## 集計方法と限界

- 集計値・対象キー・移行後ソースのSHA-256は同名のJSONに保存。ハッシュは改行をLFに揃えたUTF-8に対する値。
- 今回の移行前は `git show 534b075e:<path>`、Player の既存移行は上記2コミットの同一ファイルを比較。
- 行数は空行と行頭の `//` コメントを除外。括弧だけの行、include、宣言は含める。
- 開発時間の短縮率やゲーム全体の性能向上は、この集計から算出できない。
- 7本以外の表示用フィールド、イベント履歴などを独自に保持する値、SerpentAiの既存の手動内部モジュールは今回の対象外。

「表示に関する変更を1宣言に集約できた」「内部モジュールの中継漏れを減らした」という、保守手順の改善として説明する。
