# Game AI Layer — Behavior Tree / Blackboard / Perception

NavMesh・Patrol・Sensor という「実行手段」は揃っている一方、
**「いつ何をするか」を決める意思決定層が存在しない**。
その層を Behavior Tree + Blackboard として設計する。

## 現状の棚卸し

| 実装済み | 役割 | AI 層から見た位置づけ |
|---------|------|-------------------|
| `NavMeshSurfaceComponent` / `NavMeshBakeSystem` | NavMesh 生成 | 環境 |
| `NavMeshAgentComponent` / `NavigationSystem` | 経路探索・移動 | **アクチュエータ** |
| `NavMeshOffMeshLinkComponent` | ジャンプ・梯子 | アクチュエータ |
| `NavMeshPatrolComponent` / `NavMeshPatrolSystem` | ウェイポイント巡回 | **固定された振る舞い 1 種** |
| `NavMeshSensorComponent` / `NavMeshSensorSystem` | 視野角 + LoS 検知 | **センサー** |
| `AnimatorComponent` (BlendTree / StateMachine) | アニメーション | アクチュエータ |

**穴**: センサーとアクチュエータの間に、状況に応じて振る舞いを選ぶ層がない。

現状 `NavMeshSensorComponent::autoChase` が「見つけたら追う」を
**センサーの中に直接ハードコードしている**。これは典型的な症状で、
「見つけたら逃げる」「見つけたら仲間を呼ぶ」「体力が低ければ追わない」を
表現しようとした瞬間に破綻する。

## 設計方針

- **Behavior Tree を中核に据える。**
  **WHY BT か**:
  - **有限状態機械 (FSM)** は状態数 N に対して遷移が O(N²) に増え、
    「全状態から被弾リアクションへ」のような横断遷移で爆発する。
  - **Utility AI** はスコア関数の調整が非直感的で、
    「なぜこの行動を選んだか」のデバッグが著しく難しい。
  - **BT** は木構造で優先順位が視覚的に読め、サブツリーの再利用が効き、
    既存の `ImNodes` ベースのグラフエディタ (`AnimationGraphPanel` /
    `VFXGraphCanvas`) の資産をそのまま流用できる。
    ポートフォリオとして「実行中の木がハイライトされる」画は説得力が高い。

- **既存コンポーネントを置き換えず、リーフノードとして呼び出す。**
  `NavMeshAgentComponent` / `NavMeshPatrolComponent` は BT のアクションノードから
  操作する。**WHY**: 既存シーンとスクリプトを壊さないため。
  BT を付けないエンティティは今までどおり Patrol/Sensor 単体で動く。

- **アセット (木の構造) とインスタンス (実行状態) を分離する。**
  **WHY**: 敵 100 体が同じ木を使うとき、木の構造を 100 個複製するのはメモリの浪費であり
  キャッシュ効率も悪い。構造は `BehaviorTreeAsset` に 1 つ、
  実行状態は `BehaviorTreeComponent` 側のノード数ぶんの配列に持つ。

---

## 1. Blackboard

エージェントの「知っていること」を型付きで保持する共有データ領域。

```cpp
// キーは実行時文字列比較を避けるため、Asset ロード時に整数 ID へ解決する。
// WHY: BT は毎フレーム数十回キーを引く。文字列ハッシュ比較がホットパスに乗ると
//      エージェント数に比例して無視できないコストになる。
using BlackboardKey = uint16_t;

enum class BlackboardType : uint8_t { Bool, Int, Float, Vector3, Entity, String };

class Blackboard {
public:
    // 型が一致しない場合は false を返し、値を書き換えない (assert はしない)。
    // WHY: BT アセットとスクリプトが別々に編集されるため、型不一致は
    //      「回復可能なオーサリングミス」であってプログラムのバグではない。
    //      Docs/conventions/error_handling.md の方針に従い bool 戻り値にする。
    bool SetBool   (BlackboardKey k, bool v);
    bool SetInt    (BlackboardKey k, int v);
    bool SetFloat  (BlackboardKey k, float v);
    bool SetVector3(BlackboardKey k, const math::Vector3& v);
    bool SetEntity (BlackboardKey k, EntityID v);
    bool SetString (BlackboardKey k, std::string_view v);

    bool GetBool   (BlackboardKey k, bool& out) const;
    // ... 以下同様 ...

    // 値が最後に書かれたフレームを返す。「N 秒以内に更新されたか」の判定に使う。
    [[nodiscard]] uint32_t GetLastWriteFrame(BlackboardKey k) const;

    void Clear();
};
```

