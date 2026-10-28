<!-- @file    csharp-authoring.md -->
<!-- @brief   AI とゲーム作者向け C# の記述・参照・一括制御の契約。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-03 -->
# C# 作者向け API

- 状態: 2026-10-05、制作・実行経路の追加 API を実装。段階 1 / 2 の過去の受入と、追加機能の検証結果は Managed の README に記録する。
- ランタイム、保存形式、世代と寿命の契約: [C# ゲームスクリプトの設計](csharp-scripting.md)。
- 宣言から catalog・一括設定・保存・Playtest へ進む制作経路: [AI が作成・設定できる Script](ai-script-workflow.md)。

目的は、AI が有効な API でロジックを書けること、作者が実行順・状態遷移・副作用を読み取れること、変更と動作確認の手間が少ないこととする。参照の設定を個別の Editor 操作へ分散させず、単体・配列ともコードの宣言と Editor の一括操作で接続する。

現在のビルド入口と受入記録は [Managed の README](../../Projects/Managed/README.md)を参照する。省略時 private の標準 C#、属性なし public 設定、private + Setting、引数なし lifecycle、自己 GameObject / Transform、Input / Time を維持する。[制作・実行経路の完成](csharp-completion.md)に従う現在の実装範囲は以下。

| 実装済みの機能 | 現在の範囲 |
|---|---|
| 保存する型 | bool、各幅の整数、float/double、decimal、char/string、各整数基底 enum、Vector2/3/4、Quaternion、GameObject、typed Component/Script・DataAsset・Material/Texture 参照、配列・List、対応メンバーだけの Serializable class / struct |
| 未設定と欠損 | GameObject の CLR null と、保存 GUID が残る失効参照を区別。配列の空・順序・重複・null 要素を保持 |
| 取得・制御 | GetComponent<T>() / GetComponents<T>()、AddComponent<T>() / RemoveComponent<T>()、SetActive、Layer、Tag、Script.Enabled |
| 移動 | LocalPosition / WorldPosition / TranslateLocal / TranslateWorld / MoveTowardsWorld |
| 回転・拡縮 | LocalRotation / WorldRotation、LocalScale、Forward / Right / Up、Rotate / LookAt |
| 一括位置操作 | Objects.TranslateWorld / TryReadWorldPositions / TrySetWorldPositions。普通の GameObject[] と再利用する値・状態バッファ |
| 純粋な計算 | Vector3 の演算、Quaternion の生成・積・補間、Mathf.Clamp / Clamp01 / Lerp / 角度変換と定数 |
| 検索 | Scene.TryFind、FindAll、ReadObjects、GameObject.TryFindChild、GetChildren、GetDescendants、FindDescendants、FindPrefabPart、ReadDescendants |
| 名前と保存 | UI / AI はクラス名・オブジェクト名・階層 path を使用。内部では .cs.meta の型 GUID と対象 GUID を保存 |
| 制作 | script.configure.preview / apply で追加・設定・配列編集を計画し、一回の Undo へ接続。Play/Pause では既存 managed 設定の一時編集・専用 Undo と明示採用に対応 |
| 生成・破棄 | Scene.Create / Instantiate / CanCreate / CreateMany / InstantiateMany / CreatePool、GameObject.Destroy、Scene.Destroy / DestroyMany。Play 専用 |
| 物理 | RigidBody、各 Collider、Raycast / SphereCast / RaycastAll / OverlapSphere の mask・trigger・除外条件 |
| ゲームサービス | Animator、AudioSource と Script.Audio、UI 各型、Camera、NavMeshAgent、CharacterController の C++ 接続 |
| 延期・通知 | 所有者付き Jobs.Wait / Repeat / Run / Tween / Timeline、Events.Subscribe / Publish |
| 差し替え・配布 | Play 再読み込み、KeepOnReload と直接参照の再接続、Managed package を Scene より先に起動 |
| 衝突通知 | OnCollisionEnter / Stay / Exit(Collision)、OnTriggerEnter / Stay / Exit(GameObject)。既存の物理順で配送 |

型 GUID は通常コードに書かない。namespace は任意で、同じ単純クラス名が複数ある場合だけ完全名を使う。オブジェクトの同名一致は root / path で絞り、曖昧な先頭選択で補わない。

## 普段のコードは属性ゼロにする

通常の設定に属性を要求しない。対応型の public instance フィールドは、そのまま Inspector / AI の設定と Scene / Prefab の保存対象にする。private は内部状態、public メソッドは他のゲームロジックから呼べる操作にする。namespace、登録、Context、型推論は必須にしない。lifecycle は基底 Script の protected virtual メソッドを protected override で実装し、名前・署名の誤りをC#コンパイラで検出する。

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

段階 2 の実装 API を使う例。物理 body を持たない対象のワールド移動で、数値は秒・メートルを使う。剛体を移動する場合は後述の RigidBody API を使う。

Unity の public field の既定保存、private を属性で保存する規則、自己 Transform の直接利用を参考にする。[Unity の SerializeField](https://docs.unity3d.com/6000.0/Documentation/ScriptReference/SerializeField.html)、[Component.transform](https://docs.unity3d.com/6000.0/Documentation/ScriptReference/Component-transform.html)。FBZZ では設定を Inspector と AI が同じ schema / Op で扱い、参照設定のための手操作を必須にしない。

| 用途 | 既定の書き方 |
|---|---|
| 数値・参照・配列の設定 | public float / GameObject / GameObject[] |
| 内部状態 | float / int / bool 等。private は省略可能 |
| 自分の移動・回転 | 基底の Transform |
| 自分の GameObject が必要 | 基底の GameObject を普通の変数へ代入 |
| 他の Script / Component の操作 | GetComponent<T>() と普通のメソッド |
| 開始・更新 | 必要な protected override void Start() / Update() 等だけ |
| 子・孫・大量対象 | root を指定した検索、通常配列、foreach。一括 API は必要時だけ |

GameObject は保持できる非所有参照。固定名 Self を要求せず、他の対象のフィールドも、自分を扱う local も自由な名前にする。通常の移動のために自己参照フィールド・属性・Awake の代入を増やさない。

~~~csharp
protected override void Start()
{
    GameObject actor = GameObject;
    if (actor.TryFindChild(["Visual", "Wheel"], out GameObject wheel))
        wheel.Transform.TranslateLocal(new Vector3(0f, 0.1f, 0f));
}
~~~

サンプルと AI の推奨出力は var を使わず、float、int、bool、Vector3、GameObject 等を明示する。foreach、out、lambda の引数も型を書く。通常 C# の型推論の可否とは別に、宣言した行で型を読めるスタイルへ統一する。

### public / private の使い分け

public field は「他の C# コードから触れる」と「編集して保存する」の両方を選ぶ短い既定にする。これらを分けたいときだけ private の設定フィールドへ Setting を付ける。ゲームの状態は private にし、外へ見せたい値は public の読み取り property、変更は public メソッドにする。

~~~csharp
[Setting]
float moveSpeed = 4f;

float m_remainingSeconds;
public float RemainingSeconds => m_remainingSeconds;
~~~

クラス内のメンバーは修飾を省略すると private。[C# の private](https://learn.microsoft.com/en-us/dotnet/csharp/language-reference/keywords/private)。Script の型は class EnemyComponent : Script でよく、public / sealed / namespace は必要時だけ。設定フィールドは lowerCamelCase、内部状態は m_ を使う。public field の外部書き込みまで Engine が監視するとは扱わない。

### Inspectorで設定と実行時の状態を区別する

通常表示は保存・編集できる設定だけにする。publicの対応型とprivateの`[Setting]`は表示し、privateの内部値、`[NonSerialized]`、readonlyのうち対応型は「実行時の状態（読み取り専用）」を展開した時に参照する。閉じている間は内部値を取得しない。`[KeepOnReload]`だけでは設定にならない。配列・Listの観測はページ移動で確認でき、追加・削除・並べ替え操作は出さない。

`[Setting(Visible = false)]`は表示だけを隠し、既存の保存値、field key、参照GUIDは保持する。入れ子のVisible指定も尊重する。プロパティと自動生成のbacking fieldは列挙せず、getterを実行しない。staticは個体のInspectorに出さない。`SerializeField`や`HideInInspector`はFBZZ SDKの属性ではなく、保存には`Setting`、表示の抑制には`Setting.Visible`を使う。

ComponentRefはオブジェクト名と型名を表示し、同じ型のScriptが複数ある場合はその型の個体番号を付ける。対象が見つからなければその状態を表示する。参照GUIDとScript IDは欄のtooltipで確認でき、表示名の変更や番号の変更で保存アドレスを書き換えない。複数選択も同じ表示規則と既存の設定トランザクションを使う。

実例: `OceanCurrentComponent`ではdirectionWorld、speed、lateralVariation、phaseFrequency、pausedが通常表示に残り、observedSeconds、observedCenterVelocityは実行時の状態に移る。作者のpublic調整値は名前から推測して隠さない。

### アクセス修飾子の決定

2026-10-04 の決定として、クラスのメンバーは省略時 private の標準 C# を維持する。設定フィールドと他 Script から直接呼ぶメソッドは public を明示する。private を明示する書き方も利用でき、設定にしたい private フィールドだけ Setting を付ける。[C# のアクセス修飾子](https://learn.microsoft.com/en-us/dotnet/csharp/language-reference/language-specification/classes#1536-access-modifiers)

省略を public へ変換する構文補完、ソースの修飾子有無で保存対象を変える規則は導入しない。lifecycle は基底の protected virtual を protected override で実装する。private の同名メソッドは callback として検出せず、Analyzer が誤った宣言を診断する。

### 属性は追加条件だけに使う

| 任意の属性 | 必要になる場面 |
|---|---|
| Setting | private を設定・保存する、または Label / Help / Group / Unit / Visible を補足する |
| ValueRange | 編集・読み込み・復元で数値範囲を制約する |
| RequiredReference | 編集・保存・Undoは未接続を許し、Play開始・reloadでは生存する単体 / 非空かつ全要素が生存するcollectionを要求する |
| KeepOnReload | Play 差し替えで private の対応した状態を保持する |
| FieldKey / PreviousFieldKey | 保存キーの明示指定・改名互換 |
| System.NonSerialized | public field を外部へ公開しつつディスク保存から除外する |

`ValueRange` の境界型はフィールドの数値型に合わせる。整数リテラルは既定で `int` なので、幅に合わせて `U` / `L` / `UL` を付ける。`float` は `f`、`double` は suffix なしで書く。`sbyte` / `byte` / `short` / `ushort` / `char` と整数基底 enum は、表現できる `int` / `uint` 等の境界を使い、実値は field の範囲内で検証する。

| 境界に使う型 | 属性引数の例 |
|---|---|
| float | `[ValueRange(0f, 1f)]` |
| double | `[ValueRange(0.0, 1.0)]` |
| int / uint / long / ulong | `[ValueRange(0, 10)]` / `[ValueRange(0U, 10U)]` / `[ValueRange(0L, 10L)]` / `[ValueRange(0UL, 10UL)]` |
| decimal | `[ValueRange("0.00", "10.00")]` |

decimal 境界は C# 属性引数に decimal を使えないため、`.` を小数点とする不変文化の文字列で指定する。`ValueRange` は保存・編集・復元の全経路で検証され、配列・List では各要素へ適用する。

Attached、Watch、SceneObject、SceneObjects は初版へ入れない。自己参照は基底の通常 property、観測は Debug inspect、初期接続は共通 Op または普通の開始コードで行う。同じ目的を属性と別 API へ二重に分けない。範囲・必須参照も全フィールドへ機械的に追加しない。

Setting は Scene / Prefab / DataAsset の設定であり、セーブゲームの指定ではない。metadata と初期値から schema を生成し、独立した Default 属性や手書きの設定 schema を要求しない。Label は表示名、Help は説明、Group は見出し、Unit は表示単位、Visible は Inspector の表示だけを変える。省略時はフィールド名と初期化子を使う。

## Script を継承しない通常の C#

`.cs` をコンパイルすることと、GameObject に付けて自動実行することを分ける。通常の `class` / `static class` / `struct` / `enum` / `interface` は、`Script` の継承・登録・属性なしで使える。helper のインスタンスは普通の `new` で作り、メソッドを通常の C# として呼ぶ。

| 宣言 | 現在の扱い |
|---|---|
| 通常の class / static class | ゲームロジック、状態、utility。アタッチ・自動 lifecycle・Script catalog の対象外 |
| struct / enum / interface | 普通の C# の型。ゲームコードから直接使う |
| abstract class : Script | Script の共通基底。自身はアタッチ・catalog の対象外 |
| 具体的な class : Script | アタッチ対象。入れ子でなく、ジェネリック型でなく、引数なし constructor を持つことを要求 |

ゲーム用 `.csproj` の `Compile` に含まれる `.cs` は継承やファイル名で選別せず、一緒にコンパイルする。現在の GreenWare は `Assets/Scripts/**/*.cs` を対象にする。通常クラスのファイルを追加するために Editor で Component を登録する必要はない。helper 内の `Update()` は名前だけで自動呼び出しされず、Script の callback 等から明示的に呼ぶ。

Unity も通常のクラス・構造体をアセンブリへコンパイルし、MonoBehaviour の継承で GameObject へのアタッチと Start / Update 等の入口を提供する。この境界を参考にする。[Unity: アセンブリ](https://docs.unity3d.com/6000.0/Documentation/Manual/assembly-definitions-intro.html)、[Unity: MonoBehaviour のアタッチと callback](https://docs.unity3d.com/6000.0/Documentation/Manual/class-MonoBehaviour.html)

### helper から Engine API を使う

以下は段階 2 の実装 API を使う例。namespace と属性を要求せず、型を明示する。自分の GameObject は普通の変数へ取り、helper の引数名も自由に付ける。

~~~csharp
/// @file    MovementComponent.cs
/// @brief   通常の helper を使い、自己オブジェクトをローカル X 方向へ移動する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using Fbzz;

static class Movement
{
    /// @param delta ローカル空間の移動量 [m]。
    public static void Move(GameObject actor, Vector3 delta)
    {
        actor.Transform.TranslateLocal(delta);
    }
}

class MovementComponent : Script
{
    public float moveSpeed = 4f;

    protected override void Update()
    {
        GameObject actor = GameObject;
        float distance = moveSpeed * Time.DeltaSeconds;
        Movement.Move(actor, new Vector3(distance, 0f, 0f));
    }
}
~~~

通常の helper 呼び出しは Host の新しい入口を作らない。Engine API が許可された同期 callback 中に呼ぶと、Engine 操作は呼び出し元のスレッド・Scene・コード世代・Phase / RunMode・権限を使う。helper が受け取った `GameObject` は操作対象であり、呼び出し元の所有者を変更しない。対象の寿命と同じ Scene への所属も既存の検証に従う。helper 自身に暗黙の GameObject / Time 窓口を足さず、必要な対象と計算済みの値を普通の引数で渡す。

純粋な計算や自前のデータ処理は、通常 C# の同期規則に従って別スレッド・Task でも行える。Engine API は現在の Host callback のスレッドと実行スコープを要求し、helper、保持した GameObject、Task のいずれもその権限を callback 外へ持ち越せない。Host が Script の constructor、catalog / 設定の準備を実行する際に Live scope を与えず、callback 終了後も Engine 操作を許可しない。各 API の Phase 制約も残り、Time.DeltaSeconds を helper に移しただけで Update 等の外から読めるようにはならない。

### 純粋な数学計算

Vector3.Dot / Cross / Lerp / NormalizedOr と Mathf.Clamp / Clamp01 / Lerp / ToRadians / ToDegrees は C# 内で計算する。Host の実行スコープを要求しないため、普通の helper と constructor でも使える。

~~~csharp
Vector3 offset = destination - currentPosition;
Vector3 direction = offset.NormalizedOr(Vector3.ZERO);
float distance = Mathf.Clamp(moveSpeed * deltaSeconds, 0f, offset.Length);
Vector3 nextPosition = currentPosition + direction * distance;
~~~

両方の Lerp は t を制限せず外挿できる。0〜1 に収めたい場合は Mathf.Clamp01 を明示する。NormalizedOr は長さが 1e-6f 未満なら指定した fallback をそのまま返す。位置を書き込む Engine API は有限値を要求するが、純粋な数値計算は NaN / Infinity を自動修正しない。Clamp は min <= max を前提とする。[C++ Math との一致条件](csharp-scripting.md#計算関数の公開順と一致条件)

### helper の保存とアセンブリ分割

通常クラスを使えることと、そのインスタンスを Scene / Prefab へ保存できることは別にする。Serializable の class / struct は、対応する instance field だけを version 付き codec で保存する。class は引数なし constructor が必要で、復元候補を作る間は Engine 実行スコープを与えない。getter を保存時に評価しない。Dictionary、delegate、Task 等の非対応型を明示した Setting / KeepOnReload に使うとエラーにする。

Unity の Serializable custom class と同様、保存対象の field に含まれる値を保存する。FBZZ は入れ子の深さ 32 を契約とし、循環・未知キー・重複キー・非有限値を拒否する。配列・List の総件数や文字列全体の大きさを、表示・転送の固定上限で制限しない。大きな値は全件を保存・復元し、観測ページ、一回の packet、保持 handle、常駐メモリと退避領域の資源予算を別に扱う。共有 instance の同一性を保存する仕組みではなく、各 field の値として復元する。[大きな設定の容量契約](csharp-settings-transactions.md#配列の容量と大きな設定)、[Unity: custom class の保存規則](https://docs.unity3d.com/6000.0/Documentation/Manual/script-serialization-rules.html)

入れ子設定の保存キーと旧保存キーには、GameObject の内部参照 token と同じ `$go` を指定できない。FieldKey / PreviousFieldKey で指定した場合は schema 診断として拒否し、普通の文字列を GUID 参照として変換しない。

通常は helper と Script を同じ `.csproj` に置く。別ライブラリへ分割する場合は標準の `ProjectReference` / assembly reference を使い、Unity の `.asmdef` に相当する独自宣言を必須にしない。別 DLL のゲーム依存型は同じ collectible 世代の依存として扱う。現在のアタッチ catalog はロード対象のゲーム assembly 内の具体的な Script を列挙し、依存 DLL の全型を追加候補へ自動登録しない。旧世代の Type / delegate / helper instance を外部へ保持すると世代の解放を妨げ得るため、通常の helper state を世代を越えて自動保存・移植する契約にはしない。

## 単体参照と初期設定

保存する Component / Script の参照には `ComponentRef<T>` を使う。`public ComponentRef<HealthComponent> health;`、`ComponentRef<RigidBody>[] bodies = [];` のように宣言できる。通常の `GetComponent<T>()` が返す直接の Component は private の実行時 cache に使う。直接の Component 型を Setting とした場合は wrapper を案内する診断になる。

`default` は未設定で、`IsEmpty` は実行スコープなしで確認できる。callback 内の `TryGet(out T target)` は保持した個体の正確な寿命だけを調べ、失効した参照を GUID で再検索しない。`IsMissing` は設定済みで接続先が失効・欠損した状態、`IsAlive` は現在の同期 read scope で有効な状態を表す。`new ComponentRef<T>(liveComponent)` は現在の生存個体から参照を作る。`reference.Resolve()` は保存住所を現在の Scene で明示解決し、新しい参照を返す。元の参照は変更しない。

保存データには owner GUID と native Component kind、または owner GUID・savedScriptId・型キーを記録する。runtime の Scene / GameObject token、native row epoch、Script instance・コード世代・adapter birth はディスクへ保存しない。Scene load と Prefab remap は構築完了後に保存住所を明示的に接続する。reload transaction は旧 exact token と同じ Script birth を検証して新しい field wrapper を作る。callback 等で保持していた古い `ComponentRef<Component>` 自体は新世代へ接続しない。削除・再追加・add の Undo/Redo は同じ保存IDでも新しい birth を持つ。

Material と Texture は `MaterialAssetRef` / `TextureAssetRef` を使い、`DataAssetRef<T>` と区別する。constructor は GUID または `guid:` 参照だけを受け付け、`default` は空参照になる。Inspector picker と Material API の resolver は実際のアセット型を確認する。未配置・一時的に欠損した GUID も設定として保存され、codec は Scene オブジェクトや DataAsset の代理として変換しない。

Play 中の非同期利用権は `Assets.Request(textureRef)`、`Assets.Request(materialRef)`、`Assets.RequestModel(guid)` で取得する。返る `AssetLease` は保存しない Script 所有 handle で、`Status` / `IsReady` / `IsFailed` の照会、`Retry()`、品質指定、`Dispose()` による解放を行う。Script の無効化・fault・破棄・世代退役では Engine が要求を解放し、保持中の handle は失効する。`Dispose()` は自動解放後も安全に呼べる。`PrefetchScene(name)` と `GetScenePrefetchProgress(name)` は SceneManager の登録済み Scene bundle を扱い、Script の AssetLease とは別の SceneManager 所有物である。

~~~csharp
AssetLease? m_texture;

protected override void OnEnable()
{
    m_texture = Assets.Request(new TextureAssetRef("texture-guid"), prefetch: true);
}

protected override void OnDestroy()
{
    m_texture?.Dispose();
}
~~~

~~~csharp
public GameObject target;
public GameObject[] patrolPoints = [];
~~~

コードの既定値を用意し、保存された field key があればその値を復元する。FieldKey / PreviousFieldKey の移行後も、未設定・空配列・欠損は元の設定として保つ。フィールド名から相手を推測したり、保存した null を別の検索結果で埋めたりしない。

AI / Inspector は同じ一括設定 Op で GUID を接続する。階層から weapon を選び、巡回地点を順番付きで設定するために、Script へ接続用属性を足す必要はない。自分の GameObject は Host が所有者から提供し、通常の保存フィールドではない。

生成先の Scene から相手を取得したいときは、普通の開始コードを使える。

~~~csharp
GameObject m_target;

protected override void Start()
{
    Scene.TryFind("Player", out m_target);
}
~~~

Scene.TryFind は所有者と同じ Scene の名前一致を調べる。既定では inactive を除外し、一個なら true、零個または複数一致なら false / null、失効スコープは例外で拒否する。includeInactive = true を明示できる。タグ検索は Scene.FindAll(tag: "Player") 等で全一致を取得し、一個を必要とする場合は件数を確認する。TryFindWithTag という SDK メソッドはない。Start 時点で対象がまだ生成されていないゲームでは、通常の明示呼び出しで再取得する。

この private の実行時参照は Prefab に外部 Scene の GUID を保存しない。Play 差し替えでは直接の参照 field をエンジンが再接続し、Start の再実行や KeepOnReload を要求しない。タグの現在状態から取り直す意図がある場合だけ AfterReload で検索する。Prefab 内部の固定部位は public または Setting の参照へ source GUID を保存し、生成時に instance GUID へ remap する。共有 Prefab の外部接続は、生成先で検索するか、配置計画で各個体の設定を明示する。

GameObjectと書いても未設定ならCLR nullを取り得る。RequiredReferenceは対象が生きているPlay開始・reload条件で、参照型の宣言から自動生成しない。編集・保存・Undoは未接続の途中状態を許す。配列の非nullはcodec、非空・全要素の有効性はRuntimeの必須条件とする。「対象がいれば追跡、なければ巡回、地点もなければ停止」には、任意のtargetと空配列をそのまま使う。

### 参照の型に記号を付けない

ゲーム作者の既定は GameObject target; / HealthComponent health とし、参照型の nullable 注釈、null-forgiving、null 条件呼び出しを通常例に要求しない。ゲーム用 csproj は Nullable = warnings とし、注釈の context を off、null 解析の warning context を on にする。標準 C# の設定であり、独自のパーサー・変換や全フィールドへの警告抑制属性は作らない。[Microsoft の Nullable 設定](https://learn.microsoft.com/en-us/dotnet/csharp/language-reference/compiler-options/language#nullable)

~~~csharp
HealthComponent health = target.GetComponent<HealthComponent>();
if (health != null)
    health.TakeDamage(10f);
~~~

~~~xml
<PropertyGroup>
  <Nullable>warnings</Nullable>
</PropertyGroup>
~~~

段階 1 のゲーム用 csproj にこの設定を反映した。今後生成するゲームプロジェクトも同じ既定とし、作者にファイルごとの directive を書かせず、補完・AI 生成・サンプルを揃える。

段階 2 の compiler library は、Inspector が代入する対応型の mutable instance 設定 field に限り CS0649 を抑制する。`public GameObject target;` に初期化記号を付ける必要はない。属性なし private の状態、static、readonly、NonSerialized、未対応の保存型の未代入警告は残す。nullable 解析の警告や実行時の null / Missing 検証を抑制するものではない。

注釈を省略してもnullが消えるわけではない。通常のifで未設定を扱い、保存設定の未接続をInspector / AIに表示し、明示したRequiredReferenceはPlay開始・reload時に検証する。編集・保存・UndoのSnapshot復元は未接続も保持し、codec・型・finite・rangeの検証は省略しない。GameObject / Componentの失効はIsAliveで別に確認する。宣言からの非null保証・未初期化検出はenableより弱くなり、flow解析やSDKの契約から分かる警告は保持する。Editor検証が任意のC# dereference全てを証明するとは扱わない。

Host / ScriptApi 自体は Nullable = enable を維持し、null を返し得る取得 API の metadata と catalog の mayBeNull を正確に出す。作者が通常の C# の nullable 機能を選んで使うことは妨げず、型の省略・独自 null 演算子は導入しない。

## 子オブジェクト参照で不足していた契約

Scene 全体のタグ検索だけでは、指定した一個体の部位を表す用途には足りない。同じ Enemy Prefab が複数あれば、Weapon タグの単体選択は曖昧になり、配列選択は別個体の部位も含む。

C++ の GameObject は GetParent / GetChild / FindInSubtree を持つ。ただし FindInSubtree は自分も含めた DFS で最初の同名を返す。C# には後述の Parent / GetChildren / GetDescendants / TryFindChild を公開し、単体検索では基準 root と一致数を検証する。

さらに PrefabInstantiate は子を含む全名称を Scene 全体で一意化する。同じ Prefab の二個目では Weapon が Weapon_1 になる可能性があり、実体名による同じパスの再検索だけでは Prefab の部位参照を作れない。

| ケース | 必要な選択範囲 |
|---|---|
| Script が付いた対象の子 | GameObject の直下 |
| 任意の GameObject の子・孫 | 指定 root の直下または部分木 |
| 兄弟 | Parent の直下。root と同一対象を除く条件は明示 |
| 親・先祖 | Parent、または親を一段ずつ辿る通常コード |
| 同じ Prefab の複数個体 | 指定した instance の範囲内 |
| 子の配列・巡回順 | SDK は選択した階層の順序。Editor の scene_find の GUID 順とは区別 |
| 改名・親変更・削除 | 保存済み GUID を保ち、別対象へ黙って接続しない |

### 初版は属性を増やすより root を明示する

階層参照は、GameObject を基準にした通常の API と Editor 一括接続の選択元で扱う。ParentObject / ChildObject / ChildObjects のような派生属性は使わない。保存値優先とは別の依存解決・自動再接続・GUID cache の規則を、小さいゲームコードにも要求しない。

~~~csharp
public GameObject weapon;
public GameObject[] patrolPoints = [];
~~~

AI は Script の GameObject または明示した root の階層・Prefab 定義内で候補を選び、weapon と patrolPoints を一つの Op 計画で設定する。作者は参照を使うために検索コードを書かなくてもよい。接続結果は通常の設定フィールドの GUID として保存し、変数名から対象を推測しない。

保存した武器と設定で指定した root は独立した参照である。root の参照だけを変更しても武器を自動で切り替えない。関連対象を変更する意図の Editor 計画では、その参照群を同じ preview / apply に含める。実行時に関係を変える場合も、通常コードで参照を明示更新する。親子関係が必須のロジックは Parent や GetDescendants の結果を使って確認する。IsDescendantOf という SDK メソッドは公開していない。

### GameObject を基準にした検索 API

以下は現在の SDK にある検索 API。Editor の root / tag / scope selector と、実行中の SDK 検索は別の入口である。

| API | 契約 |
|---|---|
| actor.Parent | 現在の直近の親。root なら null |
| actor.GetChildren(includeInactive = true) | actor を含まない直下の子。Hierarchy 順のスナップショット |
| actor.GetDescendants(includeInactive = true) | actor を含まない全子孫。Hierarchy 順の DFS |
| actor.TryFindChild(names, out child, includeInactive = true) | 現在の実体名を各段の直下で完全一致。各段が一個であることを要求 |
| actor.FindDescendants(name = "", tag = "", includeInactive = true) | actor を除く全子孫の一致を Hierarchy 順で返す。空 name / tag は条件なし |
| actor.ReadDescendants(output, name = "", tag = "", includeInactive = true) | 同じ子孫検索を caller のバッファへ返す。容量不足は部分出力しない |
| actor.FindPrefabPart(sourceId, includeInactive = true) | actor 自身と子孫の prefabSourceId 一致を配列で返す。名前経路を source ID へ自動変換しない |

TryFindChild の names は名前の一次元配列または ReadOnlySpan<string>。例えば ["Visual", "Weapon"] とし、"/" 区切りの独自パス言語は使わない。名前に "/" があっても一セグメントとして扱える。空配列・空セグメントは不正入力。ワイルドカード、絶対パス、親へ移動する特別構文は使えない。TryFindChildPath / TryFindDescendantWithTag / FindDescendantsWithTag / TryFindPrefabPart は旧案の名前であり、現在の SDK にはない。

TryFindChild は未検出・複数一致なら false と out = null を返し、失効 root・不正入力・実行スコープ違反は例外として区別する。曖昧な単体を「最初の一個」で救済しない。配列検索の一件だけを必要とする場合は、作者が Length == 1 を確認する。既存の callback スコープと Scene / Entity 世代の検証に従い、別 Scene を黙って検索しない。

~~~csharp
GameObject parent = actor.Parent;
GameObject[] children = actor.GetChildren();

GameObject[] foundWeapons = actor.FindDescendants(tag: "Weapon");
if (foundWeapons.Length == 1)
    weapon = foundWeapons[0];

GameObject[] wheels = actor.FindDescendants(tag: "Wheel");
~~~

動的検索は呼んだ時点の実際の階層を対象とする。Parent 取得も保持した親を後から自動変更する仕組みではない。配列を返す API は確保と走査の費用があり、初期接続・生成完了・関係の変更時に使う。大量対象には実装済みの ReadDescendants / Scene.ReadObjects と再利用バッファを使い、毎フレームの再検索・ToArray を標準にしない。

### 深い階層と処理コスト

通常の GameObject 階層の深さを、参照属性や名前セグメント数の小さい固定上限で制限しない。範囲内の全子孫を選べるようにし、実際の総個数は現行 Scene の 4096 枠に従う。GetChildren は直下だけ、GetDescendants と子孫タグ検索は任意の深さを対象とする。

保存済み GUID は接続・復元時に generation 付き EntityID へ対応付ける。通常 callback で参照を使うたびに名前パスや親を root まで辿らない。これは参照先の照会の契約であり、active-in-hierarchy の確認や Transform の変更費用が深さに依存しないという意味ではない。

子孫の検索・Editor の全件選択は、明示的な stack / queue と訪問数の検証で行う。親子循環・無効なリンクを通常の読込診断で拒否し、探索予算・出力容量不足を途中の結果だけの成功として返さない。現行 FindInSubtree と Transform 更新は既に反復処理だが、Scene の子孫破棄、Prefab 差し替えの identity 収集、Editor 検索等に再帰処理が残る。数千段の安全性が実装・検証済みとは扱わない。

ActiveInHierarchy と C++ 内部の IsDescendantOf は現行では親を辿る。Transform の部分更新も先祖と子孫を同期するため、深い階層の多数対象を別々に変更すると同じ階層を繰り返し処理し得る。一括 API でも即時同期の意味を維持し、この費用は参照照会・境界呼び出しとは別に計測する。

深い通常階層と Prefab の入れ子は区別する。深い子孫の選択に、別の Prefab 個体を推測する処理は不要である。FindPrefabPart は指定 root の部分木を検索するだけで、専用 instance-root ID や Prefab 定義の名前経路を推測する機能ではない。

### Prefab 内部参照は GUID で確定する

初版の通常参照は Prefab 編集時に解決して設定フィールドへ保存する。ScriptComponents.fields に GUID 文字列・配列で投影し、既存の Prefab 生成時 source GUID → instance GUID の再帰リマップへ参加させる。Apply 時の instance → source 変換も利用する。子ごとの実体名が変わっても、同じ生成個体の対象へ接続できる。

実装済みの FindPrefabPart(sourceId) は実体名の検索と区別する。指定 root 自身と子孫の prefabSourceId 一致を全件返す。同じ asset を持つ全 Scene 検索で代替しない。定義の名前経路から source ID を自動解決する入口はないため、固定部位の永続参照には保存済み GUID を使う。

現行は専用 instance-root ID を持たず、root に asset 参照と snapshot、子孫に source ID を持つ。FindPrefabPart は一致配列を返し、入れ子を含む複数一致も一件へ絞らない。別個体を除外するには検索 root を明示する。single-root 制約は現在の Instantiate / InstantiateMany の生成契約であり、この検索へ追加の制約を仮定しない。

## 配列を普通に書けること

以下は物理 body を持たない対象を、ワールド前方へ動かす例である。

~~~csharp
/// @file    EnemyGroupComponent.cs
/// @brief   一群の対象をワールド前方へ移動する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using Fbzz;

class EnemyGroupComponent : Script
{
    public GameObject[] enemies = [];
    public float moveSpeed = 4f;

    protected override void Update()
    {
        Vector3 displacement = Vector3.FORWARD * moveSpeed * Time.DeltaSeconds;

        foreach (GameObject enemy in enemies)
        {
            if (enemy is null || !enemy.IsAlive)
                continue;

            enemy.Transform.TranslateWorld(displacement);
        }
    }
}
~~~

配列は通常の C# 配列で、Length、添字、foreach をそのまま使う。巡回地点、攻撃対象、カメラ切替先、生成した敵の一覧にも同じ参照型を使う。配列を書き換えることと、Scene の対象を生成・破棄することは別の操作とする。

冒頭の対応型と Serializable 値は、一次元配列・List の要素にも保存できる。数値の幅・enum/Flags・参照は単一値と同じ [共通 schema](csharp-schema.md) に従い、64 bit 整数や decimal を float に変換しない。数値配列も同じ public の規則で宣言できる。配列自体と string 要素の null は認めず、空配列と string.Empty を使う。GameObject 要素の null は未設定スロットとして保持する。

~~~csharp
public float[] spawnDelays = [0f, 0.5f, 1f];
public Vector3[] spawnPositions = [];
public GameObject[] cameras = [];
~~~

List<T> は対応した要素型を保存できる。入れ子の一次元配列・List は複合 codec の上限内で扱い、多次元配列、Dictionary、循環する任意の object graph は保存しない。実行中に個数が増減する管理には List<GameObject> を使い、必要時に配列・連続バッファへまとめられる。毎フレーム ToArray する形式を標準例にしない。

### 配列の保存と欠損

- 手動配列の順序、重複、空配列、未設定スロットをそのまま保存する。
- GameObject の保存値は GUID。実行時 ID、native ポインター、managed オブジェクトグラフは保存しない。
- 解決できない GUID は元の GUID と添字を保ち、IsAlive が false の参照として表示する。自動で要素を詰めたり、別の対象へ接続し直したりしない。
- 未設定は通常の null とする。欠損参照を null と同一視する演算子は追加しない。通常の GameObject[] でも未設定スロットは null とし、foreach で普通に分岐する。
- 配列自体の null は保存・KeepOnReload 退避・復元時のエラーとし、既定は空配列。必要な要素の欠損は RequiredReference で検証する。
- SDK の Scene.FindAll と子・子孫の検索配列は Hierarchy 順とする。Editor の検索結果の並びを SDK の契約へ流用しない。巡回順など意味のある順序は保存配列で指定する。

既存 C++ の RefListField は解決できない EntityID を空文字列へ変換するため、段階 2 は ObjectReferenceField / ObjectReferenceListField で保存 GUID と寿命検証済み EntityID を分けた。未解決 GUID とスロットを保持する codec を既存 Serializer / Inspector へ接続し、保存往復を受入検証に含める。

## 大量対象の制御

対象ごとの判断は foreach で書く。一群が共通の移動量を受ける場合は、実装済みの一括操作を使える。

~~~csharp
protected override void Update()
{
    Vector3 displacement = Vector3.FORWARD * moveSpeed * Time.DeltaSeconds;
    Objects.TranslateWorld(enemies, displacement, activeOnly: true);
}
~~~

Objects はその Script に結び付いた instance のサービスであり、全体の Scene singleton ではない。一括 API は native 側で既存の操作を反復し、配列ごとの managed / native 境界呼び出しへまとめる。対象数に比例する検証・計算・親子同期は残るため、配列化だけで処理速度が上がるとは扱わない。

移動、隊列、弾、演出などの共通制御は、一個の Script から複数対象へ適用できる。全対象に同じ Script を付ける必要はない。個別の行動や状態を持つ対象には個別 Script を使い、既存の C++ / C# ScriptEntry の実行順を維持する。C# callback 全体をまとめ直す別の更新順は作らない。

| 実装済みの一括 API | 契約 |
|---|---|
| Objects.TranslateWorld(objects, displacement, activeOnly = false) | 共通のワールド移動量。BatchResult に入力数・適用数・スキップ数と status を返す |
| Objects.TryReadWorldPositions(objects, positions, statuses) | caller 所有バッファへ添字対応で読み取り。有効要素だけ positions を更新し、全要素の状態を返す |
| Objects.TrySetWorldPositions(objects, positions, statuses) | 添字対応でワールド位置を設定。失効・未設定要素をスキップし、各要素の状態を返す |

BatchResult は Succeeded / Status / InputCount / AppliedCount / SkippedCount を持つ。失敗時も InputCount は入力スロット数を保持し、AppliedCount と SkippedCount は 0。要求全体を拒否した場合のスキップ数を、正常な要求内の欠損数と混同しない。

Scene.DestroyMany(objects, delay = 0f) は同じ対象を一回だけ既存の破棄 queue へ要求し、未設定・失効・重複スロットをスキップする。検索・一括生成も以下の API を公開している。

| 実装済みの検索・生成 API | 契約 |
|---|---|
| Scene.FindAll(name = "", tag = "", includeInactive = false) | Scene 全体の一致を Hierarchy 順の配列で返す。検索と配列確保の費用がある |
| Scene.ReadObjects(output, name = "", tag = "", includeInactive = false) | caller バッファへ全一致を返す。ObjectQueryResult の RequiredCapacity を確認し、容量不足では部分出力しない |
| Scene.CreateMany(name, poses, output) | SpawnPose ごとに空 GameObject を作る。BatchResult を返し、失敗時は Scene と出力を保つ |
| Scene.InstantiateMany(prefab, poses, output) | 同一の single-root Prefab を複数生成し、pose ごとの root を出力へ返す。全階層の容量を事前検証 |

TryFindAllWithTag / FindAllWithTag / TrySpawnMany は旧案の名前であり、現在の SDK にはない。ObjectQueryResult は Status / Count / RequiredCapacity、生成の BatchResult は Status と件数を確認する。

読み書きのバッファには Span / ReadOnlySpan を受け、既存の配列を再利用する。ObjectStatus は VALID / UNSET / EXPIRED を添字ごとに示す。入力数を上回るバッファの余りは未変更。長さ不足、Scene 不一致、無効なフェーズ、非有限の変更入力などの要求全体のエラーは、事前検証で失敗し、対象も出力バッファも途中まで変更しない。Try の true は要求全体を処理できたことを表し、各要素の有効性は statuses で確認する。callback 外・別スレッド・失効した Script サービスの利用は、通常 API と同じ実行スコープの例外になる。

入力の総スロット数に 32768 等の固定上限を設けない。同じ対象の重複も一スロットずつ数え、内部の64 KiB packet を越えると同期 call が所有する全件候補へ転送する。全件の preflight 後に一度だけ変更するため、packet 境界をまたぐ親子・重複の入力順と全体失敗時の未変更を保持する。実 Scene の 4096 Entity 枠は別の資源契約。Native の全件候補と確定出力は総件数に比例する作業領域を必要とし、温まった managed packet/結果 buffer の再利用と区別する。空配列も一回だけ native へ送り、実行スコープとフェーズを検証する。無効な引数を SDK で検出した場合は送らない。

一括位置設定の数値検証は有効な対象へ実際に使う positions[i] だけに行い、未設定・失効スロットの値は参照しない。共通 displacement は要求全体の入力として常に検証する。

失効参照を含む正常な要求は、一要素の欠損で群れ全体を停止させない。読み取りで欠損した位置は未変更とし、statuses が有効な要素だけ計算する。書き戻しでも失効を検証する。状態・位置のバッファは対象数の変更時に用意し、通常フレームでは確保と全 Scene 検索を行わない。

位置の読み取りは既存の WorldPosition と同じキャッシュを使い、Transform を再同期しない。描画補間中の値や、Engine が直接書いた非有限値も読み取り自体で修正・拒否しない。変更の事前検証は必要な姿勢を非公開の作業領域へ複製し、入力順の移動・子孫同期まで試算する。後半の重複参照で overflow する場合も、Scene へ一件も適用しない。

有限なローカル姿勢から再計算できる古い world キャッシュは、位置設定の同期で修復できる。共通移動では同期前の現在 worldPosition に displacement を加えるため、その読み取り値が非有限なら失敗する。計算で使う不正なローカル姿勢・退化した回転も、適用前に要求全体を拒否する。

~~~csharp
class FormationComponent : Script
{
    public GameObject[] members = [];
    Vector3[] positions = [];
    ObjectStatus[] statuses = [];

    protected override void Update()
    {
        if (positions.Length != members.Length)
        {
            positions = new Vector3[members.Length];
            statuses = new ObjectStatus[members.Length];
        }

        if (!Objects.TryReadWorldPositions(members, positions, statuses))
            return;

        for (int index = 0; index < members.Length; index++)
            if (statuses[index] == ObjectStatus.VALID)
                positions[index] += Vector3.UP * Time.DeltaSeconds;

        Objects.TrySetWorldPositions(members, positions, statuses);
    }
}
~~~

位置・状態の作業バッファは private にし、用途ごとに確保して再利用する。ObjectStatus は uint 基底の実行結果 enum で、通常は保存設定ではなく作業バッファに置く。enum の保存対応は int 基底に限定しない。

参照配列は対象が inactive になっても保持する。通常の一括操作は指定配列に従い、inactive を暗黙に除外しない。TranslateWorld の activeOnly: true は呼び出し時の active-in-hierarchy を検証してスキップする指定で、上の foreach と同じ意味になる。pool 待機中の対象を除外するための配列再検索・再確保を不要にする。

TranslateWorld と位置設定は入力順に、通常の foreach と同じ既存操作を実行する。手動配列の重複は操作の反復として扱い、親子を含む配列も既存の即時 world 同期に従う。位置設定では、後に設定した親の移動で先に設定した子が動くこともある。勝手な並べ替え、一般の移動での重複除去、同期の延期はしない。DestroyMany の重複除去は新しい一括操作の契約であり、現行 Destroy の queue が重複を除くという意味ではない。結果は受理した固有対象数とスキップ数を示し、実際の破棄・枠の解放は既存 Cleanup で行う。

大量対象の API を呼ぶたびに GUID で Scene 全体を検索しない。接続時に Scene の寿命と generation 付き EntityID へ対応付け、実行時は有効性を確認する。GUID の対応付け直しは Scene の復元などの明示した境界で行う。既存のタグは直接変更できるため、タグ index や自動追跡 query が実装済みとは扱わない。

Dynamic body の速度・力・移動は実装済みの FixedUpdate の RigidBody API を使う。現行 native に Kinematic モードはなく、C# にも公開しない。Transform の一括変更を物理シミュレーションの代わりにしない。変更する対象に enabled な RigidBodyComponent と実体 body がある場合は、事前検証で要求全体を PHYSICS_BODY として拒否する。読み取り、無効な body、activeOnly で除外された inactive 対象はこの拒否条件に含めない。

## 生成・破棄と Physics

生成は Scene.Create(name) または Scene.Instantiate(prefab) で root の GameObject を返す。両方に (name / prefab, Vector3 position, Quaternion rotation) の overload があり、position はワールド座標、rotation は正規化して使う。Create は空オブジェクトを作る。Collider / RigidBody 等は設定済み Prefab または AddComponent<T>() で追加する。native Component は同じ型があれば既存個体を返し、managed Script は追加ごとに新しい個体を作る。Transform は常設で削除できない。

Instantiate は既存の Prefab 復元と内部参照の GUID 再対応付けを使い、single-root に限定する。アセット不在・容量不足は null、破損アセット・不正な姿勢・複数 root は例外になる。CanCreate(count) は子孫を含む Entity 枠を確認するだけで予約しない。破棄待ちも容量を占有する。

生成中に新規 Script を構築・設定する間は、外側 callback の managed 操作権限を停止し、戻ると復旧する。Awake / Start は次の既存 ScriptSystem の初期化で駆動する。既存 serializer の OnAfterDeserialize / OnValidate は復元時に呼ばれるが、ワールド API は利用できない。失敗した append は追加実体を取り消す。ユーザー callback や外部 I/O の副作用まで巻き戻す保証は付けない。

GameObject.Destroy(delay)、Scene.Destroy(target, delay)、Scene.DestroyMany(objects, delay) は Play の有効な Live callback で既存の破棄 queue へ要求する。delay は有限かつ非負の秒。要求直後は IsAlive が true のままで、実際の削除と参照失効は Cleanup で行う。OnDestroy は managed の後始末に使い、Engine API の読取・書込は許可しない。破棄時の OnDisable や CollisionExit / TriggerExit を保証しない。

~~~csharp
class ProjectileSpawnerComponent : Script
{
    public string projectilePrefab = "Assets/Prefabs/Projectile.prefab";
    public float initialSpeed = 12f;
    bool m_fireRequested;

    protected override void Update()
    {
        if (Input.GetKeyDown(KeyCode.SPACE))
            m_fireRequested = true;
    }

    protected override void FixedUpdate()
    {
        if (!m_fireRequested)
            return;
        m_fireRequested = false;

        GameObject projectile = Scene.Instantiate(projectilePrefab,
            Transform.WorldPosition, Transform.WorldRotation);
        if (projectile is null)
            return;

        RigidBody body = projectile.GetComponent<RigidBody>();
        if (body is not null)
            body.Velocity = Transform.Forward * initialSpeed;
        projectile.Destroy(5f);
    }
}
~~~

上の例は Update で一回性の入力をラッチし、FixedUpdate で一度消費する。Input のフレーム状態自体を生成回数の制限としない。

RigidBody は取得時の剛体個体を参照する非所有 facade。private field に cache でき、操作時に callback と Scene / GameObject の寿命、Component row の birth を照合する。body を除去すると IsAlive は false となり、同じ GameObject への再追加でも古い参照は復活しない。新しい body は GetComponent<RigidBody>() で明示的に取得する。無効化された body の存在確認と Enabled は読めるが、状態取得・変更は拒否する。

| RigidBody の操作 | 契約 |
|---|---|
| Velocity / AngularVelocity | ワールド速度 [m/s] / 角速度 [rad/s]。書込は FixedUpdate のみ |
| Position / Mass / GravityScale / IsStatic / IsSleeping | 確定物理位置の読取。質量零は無限質量、重力倍率は負値も許可 |
| AddForce / AddImpulse / AddTorque / AddForceAtPoint | ワールドの力 [N]、インパルス [N·s]、トルク [N·m]、作用点 [m] |
| WakeUp / Sleep / SetFreezePosition / SetFreezeRotation | native の sleep と固定軸。変更は FixedUpdate のみ |
| Teleport(worldPosition) | 固定軸・速度・ローカル回転・拡縮を維持し、body と子孫 Transform を即時同期 |

剛体変更は Play の FixedUpdate に限定し、非有限値・蓄積 overflow・表現不能な親の零スケール・子孫姿勢 overflow を適用前に拒否する。Physics の検索と剛体読取は Play の有効な Live callback で使える。OnDestroy、検証、シリアライズ、復元の callback では、生成・破棄・Physics を含む Engine API の読取・書込を拒否する。Physics.WorldGravity / LayersCollide は読取専用で、レイヤーは 0..31。

Raycast(origin, direction, maxDistance, out RaycastHit hit)、SphereCast(center, radius, direction, maxDistance, out hit)、RaycastAll、OverlapSphere は現在の Scene の active / enabled Collider だけを、確定した物理姿勢で検索する。direction は自動正規化し、零方向は未ヒット。距離・半径は有限かつ非負。RaycastHit は GameObject / Point / Normal / Distance を持つ。RaycastAll は距離順に形状ごとのヒットを返し、OverlapSphere は GameObject の重複を除く。

検索は World に登録・同期済みの Collider を読む。生成、Transform の変更、Teleport の直後に同じ callback から検索しても、その変更を自動同期しない。新規 Collider の登録と変更した姿勢は次の PhysicsSystem の同期・step で検索へ反映される。

SphereCast は現行 native の形状近似を使用し、Point は掃引球の中心。native Collider の pointer を C# に渡さず、削除済み・別 Scene の古い World 登録は検索から除く。検索配列はその時点の結果で、対象の破棄に追従して配列を差し替えない。

~~~csharp
protected override void OnCollisionEnter(Collision collision)
{
    GameObject obstacle = collision.Other;
    if (obstacle is null || !obstacle.IsAlive)
        return;
    GameObject.Destroy();
}

protected override void OnTriggerEnter(GameObject other)
{
    if (other is not null && other.IsAlive)
        other.Destroy();
}
~~~

Collision は相手の GameObject と Point / Normal / Depth / RelativeVelocity / ApproachSpeed / Impulse の読取値を持つ。Stay / Exit の strength は native と同じ零。既存 PhysicsSystem の通知順を保ち、相手側には法線と相対速度を反転する。6 callbackは対応するpayloadを受けるprotected overrideで実装する。通知中の Time.DeltaSeconds は固定時間差で、剛体の変更は次の FixedUpdate へ渡す。例外は受信 Script だけを停止し、他の個体へ波及させない。

## 生成量と Scene 容量

[現行 Scene の容量契約](scene-capacity.md)では上限は 4096 Entity 枠である。非表示・非アクティブ、pool 内の待機中、破棄待ちも枠を使う。Prefab 一個に子孫があれば、その全 GameObject が必要枠に数えられる。配列の長さは Scene の作成可能数を増やさない。

Scene.InstantiateMany は single-root Prefab に限定する。既存の通常 Prefab が複数 root を持てる機能は変更せず、SDK の一括生成で複数 root を黙って捨てない。root 数だけでなく全 Prefab 階層の必要枠を overflow なく数える。主スレッドの同一処理中に容量を検証し、非公開の作成段階を終えてから Scene へまとめて公開する。公開前の作成失敗は新規作成分を取り消し、既存 Scene と出力を保つ。capacity precheck は予約ではないので、検証と作成の間にユーザー callback や別の作成処理を挟まない。

Awake / Start とユーザー通知は公開後に既存の初期化規則で行う。それ以降のゲームロジック・外部 I/O を巻き戻す保証は付けない。参照は既存 Prefab の GUID 再対応付けを利用する。配列への追加は生成で返された参照を使い、タグによる全 Scene 再検索を必須にしない。

4096 枠を超えるゲームでは、Entity 容量と storage の別設計が必要になる。描画だけの粒子や群集を全て GameObject にするかも用途で判断する。この C# API の設計から独立して評価し、実装済みの大規模対応として約束しない。

## callback とゲームロジック

作者は protected override void Update()、FixedUpdate()、LateUpdate() を書く。Awake、Start、OnEnable、OnDisable、OnDestroy、Connect、AfterReload、ReloadRetired も基底の同期callbackを override し、使用するものだけ書く。基底のcallback宣言、Hostの予約名、Analyzerは共通のcallback契約から生成する。名前・署名の誤りはC#コンパイラ、overrideの欠落・static / async等の不正なlifecycleはAnalyzerとHostで診断する。通常のC#メソッドやhelperは自由に使える。配布SDKでは [導入手順](../csharp-start.md) と、公開API/XML・SDKカタログから正しい宣言を確認できる。

| 入口 | 用途と利用条件 |
|---|---|
| Awake / Start | 開始処理。既存の全 Awake 後の Start を維持 |
| Update | 入力、意思決定、通常の非物理操作。Time.DeltaSeconds はフレーム差 |
| FixedUpdate | 物理前の固定ステップ。Time.DeltaSeconds は固定時間差 |
| LateUpdate | 通常更新後の追従。Time.DeltaSeconds はフレーム差 |
| Connect | 所有者付き購読の登録。ワールド変更を許さない |
| AfterReload | Play 差し替え後の延期処理等の再登録 |
| ReloadRetired | 旧世代のmanaged後始末。Engine APIを許可しない |
| 衝突・Trigger 通知 | 型付き payload と固定時間差。剛体変更は次の FixedUpdate へ渡す |
| その他のイベント・完了通知 | 実際の payload だけを引数へ渡す。時間積分用の DeltaSeconds は利用不可 |

Animation・root motion・Sequence・Navigation の native 通知も protected override へ同期配送する。binding を持たない Sequence EventTrack は `Events.Subscribe<SequenceEvent>` で受ける。値・権限・所有購読の契約は [Engine 通知](csharp-engine-notifications.md) を参照する。

Time / Input / Scene / Objects / Jobs / Events / DataAssets は Script の instance の窓口とする。Host の入口が実行元・コード世代・呼び出し ID・Phase / RunMode を持つ内部スコープを作る。普通の他 Script のメソッド呼び出しでは実行元を切り替えず、現在の権限を使う。Script 固有の窓口は対象 Script の生存・世代も検証し、登録仕事はその Script に所有させる。構築・復元中、呼び出し外、別スレッド、旧 token の要求を拒否する。GameObject / Component の参照は保持でき、各操作が現在のスコープと対象の寿命を検証する。

fault / 必須条件による停止はInspectorの理由表示、共通Query `script.status` で確認する。修正後はInspectorの「再検証して再開」またはAction `script.resume` を使い、Runtime必須参照の検証に成功した場合だけ停止を解除する。引数は `node` とinspect/configureから返された正確な `scriptId`、任意の `enabled`。成功済みAwake / Startは繰り返さず、失敗した開始段階やConnectは次tickで再試行する。無効・非active中のConnectは延期し、停止中のJobs / Eventsはreplayしない。コードreloadだけでは停止を自動解除しない。

Connect は初回 Start 成功後と、開始済み個体の再有効化で購読を登録する。無効化・fault・破棄・世代退役では仕事と購読を解除する。Play 差し替えでは Awake / Start を繰り返さず、AfterReload の後に有効な開始済み個体へ Connect を配送する。Jobs.Wait / WaitFrames / Repeat / Run / Tween / Timeline と Events.Subscribe / Publish は実装済みで、登録の所有者はサービスを持つ Script。時間積分用の Time.DeltaSeconds は JOB / EVENT callback では利用できない。

順序は初回が `Start → Connect`、Play 差し替えが `AfterReload → Connect`。自身の Connect で登録する購読へ、Start / AfterReload 内の同期 Publish は届かない。接続後の通知が必要なら `Jobs.WaitFrames(1, () => Events.Publish(value))` 等へ延期する。秒の Tween とフレーム待機の完了順は固定せず、両方を待つロジックでは各完了状態を確認する。

位置設定は Transform.WorldPosition = destination の通常の property 代入で書けるようにする。LocalPosition は親座標、WorldPosition はワールド座標。setter が入力検証と親子の即時同期を行い、別の SetPosition メソッドを併設しない。Transform 参照も保持可能とし、過去の操作権限・時間差・呼び出し ID を参照に保存しない。

入力の観測値は一フレーム内で安定させる。Fixed が零回・複数回のフレームでも、ジャンプ等の一回性要求はロジック側でラッチして一度消費する。InputPressed を全 Fixed 呼び出しへそのままコピーする形式を標準例にしない。

入力名は実行中の InputActionMap を正本にする。ボタンは `Input.Button("Jump").Held / Pressed / Released`、軸は `Input.Axis("MoveX").Value` で読む。文字列の名前を受けるのは型付きハンドルを取得する窓口で、名前の大文字小文字とボタン・軸の種別を確認する。未知名は無入力の false / 0 として通さず、名前と種別、`GetButtonNames` / `GetAxisNames` による発見方法を含む例外で知らせる。ボタンが押されていない、または登録済みの軸が零でも取得は成功する。

~~~csharp
ButtonAction m_jump;
AxisAction m_moveX;
bool m_jumpRequested;

protected override void Connect()
{
    m_jump = Input.Button("Jump");
    m_moveX = Input.Axis("MoveX");
}

protected override void Update()
{
    float horizontal = m_moveX.Value;
    if (m_jump.Pressed)
        m_jumpRequested = true;
}
~~~

ハンドルは callback 内で取得・使用し、毎回 `Input.Button("Jump").Pressed` と読む短い書き方も使える。cache する場合は Connect で取得し、コード差し替え時にも取得し直す。ButtonAction / AxisAction は名前だけを持つ非所有の値型で、過去の callback・thread・コード世代・操作権限を保持しない。読取時には現在の callback と最新の入力設定を検証するため、設定から名前を削除した後の古いハンドルも診断する。default 値の読取は未接続として拒否する。constructor / field initializer から Engine API を呼べるようにはしない。

`Input.HasButton(name)` / `HasAxis(name)` は押下・軸値に依存せず存在を返す。`GetButtonNames()` / `GetAxisNames()` は現在の登録順で全名前の snapshot を返し、後の設定変更は次の取得で反映する。caller の `Span<string?>` を渡す overload も同じ全件を返す。容量不足は `Required` と `Written=0` を返し output を変更しない。成功時も未使用の末尾は保持する。取得は1回の設定 snapshot を64KiB以下のpacketへ分けるため、全体1MiB上限や名前の暗黙truncateを置かない。既定名の生成定数を別の正本にせず、プロジェクト設定・リバインド・実行中の編集と同じ map から読む。KeyCode による GetKey / GetKeyDown / GetKeyUp は直接キーを扱う用途で維持する。

状態遷移は通常の enum / switch、時間は秒の値、操作はメソッド呼び出しで表す。計算と意思決定は必要な状態・入力・秒数を引数に取る通常の関数へ分けられる。小さい処理にも Settings / State の別クラスを必須にしたり、全 helper に巨大な Context を渡したりしない。

### 回転・拡縮と方向

通常の property とメソッドを使う。LocalRotation はローカルの Quaternion を読み書きし、WorldRotation はワールド回転の読み取り専用。LocalScale はローカル拡縮を読み書きする。Forward / Right / Up はワールド回転に +Z / +X / +Y の基底を掛けた方向で、取得だけのために親子のキャッシュを再計算しない。WorldScale の公開は今回含めない。

~~~csharp
Quaternion facing = Quaternion.FromAxisAngle(Vector3.UP, Mathf.ToRadians(90f));
Transform.LocalRotation = facing;
Transform.LocalScale = new Vector3(2f, 1f, 2f);
Transform.Rotate(Vector3.UP, turnSpeed * Time.DeltaSeconds);
Transform.LookAt(target.Transform.WorldPosition);
~~~

Quaternion.FromAxisAngle の角度と FromEuler の各成分はラジアン。FromEuler は native Math と同じ YXZ 内因順 Qy * Qx * Qz。Transform.Rotate(axis, degrees) は度数で、C++ ScriptTransformProxy と同じ delta * LocalRotation の順に回す。自分の軸を回転後の姿勢で解釈する別 overload は増やさない。長さ EPSILON 未満の axis は native の no-op だが、呼び出し権限と対象寿命の検証を省略しない。

LookAt の対象はワールド位置 [m]。更新前に親子の姿勢を同期し、native Transform.LookAt と同じ親回転の逆算を行う。対象との差の長さ二乗が 1e-6 未満なら回転を維持する。ローカル回転の setter は入力成分をそのまま正本へ保存し、子孫のワールド姿勢へ即時反映する。親子同期で生成するワールド回転は root がローカル値、親付きが親とローカルの正規化した積になる。WorldRotation の getter は描画用のワールドキャッシュを返し、読み取り時には再同期しない。

LocalRotation の書き込みは有限で、長さも有限かつ EPSILON 以上の Quaternion に限定する。LocalScale は有限なら零・負の成分も許可する。Rotate / LookAt も有限入力を要求する。失効した対象・別 Scene・不正な値・子孫の計算 overflow を拒否し、変更は未適用のままにする。有効で実体を持つ物理 body への回転・拡縮も専用物理 API ができるまでは拒否する。native が拒否すると既存 Script API と同じ例外診断で実行元を停止し、変更要求を再送しない。

Quaternion は XYZW の 16 byte 値型で、Engine や実行スコープに依存せず通常 helper や別スレッドでも計算できる。FromAxisAngle / FromEuler / LookRotation / Dot、Quaternion と Quaternion・Vector3 の積、Length / LengthSquared / Normalized / Conjugate / Inverse / Lerp / Slerp を提供する。Lerp / Slerp の t は clamp しない。Slerp は正規化済み入力を前提として短弧を使う。Normalized は長さ、Inverse と LookRotation は長さ二乗に native の EPSILON 判定を使うため、退化閾値を混同しない。退化した純計算の回復値は native と同じ IDENTITY で、非有限値の通常の float32 演算は置き換えない。default(Quaternion) は零で、回転の既定値は Quaternion.IDENTITY を明示する。Equals は Vector3 と同じ厳密な成分比較とし、native の NearlyEqual 演算子とは区別する。

Quaternion / Quaternion[] / List<Quaternion> は現在の保存 codec に含む。public は設定・保存、private + Setting は編集・保存、KeepOnReload は Play 状態保持に使える。保存は有限な XYZW を保持し、正規化や非退化を要求する Transform / Physics の変更条件とは分ける。回転の既定値には Quaternion.IDENTITY を使う。

### よく使う計算を SDK の操作にまとめる

位置の差、正規化、距離の clamp を毎回再実装せず、通常の Transform 操作と同じ契約を持つ helper を提供する。

~~~csharp
Transform.MoveTowardsWorld(
    target.Transform.WorldPosition,
    moveSpeed * Time.DeltaSeconds,
    stopDistance);
~~~

MoveTowardsWorld(destination, maxDistance, stopDistance = 0f) は段階 2 で実装した。destination はワールド位置、maxDistance は今回の最大移動量 [m]、stopDistance は停止半径 [m]。停止半径の内側では変更せず、外側では超過距離と maxDistance の小さい分だけ近づく。全入力を有限、距離を非負として事前検証し、既存 world 更新へ接続する。時間を内部で暗黙に掛けない。物理 body は実装済みの RigidBody API を使い、Transform helper で移動しない。

小さいゲーム用 helper は通常の C# として書ける。ScriptApi に入れるものは複数用途で共有でき、単位・失敗・座標を固定できる操作に絞る。追跡・巡回等のゲーム判断まで Engine 内へ移さない。

## 他の Script を型付きで操作する

Script も Component として取得し、public メソッドを普通に呼ぶ。Unity の GetComponent<T>() を参考にし、作者に native Component と C# Script の取得 API を使い分けさせない。[Unity の GetComponent](https://docs.unity3d.com/6000.0/Documentation/ScriptReference/GameObject.GetComponent.html)。文字列のメソッド名、Try と out、操作用 lambda、interface・partial・生成 wrapper の手書きを要求しない。

~~~csharp
if (target is not null && target.IsAlive)
{
    HealthComponent health = target.GetComponent<HealthComponent>();
    if (health is not null)
    {
        health.TakeDamage(attackDamage);
        float remainingHealth = health.RemainingHealth;
    }
}
~~~

~~~csharp
/// @file    HealthComponent.cs
/// @brief   体力を保持し、ゼロになった対象の破棄を要求する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
using System;
using Fbzz;

class HealthComponent : Script
{
    public float initialHealth = 100f;

    [KeepOnReload]
    float m_health;

    public float RemainingHealth => m_health;

    protected override void Awake()
    {
        m_health = MathF.Max(1f, initialHealth);
    }

    public void TakeDamage(float damage)
    {
        if (!float.IsFinite(damage) || damage <= 0f || m_health <= 0f)
            return;

        m_health = MathF.Max(0f, m_health - damage);
        if (m_health <= 0f)
            Scene.Destroy(GameObject);
    }
}
~~~

KeepOnReload は Play 差し替えで体力を保持するための任意指定。public field の設定と private 状態の分離は普通の C# として行い、状態クラスの別定義を必須にしない。

| API | 普通の使い方 |
|---|---|
| GameObject.GetComponent<T>() where T : Component | 指定 GO の T。無ければ null |
| GetComponent<T>() | Script 自身が付いた GO の T |
| GameObject.GetComponents<T>() / GetComponents<T>() | 当該 GO の T を通常の T[] で返す。無ければ空配列 |

~~~csharp
HealthComponent health = GetComponent<HealthComponent>();
if (health != null)
    health.TakeDamage(10f);

HealthComponent[] healthComponents = target.GetComponents<HealthComponent>();
foreach (HealthComponent item in healthComponents)
    item.TakeDamage(10f);
~~~

SDK 内の Script は Component を継承し、ゲーム作者は class HealthComponent : Script のまま書く。GetComponent の公開対象は Script 派生型と SDK に対応を登録した native Component facade とする。SDK 内部で managed 型 / ID 取得と native binding を分け、C++ の実装型やポインターを C# の T にキャストしない。初版は Component 自体を返す異種全件取得へ広げない。

`DrawGizmos` / `DrawGizmosSelected` の readonly preview では、generic 型が登録済み native facade の取得と、保持した native `ComponentRef<T>` の `TryGet` / `IsAlive` / `IsMissing` だけを追加で許可する。managed Script の実体取得と参照の `Resolve` は許可しない。API catalog の `conditionalAllowedCallbacks.condition` と配布 XML がこの条件を示し、通常の `allowedCallbacks` の read 権限は広げない。caller-buffer の集合取得は後続 packet・UTF8・有限値まで検証した後に全件をコピーし、不正応答でも caller の先頭・末尾を変更しない。

native facade は Transform、RigidBody、Collider 各型、Animator、AudioSource、UI 各型、Camera、NavMeshAgent、CharacterController を登録する。native C++ Script を managed の T として取得しない。

単数の managed Script は、当該 GO の現行世代・公開済み・T へ代入可能な ScriptEntry を登録順に調べ、最初の一致を返す。複数なら GetComponents で同じ順序の全件を取り、ゲームが選ぶ。Unity は単数の最初の一致を返し、検査順は未定義とするため、この順序保証は FBZZ の契約。[Unity の複数一致](https://docs.unity3d.com/6000.0/Documentation/ScriptReference/GameObject.GetComponent.html)。native facade は公開 binding の型に対応する Component を取得する。

未検出・Missing・失効は null / 空配列として扱う。別 Scene・実行スコープ外・別スレッド・旧世代の要求は診断して拒否し、成功結果と区別する。開始済みを要求せず、enabled / faulted / requirementsBlocked は自動 callback 配送の停止であり、明示した通常メソッドを一律に拒否する条件にはしない。初期化済み状態を必要とする public メソッドはゲームが契約を定める。

通常の呼び出しは Host の新しい入口にならない。A の Update から B.TakeDamage を呼ぶと、Engine 操作は A の Phase / RunMode / 時間差 / 権限を使う。B 自身の GameObject / Scene / Events 等の窓口は B の生存・現行世代も照合する。B の登録仕事・購読の所有者は B とし、disabled / faulted の配送制約を保つ。disabled の個体で登録した仕事も停止状態を迂回して配送せず、faulted / requirementsBlocked owner への新規登録は拒否する。

GameObject と native Component facade は共有可能な非所有 ID 参照で、操作には現在の実行元の権限を使う。managed Script 実体も、現行コード世代で通常の field / local / collection に保持してよい。同じ callback 内の配列や同期 helper を禁止しない。通常の C# body を Host が全て捕捉・隔離する仕様ではなく、任意の外部保管先に残した旧 Script の純 managed 更新を CLR が完全禁止する保証はない。

async void の公開ゲーム操作と、世代をまたいで Script / 実行権限を Task 等へ残す形を診断対象にする。同期例外が最外の Host 入口へ伝わった場合、その実行元 A を faulted にする。B だけの停止、public メソッド全体の rollback、自動再送、呼出循環の自動拒否は約束しない。通常の非同期計算は利用できるが、Engine スコープを持ち越せず、延期処理は所有者付きサービスで行う。通常呼び出しへ特別な call site 検証・lambda 制限を要求しない。

TryGetScript、TryCall と呼び先ごとのスコープ切り替えは前 Draft から取り下げた。現行ゲーム Script は C# のみで、C++ Engine は公開イベント schema を通して通知を配送する。

### Component 参照を取得して使い回す

~~~csharp
HealthComponent m_health;

protected override void Awake()
{
    m_health = GetComponent<HealthComponent>();
}

public void TakeHit(float damage)
{
    if (m_health is not null && m_health.IsAlive)
        m_health.TakeDamage(damage);
}
~~~

Awake で取得して保持する普通の書き方を対応対象にする。native facade は Scene / Entity / Component の寿命を検証する ID 参照。managed Script は現行世代の CLR 個体。毎フレームの GetComponent を必須にしない。保持中の対象が破棄される場合は IsAlive を確認し、CLR null と失効を同一視しない。native facade と Transform の `==` / Equals は取得時 identity の純粋な比較で、別の CLR wrapper でも同じ個体なら等しく、失効しても等価性は変わらない。Script は CLR 参照を比較する。

`owner.UI` / `Script.UI` は UI の所有者サービス、`owner.Audio` / `Script.Audio` は 2D 再生・合成音・所有 loop のサービス。Component として追加・取得せず、差し替え後も所有者 property から使う。`GetComponent<UIText>()` / `<UIImage>()` / `<UIButton>()` 等は実 widget、`GetComponent<AudioSource>()` は実音源の寿命を保持し、その型に関係する操作だけを持つ。widget 横断操作は `owner.UI` と明示する。除去予約中の Component はすぐ未検出になり、同型の再追加は Cleanup 完了後に行う。次の callback が Cleanup 後である保証はない。

大量の取得は作者が buffer を確保して `GetComponents<T>(buffer.AsSpan())` を使える。結果の Required が全件数、Written が書いた件数。不足なら Written=0 で buffer は未変更なので、Required に合わせて再確保できる。成功時は全件を書き、末尾の余りは保持する。型 cache 初期化後は取得側の集合一時配列を確保しない。通常の `GetComponents<T>()` も全件を返すが、戻り値の配列は都度所有する。内部の packet 上限をユーザーの集合上限にしない。

Script ごとに配列を一度確保して再利用する。件数が変わるゲームでは、不足通知を受けた後の初期化・状態切替など、作者が選んだ安全な時点で容量を調整する。毎フレームの再確保や static な共有 buffer は必要ない。

~~~csharp
using System;
using Fbzz;

public sealed class ComponentReader : Script
{
    private Script?[] m_components = new Script?[16];
    private int m_required;

    protected override void Awake() => PrepareCapacity();

    public void PrepareCapacity()
    {
        CollectionReadResult count = GetComponents<Script>(Span<Script?>.Empty);
        if (count.Required > m_components.Length)
            m_components = new Script?[count.Required];
        m_required = 0;
    }

    protected override void Update()
    {
        CollectionReadResult result = GetComponents<Script>(m_components.AsSpan());
        if (!result.Complete)
        {
            m_required = result.Required;
            return;
        }
        for (int index = 0; index < result.Written; index++)
        {
            Script? component = m_components[index];
            if (component is null || !component.IsAlive) continue;
            // @note この個体へのゲーム固有処理をここで行う。
        }
    }
}
~~~

`m_required` が非零なら、所有者の準備処理などから `PrepareCapacity()` を現在の callback 内で明示的に呼ぶ。managed 参照の配列を stackalloc しない。

`Animator.GetCurrentBlendWeights(buffer.AsSpan())` は同じ Required / Written 契約。buffer は再利用できるが、非空の名前は結果の所有文字列として確保する。小さい transfer は stack、大きい結果は pool 初期化後の固定 packet で転送し、全結果の byte 一時配列を作らない。通常版は全件の配列を返す。どちらも件数・total bytes を黙って打ち切らない。

Play 差し替えでは、Script が直接持つ mutable instance field の GameObject / Component 参照を、public / private や保存属性によらず値 ID へ退避する。全候補を構築してから対象の新 CLR 個体へ再接続し、旧 CLR 個体をコピーしない。Script は owner GUID と保存 ID `savedScriptId` で照合し、型名・配列順に依存しない。同型 Script が複数あっても個体を取り違えない。元の Scene / GameObject の実行時寿命も一致させ、同じ保存アドレスの別実体へ旧 cache を復活させない。

`node.components` の各 Script 行と `script.inspect` は `savedScriptId` と現在の `scriptId` を別々に返す。永続アドレスは node の `id` と `savedScriptId` の組である。再生成・Play clone・保存復元後は同じ保存 ID の行を一覧から再取得し、その行の現在の `scriptId` で検査・編集する。型名や index で同型の複数個体を推測せず、保存 ID を実行時 handle の代わりに直接渡さない。

native facade は Component の追加時に一元発行した寿命 token を保持する。削除後は IsAlive が false、同型を再追加しても保持済み facade は失効したままになる。新しい個体は明示的な GetComponent で取得する。dense 配列の swap と Scene の move は生存個体の token を保持し、Clear は失効させる。Transform は GameObject の寿命に従う。保存ファイルには runtime token を書かず、保存 ID と runtime 寿命を混同しない。

対象の消失・型変更は null にし、通常の field と if で未設定を分岐する。保存する GameObject の欠損 GUID は既存の保持契約に従う。static、任意 helper の object graph、nested collection、Task、delegate の中まで自動的に再接続しない。Script の直接 field にある一次元の GameObject[] / Component[] / List<GameObject> / List<Component> と型付き Script / native Component の配列・List は、要素ごとの参照 ID を退避し、順序・null・重複を保持する。正式 codec に対応した保存対象 / KeepOnReload の GameObject 参照も path ごとの元の実体 token で復元し、同 GUID の再検索で失効を取り消さない。参照再接続とディスク保存の対応範囲は catalog の schema に従い、private のキャッシュに保持用属性を要求しない。

## 設定・状態・観測

| 宣言 | 通常 Inspector | ディスク保存 | Play 差し替え |
|---|---|---|---|
| public の対応 field | 編集 | 保存 | 現在値を復元 |
| private の対応 field | 非表示 | 保存しない | 既定値 |
| private + Setting | 編集 | 保存 | 現在値を復元 |
| private + KeepOnReload | 非表示 | 保存しない | 現在値を復元 |
| public + NonSerialized | 非表示 | 保存しない | 既定値。KeepOnReload の併用は可能 |
| public property | 自動評価しない | 保存しない | 自動復元しない |

直接の GameObject / Component 参照 field は、private / NonSerialized でも前節の参照再接続を行う。上表の private の既定値へ戻る規則は通常の数値等の内部状態に適用する。参照再接続はディスク保存と別であり、KeepOnReload の数値状態保持も別の用途とする。

通常の設定は public、内部状態は private で足りる。public が設定と外部アクセスの両方を選ぶことは明示し、アクセスを絞る用途だけ Setting を使う。public の const / static / readonly、property、Script SDK の基底サービスは自動保存しない。非対応の public mutable field は保存・編集から除外し、警告と catalog の非対応理由を返す。普通の C# としては使用でき、NonSerialized を付ければ非保存の意図を明示できる。明示した Setting / KeepOnReload の非対応型はエラーにする。任意の collection や object graph を public にしただけで保存できると扱わない。

Debug inspect では対応した private field も属性なしで読み取り、Inspector と AI が状態を見られるようにする。任意の property getter・ユーザーコードを自動実行しない。未対応値は型と取得不可の理由を返す。読み取りは更新の実行スコープを作らず、差し替え中・復元中の途中状態を公開しない。

Setting、ValueRange、RequiredReference、単位、座標、field key を同じ schema に記録する。GameObject や対応した一次元配列も public の規則で設定できる。public と Setting は重ねても同じ設定として一回だけ処理し、Setting と NonSerialized の競合は診断する。KeepOnReload の保持 codec と観測可能型は別に定め、delegate、Task、待機位置を復元しない。

## AI による参照の一括設定

コードの編集と、シーン固有の GUID 接続を AI が同じ作業内で完了できるようにする。Inspector のクリック回数を減らすだけでなく、SDK schema と共通の Editor Op から書き込める形を用意する。

1. 型 catalog からアタッチ対象、field key、単体 / 配列、対象型、必須条件を読む。
2. Scene 全体 / 指定 root の直下 / 指定 root の子孫 / 指定 Prefab 個体の定義を区別し、件数と GUID を確認する。
3. Script の追加、設定値、複数の参照フィールド・配列変更を一つの計画へまとめる。
4. 計画の候補・並び・差分を preview し、返された plan token を保持する。
5. 同じ計画を apply し、要求検証と Playtest で動作を確認する。

段階 2 の script.configure selector は、名前または path の root、直下 / 子孫 / Scene / self、タグ、inactive の条件を受け取る。名前を GUID へ解決して保存し、表示 path を保存 ID にしない。scene_find 自体の root / subtree scope 拡張と、ディスク上の Prefab 定義を直接設定する計画は後続案とする。

preview は選択 root、対象 GUID、順序、候補数、依存 fingerprint と設定差分を保持する。Prefab の各実体は指定した root の範囲へ限定する。段階 2 の全件 selector は Hierarchy 順、明示配列は指定順を使う。Prefab 定義を直接変更する計画の依存情報は後続案で追加する。

追加の preview は Scene に公開していない候補 Script と確定前の設定を plan に保持し、apply で全件検証後に公開する。破棄・失効した plan は候補と Host の保持値を解放する。追加の Undo 後に Redo する場合は新しい個体へ保持済み補正結果を復元する。準備中 plan の保持上限は 8 件で、世代変更時には失効する。

preview は UI と API が共有する確認データであり、通常の変更で人のクリック承認を必須にするものではない。apply は Scene / schema / Hierarchy / Prefab 定義の依存状態、root と候補の有効性、指定した直下・子孫・Prefab 個体への所属を再検証し、古い計画を拒否する。現行 UndoStack の revision だけでは Undo / Redo や直接変更を覆えないため、その値を全体の変更世代とみなさない。preview 後に検索条件を黙って再評価して別の候補へ適用しない。

配列編集は Replace / Append / Remove / Reorder を区別する。Replace は完全な順序付き配列、Append は指定要素を末尾へ追加、Remove / Reorder は添字を使って重複参照も区別する。更新は一回の Undo にまとめ、途中のフィールドだけ適用された状態を公開しない。大量計画を分割転送する場合も、転送完了と検証が終わるまで Scene へ反映しない。

Authoring の JSON 設定更新は、同じ RFC6901 leaf path と同じ GUID の参照 token を保持する。数値設定だけを変えて失効参照を復活させない。同じroot内の別pathにあるGUIDを移動した場合も、保持されたObjectId / SceneLifetimeが一意ならtokenを再利用し、既存rootにない新GUIDだけを新規に解決する。同GUIDの異なるtokenがあり移動を特定できない場合は、incoming同GUID leaf全件へ明示的な再選択intentを指定しない限り拒否する。同じ GUID を明示的に選び直す場合は Configure の `rebindReferences: ["/settings/target"]` を指定する。path は `$go` の内部ではなく参照 leaf 自体で、配列要素は `/settings/targets/0`、名前の `~` と `/` は `~0` と `~1` にescapeする。未知・重複・参照以外・nullのleafへのintentは設定全体を拒否する。Inspector のpickerは再選択intentを自動で付ける。同GUID列のJSONではidentityだけの並べ替えを表せず、位置別tokenを保持する。InspectorもGUID列が同じでnative IDだけが移動する曖昧な操作を診断して取り消す。Undo/Redo は保存した exact reference token を照合・復元し、同じ GUID の別個体へ再接続しない。token だけの移動は明示的な referenceMoves として伝え、値だけでは特定できない操作を黙って推測しない。

scene_find は1page最大500件を返す。`nextOffset` が数値なら同じ条件の次要求に `offset` として渡し、nullまで取得すると全一致へ到達できる。`count` は全一致数、`returned` は今回の件数、`truncated` は続きの有無。順序は既存のScene階層走査順で、`sample=request` はpage間のScene変更を同時点に固定しない。編集中に対象が変わる場合は検索を取り直し、欠けた一覧を全件計画として使わない。run_transaction は最大128commandsである。script.configure の全件 selector と接続計画は Editor 内で対象を解決するため、対象一個につき一commandを送らずに設定できる。

script.inspect / script.catalog は読み取りの接続点で、Script 追加と field 一括更新は script.configure.preview / apply の共通 Op を使う。Inspector も読み取りから設定案を作り、同じ Op の正確な個体照合・全件検証・一回の Undo を通す。Play/Pause では既存 managed Script の設定を一時編集でき、専用 Undo を使う。停止時は Edit の元状態を戻し、残す変更は明示 capture・review・adopt で Edit の Undo へ接続する。Play 中の Script 追加・EditorAction・native 設定編集は対応しない。script.api.catalog は現行 SDK の signature・単位・座標・Phase・失敗条件を公開する。

追加と初期設定は一個の Script.Configure Op の非公開候補で準備してから公開する。現行 run_transaction は事前 Scene から command を構築して逐次実行するため、追加の後に設定 command を並べるだけではこの契約を満たさない。適用済み catalog 世代、正確な Script 個体 ID、再送の operation ID、commit receipt と保存状態を含む契約は [制作経路](ai-script-workflow.md)を正本とする。

`script.inspect` の通常取得は `sample: summary`、`snapshot: false` の少量投影である。巨大な配列・List・文字列は値を切り捨てず、`$deferred` の RFC 6901 path と `total` を返す。文字列 summary の単位は `utf16_code_units` であり、全文字列を UTF8 へ変換するための走査を行わない。通常 summary は複数フィールドの同時点を保証する観測 handle を作らない。

同じ query に `path`、`start`、`count` を指定すると、選択した subtree だけを不変の observation として取得する。返された `observation.observationId` を以後の query に指定し、`nextStart` から続きを読む。配列・List の総件数に 4096 / 32768 といった表示上限を持たない。snapshot は必要な場合だけ常駐メモリから所有する退避領域へ移し、全件 JSON 化してからページへ切り取らない。設定後の live 値変更は取得済み snapshot のページへ混ざらない。`capturedRevision` は snapshot の内容を示し、現在の設定 revision ではない。

文字列の snapshot page は `unit: utf8_bytes` とし、`start` は UTF8 文字境界、`count` は要求 byte 数である。途中の文字を返さず、返された `nextStart` を使う。大きな入れ子の値は次の `$deferred` path へ降りて読む。`wireBytes` は一応答の容量で最小 2048 byte、省略時 65536 byte。field の総データ容量とは別である。参照は page の RFC 6901 path ごとに正確な GameObject ID と Scene 寿命を返し、GUID の再解決を行わない。

取得済み handle は同じ owner・Script 個体・managed 世代にだけ使える。成功した page 取得で idle 期限を延長する。`observationId` と `close: true` は保持資源を明示解放し、Script の破棄・世代退役・期限切れでも所有する領域だけを回収する。Host の handle 件数・常駐メモリ・退避容量は設定可能な観測資源予算であり、設定値自体の総件数制限ではない。容量不足は必要な wire 容量または解放・予算変更を診断する。catalog は実際に公開済みの SDK / Op を示す。

## 評価手順

[GreenWare の旧計測](../measurements/greenware-script-authoring-2026-09-29.md)は SDK 0.9.1 の C++ Script authoring 時点の記録であり、現行 SDK の C# の記述量や互換性を示さない。C# の制作時間、AI の成功率、一括 API の処理速度は別に評価する。

| 課題 | 合格条件 |
|---|---|
| 単体のプレイヤー・追従カメラ | 引数なし callback、自由な GameObject 名、private / namespace / 登録の省略、属性ゼロの基本設定、正しい時間・座標、親子同期 |
| 追跡・巡回・停止の敵 | 任意 target と空配列でモードを組み立てられ、常時必須条件で巡回個体を排除しない |
| 公開メソッドによる攻撃 | GetComponent と普通の call、型一致・複数時の順序・世代・同じ Scene、実行元の権限と例外帰属、受け手所有の登録仕事、長期保持の診断 |
| Awake で取得する Component cache | private field へ属性なしで保持。GO 寿命は失効、native facade は現行 Component の view、Play 差し替えで managed の正確な新個体へ再接続。自己参照・相互参照・消失・復元失敗を確認 |
| 巡回地点配列 | 手動順序、重複、空配列、未設定、欠損 GUID が保存往復する |
| 子・孫・兄弟の参照 | 指定 root だけで選び、root 自身・別階層・同名の先頭へ接続しない |
| 深さ 16 / 128 / 1024 の階層 | 接続・全件探索・保存復元・破棄・Prefab 差し替えを確認。GUID 対応付け後の参照利用で階層の再検索をしない |
| 破損した階層 | 循環・無効リンクを診断し、無限走査・部分結果の成功・assert だけの読込失敗を避ける |
| 同一 Prefab の複数個体 | 実体名の一意化後も各個体の部位へ接続し、Apply / 保存 / 再生成で保持する |
| 親参照だけの変更 | 保存済み子参照を黙って変更せず、関連参照を明示した一括計画で切り替える |
| 敵の群れ | 一個の Script から制御でき、生成・破棄で保持配列を黙って差し替えない |
| 100 / 1000 対象の移動 | foreach と batch が同じ結果。親子・重複・失効対象も確認 |
| Prefab の一括生成 | 子孫を含む容量検証、公開前失敗で部分生成なし、開始通知の順序維持 |
| AI の配列接続 | 500 件超を落とさず処理し、128 commands を越える個別転送を不要にする |
| 物理・UI・差し替え | Fixed 一回性、購読一回性、所有者破棄・旧世代の拒否、必要な状態復元 |

対象数は全 Scene の 4096 枠以内で試験し、容量超過を別の失敗シナリオにする。バッファ不足時の部分出力禁止、Scene / schema が変わった計画の拒否、一回の Undo、属性競合も確認する。

作者の比較は同じ要件と SDK catalog で Unity の最小例・前 Draft・本案を並べ、必須属性数、参照型の記号・初期化の定型、接続の宣言と手操作、初回生成・ビルド成功、動作合格までの修正回数、変更箇所、保存から反映までを記録する。移動・追跡・配列制御・単純なダメージは、属性なしで実装可能を初期の目標にする。private の Play 状態保持等、追加要件の属性は別に数える。性能は起動直後と JIT 初期化後を分け、Script callback 数、境界呼び出し数、native 処理、managed 確保、GC、長いフレームを測る。bulk の改善量や C# の速度差は実測後に示す。

## 参考

- [Unity: 保存対象の型、配列と List](https://docs.unity3d.com/6000.0/Documentation/Manual/script-serialization-rules.html)
- [Microsoft: C# 配列](https://learn.microsoft.com/en-us/dotnet/csharp/language-reference/builtin-types/arrays)
- [Microsoft: private と通常のクラス内アクセス](https://learn.microsoft.com/en-us/dotnet/csharp/language-reference/keywords/private)
- [Microsoft: Nullable の注釈・警告 context](https://learn.microsoft.com/en-us/dotnet/csharp/language-reference/compiler-options/language#nullable)
- [Microsoft: Span<T>](https://learn.microsoft.com/en-us/dotnet/api/system.span-1?view=net-10.0)
- [Unity: Transform.Find の直下・名前パス・非再帰の契約](https://docs.unity3d.com/6000.0/Documentation/ScriptReference/Transform.Find.html)
- [Godot: Node の get_node / find_child と検索範囲](https://docs.godotengine.org/en/stable/classes/class_node.html)

上記は通常の言語・コレクションと既存エンジンの保存設計の参考であり、Objects、一括 Editor 計画が既存エンジンに同じ API として存在する根拠ではない。
