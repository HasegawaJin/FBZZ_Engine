<!-- @file    ai-script-workflow.md -->
<!-- @brief   C# の宣言から AI の設定・参照接続・保存・動作確認へつなぐ制作契約。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-04 -->
# AI が作成・設定できる Script の制作経路

- 状態: 2026-10-05、[制作・実行経路の完成](csharp-completion.md)の実装と受入検証を完了。最新の結果は [Managed の検証記録](../../Projects/Managed/README.md)に置く。日付付きの段階 1 / 2 の記録は過去の受入範囲として残す。
- 作者の言語・参照契約: [C# 作者向け API](csharp-authoring.md)。
- 保存・実行・コード世代: [C# バックエンド](csharp-scripting.md)。
- 既存の検証入口: [AI の検証ループ](ai-verification-loop.md)。

目的は、AI がゲームコードを書き、設定と参照を接続し、保存と動作確認まで完了できることとする。作者は普通の C# と公開する設定を宣言する。Editor はその宣言から Inspector、AI の catalog、保存 schema、共通の設定 Op を作る。別の手書き JSON schema や、AI だけの編集経路を作者へ要求しない。

## Script の基本形

通常の public 設定には属性を要求しない。初期値を普通のフィールド初期化子へ一度だけ書き、自己の移動は Transform を使う。namespace、public / sealed の型宣言、Context は不要とし、変数には型を明示し、参照型に nullable の記号を付けない。未設定は通常の if で扱う。lifecycle は protected override を使い、基底のcallback宣言とAnalyzerで名前・署名の誤りをビルド時に検出する。[SDKだけで開始する導入手順](../csharp-start.md) と雛形は同じ配布物に含める。

~~~csharp
/// @file    FollowTargetComponent.cs
/// @brief   指定した対象へ近づき、停止距離を保つ。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using Fbzz;

class FollowTargetComponent : Script
{
    public GameObject target;
    public float moveSpeed = 4f;
    public float stopDistance = 1f;

    protected override void Update()
    {
        if (target is null || !target.IsAlive)
            return;

        Transform.MoveTowardsWorld(
            target.Transform.WorldPosition,
            moveSpeed * Time.DeltaSeconds,
            stopDistance);
    }
}
~~~

target の GameObject 参照設定と MoveTowardsWorld は現在の SDK で利用できる。RigidBody の C# facade も実装済みで、body の変更は FixedUpdate の契約に従う。上の例は通常 Transform を動かす処理であり、物理 body の移動にそのまま使わない。移動・時間・停止距離は [共通 helper](csharp-authoring.md#よく使う計算を-sdk-の操作にまとめる)、正確な物理操作の署名と Phase は script.api.catalog に従う。

巡回地点も属性なしの配列でよい。AI は指定 root の階層から地点を選び、順番付き GUID 配列を一回の設定計画で保存する。各地点をクリックして接続する必要をなくす。

~~~csharp
public GameObject[] patrolPoints = [];
~~~

設定を他の C# コードから直接変更させたくない場合だけ private + Setting にする。観測は属性なしの Debug inspect、他 Script / Component の操作は GetComponent<T>() と通常のメソッド。Attached / Watch / SceneObject(s) や操作用 lambda を通常制作の必須条件にしない。

追跡が任意なら GameObject target; の未設定を if で扱い、空配列と合わせて追跡 / 巡回 / 停止を選ぶ。RequiredReference / ValueRange は要件がある対象だけへ追加する。Settings / State の別クラスを必須にせず、規模が増えたら enum / switch、ヘルパー、DataAsset へ分ける。var を使わず、foreach / out / lambda の引数にも型を書く。

## 一つの宣言から返す情報

現行 script.catalog / script.inspect は保存用の type key、表示用の CLR クラス名 displayName、managedGeneration、宣言元 sources、設定 schema と managedMetadata を返す。GameObject 参照は未設定と欠損を区別し、配列の順序・重複・null slot を保つ。C# のビルド要求ごとの状態応答は build.run / build.status から取得する。script.api.catalog は公開済み ScriptApi の型・メソッド・property・定数と bridgeSchema を返し、単位・座標・Phase・失敗条件を確認できる。ゲーム DLL の任意の public メソッド一覧を返す API ではなく、その署名はゲームのソースと IDE で確認する。

| 情報 | 内容と利用先 |
|---|---|
| 型 | script.catalog に登録された Script の安定した型 GUID、CLR 名、ソース位置 |
| 設定 | public 自動設定 / private + Setting の区別、field key、型、初期値、現在値、任意 metadata |
| 制約 | ValueRange、必須参照、単体 / 配列、対応した保存型、エラーのソース位置 |
| 参照 | 単体 / 配列の GUID、未設定・欠損、自己 GO、root を指定した選択元。名前から相手を推測しない |
| 実行 | 宣言された lifecycle、要求 Component、faulted / requirementsBlocked |
| SDK の公開操作 | script.api.catalog のメソッド / property の署名、引数・戻り値の型、説明、実行契約と bridgeSchema |
| 観測 | 属性なしの Debug field と読み取り可否。private も観測でき、getter を自動実行しない |
| 制作状態 | ソース fingerprint、適用済み世代、Missing / 読込失敗、保存済み / 未保存 |

初期値は候補個体の初期化値を対応 codec で取得し、Inspector と AI に同じ値を返す。独立した Default 属性に重複記述しない。初期化中の Engine 操作は禁止し、例外・非対応値・制約違反を候補の診断へ返す。catalog の初期値と inspect の現在値を別欄にする。

managed の script.inspect は faulted 個体でも cached schema と Host の field 読み取りだけを使い、getter・Reflect・lifecycle を実行しない。正常 field は fields に返し、非有限値、null の配列、不正な文字列、未対応型など読めない field は null と fieldDiagnostics の key / code / message / 任意 elementIndex で返す。observationStatus は complete または partial。fault.message は個体の最初の停止原因で、他個体のエラーや後続の field 読み取りで上書きしない。

参照の観測は現在の寿命 ID を照合し、保存 GUID が同じ対象を作り直しても失効を解除しない。配列・List・文字列の総容量を JSON 応答の容量で制限しない。通常取得は少量の summary と deferred path、詳細は path・start・count を指定したページで読み、同時点の複数ページが必要なら不変の observation handle を使う。wire 容量、保持 handle、常駐メモリ・退避領域の予算は設定値全体と分離する。毎フレーム処理には Objects の再利用バッファや GetComponents の呼び出し側バッファを使う。GetComponents(buffer) の不足時は Required を確認し、Written=0、出力未変更の契約に従う。呼び出し側の確保済み容量が実際の Scene 件数より大きくても有効である。[観測と容量の作者向け契約](csharp-authoring.md)

型catalogでは保存型と数値等の初期値を検証し、RequiredReferenceの未接続を型の登録拒否にしない。schemaは入れ子のleafにもrequiredReference / requiredAt: Runtimeを載せる。Authoringの作成・設定preview/applyとSnapshotの保存・Undoでは未接続状態を扱え、RuntimeのPlay開始・reloadで生存する必須単体、非空かつ全要素が生存する必須collectionを検証する。codec・型・finite・rangeは段階によらず一括検証し、失敗をleaf pathで知らせる。privateの開始コードで取得する外部参照を設定の必須条件にしない。

Setting の metadata は制作時の意味を伝える。Unit は説明であり、自動換算や物理の変更を行わない。Visible = false でも AI の schema から隠さない。フィールド名を変数用途の推測や自動接続の条件にしない。

現在の共通 Op は code と message で曖昧な型・対象、field、制約違反と stale を診断する。ビルド診断はソース位置を持つ。AI は現在の応答と preview の対象・差分を使い、失敗した範囲を確認する。すべての Op が owner GUID / scriptId / field key / 期待条件を個別欄に返すとは扱わない。

SDK の公開メソッド catalog は読み取りであり、public を Editor / AI の任意実行許可に変換しない。ゲームの相互操作は [GetComponent と普通の public メソッド](csharp-authoring.md#他の-script-を型付きで操作する)、制作時の書き込みは共通 Configure、動作の表明は既存 Playtest を使う。

SDK は通常の csproj、補完、短い使用例、ソース位置付きの診断を提供する。ゲーム用 template の Nullable = warnings、記号なしの参照型、通常の if による null guard をサンプルと AI の既定へ揃える。Host / ScriptApi の nullable metadata と catalog の mayBeNull は保持し、設定の未接続・欠損・必須条件も catalog / inspect へ返す。新しい Script の作成時にプロジェクト参加と .meta の扱いを制作入口へ集約し、人間・AIに登録リストの編集を要求しない。型とメソッドの成功版だけを公開し、古い code generation の署名を現行 API と扱わない。

## AI の作業手順

1. SDK と現行 Scene を読む。既存の GO、Prefab、Script、アセット GUID を再利用し、必要な型だけを書く。
2. 既存のビルド入口から候補を作り、要求したソース版の適用完了を待つ。コンパイル成功だけで新 schema を使わない。
3. 必要な GO / Prefab 個体を既存 Op で用意する。Scene 容量は子孫も含めて確認する。
4. 適用済み catalog を読み、Script 追加、数値設定、単体参照、配列を共通の設定計画へまとめる。
5. preview の候補・差分・診断を読み、解決できた計画を apply する。通常の制作で人のクリック承認を必須にしない。
6. 設定の読戻しと要求検証を行い、既存ファイルへ差分保存する。Playtest の入力・状態・結果でゲームの動作を確認する。

Editor の見た目を操作しないと設定できない機能を残さず、Inspector と AI から同じ Op を呼ぶ。Scene の新規生成、ソースのコンパイル、Script 設定、ファイル保存、任意の callback を一つの不可分な transaction として約束する範囲にはしない。

### ビルド完了と適用完了を区別する

build.run の target="managed" で共通 Op script.managed_build を呼び、非同期の managedRequestId と sourceFingerprint を返す。UI / パレット / editor.op.invoke も同じ Op を使う。コンパイル中、他ビルドの実行中、Play 中も要求をキューへ置ける。同じ版の進行中要求は同じ ID へ合流する。

build.status (MCP 名 build_get_status) に managedRequestId を渡し、managed.request.terminal=true を待ってから managed.request.state="applied" を確認する。現在の要求は queued / compiling / compiled / applying / applied / failed / superseded を区別する。Edit / Play とも候補を検証して適用し、Play 中という理由だけで Stop まで保留しない。古い catalog のまま新設定を送らない。GC 回収待ちは compiled と errorCode="COLLECTION_PENDING" で返す。新しいソース版が要求された旧要求は superseded の終端状態となり、遅れて完了したコンパイラー応答でも復活しない。failed / superseded は適用待ちを終了させ、必要なら現在版を再要求する。compiled / applying は短い処理ではポーリング間に通過するため、全状態を一度ずつ観測できるとは約束しない。deferred は段階 1 / 2 の Play 保留で使った互換状態名で、現在の Play 適用条件ではない。

managed は available / busy / availabilityDiagnostic / appliedGeneration / latest / history を持ち、要求 ID を指定した場合は request も持つ。各要求には sourceFingerprint / effectiveSourceFingerprint / candidateId / previousGeneration / appliedGeneration / automatic / state / terminal / exitCode / errorCode / message を返す。latest と request はコンパイラーの diagnostics (severity / file / line / column / code / message) と最大 64 KiB の log を含み、history は要約だけを返す。候補拒否の診断は message に保持する。自動監視・起動時のビルドも同じ履歴へ記録し、直近 32 要求を保持する。未知・期限切れ・別プロジェクトの ID は MANAGED_REQUEST_NOT_FOUND とし、最新要求へ読み替えない。要求 ID は Editor プロセス内で一意で、再起動をまたぐ保存用 ID ではない。

sourceFingerprint は要求時の .csproj / Assets 以下の .cs / .cs.meta のパスと内容を含む既存の 64-bit FNV 版識別で、16 桁の hex 文字列として返す。importer 自身が追加した .cs.meta は effectiveSourceFingerprint へ反映し、要求の sourceFingerprint は変えない。暗号学的な内容証明ではない。candidateId はビルド出力ディレクトリの識別子であり、CoreCLR 世代番号ではない。実際に適用された native runtime 世代は appliedGeneration に 10 進文字列で返す。要求の appliedGeneration="0" は未適用、managed.appliedGeneration は現在利用中の世代であり、候補失敗や GC 回収待ちで旧世代を上書きしない。

トップレベルの building / latest / history / hasActiveFailure は他の Engine build 応答を保つ。C# の完了判定には managed.request を使う。共有ビルド入口の BUSY はコンパイル失敗ではなく queued / BUILD_BUSY として同じ要求を再試行する。Play 更新は候補検証・復元が成功したときに commit し、commit 前の失敗では旧個体を維持する。ビルド成功だけでは既存 Script の faulted / requirementsBlocked を解除しない。

停止理由は共通Query `script.status` とInspectorから取得し、原因修正後にAction `script.resume` でRuntime必須参照を再検証して再開する。`node` は対象GOのGUID、`scriptId` は既存inspect/configure応答の値をそのまま使い、`enabled` は省略時true。成功済みAwake / Startを再実行せず、失敗した開始段階/Connectは次tickで再試行する。GO非activeやenabled=falseのConnectは延期し、停止中の仕事・イベントはreplayしない。診断はUTF-8境界で8KiB以内に制限され、切り詰めた場合は `[diagnostic truncated]` を返す。

## script.configure の共通 Op

段階 2 の登録名は script.configure.preview と script.configure.apply。通常の Op 呼出しで利用し、preview の settings に objects、script、fields をまとめて渡す。Edit 専用で、Play 中の書き込みは受け付けない。

~~~json
{
  "settings": {
    "objects": { "scope": "scene", "tag": "Enemy" },
    "script": "FollowTargetComponent",
    "fields": { "target": "Player", "moveSpeed": 4.0 }
  }
}
~~~

script は一意なクラス名、同名型がある場合は CLR 完全名。objects はオブジェクト名、階層 path、selector、selector 配列を受ける。保存 GUID を普段の入力へ要求しない。mode の既定 configure は既存の単一一致を更新し、無ければ追加する。明示 add は新しいアタッチを作る。同型が複数あれば scriptIndex または現在の scriptId を指定する。

| selector | 範囲 |
|---|---|
| "Player" または {"name":"Player"} | Scene 内の単一名一致。複数なら診断 |
| "Enemy/Visual/Weapon" または {"path":["Enemy","Visual","Weapon"]} | 階層の各段で単一一致。名前に / がある場合は配列形式 |
| {"root":"$owner","name":"Weapon","scope":"descendants"} | 設定先個体の子孫だけ。owner 相対 root は field の参照 selector 内で使う |
| {"root":"PatrolRoute","scope":"children","tag":"PatrolPoint"} | 指定 root の直下を Hierarchy 順で全件 |
| scope: scene / self / children / descendants | scene 以外は root 必須。includeInactive の既定は true |

単体参照は一致一個を要求する。配列は明示順の selector 配列、または全件 selector を使える。GameObject field への null は未設定を表し、空配列は空として保存する。Editor が解決した名前は対象 GUID へ保存し、起動時に名前検索で置き換えない。

配列全体の指定は replace。差分は {"edit":"append","values":[...]}、{"edit":"remove","indices":[...]}、{"edit":"reorder","indices":[...]}、または明示 replace を使う。remove は添字で重複要素を区別し、reorder は全添字を重複なく一度ずつ指定する。

既存参照を含む JSON を送り直しても、同じ RFC6901 leaf path と同じ GUID の寿命tokenは保持する。同root内の別pathから移動したGUIDは、既存ObjectId / SceneLifetimeが一意なら再解決せずtokenを再利用する。既存rootにない新GUIDだけを新規に解決する。異なるtokenの同GUIDを移動すると曖昧な場合は、incoming同GUID leaf全件に再選択intentを指定するまで拒否する。失効参照を同じGUIDへ明示的に接続し直す時は、preview request の `rebindReferences: ["/settings/target"]` を指定する。参照 leaf 自体を指し、`$go` 内のpathは使わない。配列は `/settings/targets/0`、名前の `~` と `/` は `~0` と `~1` にescapeする。未知・重複・参照以外・nullのleafへのintentは全体を拒否する。同GUID列のJSONによるidentityだけのreorderは表現できず、位置別tokenを保持する。数値設定だけの変更は既存の失効状態を保ち、Snapshot のUndoは保存GUIDから復元する。

previewは候補constructorを実行するActionで、readonly Queryではない。Engine APIを許可せずSceneへ候補を公開しないが、任意C# constructorの外部I/Oを取り消す保証はない。成功時のplan tokenをapplyのplanに渡し、Authoring purposeで全設定のcodec・型・finite・rangeを検証後に適用して一件のUndoを積む。必須参照の未接続だけは編集途中として許し、Runtime開始・reload時に満たす。SnapshotでUndo・保存が未接続を復元できることと、Runtimeの開始条件を分ける。ビルド、ファイル保存、callbackの副作用まで同じtransactionに含めない。

preview の候補個体は検証後に解放し、plan は型キー・対象・値・差分と依存状態だけを保持する。未適用の plan で古い CLR 世代を固定しない。追加する個体は apply で constructor を再実行し、preview が確定した全設定を復元して再検証する。constructor の内部状態や外部副作用が preview と同一になる保証はない。既存個体の更新は同じ個体へ指定した設定差分だけを反映する。

準備済み plan は最大 8 件、適用済み receipt は最大 128 件を session 内で保持する。同一 token の再送は receipt を返し、追加や append を繰り返さない。Scene identity、適用世代、対象を決める階層・名前・タグ・active 状態、Script 個体、変更前値が変わったら STALE_PLAN で拒否し、新しい preview を要求する。期限外の token は黙って新規計画へ変えない。Undo 後の再送も勝手に再適用しない。

以下の表は将来の応答拡張も含む設計契約。現在は token と Scene / 設定 fingerprint を使い、独立した operation ID、request hash、schema hash の公開を完了済みとは扱わない。

| 計画の情報 | 契約 |
|---|---|
| operation ID と request hash | 同一の意図の再送を判定する |
| Scene identity / epoch | 別 Scene や復元前の実体に適用しない |
| 対象 | owner GO GUID。既存更新は inspect が返した正確な scriptId、追加は計画内の作成 key と型 GUID |
| コード | 適用済み catalog generation と schema hash |
| 差分 | public または Setting の設定 field key と値、配列操作、変更前の値または fingerprint |
| 選択元 | root GUID、直下 / 子孫 / Scene、タグ・名前セグメント、inactive 条件、実体階層 / Prefab 定義 |
| 確定候補 | 選択済み GUID と順序、件数、選択に使った階層・Prefab の依存 fingerprint |
| 必要 Component | 現状の不足診断。追加する場合は対応 Op の明示項目 |
| 結果 | commit receipt、変更差分、新 scriptId、保存状態、未解決・停止理由 |

同じ型の単一一致ならクラス名で選べる。複数アタッチは scriptIndex または現在の scriptId で区別し、曖昧な先頭を選ばない。現行 scriptId は Entry と live Script 個体の識別から作られ、Scene 復元・Undo・再生成・コード世代の変更後は取り直す。GUID が同じ GO でも古い Script 個体へ書き込まない。

設定の書き込み先は schema の editable field、すなわち対応 public field または private + Setting とする。private の実行状態、Debug field、NonSerialized、非対応の public field は読み取り情報があっても書き込み先にしない。差分にない設定を初期値へ戻さず、Visible = false も同じ型・制約で編集できる。

複数対象へ同じ変更を行う入力は対象集合と共通差分で表せるようにする。各対象の weapon を接続する場合は「各 owner GUID を root にした同じ選択規則」を渡し、native 側で個体ごとの候補を確定する。preview には全対象の exact scriptId・参照結果・差分を保持し、一個でも未解決なら部分適用しない。共有 Prefab 定義の変更、Scene 個体の override、指定個体群への共通変更は変更先として区別し、暗黙に Prefab Apply しない。

### 追加と設定を公開前に完成させる

1. コード世代、対象、変更前値、選択候補、必要 Component と値制約を検証する。
2. preview は非公開候補で Reset・OnValidate と値を検証し、候補と完全な変更 snapshot を apply または破棄まで保持する。初回 apply は保持した候補を公開し、callback を再実行しない。Editor へは exact 個体・revision・変更件数の bounded metadata を渡し、値の詳細は範囲 query で読む。既存 Script は指定差分の候補表を作り、配列の元データを準備中に変更しない。
3. 初期化子・保存値・指定差分を区別し、GO 階層と Prefab の対応が完成した状態で計画の GUID を解決する。保存済みの null / 空配列を別の検索結果で埋めない。
4. Authoringのcodec・型・finite・rangeを全件検証し、失敗ならScriptの一部だけを追加・変更しない。必須参照の未接続は編集途中として保持し、Runtime開始・reloadでRequiredReferenceと必要Componentの要求を満たす。外部Sceneのruntime検索はこの設定検証で実行しない。
5. commit 直前に依存が変わっていないことを再検証し、対象変更をまとめて公開して一件の Undo を積む。
6. 公開後は既存の開始・有効化規則へ参加させる。callback の副作用を巻き戻す保証は付けない。

ユーザーコードの候補構築・復元中は読み取りを含む Engine API を拒否し、参照解決は Host が行う。既存 Script の設定更新は同じ個体の field を一括変更し、不要な再生成や Awake / Start の再実行をしない。処理中の Editor 観測・callback へ途中の値を公開しない。

現在の run_transaction は既存 Scene に対して先に command を構築し、後から順に実行する。追加済み Script を後続 builder が参照できるとは限らず、CompositeCommand は候補検証・失敗 rollback を提供しない。単に二つの command を包むだけでこの契約を満たしたとは扱わず、共通 Op 自体へ準備と commit を実装する。

要求された Component を不足時に勝手に追加しない。初版は必要な native Component を既存 Op で先に用意してから Script を接続する。共通計画内の明示追加は、対応する Component の候補準備・Undo が検証できたものだけへ拡張する。

### preview の一致を保つ

preview は機械と UI が読む確認データで、再検索後の候補への許可ではない。apply では、選択 root、候補の生存と所属、対象設定の変更前値、コード schema、必要な階層・Prefab 定義の状態を照合する。関連条件が変わったら stale plan を返し、元の計画を黙って作り直さない。

現行 UndoStack の revision は Push 時の更新であり、Undo / Redo と全 Scene の直接変更を覆う世代番号とは扱えない。新しい変更世代、または依存データの fingerprint を共通 Op で検証する。hierarchy の並べ替えも、巡回順を確定した計画の依存に含める。

### 再送で二重追加しない

段階 2 は同じ plan token の apply 再送へ記録した receipt を返す。保持件数は準備 8、receipt 128。独立した operation ID と request hash の公開は後続案。Script の二重アタッチや配列の二重 append を防ぐ。

apply はメインスレッドで commit と receipt 登録を完了してから応答する。非同期の pending 応答や operation ID 予約は現在の実装範囲に含めない。

receipt は「一度 commit した事実」と「現在も効果が有効か」を分ける。Undo / Scene 復元 / reload 後は current validity と再計画の必要を返し、昔の scriptId を現在の対象と扱わない。Undo された効果を再送で勝手に実行し直さず、新しい意図には新 ID と新 preview を使う。receipt の保持期間・session 境界を公開し、期限切れを新規要求へ黙って置換しない。

commit 時点の記録と現在の保存状態も別欄にする。設定の commit 後に保存が失敗・成功した結果を、過去の receipt だけから判断しない。

## 子・配列・Prefab の接続

| 作りたい設定 | 選択・保存方法 |
|---|---|
| Scene に置いた敵の追跡対象 | player の GUID を public target へ保存 |
| 敵の weapon | 指定 Enemy root の部分木だけで選び、GUID を保存 |
| 順番のある巡回地点 | 指定 root の Hierarchy 順、または明示順を配列へ Replace |
| 複数 Enemy の内部部位 | 各個体の root を別々に指定。全 Scene の Weapon タグで混ぜない |
| Prefab 共通の内部参照 | Prefab 定義内で設定 field の source GUID を保存し、生成時に instance GUID へ remap |
| 生成先 Scene の Player | private 参照へ普通の Start で検索、または配置計画で各個体の設定を明示 |

共有 Prefab の外部参照を生成先で取得したい場合は、開始コードを使える。

~~~csharp
GameObject m_target;

protected override void Start()
{
    Scene.TryFind("Player", out m_target);
}
~~~

private の参照は設定 schema へ含めず、外部 Scene GUID や null の保存 key を Prefab に作らない。Scene.TryFind は名前の一致一個を要求する通常 API で、見つからないときのゲーム動作は Update 等で分岐する。タグ検索は Scene.FindAll / ReadObjects と GameObject.FindDescendants / ReadDescendants の条件で指定できる。生成後に現れる対象は通常の明示処理で再取得する。

Play 差し替えで開始済み個体の Start は再実行しない。直接の mutable instance field の GameObject / Component 参照は public / private とも属性なしで再接続する。タグの現在状態から取り直す意図がある場合だけ AfterReload に検索処理を集約する。任意の helper / collection に埋めた参照を自動で再接続する保証は付けない。

保存した null / 空配列 / 欠損 GUID は設定として保ち、別の検索結果で埋めない。FieldKey / PreviousFieldKey の移行後に key が無い場合はコードの既定値を使う。タグ変更・対象の生成・破棄で保存配列を自動追跡しない。

配列編集は Replace / Append / Remove / Reorder を指定する。Remove / Reorder は添字を使い、重複参照も区別する。大量の選択は共通 Configure 内で全件を解決し、preview の表示量と保存する対象集合を区別する。専用の配列ページ取得 API を前提にせず、500 件の scene_find 上限や 128 commands の run_transaction 上限で接続内容を切り捨てない。

階層の深さで参照形式を変えず、解決した GUID を保存する。表示 path は説明の補助とし、改名で参照を切る ID にはしない。候補の単体一致が曖昧なら対象と原因を返し、先頭を選ばない。

## 保存と動作確認

設定後に inspect で実際の値・参照・配列順・要求状態を読み戻す。scene.save と Prefab / Asset の既存保存経路でディスクへ反映し、既存ファイルと .meta の GUID、未知の fields、Missing Script を保持する。生成用スクリプトで Scene 全体を作り直さない。

apply 成功と保存成功は別の結果にする。保存できなければ applied but unsaved として変更と理由を返す。Play の runtime 状態を authoring ファイルへ自動保存しない。保存・再読込の確認でも参照・配列が維持されることを受入条件とする。

Playtest は既存 scenario_run / scenario_status とシナリオを使う。「コンパイルできた」「例外が無い」だけで完成にしない。追跡例なら、固定した入力とフレーム進行で接近量・停止距離・動かない条件を検証する。巡回順、武器の個体所属、対象破棄後の停止、差し替え時の状態は必要なゲーム要求で検証する。見た目の要求は Game view と既存 compareImage を使い、失敗を消す目的の baseline 自動更新はしない。

| 制作経路の受入条件 | 確認する結果 |
|---|---|
| 追加と初期値・参照の接続 | 一度で完了し、一回の Undo。公開前失敗では Script / 設定の部分適用なし |
| ID 再送、Append 再送 | 二重追加なし。Undo 後の再送で勝手に効果を復活させない |
| Undo / Redo、Editor 側変更 | 古い計画を拒否し、計画し直せる診断 |
| ビルド失敗・予約・競合 | 要求したソース版の適用前に新 schema で書き込まない |
| Prefab 複数個体 | 内部参照が個体ごとに分離し、private の外部検索は生成先で行い、Play 差し替え時の保持・再検索も区別 |
| 配列 100 / 1000 件、深い階層 | 全件・順序・重複・欠損を保ち、表示上限で保存結果を省略しない |
| 保存失敗・保存往復 | 未保存を明示し、再読込で値・GUID・Missing が維持される |
| 実ゲームの振る舞い | 入力と観測値から合否が分かり、要求したゲームルールを満たす |
| 人間・AIの書きやすさ | 明示型、省略可能な private、属性ゼロの public 設定・配列、任意参照、共通 helper、普通の型付き public 操作で同じ課題を完成できる |

## 導入順

[C# バックエンドの導入時の段階](csharp-scripting.md#導入時の段階と受入条件)を過去の導入順として残す。段階 1 は primitive の縦断と適用完了、段階 2 は正式な型 GUID・参照 codec・設定 schema・共通 Op と保存往復を確認した。現在は DataAsset、動的生成・破棄、実行中の更新・状態保持、所有者付き Jobs / Events、製品版 package も実装し、[完成範囲](csharp-completion.md)の検証を行った。AI の作業は、コード・設定・保存・Playtest による一往復を実際のゲーム要求に対して繰り返す。

## 確認した現行の接続点

| 接続点 | 現行ファイル |
|---|---|
| 読み取り catalog / inspect | `Projects/Editor/src/Ai/Bus/ScriptHandlers.cpp` |
| transaction の command 構築 | `Projects/Editor/src/Ai/EditorBusDispatcher.cpp` |
| Composite / Undo revision | `Projects/Editor/src/Util/UndoStack.cpp` |
| Scene 検索と MCP の上限 | `Projects/Editor/src/Ai/Bus/SceneHandlers.cpp`、`Projects/EditorMcp/src/Tools/SceneTools.ts` |
| GUID の保存・要求検証 | `Projects/Engine/src/Scene/SceneSerializer.cpp`、既存 ScriptSystem |
| Op の共通登録 | `Projects/Editor/src/Op/` |
| シナリオの実行・表明 | `Projects/Editor/src/Playtest/PlaytestRunner.cpp`、`Projects/Editor/src/Ai/Bus/PlaytestHandlers.cpp`、[AI の検証ループ](ai-verification-loop.md) |

2026-10-04 の段階 2 では Projects/Editor/src/ManagedScriptMetadata.cpp と ManagedScriptEditor.cpp、Projects/Managed/Fbzz.ScriptGenerator、Projects/Editor/src/Op/ScriptConfigureOperators.cpp を接続し、型 GUID の発行、partial の明示 primary、source checksum、共通 plan を実装した。単体 / native の 256 件と、実 Editor の名前・参照・保存・実行 84 手順を確認し、この時点の受入を完了した。型移行・Prefab 往復・Undo / stale / 再送は TestKit の検証であり、全条件を実 Editor の 84 手順で実行したという意味ではない。[当時の検証記録](../../Projects/Managed/README.md#2026-10-04-の検証)。追加 API・Play 更新・配布・正常終了を含む現在の結果は [Managed の最新検証記録](../../Projects/Managed/README.md)を参照する。