**WHY `GetLastWriteFrame` を持つか**: 「最後にプレイヤーを見てから 5 秒経ったら
警戒を解く」という記憶の減衰は AI の頻出要件で、
これを各ノードがタイマーを自前で持って実装すると同じコードが散乱する。
Blackboard 側が書き込み時刻を持てば `IsSet(key, withinSeconds)` 一発で書ける。

### 既定キー

Perception システムが自動で書き込む予約キー。

| キー | 型 | 内容 |
|------|----|------|
| `Self` | Entity | 自分自身 |
| `TargetEntity` | Entity | 現在の敵対対象 |
| `TargetPosition` | Vector3 | 対象の現在位置 (視認中のみ更新) |
| `LastKnownPosition` | Vector3 | 最後に見た位置 |
| `HasTarget` | Bool | 対象を視認中か |
| `HomePosition` | Vector3 | 初期位置 (帰還先) |
| `Health01` | Float | 体力の正規化値 |

---

## 2. Behavior Tree

### ノード種別

```cpp
enum class BTStatus : uint8_t { Success, Failure, Running };
```

**Composite (子を持つ)**

| ノード | 挙動 |
|-------|------|
| `Sequence` | 子を順に実行。1 つでも Failure なら Failure。全部 Success で Success |
| `Selector` | 子を順に実行。1 つでも Success なら Success。全部 Failure で Failure |
| `Parallel` | 全子を同時実行。成功条件は `successPolicy` (`RequireOne` / `RequireAll`) |
| `RandomSelector` | 重み付きランダムで子を 1 つ選ぶ |

**Decorator (子を 1 つ持つ)**

| ノード | 挙動 |
|-------|------|
| `Inverter` | Success ⇄ Failure を反転 |
| `Succeeder` | 子の結果に関わらず Success |
| `Repeat` | 子を N 回 (または無限に) 繰り返す |
| `Cooldown` | 直前の実行から N 秒経つまで Failure |
| `Blackboard Condition` | Blackboard の条件を満たすときだけ子を実行 |
| `Time Limit` | N 秒を超えたら Failure で打ち切る |

**Leaf — Action**

| ノード | 対応する既存機能 |
|-------|---------------|
| `MoveTo` | `NavMeshAgentComponent::SetDestination` |
| `Patrol` | `NavMeshPatrolComponent` を 1 ステップ進める |
| `Wait` | — |
| `LookAt` | Transform 回転 |
| `PlayAnimation` | `AnimatorComponent` のトリガー設定 |
| `PlaySound` | `AudioSourceComponent` の one-shot (→ `audio-system.md` の SoundCue) |
| `SetBlackboard` | Blackboard 書き込み |
| `RunScript` | ユーザースクリプトのメソッド呼び出し (後述) |

**Leaf — Condition**

`HasTarget` / `IsTargetInRange` / `IsHealthBelow` / `BlackboardCompare` / `HasLineOfSight`

### 実行モデル

**アセット (共有・不変)**

```cpp
// 木をフラットな配列で持つ。子は連続領域に置き、[firstChild, firstChild+childCount) で参照する。
// WHY: ポインタ木だとノードごとにキャッシュミスが出る。
//      配列化すれば深さ優先の走査順とメモリ順が一致し、プリフェッチが効く。
struct BTNode {
    BTNodeType type;
    uint16_t   firstChild  = 0;
    uint16_t   childCount  = 0;
    uint16_t   paramOffset = 0;  // パラメータプールへのオフセット
};

struct BehaviorTreeAsset {
    std::vector<BTNode>        nodes;      // [0] がルート
    std::vector<uint8_t>       params;     // ノードパラメータの型消去プール
    std::vector<BlackboardDef> blackboard; // キー定義 (名前 → ID + 型 + 既定値)
};
```

**インスタンス (エージェントごと)**

```cpp
struct BehaviorTreeComponent {
    std::string treePath;             // .behaviortree アセット
    bool  enabled  = true;
    float tickRate = 0.1f;            // 評価間隔 [s]。0 で毎フレーム

    // --- ランタイム ---
    AssetHandle<BehaviorTreeAsset> m_asset;
    Blackboard                     m_blackboard;
    std::vector<uint8_t>           m_nodeState;   // ノードごとの Running 状態
    std::vector<float>             m_nodeTimers;  // Cooldown / Wait / TimeLimit 用
    uint16_t                       m_runningLeaf = 0xFFFF;
    float                          m_tickTimer   = 0.0f;
};
```

**WHY `tickRate` を既定 0.1 秒にするか**: BT の評価は毎フレームである必要がない。
人間の反応速度は 200ms 程度で、100ms 間隔なら知覚上まったく違和感がない。
敵 100 体を毎フレーム評価すると純粋に無駄で、
1/6 に間引けばそのぶんを描画に回せる。**ただし位相をずらす**
(エンティティ ID から初期オフセットを与える) — 全個体が同じフレームに
評価されるとそのフレームだけスパイクする。

### Running 状態の保持と中断

`MoveTo` のように複数フレームにまたがるノードは `Running` を返す。
次の tick では**ルートから再評価せず、前回の Running リーフから再開する**。

**ただし Decorator に `observerAborts` を持たせ、条件変化で中断できるようにする。**

```cpp
enum class AbortMode : uint8_t {
    None,        // 中断しない
    Self,        // 自分のサブツリーが Running のとき、条件が偽になったら中断
    LowerPriority, // 自分より右の (優先度が低い) 枝が Running なら中断して自分を実行
    Both
};
```

**WHY 必要か**: これがないと「巡回中にプレイヤーを発見しても、
現在のウェイポイントに着くまで反応しない」という致命的に鈍い AI になる。
`LowerPriority` は BT が FSM に対して優位を持つ最大の理由で、
これを省くと BT を採用する意味の大半が失われる。

実装は「Running 中の毎 tick で、Running リーフより**左側**にある
`observerAborts` 付き Decorator の条件だけを再評価する」方式にする。
**WHY 全ノードを再評価しないか**: 木全体の再評価は O(N)。
中断候補は左側の Decorator のみなので、その集合を
アセットロード時に事前計算しておけば O(中断候補数) で済む。

---

## 3. Perception System

`NavMeshSensorComponent` を「センサー = 事実の収集のみ」に純化し、
判断は BT に委ねる。

```cpp
struct PerceptionComponent {
    // --- 視覚 ---
    bool  sightEnabled   = true;
    float viewDistance   = 15.0f;
    float viewAngleDeg   = 110.0f;
    // 近距離では視野角に関係なく気づく (背後 1m は「気配で分かる」)
    float peripheralRadius = 1.5f;

    // --- 聴覚 ---
    bool  hearingEnabled = true;
    float hearingRadius  = 20.0f;

    // --- 記憶 ---
    float memoryDuration = 5.0f;   // 見失ってから忘れるまで [s]

    // --- 段階的な認識 (即座に「発見」しない) ---
    // 視界内に居続けると 0 → 1 へ上昇し、1 で「発見」。外れると減衰する。
    float awarenessGainRate = 1.5f;   // 毎秒
    float awarenessDecayRate = 0.5f;  // 毎秒
    float m_awareness = 0.0f;
};
```

**WHY 段階的な認識 (awareness) を入れるか**: 視界に入った瞬間に敵が
100% 反応すると、プレイヤーに「見つかりそう」という緊張と
「まだ間に合う」という猶予が存在しなくなる。
ステルス性のあるゲームでは必須で、そうでなくても反応のワンテンポが
「生き物らしさ」を作る。距離が近いほど `awarenessGainRate` を上げる。

### 聴覚 — 音イベントのブロードキャスト

```cpp
// 世界に「音がした」ことを通知する。範囲内の PerceptionComponent が受け取る。
// WHY EventBus を使うか: 音を出す側 (足音・銃声・破壊) が
//      AI の存在を知る必要がない。既存の EventBus で疎結合にする。
struct NoiseEvent {
    math::Vector3 position;
    float         loudness;   // 実効半径 [m]
    EntityID      instigator; // 音を出した主体 (自分の音に反応しないため)
};
```

**WHY 聴覚を入れるか**: 視覚のみの AI は、プレイヤーが背後から走って接近しても
無反応で、明らかに不自然に見える。聴覚は実装コストが極めて低い
(距離比較のみ、レイキャスト不要) 割に、AI の説得力への寄与が大きい。

---

## 4. Script との接続

BT のリーフに `RunScript` を置き、ユーザースクリプトのメソッドを呼ぶ。

```cpp
// ユーザースクリプト側 (Assets/Scripts/)
class EnemyBrain : public fbzz::Script {
public:
    // FBZZ_BT_ACTION で登録したメソッドが BT のリーフから呼べる。
    // ScriptCodeGen が .generated.hpp に登録コードを吐く。
    FBZZ_BT_ACTION(Attack)
    BTStatus Attack(float dt) {
        if (m_attackTimer > 0.0f) { m_attackTimer -= dt; return BTStatus::Running; }
        GetComponent<fbzz::scene::AnimatorComponent>()->SetTrigger("Attack");
        m_attackTimer = 0.8f;
        return BTStatus::Success;
    }

    FBZZ_BT_CONDITION(HasAmmo)
    bool HasAmmo() const { return m_ammo > 0; }
};
```

**WHY マクロ + コード生成か**: 既存の `FBZZ_REGISTER_SCRIPT` /
`ScriptCodeGen` の仕組みがそのまま使え、DLL 境界を越える関数ポインタ登録を
手書きしなくて済む。BT エディタ側もこの登録情報を読んで
「利用可能なアクション一覧」をドロップダウンに出せる。

### `ScriptAIProxy`

```cpp
struct ScriptAIProxy {
    Script* script = nullptr;

    // --- Blackboard ---
    bool  SetBBBool   (std::string_view key, bool v) const;
    bool  SetBBFloat  (std::string_view key, float v) const;
    bool  SetBBVector3(std::string_view key, math::Vector3 v) const;
    bool  SetBBEntity (std::string_view key, uint64_t entityId) const;
    bool  GetBBBool   (std::string_view key, bool defaultValue = false) const;
    float GetBBFloat  (std::string_view key, float defaultValue = 0.0f) const;

    // --- 制御 ---
    void SwitchTree(std::string_view treePath) const;  // 木の動的差し替え
    void RestartTree() const;
    void SetTreeEnabled(bool enabled) const;

    // --- Perception ---
    bool     HasTarget() const;
    uint64_t GetTargetEntity() const;
    float    GetAwareness() const;
    void     ForceDetect(uint64_t entityId) const;   // 演出上の強制発見

    // --- 音イベント ---
    void EmitNoise(math::Vector3 position, float loudness) const;
};
```

**WHY `SwitchTree` を持つか**: ボスの第 2 形態、味方化、パニック状態など、
「振る舞いの体系そのものが変わる」局面がある。
1 つの巨大な木に全部詰めるより、木を差し替える方が読みやすく編集しやすい。

---

## 5. エディタ統合 — `BehaviorTreePanel` (2026-08-09 実装)

共通 `GraphCanvas` (`Docs/design/graph-editor-framework.md`) の上に実装した。
描画・パン・ズーム・選択・接続ドラッグ・ショートカットはキャンバス、
木の妥当性・Undo・保存はパネル、という分担。

**これがフレームワークの 3 つ目の利用者であり、最初の「新規」利用者**でもある。
VFX / Animation は既存コードの移行だったため、API の穴は
ここで初めて表に出た (接続中の妥当性プレビューとピンの逆引きが無かった)。
どちらも framework 側へ足して解決している。

### 実装した面

- **ノードパレット**: Composite / Decorator / Action / Condition / Stub の 5 群。
  各項目に「何をするノードか」の 1 行を出す。
- **接続の妥当性プレビュー**: 掴んだピンから繋げない親は減光される。
  木のルール (子数上限・循環・葉に子を付けない) を `GraphView::linkDragFilter` で渡し、
  描画はキャンバスが行う。**繋いでから弾かれるのではなく、繋げない先が最初から暗い。**
- **リンクを切る = 親を外す** (ノードは消さない)。枝を一時的に外して試す操作は
  BT の作業で頻繁に起きるため、切った瞬間にノードごと消えると作り直しになる。
- **削除・複製は子孫ごと**。BT の枝は「まとめて 1 つの意味」なので、
  親だけ消して子が浮くと、残された枝が何のためのものか判らなくなる。
  複製の id 再割当は `GraphSubgraphOps::ExtractSubgraph` を使う。
- **Blackboard サイドバー**: キーの追加・改名・型変更。予約キー (`reserved`) は
  固定添字で `PerceptionSystem` 等が書き込むため、改名・削除を禁止する。
- **Inspector**: `order` (= 優先度) を最前面へ出す。BT で最も重要な値なのに
  ノード本体では読み取れないため。`abortMode` は純粋条件以外ではグレーアウトし、
  なぜ設定できないかを理由ごと表示する。
- **検証バナー**: `ValidateBehaviorTreeAsset` のエラー (保存拒否) と
  `CollectBehaviorTreeWarnings` の警告を分けて出す。警告はクリックで原因ノードへ飛ぶ。
- **Undo/Redo**: 96 件上限のスナップショット。木は数十ノードなので丸ごと持てる。
  差分 Undo は「親を付け替えたら order も変わる」ような連動を取りこぼしやすい。
- **実行中のハイライト**: 今開いている木を実際に走らせているエージェントを 1 体拾い、
  `BehaviorTreeRuntime::authoringIdOf` で DFS 配列の実行状態を authoring id へ写す。
  Success / Failure / Running を背景色、Running の枝はリンクを太く。
  Play 中はキャンバスを閲覧専用にする (木を書き換えると走っている木と食い違う)。

### AI 連携 (`bt.*`)

VFX と同じ「アセットを読む → 規約を読む → 編集する → 検証する」の流れへ揃えた。

| 種別 | 面 | 役割 |
|---|---|---|
| query | `bt.tree` | 木構造・Blackboard・検証結果。ノードは `parentId` / `order` 順に並べて返す |
| query | `bt.lint` | 保存は通るが意図どおり動かない構成。`severity` / `fix` / `autoFixable` 付き |
| query | `bt.guide` | オーサリング規約 (検査できるものは `lintCode` 付き)・骨格・子数上限と `canAbort`・呼ぶ順 (`workflow`) |
| query | `bt.schema` | 種別ごとに「ランタイムが実際に読むフィールド」。`bt.node.setField` の対 |
| query | `bt.nodeField` | ノードの現在値。`setField` と同じ表現なので読んで一部だけ変えて書き戻せる |
| query | `bt.runtime` | Play 中の各ノードの最終 status と Blackboard の実値・最終書き込み時刻 |
| query | `bt.diff` | 2 つの木の構造差分 (`parentId` / `order` の変化とキーの増減を含む) |
| query | `bt.templateCatalog` | `Assets/AI/Templates` の骨格一覧 |
| command | `bt.node.add` / `remove` / `duplicate` / `setParent` / `setOrder` / `setField` | ノード編集 |
| command | `bt.blackboard.add` / `remove` | キー編集 |
| command | `bt.autoLayout` | 深さを列にして並べ直す (間隔は `BehaviorTreePanel::AutoLayout` と同値) |
| command | `bt.repair` | `autoFixable=true` の issue を機械的に直す |
| command | `bt.template.apply` | 骨格を `.behaviortree` として書き出す (新規作成可・Undo 可) |

設計上の要点:

- **`order` を必ず返す**。BT の挙動は木の形ではなく `order` で決まるので、
  返さないと AI は「上から順に読めば優先順位」という自然な読み方をした瞬間に間違える。
  配列の並びも `parentId` / `order` 順にしてある。
- **拒否は理由ごと返す**。`abortMode` を副作用のあるノードへ付けた場合は
  `BT_ABORT_NOT_ALLOWED` で「中断チェックのたびに世界が変わり木が非決定的になる」まで返す。
  存在しない Blackboard キーは `BT_UNKNOWN_KEY` で弾く — 保存も Compile も通ってしまい、
  実行時に黙って無視されるという最も気づきにくい壊れ方をするため。
- **判定ロジックは Editor と AI で同じ規則**。「AI からは繋げるが人間の UI では弾かれる」
  という食い違いを作らない。
- 保存前に必ず `ValidateBehaviorTreeAsset` を通す。壊れた木を書き出すと、
  次に開いたときに「AI が壊した」のか「元から壊れていた」のか区別できなくなる。
- **書ける集合を目録として公開する**。`bt.schema` の `appliesTo` は
  「Inspector に出ているか」ではなく「ランタイムが実際に読むか」で決める。
  名前が実在しても種別が読まないフィールドは保存も Validate も通り、
  「設定したのに行動が変わらない」としか見えないので、`BT_FIELD_NOT_APPLICABLE` で拒否する。
  食い違っていた `LookAt` の `range` / `IsTargetInRange` の `turnSpeedDeg` は
  Inspector 側から取り除き、人と AI が同じ目録を見るようにした。
- **lint の code は Engine が唯一の正本**。`CollectBehaviorTreeWarnings` の code を
  Editor の警告 banner と `bt.lint` の両方が使う。別実装にすると必ずドリフトする。
  `bt.guide` の規約は、検査できるものに `lintCode` を添えて実装と対応付ける。
- **静的検査だけでは足りない**。BT は「木としては正しいが意図どおり動かない」壊れ方をし、
  しかも画面に異常が出ない (敵は動いている)。原因は「条件が偽」「割り込めていない」
  「到達していない」の 3 通りで、木からも画像からも区別できないため、
  `bt.runtime` で各ノードの最終 status と Blackboard の実値を返して初めて 1 回で決まる。

---

### 当初案 (参考)

`VFXGraphCanvas` / `AnimationGraphPanel` と同じ `ImNodes` 基盤で実装する。

- ノードパレットからのドラッグ配置、接続、`GraphLayout.hpp` による自動整列
- Inspector でノードパラメータと Blackboard キー定義を編集
- **実行中のハイライト**: Play モード中、選択エンティティの木で
  `Running` の経路を色付けし、各ノードの直近の返り値を表示する。
  **WHY 最優先で作るか**: BT のデバッグは「なぜこの行動を選んだのか」の追跡が
  すべてであり、これがないとオーサリングが勘に頼る作業になる。
  ポートフォリオとしての見栄えも最も高い部分。
- **Blackboard ウォッチ**: 選択エンティティの Blackboard 全キーを実時間表示する。

### シーンビューのデバッグ描画

`DebugDraw` を使い、選択中エンティティに対して描画する。

- 視野角の扇形 (awareness に応じて色が変化: 白 → 黄 → 赤)
- 聴覚半径の円、`NoiseEvent` 発生位置の一時マーカー
- `LastKnownPosition` へのマーカー
- NavMesh の現在経路 (`NavigationSystem` に既存があれば流用)

---

## 6. `NavMeshSensorComponent` との関係

**既存コンポーネントは非推奨化せず、そのまま残す。**

- BT を持たないエンティティ → 従来どおり `autoChase` で動く
- `BehaviorTreeComponent` を持つエンティティ → `PerceptionComponent` を使い、
  `NavMeshSensorComponent` が同居していれば `autoChase` を実行時に無視する

**WHY 削除しないか**: 既存シーン (`.scene`) に保存済みのコンポーネントを
消すとロードが壊れる。また「見つけたら追う」だけで十分な雑魚敵に
BT アセットを用意させるのは過剰。両方の道を残すのが正しい。

---

## 実装順序

| 段階 | 内容 | 単体で価値があるか |
|-----|------|-----------------|
| 1 | `Blackboard` + `BehaviorTreeAsset` (`.behaviortree` TOML) + ランタイム評価 | ○ (スクリプトから木を組めば動く) |
| 2 | `BehaviorTreeSystem` + `SystemScheduler` 登録 + 基本ノード一式 | ○ |
| 3 | `PerceptionComponent` + `PerceptionSystem` (視覚 + awareness) | ○ |
| 4 | `BehaviorTreePanel` (編集 + 実行ハイライト) | ◎ 最も見栄えする |
| 5 | `observerAborts` による中断 | ○ AI の質が跳ね上がる |
| 6 | 聴覚 + `NoiseEvent` | ○ |
| 7 | `ScriptAIProxy` + `FBZZ_BT_ACTION` コード生成 | ○ |

**スコープ外**: グループ AI (連携・陣形・役割分担)、
影響マップ (Influence Map)、動的カバーポイント選択。
**WHY**: いずれも単体 AI が完成してから積むべき層で、
先に手を付けると土台の設計を歪める。

---

## テスト

`Projects/Tests/AI/main.cpp` を新設する。Scene / NavMesh に依存しない
純粋な木の評価をテストする。

- [ ] `Sequence` / `Selector` / `Parallel` の Success / Failure / Running 伝播
- [ ] `Running` リーフから再開し、ルートを再評価しない
- [ ] `LowerPriority` の abort が Running を正しく中断する
- [ ] `Cooldown` / `TimeLimit` が指定秒数で動く
- [ ] Blackboard の型不一致書き込みが false を返し、値を壊さない
- [ ] `GetLastWriteFrame` による時間条件が正しい
- [ ] `.behaviortree` の保存 → 読込で木構造とパラメータが往復する
- [ ] `tickRate` の位相分散が同一フレームへの集中を避ける
- [ ] awareness の増減が指定レートどおりに収束する

## 実装チェックリスト

- [ ] `Blackboard` + `BlackboardDef` (キー → ID 解決)
- [ ] `BehaviorTreeAsset` (フラット配列) + `.behaviortree` シリアライザ + Importer 登録
- [ ] Composite / Decorator / Leaf ノード一式
- [ ] `BehaviorTreeComponent` + `Reflect` + `SceneSerializer` 対応
- [ ] `BehaviorTreeSystem` + `SystemScheduler` 登録 (Phase / ComponentAccess)
- [ ] `tickRate` の位相分散
- [ ] `observerAborts` と中断候補の事前計算
- [ ] `PerceptionComponent` + `PerceptionSystem` (視覚 / awareness / 記憶)
- [ ] `NoiseEvent` の `EventBus` 定義 + 聴覚判定
- [ ] `NavMeshSensorComponent::autoChase` の共存規則
- [ ] `ScriptAIProxy` + `ScriptProxies.cpp` + `Script.hpp` 登録
- [ ] `FBZZ_BT_ACTION` / `FBZZ_BT_CONDITION` + `ScriptCodeGen` 対応
- [x] `BehaviorTreePanel` (共通 GraphCanvas) + 実行ハイライト + Blackboard サイドバー
- [x] AI 連携 `bt.tree` / `bt.lint` / `bt.guide` / `bt.node.*` / `bt.blackboard.*` / `bt.autoLayout`
- [x] AI 連携の強化 `bt.schema` / `bt.nodeField` / `bt.runtime` / `bt.diff` /
      `bt.templateCatalog` / `bt.template.apply` / `bt.repair` / `bt.node.duplicate`
- [ ] `DebugDraw` による視野・聴覚・経路の可視化
- [ ] `Projects/Tests/AI/main.cpp`
- [ ] Visual Studio 2022 全体ビルド
