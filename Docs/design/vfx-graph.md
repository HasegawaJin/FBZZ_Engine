# VFX Graph アセット設計

## 目的

ParticleEmitter、Trail、MeshTrail、Light、Audio、Decal、別のVFX Graphを一つの`.vfx`アセットへ束ね、
シーン上の`VFXGraphComponent`から同じ演出を何度でも生成・再生できるようにする。

## 実行モデル

- `.vfx`はEntryを起点とする有向非巡回グラフ（DAG）とする。
- ノード間リンクは`On Complete`、`On Start`、`On Collision`と追加Delayを持つ。
- `On Collision`はParticleノードだけをsourceにでき、1グラフ周期につき一度だけ発火する。
- 一つの出力から複数ノードへ接続すると並列再生になる。
- DelayノードはGameObjectを生成せず、後続ノードの開始だけを遅延する。
- Effectノードは開始時に一時GameObjectを有効化し、終了時に停止・無効化する。
- グラフのループはアセット内の循環リンクではなく、`VFXGraphComponent::loop`で表現する。
- Sub Graphは同じ実行系を再帰利用する。循環参照事故に備えて実行時深度を8段へ制限する。
- EditorのTimeline scrubはParticleのrandomSeedから再シミュレートし、同じ時刻を決定論的に再現する。

## 永続化と所有権

- `.vfx`はTOML形式で、ノード設定、リンク、Editor座標を保存する。
  - **形式選択の根拠**: Scene/.terrain/.mat と同じ toml++ で統一する（コーデック二系統化を避け、コメント可・diff容易）。
    JSON との性能差はロード時1回・数十ノードの小ファイルではマイクロ秒オーダーで、毎フレームのホットパスでもないため
    フォーマットは性能の変数にならない（スキーマ走査シリアライズのコストも形式非依存）。よって性能理由での JSON 化はしない。
  - **将来の逃げ道**: 万一 `.vfx` が巨大化しロード時間が実測で問題化したら、オーサリング用テキスト(TOML)は保ったまま
    ランタイムはベイク済みバイナリへ落とす（Library/Baked 隔離。TOML→JSON ではなくテキスト／ランタイムの分離で解く）。
- Particleノードはシーン用`ParticleEmitter`と同じauthoring型・共通codecを使う。Mesh Surface、
  CPU/GPU、Curve、Gradient、Burst、SubEmitter、Collision、Flipbook、Soft Particle、LODを欠落なく保持する。
- 実行時生成GameObjectは`__VFX_`接頭辞を持ち、SceneSerializerの既存規約で保存対象外となる。
- 生成GameObjectはVFXGraphComponent所有GameObjectの子とし、停止・再読込・親破棄時にまとめて破棄する。
- アセット参照はGuidRefCodecで`guid:`形式へ変換し、移動・改名へ耐性を持たせる。
- Scriptは`vfx.Play()` / `Stop()` / `SetSpeed()`と再生時刻照会を`ScriptVFXProxy`経由で行う。

## 対応ノード

| ノード | 役割 |
|---|---|
| Entry | グラフ開始点 |
| Delay | 後続開始を遅延 |
| Particle | ParticleEmitterを生成 |
| Trail | TrailComponentを生成 |
| MeshTrail | MeshTrailComponentを生成 |
| Light | Point Lightを生成 |
| Audio | AudioSourceComponentを生成 |
| Decal | DecalComponentを生成 |
| Sub Graph | 別の`.vfx`を子インスタンスとして生成 |

## 制作品質とbudget

- アセット単位でParticle、Light、Audio voiceのbudgetを持ち、Inspectorに使用量と超過を常時表示する。
- Pauseは生成Componentを破棄せずシミュレーション速度を0にし、Resume時に連続した状態から再開する。
- Stopは生成ノードを停止・無効化し、Restartはスケジュールとイベント発火状態を先頭へ戻す。
- 保存時のDAG検証は重複リンク、循環、到達不能、無効なイベントsource、負の時間を拒否する。

## 公開パラメーターとインスタンス別override

一つの`.vfx`を複数GameObjectで使い回しつつ、爆炎の色・規模・差し替えテクスチャなどを
インスタンスごとに変えるための仕組み。UnityのVFX Graph exposed property + Material property override
に相当する。グラフを複製せず、公開した値だけをインスタンス側で上書きする。

### 三層構成

- **公開パラメーター定義**（アセット）: グラフ作者が`.vfx`に名前付きパラメーターを宣言する。名前は
  一意キー・binding名・Inspector表示名を兼ねる。default値と、数値型のInspectorレンジ(min/max)を持つ。
- **binding**（アセット）: 公開パラメーターを1つ以上のノード設定フィールドへ紐付ける。1つの
  パラメーターを複数フィールドへ束ねられる（例：`Tint`で複数Particleの色を同時駆動）。
- **インスタンスoverride**（Component）: `VFXGraphComponent`が上書きした分だけをsparseに保持する。
  未上書きのパラメーターはアセットdefaultにフォールバックする。

### 対応型

Float / Int / Bool / Color(Vector4) / Vector3 / Asset参照(Texture・Material)。Asset参照は
GUID参照で保持し、既存`guid:`規約で移動・改名へ耐性を持たせる。

```cpp
enum class VFXParamType : std::uint8_t { Float, Int, Bool, Color, Vector3, AssetRef };
```

### 解決フロー

- ノード生成時、公開パラメーターごとに「override があればその値、無ければアセットdefault」で最終値を確定する。
- 確定値をbinding経由で生成直後のノード設定へ流し込む（既存のノード→Component適用の直前に挿入する）。
- Sub Graphは親のoverrideを引き継がず、それぞれ独立に自身のパラメーターを解決する（意図しない伝播を防ぐ）。
- 名前・型の不一致（アセット改訂で公開パラメーターが消えた／型が変わった）はoverrideを無視し、Inspectorに警告表示する。

### 値ソース（確定 — 統一variant `VFXParamValue`）

公開パラメーターのdefault・インスタンスoverride・後述のAttribute Binding・Signal Graphは、いずれも
「パラメーターへ最終値を与える源」という同一概念である。これを別々に実装すると解決フローが分岐して破綻するため、
**単一のvariant `VFXParamValue` を値ソースの唯一の格納形とし、静的な定数値はその特殊形とする**。

```cpp
// 値ソースの統一表現。default も override も、この型ひとつで保持する。
// AttributeRef/Signal は段階導入だが、格納形を最初からvariantにしておくことで
// 「後で差し込む」際に override 格納やシリアライズを作り直さずに済む。
struct VFXParamValue {
    std::variant<
        Constant,     // 定数（Float/Int/Bool/Color/Vector3/AssetRef を型タグで保持）— MVPはこれのみ
        CurveSource,  // lifetime/time 駆動の ParticleCurve 差し替え
        GradientSource,// ParticleGradient 差し替え（色ソース）
        RandomRange,  // [min,max] からrandomSeedで決定論サンプル
        AttributeRef, // 実行時ゲーム属性への参照（後述 Attribute Binding）
        SignalRef     // Signal Graph の出力（別フェーズ）
    > source;
};
```

- **解決の一本化**: 上の「解決フロー」は override/default を選んだ後、`VFXParamValue` を評価して確定スカラーを得る、
  という一段に統一する。定数は即値、Curve/Gradient/Randomは評価時刻・seedで、Attribute/Signalは
  `VFXGraphSystem` が毎フレーム評価して書き戻す。
- **binding先との対応**: 評価結果は binding の `schemaPath` が指す leaf の `PropertyDesc::set` で書く
  （後述「binding機構」）。Curve/Gradientソースは `colorGradient`/`sizeCurve` leaf を丸ごと差し替え、
  スカラーソースは対応 leaf に書く。スキーマが完全なのでどちらも同じ経路で扱える。
- **決定論**: Curve/Random/Signalの評価は常にParticleの`randomSeed`と評価時刻のみに依存させ、
  Timeline scrub と `vfx.preview` が同一時刻で同一結果になる前提を全値ソースで守る。

### binding機構（確定 — schemaPath方式。基盤は「単一 authoring スキーマ」）

紐付けは `VFXParamBinding{ paramName, nodeId, schemaPath }` とし、`schemaPath`（例 `"particle.colorStart"`、
`"particle.colorGradient"`、`"light.intensity"`）を**エンジン共通の型スキーマ**で解決する。これは当初の
「案A: フィールドパス」だが、**下記の根本解決でスキーマが完全化されて初めて正しく成立する**表現である
（未完全な `IReflector` を辿る当初案Aは成立しなかった — 経緯は次項）。

#### 根本原因 — authoring データが5面に手書きされドリフトする

1つのノード設定の「どのフィールドが何型で、どう編集・保存・公開されるか」が、いま独立に手書きされている:

| 面 | 実体 | 問題 |
|---|---|---|
| リフレクション | `ParticleEmitter::Reflect` ([ParticleEmitter.hpp:349](../../Projects/Engine/include/Engine/Scene/Components/ParticleEmitter.hpp#L349)) | `Field(name,ref)` を順に呼ぶだけの**フラット・書き込み専用ビジター**。metadata を保持せず、`sizeCurve`/`colorGradient`/`bursts` などは**そもそも面に出ない**（IReflector に Curve/Gradient/vector 用の overload が無い） |
| シリアライズ | TOML codec（手書き） | フル対応だが Reflect と二重管理 |
| Inspector | `ParticleEmitterModules`（手書き UI） | さらに別実装 |
| bind ターゲット | （検討中だったレジストリ） | 実質**手書き4面目** |
| AI編集 | `vfx.node.setField` のフィールド名 | さらに別 |

案A/案B/レジストリ/値ソース variant はいずれも「リフレクションが不完全」という**同一の根**への場当たりだった。
レジストリ案は blast radius こそ小さいが、この**ドリフト構造を温存**する。よって症状ではなく根を断つ。

#### 根本解決 — `IReflector` を「型スキーマ」へ格上げする（エンジン全体）

書き込み専用ビジター `IReflector` を、**metadata を保持する型スキーマ `ITypeSchema` / プロパティ記述子**へ格上げする。
各型は authoring フィールドを**一度だけ**記述し、そこから全消費者を機械的に導出する:

```cpp
// フィールド1つの完全な記述。値そのものではなく「型・意味・アクセサ」を持つ。
struct PropertyDesc {
    std::string_view key;          // 安定キー。ネスト時は "colorGradient" 等の相対名
    PropertyType     type;         // Float/Int/Bool/Vector3/Color/AssetRef/Curve/Gradient/Enum/Struct/Array
    std::string_view display;      // Inspector 表示名
    std::string_view category;     // グループ見出し
    RangeHint        range;        // 数値レンジ（任意）
    bool             exposable;    // VFX公開パラメーターの bind 候補になれるか
    // 型付きアクセサ。生メンバへ直接到達するため Curve/Gradient/入れ子/配列にも届く。
    std::any (*get)(const void* owner);
    void     (*set)(void* owner, const std::any& value);
    const ITypeSchema* (*childSchema)(); // Struct/Array の要素型スキーマ（入れ子解決用）
};

struct ITypeSchema {
    virtual std::span<const PropertyDesc> Properties() const = 0;
};
```

この1定義（型ごとの `PropertyDesc` 列）から、次を**すべて生成ビューとして導く**:

- **シリアライズ**（TOML 入出力）— スキーマ走査で read/write。
- **Inspector 行** — `type` からウィジェットを選ぶ。手書き UI は逓減。
- **bind ターゲット目録 = `exposable == true` の leaf** — 別レジストリは存在しない。
- **AI introspection** — `vfx.params` / dryRun 差分 / `setField` 検証がスキーマを直接読む。
- **値ソース variant のアタッチ先** — 任意 leaf（Curve/Gradient 含む）に `VFXParamValue` を差せる。

→ binding・レジストリ・値ソースは独立システムをやめ、**スキーマの射影**になる。同じ事実を書く面が
「1スキーマ＋N生成ビュー」に畳まれ、ドリフトの class ごと消える。

#### binding は schemaPath の射影

- `schemaPath` はスキーマを**ドット区切りで辿る**（`"particle.colorGradient"` = ParticleEmitter スキーマの
  `colorGradient` leaf）。スキーマが完全なので Curve/Gradient/入れ子にも到達でき、当初案Aの限界は消える。
- 公開可能目録＝各ノード型スキーマの `exposable` leaf。Editor の「フィールド右クリック→公開」は
  スキーマ候補を提示するだけ、AI の `vfx.params` は同じ集合を読むだけ（面が一致する）。
- 検証: `schemaPath` が未解決、対象ノード型に無い、または型不一致なら保存時DAG検証で拒否する。

#### 移行方針（エンジン全体だが big-bang にしない）

全 `*Component::Reflect` を一度に置換しない。**アダプタで両立**させ、型単位で段階移行する:

1. `ITypeSchema` を追加し、**既存 `IReflector` はスキーマ上に薄く再実装**（`Field` 呼び出し＝スキーマ走査へのアダプタ）。
   既存の全コンポーネントは無改修で従来通り動く（回帰ゼロが移行の前提条件）。
2. まず VFX authoring 型（`ParticleEmitter` + `VFX*Settings`）にスキーマを与え、bind/AI/値ソースを
   スキーマ消費に切り替える（新規コードなので回帰しない）。
3. 以降、Inspector/TOML をスキーマ生成へ型ごとに寄せていく。急がない。既存の手書き面はスーパーセットなので共存できる。

### 永続化とUI

- 公開パラメーター定義・binding・default値は`.vfx`(TOML)に、既存ノード/リンクと同じセクション体系で保存する。
- インスタンスoverrideは`VFXGraphComponent`のReflect対象とし、シーン/prefabへ保存・Undo対象とする。ランタイム状態とは分離する。
- Component Inspectorは公開パラメーター一覧を表示し、各行に「override中か」チェック＋型別ウィジェット
  （数値レンジ・Color・Vector3・AssetPathField）を出す。overrideをクリアするとdefault表示へ戻す。prefab override差分の視覚化に合わせる。
- VFX EditorのInspectorに公開パラメーターの宣言・binding編集UIを追加する。
- Scriptからの動的override設定は`ScriptVFXProxy`に`SetFloat/SetColor/…(name, value)`を追加して行う。

## AAA表現の到達目標

公開パラメーターとbindingを「作り込みの制御軸」に据え、以下をノード機能として持ちつつ、
表現の強度を少数の公開パラメーターへ束ねてインスタンス／AIから一括制御できる状態を目標とする。
個別機能は段階追加でよいが、いずれも「公開パラメーターで駆動できる」ことを設計条件とする。

- **値ソースの拡張**: 公開パラメーターは定数に加え、Curve（時間・lifetime駆動）／Gradient／
  ランダムレンジを値ソースにできる。overrideはこの値ソースごと差し替える。格納は前掲の統一variant
  `VFXParamValue` に集約し、定数はその特殊形とする（実装分岐を作らない）。
- **描画表現**: HDR emissive + additive/alpha/premultiplied blend、屈折・熱歪み(distortion)、
  法線付きlit smoke(six-way lighting)、velocity-stretch／camera-facing billboard、ribbon/beam、
  mesh particle、curl/vector fieldノイズ、flipbook motion-vector補間、GPU depth collision。
- **接続と追従**: ボーン／ソケット追従、skinned meshからのspawn、`On Collision`以外の
  イベント（OnDeath等）でのサブグラフ発火。
- **決定論**: randomSeedからの再シミュレートで、同一時刻を常に同一結果として再現する
  （AIの視覚評価とTimeline scrubが同じ静止画を得るための前提）。

## AI連携オーサリング

AIがエフェクトを「作る・直す・調整する」ループを、既存のquery/command＋dryRun＋viewport.capture基盤
（`fbzz.editor.v1`、`EditorMcp`→`EditorBusDispatcher`）へ載せる。

- **原則**: AIはraw fieldを盲打ちせず、意味づけされた公開パラメーターとテンプレートを一次面として
  オーサリングする。低レベルのノード／フィールド編集も可能だが、budgetとDAG検証のguardrail内に限る。
- **意味論規約**: 公開パラメーター名は自然言語で意図が通る語彙（`Intensity` `Scale` `Tint`
  `Smokiness` `Duration` `Turbulence`…）を推奨する。プロンプト→パラメーター調整の写像を安定させる。
- **決定論プレビュー**: `vfx.preview`（新query）は指定`.vfx`を指定時刻へscrubして静止画を返す。
  代表時刻（t=0 / peak / end）の一括captureでAIが山場を評価し、反復する。既存`viewport.capture`と同系。
- **反復ループ**: 提案 → `dryRun`で適用前差分と更新後budgetを確認 → apply → 複数時刻capture →
  視覚評価 → override／field微調整、を閉ループで回す。budget超過applyは拒否し理由を返す。
- **テンプレートライブラリ**: Explosion／Fire／Smoke／Impact／Magic等の骨格`.vfx`を用意し、
  AIは複製→公開パラメーターで作り分ける。「グラフを複製せず値だけ変える」override方針と一致する。

### AIコマンド／クエリ面（contract拡張）

`editorContracts.ts`のquery/command union と `EditorBusDispatcher` へ対で追加する。全commandはdryRun対応・
Undo可・budget/検証guardrail適用とする。

| 種別 | 追加面 | 役割 |
|---|---|---|
| query | `vfx.graph` | ノード・リンク・budget照会（実装済） |
| query | `vfx.params` | 公開パラメーター定義と、対象インスタンスの解決済み値・override状態 |
| query | `vfx.preview` | 指定時刻へscrubした決定論的静止画（AIの視覚評価用） |
| query | `vfx.schema` | ノード型の authoring スキーマ（フィールド名・型・レンジ・`exposable`）。`setField`/`param.bind` の正当な `schemaPath` 集合をAIが引く |
| command | `vfx.node.*` | ノードのadd／remove／setField（`setField` の対象・型はスキーマで検証） |
| command | `vfx.link.*` | リンクのadd／remove |
| command | `vfx.param.*` | 公開パラメーターのdeclare／bind（`schemaPath` 指定）／setDefault |
| command | `vfx.instance.*` | インスタンスoverride設定／clear |
| command | `vfx.template.apply` | テンプレートの適用 (`mode`: replace=複製 / merge=既存へ追記 / subgraph=参照) |

## 新機能

公開パラメーター・binding・AI連携を土台に、表現力とオーサリング効率を引き上げる拡張機能群。
Signal Graph以外は上記三層に載る増分で、段階導入できる。

### ランタイム／オーサリング基盤

- **Attribute Binding（動的データ駆動）** — override先を「固定値」だけでなく実行時ソースへ拡張する。
  bindソースはScriptフィールド／Animator param／物理速度／HP等ゲーム属性とし、`VFXGraphSystem`が
  毎フレーム解決して公開パラメーターへ書き込む。エフェクトがスクリプト個別実装なしでゲーム状態に反応する。
  overrideの格納を「定数 or Attribute参照」のvariantへ一般化し、静的overrideはその特殊形とする。
- **Variant Sets（名前付きプリセット）** — 複数の公開パラメーターに対する名前付きoverrideセット
  （`Small`／`Medium`／`Large`等）をアセットへ束ねる。インスタンスはvariant名を選ぶだけで一括適用でき、
  個別overrideで上からさらに微調整できる。AIのバリアント一括生成の格納先を兼ねる。
- **Sub-graph パラメーター転送** — 子`.vfx`の公開パラメーターを親グラフの公開パラメーターへ再公開（forward）する。
  入れ子でも最上位の少数パラメーターから全体を制御でき、Sub Graphがoverrideを継承しない原則と両立する
  （転送は明示的な再公開であり暗黙の伝播ではない）。
- **Signal Graph（別フェーズ）** — `time`／`noise`／`sine`／`remap`／四則等の小ノードグラフで
  公開パラメーターを駆動する式評価系。単独でスコープが大きいため、Attribute Binding・Variant Sets確立後の
  別フェーズとする。値ソース抽象（定数／Curve／Attribute／Signal）の一員として差し込む。

### AIオーサリング

いずれも「決定論プレビュー＋budget/検証guardrail＋反復ループ」を共通土台とし、
公開パラメーターを一次面に操作する。

- **参照画像ガイドオーサリング** — 参照スクショ／GIFを入力に、AIが`vfx.preview`の静止画と見比べて
  公開パラメーターを反復調整し見た目を寄せる。評価はAIの視覚判断に委ね、決定論scrubで同一時刻を比較する。
- **自動budget最適化 / auto-LOD** — 目標予算を与えると、粒子数削減・CPU→GPU切替・距離LOD生成を
  capture回帰で見た目を保ちつつ適用する。budget超過は既存guardrailで拒否し、最適化はその範囲内で行う。
- **NL→グラフ生成** — 自然言語指示からテンプレート`.vfx`を複製し、公開パラメーター調整で骨格を自動生成する。
  ゼロからのノード羅列ではなく、検証済みテンプレート＋パラメーター化を出発点にして失敗率を下げる。
- **バリアント一括生成** — 1グラフから複数のスタイル違いをAIが量産し、Variant Setsとして格納する。

## 拡張方針

新しいEffect種別は`VFXNodeType`、設定構造体、TOML入出力、VFXGraphSystemの生成処理、
VFXEditorのInspectorを一組として追加する。グラフの時間評価・接続検証は共通のまま再利用する。
新しい公開パラメーター型は`VFXParamType`、default/override値の格納、TOML入出力、解決フロー、
Inspectorウィジェットを一組として追加する。
新しいAIオーサリング面は`editorContracts.ts`のunion、`EditorMcp`ツール、`EditorBusDispatcher`の
ハンドラ、dryRun応答を一組として追加し、視覚検証は`vfx.preview`／`viewport.capture`へ集約する。
authoring スキーマに新フィールドを1つ追加すれば、Inspector・TOML・bindターゲット候補・AIの `vfx.schema`/
`setField`検証が同時に更新される（1定義→N生成ビューの原則）。手書き面を増やさないこと。

## 実装フェーズ（依存順）

根本解決を前提に順序を固定する。**Phase 0 が全ての土台**であり、これを飛ばして bind/AI を積むと
手書きドリフト構造に逆戻りする。

- **Phase 0 — リフレクション基盤の格上げ（エンジン全体）**: `ITypeSchema` / `PropertyDesc` を追加し、
  既存 `IReflector` をスキーマ上のアダプタとして再実装（全コンポーネント無改修・回帰ゼロが完了条件）。
  `ParticleEmitter` + `VFX*Settings` にスキーマを付与し、Curve/Gradient/入れ子 leaf まで到達可能にする。
- **Phase 1 — 公開パラメーター＋binding（MVP）**: 値ソースは `VFXParamValue::Constant` のみ。
  binding = `schemaPath`、override は sparse、解決フロー、`.vfx`(TOML)／Component への保存、
  Inspector 行、`vfx.params`/`vfx.param.*`/`vfx.instance.*`。ここまでで「1グラフを値だけ変えて量産」が成立。
- **Phase 2 — 値ソース拡張**: `VFXParamValue` に Curve/Gradient/RandomRange を追加。決定論評価を全ソースへ。
- **Phase 3 — AAA描画/追従の段階追加**: distortion・six-way lit smoke・ribbon/beam・mesh particle・
  vector field・motion-vector flipbook・bone/socket追従・OnDeath等イベント。各機能は「スキーマに
  exposable フィールドとして載る」ことを追加条件とする。
- **Phase 4 — AI高度化**: 参照画像ガイド／auto-budget・LOD／NL→グラフ生成／バリアント一括生成。
  いずれも決定論プレビュー＋budget/検証guardrail＋反復ループを共通土台とする。
- **Phase 5（別スコープ）— Attribute Binding / Variant Sets / Sub-graph転送 / Signal Graph**: いずれも
  `VFXParamValue` variant への追加要素として差し込む。Signal Graph は単独スコープが大きく最後。

## 不変条件 — スキーマ leaf の集合 == authoring フィールドの集合

設計の根幹は「1 スキーマ定義 → Inspector / TOML / binding / AI の N 生成ビュー」だが、
これは**放っておくと必ず崩れる**。実際、`ParticleEmitter` へフィールドを足しても
`GetParticleEmitterSchema()` への追加を忘れることができ、その結果
`simulationSpace` / `sortMode` / `blendMode` / `collision*` / `bursts` / `*SubEmitter` などが
「保存はされるが binding にも AI にも見えないフィールド」として静かに増えていた。

そこで次を**不変条件**とし、機械的に守らせる:

- authoring フィールドを追加したら、必ず `VFXAuthoringSchema.hpp` の該当スキーマへ leaf を足す。
- `Projects/Tests/VFXSchema` が「スキーマ leaf ⊆ TOML キー」「TOML キー ⊆ スキーマ leaf」の
  双方向と、全 leaf の get/set 往復を検証する。**追加漏れ・型宣言ミスはここで落ちる。**
- 意図的に公開しないフィールド (`enabled` / `playing` のような再生状態) は、
  テスト内の除外リストへ理由付きで明示する。黙って外さない。

型ごとの注意:

- **enum** は `MakeEnumProperty` を使う。生の `MakeProperty` で enum 型のまま `std::any` へ入れると、
  int を渡す AI・汎用 Inspector から `any_cast` が外れて set が**黙って失敗**する。
- **int 以外の整数型** (`uint32_t` 等) は同じ理由で `MakeIntProperty` を使う。
- **配列** は `MakeArrayProperty` を使い、`"bursts[0].count"` の添字付き `schemaPath` で要素へ降りる。

## 基底グラフ作成 — Template と Recipe (2026-08-09)

新規 `.vfx` は Entry ノード 1 個から始まる。ところが VFX で最も難しいのは
「どの層を、どの順で、どのブレンドで重ねるか」であって、ノードを置く作業ではない。
その知識は `vfx.guide` の recipe として AI へは渡していたが、人間側には同じものが無く、
Editor では毎回ゼロから積み直していた。**土台を起こす経路そのもの**を作り直した。

### まず塞いだ 4 つの穴

`MergeGraphTemplateInto` が写していたのは nodes / links / groups / parameters / bindings
だけで、以下は静かに壊れていた。

| 症状 | 何が起きていたか |
|---|---|
| Variant / Sub-graph 転送 / Signal Graph の消失 | 3 つとも一切写していなかった。Signal で駆動していた公開パラメーターが**定数へ戻った状態**で取り込まれ、警告も出なかった |
| 公開パラメーターの誤接続 | 同名は既存優先で定義を捨てるのに binding は「target に同名 param があれば」持ち込む。型もレンジも駆動先も違う既存パラメーターへ**Template の配線が黙って繋がる** (同じ `Blast Scale` でも一方は `sizeStart`、他方は `emitRate`) |
| budget が合算されない | ノードだけ増えて上限は target のまま。Explosion (4200 粒) を上限 1000 のグラフへ入れても DAG 検証は通り、**実行時にだけ粒子が出ない** |
| 素材参照が未検査 | Template を別プロジェクトへ適用すると素材が無いまま保存も検証も通り、「赤くならず何も出ない」という最も辿りにくい形で失敗する |

対処は順に「全要素を id・名前を振り直して持ち込む」「型・レンジ・値ソース種別・
**駆動先 schemaPath の集合**まで一致したときだけ相乗りさせ、それ以外は `Name (2)` へ改名して
両方生かす」「取り込み後の実使用量へ上限を合わせる」「`CollectMissingVFXReferences` で
適用前に不足を出す」。改名・budget 引き上げ・不足素材は**必ず報告面へ出す**
(Editor はダイアログ、AI は応答の `detail`)。黙って起きると次の一手が存在しない名前を指す。

### 取り込みの粒度と接続 — `TemplateMergeOptions`

- **層 (グループ枠) 単位の部分取り込み** — Template は既に `Impact / Ejecta / Aftermath /
  Fields & Ground` のように層で切ってある。`groupFilter` に group id を並べれば
  その層だけを取り込める。入口リンクが範囲外に残った塊は anchor から直に起動させる
  (切り出したのに実行されない塊を作らないため)。
- **接続先の指定** — `anchorNodeId` / `anchorTrigger` / `anchorDelay`。Entry 固定だと
  「ヒットの後段へ煙を足す」のに取り込んでから配線し直す手作業が必ず挟まる。
  `On Collision` / `On Death` を Particle 以外へ指定した場合は、保存直前の一般検証ではなく
  **この時点で理由ごと**弾く。
- **空間の親** — `parentNodeId`。link (発火順) とは別軸であることを UI にも明記する。
- **Variant の即時適用** — `variantName` を渡すと、その値を公開パラメーターの既定値へ焼き込む。

### 3 つ目の適用方法 — Sub Graph 参照

Merge はコピーなので、Template を後から直しても取り込み済みのグラフへは伝播しない。
「更新が伝わる基底」が要る場面のために、複製せず Sub Graph ノードとして参照する
`TemplateApplyMode::SubGraph` を対等な選択肢として置いた。使い分けはユーザーが決める。

取り込んだ塊の出所は `VFXGraphGroup::sourceTemplate` / `sourceTemplateVersion` /
`memberNodes` として**構造で**残す。以前は `note` へ `"Merged from template"` と
書くだけだったため、「この塊だけ入れ替える / 消す」の対象を機械的に決められなかった。

### Template をアセットとして一級化

`.vfx` に `description` / `tags` / `requiredRoles` / `thumbnailTime` を持たせた。
**以前は用途を `graph.name` へ押し込んでいた**ため、AI は `graphName` として読めるのに
Editor のカタログはファイル名しか出せず、人が見る面と AI が読む面が食い違っていた
(この設計ドキュメントが自分で禁じている形)。`vfx_knowledge_promote` も
名前ではなく `description` へ書く。

カタログ (`VFXTemplateCatalog`) は再帰走査してサブフォルダをカテゴリとして扱い、
出所 (Project / Engine / SDK) をバッジで見せる。同名は先勝ちで解決するが、
どれが採用されたかが見えないと「直したはずの Template が反映されない」を辿れない。

`requiredRoles` の語彙 (`core` / `body` / `sparks` / `animated`) は
`vfx.assetSurvey` の coverage・`vfx.guide` の recipe と**同じ表**から来る
(`VFXRecipeLibrary`)。別に持つと「guide は煙を要求するが survey は判定しない」が生まれる。

### サムネイル

カタログが出せるのが `"Particle x3, Light x1"` という内訳文字列だけだと、爆発と魔法の
区別が名前でしかつかない。決定論スクラブがあるのだから代表時刻の 1 枚を焼けばよい。
`VFXTemplateThumbnailBaker` が `authoringGraph` 経路 (未保存グラフをプレビューへ流し込む
既存の口) へ Template を差し込み、`ResolveVFXThumbnailTime` の時刻で捕まえて
`<.vfx と同じフォルダ>/.thumbnails/<name>.png` へ書く。Templates 直下へ置くと
`.vfx` の走査に混ざるため隠しフォルダへ分ける。

### Recipe — 空 Entry から始めない

`VFXRecipeLibrary` が Fire / Explosion / Impact / Smoke / Beam / Aura の層構成を
**構造として**持ち、`BuildGraphFromRecipe` が実際のグラフを組む。目的 × 規模 (S/M/L) ×
ループ × 素材ロールを選ぶと、ブレンド・描画順・寿命の関係が崩れていない骨格が出る。
公開パラメーター (`Intensity` / `Smokiness` など) も最初から生やす — 公開パラメーターの
無いグラフは結局 raw field を触ることになり、インスタンスからも AI からも制御できない。

**この表が recipe の唯一の正本**で、`vfx.guide` (AI) と Recipe ウィザード (人間) の
両方が同じものを読む。入口は VFX Editor の `Templates > New from Recipe...` と
AssetBrowser の `Create > VFX Graph > From Recipe / From Template`。

### 不変条件 (Tests/VFXTemplates)

同梱 Template は「AI と作業者が複製の出発点にするお手本」なので、警告ゼロに加えて
次を機械的に守らせる。走査は**再帰**にしてある (非再帰のままだとカテゴリ分けした瞬間に
その Template だけ検証の外へ落ちる)。

- `description` / `tags` / 公開パラメーターを必ず持つ
- `requiredRoles` が `kVFXAssetRoles` の語彙であること (綴り違いは survey が
  充足を判定できない形で静かに効かなくなる)
- グループ id が一意であること (重複すると層の選択が別の層を掴む)
- `thumbnailTime` が再生長の内側であること

## CPU / GPU で式を揃えるべき箇所

パーティクルは CPU シミュレーションと GPU シミュレーション (`ParticleGpuSim.cs.hlsl`) の
二重実装で、片方だけ直すと「CPU では正しいが GPU では違う見た目」という形で静かに壊れる。
以下は**必ず対で変更する**こと。各実装のコメントにも相互参照を書いてある。

| 内容 | CPU | GPU |
|---|---|---|
| 値ノイズ / 格子ハッシュ | `ParticlePass.cpp` の `ValueNoise3D` | `Rendering/ParticleNoise.hlsli` (描画側と共有) |
| カールノイズ | `ParticlePass.cpp` `CurlNoise` | `ParticleGpuSim.cs.hlsl` `CurlNoise` |
| 力場 | `ApplyForceFields` | 同名関数 |
| 周回 / 放射 | `ApplyOrbitalVelocity` | 同名関数 |
| カーブ評価 (size/velocity/rotation/drag) | `ParticleCurve::Evaluate` | `EvaluateCurve4` + `gCurveFlags` / `gCurveFlags2` |
| スプライトフレーム | `ComputeSpriteFrameState` | CS 末尾のスプライト計算 |
| 色ゆらぎ | `ApplyColorVariation` | `GpuParticle::colorScale` へ倍率を保持して毎フレーム再適用 |

定数バッファ / 構造体のレイアウトは `static_assert` で守られている。サイズを変えたら
**C++ と HLSL の両方**を直すこと。宣言が散っているものは特に注意:

- `GpuParticle` (96 bytes) — `ParticleGpuSim.cs.hlsl` / `ParticleGPU.hlsl` /
  `ParticleGpuSortKeys.cs.hlsl` / `ParticleGpuMesh.hlsl` の **4 箇所**
- `ParticleRenderCB` (b2, 96 bytes) — C++ の `GeometryPasses.hpp` が正本。HLSL 側は
  `Particle.hlsl` / `ParticleGPU.hlsl` / `ParticleGpuMesh.hlsl` / `Debug/ParticleOverdraw.hlsl` の **4 箇所**。
  読まない経路でも宣言を落とすとレイアウトがずれるので、パディングまで揃えること
- `GpuParticleSortCB` (b0, 32 bytes) — `GeometryPasses.hpp` と `Rendering/ParticleSortCommon.hlsli`
- `DecalCB` (b2) — `RenderPassContext.hpp` と `Material/Decal/Decal.hlsl`
- `TrailVertex` / `TrailCB` — `GeometryPasses.hpp` が正本。Trail ノードと per-particle Trail の
  リボンが同じ頂点・同じ CB・同じシェーダーを共有する

### GPU ソートと GPU メッシュパーティクル

`sortMode != None` と `meshParticlePath` は、いずれも GPU シミュレーションと併用できる。

- ソートは bitonic sort の 3 段構成 (`ParticleGpuSortKeys` / `ParticleGpuSortStep` /
  `ParticleGpuSortLocal`)。並べ替えるのは `(キー, 粒子 index)` の対だけで、
  **粒子プールそのものは動かさない** — プールはスポーン用のリングバッファで、
  要素の位置が変わると `gpuWriteHead` が指す場所が意味を失うため。
  比較距離がグループ内へ収まった以降は LDS 段が全段を一気に回す
  (これが無いと 10 万粒子で 150 回超のディスパッチになり、ソート自体が重くなる)。
- メッシュパーティクルは `ParticleGpuMesh.hlsl` の VS が `SV_InstanceID` で粒子を引く
  インスタンス描画 1 本。CPU 経路 (`MeshTrailRenderPass`) は粒子 1 個につき DrawCall 1 本なので、
  同じ .vfx でも CPU/GPU で DrawCall 数が桁違いになる。回転軸は CPU 経路と揃えてあるので、
  片方だけ変えると同じアセットが違う向きに見える。

シェーダーは `Assets/Shaders/` と `FBZZTestStandaloneProject/Assets/Shaders/` の
**二重コピー**である。両方を更新し、両方で `compile_shaders.bat` を通すこと。

## 実装状況（2026-07-20）

設計の Phase 0〜5 はソース実装済み。VFX Editorは独立プロセス`FBZZVFXEditor.exe`として、専用Application・
専用World・専用Physics World・専用Camera・専用RenderTargetを所有する。Editorは`--project`と`--asset`だけを
渡して起動し、ゲームScene自体をVFXEditorへロードしないため、Emitter／Graph追加はプロセス境界によって
編集Sceneへ混入できない。Hierarchy／Preview／Graph／Inspectorの全境界をリサイズ可能にした。

- Phase 0〜2: 型スキーマ、公開パラメーター、sparse override、Curve／Gradient／Random値ソース、TOML往復。
- Phase 3: HDR emissive、distortion、lit smoke、固定Beam／Ribbon、Mesh Particle、curl／vector field、
  motion-vector flipbook、GPU depth collision、bone/socket、skinned spawn、OnDeathイベント。
- Phase 4: 決定論的`vfx.preview`、5種テンプレート、schema駆動AI編集、dry-run／Undo／budget guardrail、
  auto-budget／LOD、Variant一括生成。参照画像・自然言語の判断はMCPクライアント側が画像と意味パラメーターを比較して反復する。
- Phase 5: Attribute Binding、Variant Sets、明示Sub-graph転送、Signal Graphの保存・検証・実行・Editor UI。
- UX: OSファイルD&D、File > Open、カメラ操作、scrub／step、MiniMap、focus、検証banner、live budget／runtime stats、
  asset-local Undo/Redo、Template適用、独立WorldのAI captureに対応。EditorのToolsメニュー、`.vfx`ダブルクリック、
  `Ctrl+Alt+V`から独立Appを起動できる。
- AI/IPC: MCPは既存`FBZZEditorCommandBus`へ接続し、Main Editorが`vfx.preview`とVFX captureだけを
  `FBZZVFXEditorCommandBus`へ転送する。VFXアセット変更はMain EditorのUndoStackで実行し、独立Appへ
  編集前flush／編集後reloadを通知するため、Scene操作のUndoとVFX画面のライブ同期を両立する。AI captureは
  操作用Previewとは別の専用World／Physics World／RenderTargetで評価し、開いているGraphの表示状態を混入させない。
- Cross-process D&D: AssetBrowserのImGui dragを追跡し、`FBZZ VFX Editor`ウィンドウ上でreleaseされた場合は
  アセットパスを専用IPCへ送る。Graph編集中は素材からEffect Nodeを追加し、Emitter編集時はPreview Worldだけに生成する。
- モジュール構成: VFX Editor は `Projects/Editor/{include/Editor,src}/VFXEditor/` 配下の
  モジュールとして独立させている。`Document`(VFXGraphDocument / History / Clipboard / Ops = UI非依存の
  編集対象)、`Services`(TemplateCatalog / PreviewController / SequenceExporter = 画面を持たない機能)、
  `Views`(VFXEditorPanel / GraphCanvas / GraphInspector / TimelineView / PreviewView)、
  `Application`(VFXEditorApp / VFXEditorSession)の4層。View 同士が共有する選択・再生位置・表示設定は
  すべて`VFXEditorSession`に集約し、View は Session への参照だけを持つ。Session は View を知らず、
  グラフ差し替え時の後始末だけをコールバックで受け取る(依存方向を Views → Application → Services → Document
  の一方向に保つため)。
- Graph UI: Graphドキュメントの画面構成を`VFXEditorPanel::DrawGraphLayout`へ分離し、`Graph Canvas | Viewport | Inspector`を
  常時同時表示する3列ワークスペースへ変更した。各境界はドラッグでリサイズでき、Viewportは列全体へ追従して
  Graph配線中も実描画結果を隠さない。Canvas空きスペースの右クリックはクリック位置へそのままノードを生成し、
  ノード上の右クリックはDuplicate/Deleteのコンテキストメニューへ切り替わる(Unity Shader/VFX Graph相当)。
  `Ctrl+D`で選択ノードを複製できる。
- Graph編集ergonomics(Unity VFX/Shader Graph相当): Add Effectメニューに検索フィルタ(Enterで先頭候補生成)、
  ノードのコピー/カット/ペースト(`Ctrl+C/X/V`、内部リンク保持・カーソル位置貼り付け)、ボックス選択集合への
  Duplicate/Delete一括適用、`F`選択フレーム/`A`全体フレーム。リンク右クリックでトリガー種別(On Complete/Start/
  Collision/Death)を即切替(Particle以外のsourceではCollision/Deathを無効化)。グループ枠/付箋(`VFXGraphGroup`、
  TOML保存・Undo対象)をタイトルバードラッグで内包ノードごと移動・右下ハンドルでリサイズ・Inspectorで色/メモ編集。
  プレビューに床グリッド表示トグル(AI captureは常にクリーン)。
- ノードごとの`enabled`をアセットへ保存し、無効化中もDAGの配線・時間を維持したまま生成とbudget消費を止める。
  Canvas、Inspector、複数選択の右クリック、MCPの`vfx_node_set_enabled`から同じ状態を操作できる。
- Graph Grid、Preview Floor Grid、Mini Map、Live Edit、Auto-Connect、主要レイアウト寸法は
  `Assets/EditorConfig/vfx_editor_settings.toml`へプロジェクト単位で永続化する。
- Undo/Redo は96件上限のasset-localスナップショットに状態IDを併記し、保存地点までUndoした場合だけ
  dirtyを解除する。Edit > HistoryでUndo/Redo件数・現在状態・保存状態を確認でき、履歴クリアも可能。
- DebugメニューはGraph/単体Emitterの両モードに置き、Particle Overdraw、VFX Gizmo、Floor Grid、
  Loop Seam、Pause/Restart、即時validation、Graph budgetとoverdraw実測値を同じ場所で確認できる。
  DX11/DX12ランタイムコンパイルと外部HLSLビルドのエラーは起動前分も共有レジストリへ保持し、
  赤い常設bannerと詳細Windowへpath/entry point/target/コンパイラ本文を表示する。
- AIのVFX連携は44ツール。Graph設定、ノード複製/metadata、link更新、parameter削除/unbind、
  Variant削除、Group/Note CRUDまでUndo可能に公開し、`vfx_inspect_graph`もCanvas座標・Group・budget上限を返す。

EditorMcpのTypeScriptテストは25件成功。C++／HLSLの最終コンパイル確認は本リポジトリ規約どおり
Visual Studio 2022のソリューションビルドで行う。

## AAA表現の到達目標に対する追加実装（2026-07-21）

「実際にAAA級のエフェクトを作れるか」の観点で棚卸しし、不足していた4系統を実装した。

### スキーマ基盤の完全化

- `ITypeSchema` に配列アクセサ・`MakeEnumProperty`・`MakeIntProperty` を追加。
  `ResolveProperty` を `"bursts[2].count"` 形式へ対応させ、const／非constの二重実装を1本に統合した。
- `ParticleBurst` スキーマを新設し、載っていなかった leaf を全て追加。
  併せて `JsonToSchemaValue` が Enum 未対応で「スキーマには見えるがAIから変更できない」状態だったのを修正し、
  `vfx.diff` が配列要素の増加を差分から落としていたのも直した。
- `CollectLeafPaths` を `reflection` へ集約し、Inspector・AI・テストが同じ走査結果を見るようにした。
- スキーマから UI を生成する `SchemaInspector` を追加し、Graph Inspector へ
  「Advanced (schema)」として全 leaf を常時表示する。除外リストは**意図的に持たない**
  （それ自体が2つ目の手書き面になりドリフトを再生産するため）。

### over-lifetimeモジュール（CPU/GPU両実装）

非等方サイズ(`sizeAxisScale`)、回転カーブ、dragカーブ、周回／放射(`orbitalVelocity`/`radialVelocity`)、
`inheritVelocity`、Flipbookのランダム開始フレーム／ランダム行、per-particle色ゆらぎ(`colorVariation`)、
Light/Decalの時間カーブ、per-particle Trail。

- 非等方サイズは頂点フォーマットを太らせず定数バッファ経由にした（エミッター単位の値のため）。
- 色ゆらぎは当初 Custom Data → `COLOR1` の設計だったが、パーティクル描画は `h.particleShader` 固定で
  **カスタムシェーダー経路が存在せず COLOR1 を読む側がいない**ため、固定シェーダーでも効く形へ変更した。
  GPU側は毎フレーム色を作り直すので、`GpuParticle::colorScale` に倍率を保持して再適用する。
- per-particle Trail は履歴点へビルボードを連ねる方式で、真の連続リボンではない。
  `ParticleVertex`／シェーダーがビルボード前提で任意方向の帯を表現できないため。
  `trailRibbon = true` にすると履歴点をポリラインとみなし、Trail ノードと同じマイター接合で
  1 枚の連続した帯を張る (剣閃・魔法の軌跡・リボン状の炎)。帯は 1 エミッターぶんを 1 DrawCall で
  描くため、色は粒子ごとではなく `colorStart` / `colorEnd` を帯の長さ方向へ配る。
  GPU シミュレーションでは履歴を保持できないため Trail 有効時は CPU へ縮退する
  (縮退したことは `vfx_lint` の `GPU_FALLBACK` と Editor の 3 か所に出る)。

### 描画

- `BlendMode::PREMULTIPLIED` を DX11／DX12 両方へ追加（発光する芯と背景を隠す煙を1枚のテクスチャで両立）。
- `renderPriority` によるエミッター間の描画順制御。同値ならカメラから遠い順。
- 受け影。既存 `ComputeShadow` を再利用し、ビルボードは法線を持たないため N=L を渡して法線バイアスを無効化する。
- ボリュメトリック煙。専用パスではなく **PS内レイマーチ**(`effectsFlags` bit4)とし、
  既存のVB/PSO/ソート/受け影をそのまま流用する。six-way lightingとは役割が重複するため排他。
- フリップブック モーションベクター生成（ブロックマッチング）。DirectXTexがEngineに閉じているため
  生成本体は `Engine/Asset/FlipbookMotionVectors.cpp` に置き、EditorとMCPから呼ぶ。

### オーサリングUX

- **マルチトラック タイムライン**: 帯のドラッグで `startOffset`、右端で `duration`、三角で `burst.time` を編集。
  スケジュールはランタイムと同じ `BuildVFXGraphSchedule` から取るため実行時とずれない。
  掴んだ瞬間に一度だけ `PushGraphUndo()` するので1操作単位で戻せる。
- **プレビュー環境プリセット** (Dark／Daylight／Interior／Neutral Gray)、**N体同時プレビュー**、
  **A/B比較**、**ループ継ぎ目確認**。
- **Overdrawヒートマップ**: 同じジオメトリを計数シェーダーで描き直し、重なり枚数を色で表示する。
  Particleパスとは独立したパスにして、診断のために本番の描画順が変わらないようにしている。
- **汎用Flipbook Baker**: 決定論スクラブで出力したPNG連番を、指定列数または自動算出した
  等間隔グリッドへ左上から再生順に結合する。異なる解像度の混在は暗黙に拡縮せずエラーにし、
  16384px上限を事前表示する。出力PNGにはMip無効・Clampの`.meta`を併記し、フレーム境界の色混入を防ぐ。
- **GPU時間の常時表示**: `Particle` パスの実測msをStatsへ出す。budgetは粒子数でしか測れないが、
  実際のボトルネックはfill rateであることが多いため。
- **GPU縮退の常時表示**: `simulationMode = Gpu` にしても11個の条件のどれか1つで黙ってCPUへ落ちる。
  判定は `ParticleGpuSimulation.hpp` に集約し、**要求ではなく実際に走る経路**を
  Inspector の Simulation コンボ直下・グラフノードのバッジ・タイムライン下部の3か所へ出す。
  縮退時は原因の設定名（`sortMode` など）まで名指しし、tooltip に GPU へ載せる代替案を出す。
- **SubGraph のドリルダウン**: SubGraph ノードのダブルクリック（または右クリック → Enter Sub Graph）で
  中へ入り、Canvas 上部のパン屑で任意の段へ戻る。多段の入れ子も辿れる。
  中へ入る前に親グラフを必ず保存する — 親を dirty のまま置き去りにすると、
  戻ってきたときにディスクと編集内容のどちらが正か決められなくなるため。
- **Reroute ノード**: 実体も時間も持たない配線の中継点。ノードが 20 を超えて配線が
  交差だらけになったときに、実行 DAG を変えずに線を折り曲げるための逃げ道。
  Canvas ではピンだけの最小サイズで描き、Timeline にも帯を出さない。
  `startOffset` / `duration` は常に 0 として扱う（見た目の整理が挙動を変えないため）。
- **ノードのサムネイル**: Particle / Trail / Decal / Mesh ノードへ素材の絵を出す。
  `.mat` は albedo を代表画にする。ホバーで 192px へ拡大（粒子素材の要である
  アルファの縁の勾配は 48px では見えない）。
- **自己影と角度フェード**: `selfShadowStrength`（粒子群が自分へ落とす影）と
  Decal の `angleFadeStrength` / `angleFadeDegrees`（斜面での引き伸ばしを消す）を
  Inspector から調整できる。どちらも近似の内容と限界を tooltip に書いてある。

環境プリセット・床グリッドはいずれも**操作用Preview Worldのみ**へ適用し、
AI capture用Worldには持ち込まない（評価画が担当者の表示設定で変わると視覚判断が再現しないため）。
Overdraw とギズモだけは例外で、AI が `vfx_preview` の `view` で明示要求したときに限り診断表示を許す
（既定の評価画は変えない）。この規則は埋め込み版（`EditorApp`）と独立版（`VFXEditorApp`）で共通。

AI 側の評価基盤（`vfx_preview_metrics` / `vfx_preview_curve` / `vfx_preview_compare` /
カメラ制御と `vfx_preview_sweep`）は `Docs/ai-editor-integration.md` と
`Docs/design/vfx-ai-roadmap.md` を参照。

EditorMcpのTypeScriptテストは39件成功。

## 空間の親子とAI連携（2026-07-26）

### `parentNodeId` — 実行の因果とは独立した空間の木

`VFXGraphLink` が表すのは「いつ発火するか」であって「どこに置くか」ではない。衝撃波とそこから出る
煙・火花のように**まとめて動かしたいノード群は、発火順とは別の軸でまとまる**。両者を link 1 本に
兼任させると「傾けたいだけなのに発火順まで変わる」形で必ず破綻するため、`VFXGraphNode::parentNodeId`
として独立した参照を持たせた。

ランタイムはノードの実体を直接親子にせず、**Transform だけを持つ常時 active のグループ GameObject**
(`VFXGraphComponent::runtimePivots`) を間に挟む。実体はスケジュールに合わせて `SetActive` を
切り替えるため、実体同士を親子にすると「親ノードの時間が終わった瞬間に子も消える」という形で
空間の入れ子が実行の時間へ漏れ出す。グループを挟むことで位置だけを継承し、時間と
Mesh ノードの膨張スケールのような実行時エンベロープは波及しない。

- 検証: 自己参照・未知 id・循環は `ValidateVFXGraphAsset` がエラーで弾く（保存を止める）。
  Entry / Delay を親にした場合と `attachBone` との併用は `CollectVFXGraphWarnings` の
  `PARENT_HAS_NO_TRANSFORM` / `PARENT_OVERRIDDEN_BY_SOCKET` で警告する（保存は通す）。
- Editor: Inspector の Parent Node コンボ（子孫は選択肢から除外）、Graph モードの
  Hierarchy ツリー（D&D で組み替え、循環は落とせない）、Canvas ノードの `child of X` 表示。
- Preview の ImGuizmo でノードを直接掴んで動かせる。ギズモ行列は実体の world ではなく
  **オーサリング値と親チェーンから組み直す**（実体を使うと Mesh の膨張スケールが混ざり、
  離した瞬間に値が跳ねる）。現在の操作に対応する成分だけ書き戻し、分解・再合成の誤差が
  触っていない軸へ漏れないようにしている。

### ランタイム生成物の隔離

生成物は `GameObject::runtimeGenerated` を立てる。以前は名前を `__` で始める規約で
シリアライズ除外を表していたが、規約だと「見せる名前」と「保存するか」が同じ文字列に
相乗りするため、表示名を読みやすくした瞬間に保存対象へ戻る。フラグへ分離したことで
生成物の名前をノード名そのもの（旧: `__VFX_<uuid36>_3_Smoke`）にできた。

名前を短くしたぶん複数配置時に `scene.Find` が別インスタンスを掴みうるため、
`ParticleEmitter::subEmitterScopeRoot` を追加して SubEmitter の名前引きを owner 配下へ
閉じた。副次的に、UUID 込みの名前では一致しようがなくグラフ内 SubEmitter 参照が黙って
無反応だった既存の不具合も解消している。

Scene Hierarchy・`scene.tree`・`scene.snapshot` はいずれも既定で生成物を除外し、
除外件数を併記する（「出ていない = 存在しない」と読ませないため）。

### AI 連携

- `vfx.graph` がノードの `transform` / `parentNodeId` / `attachBone` を返す。これが無いと
  AI は位置の決まり方を知らないままフィールドを書き換えることになる。
- `vfx.node.setParent` を新設。`vfx.node.setField` でも `parentNodeId` は書けるが、それだと
  循環や実体を持たない親を保存直前の一般検証でしか弾けず、AI は失敗理由から何を直せばよいか
  判断できない。専用コマンドで `PARENT_HAS_NO_TRANSFORM` / `PARENT_CYCLE` / `PARENT_SELF` を返す。
- `vfx.guide` に `hierarchy` topic を追加。link と親子の混同は AI が最も踏みやすい。
- `vfx.runtime` を新設。ノードが画に出ない原因は「起動していない」「イベント待ちのまま」
  「起動しているが見えない」の 3 通りで画像から区別できない。`vfx.lint` は静的解析なので
  1 つ目しか見えず、2 つ目はスケジュール表に位置を持たないため時刻からも推測できない。
- `vfx.lint` の各 issue に `fix` / `autoFixable` / `caution` を添え、`vfx.repair` の対象を
  `SHEARED_SPRITE` / `LIGHTING_SATURATED` / `ALPHA_NO_SORT` / `MESH_NO_FADE` /
  `PARENT_HAS_NO_TRANSFORM` まで拡張した。修正が推論ではなく参照になる。

### 素材の観測（`AnalyzeTexture`）

AAA 品質の頭打ちになっていたのは「AI が素材の中身を知らない」ことだった。テクスチャは
パス文字列でしかなく、`blendMode` / `alphaSource` / `spriteColumns` / `softParticles` は
どれも素材の中身で正解が変わるのに、ファイル名から推測するしかなかった。しかも誤りは
**プレビュー画像から原因を特定できない**形で現れる（アルファが機能していない素材は
矩形の板として描かれ、事前乗算素材は縁が黒く縁取られる）ため、反復しても収束しない。

`Engine/Asset/TextureAnalysis.cpp` が画素を走査し、機械的に決まる項目を観測へ置き換える:

| 観測 | 決まる設定 |
|---|---|
| アルファの min/max（実データを持つか） | `alphaSource` — 全画素 1.0 なら Luminance が必須 |
| 全画素で `RGB <= A` か | `blendMode = Premultiplied` |
| 中心と外周の輝度比（発光する芯） | `blendMode = Additive` |
| 不透明部分の占有率 | `blendMode = Alpha` + `sortMode = BackToFront` |
| タイル境界の不連続 / コマ間の内容量の揃い | `spriteColumns` / `spriteRows` |
| 縁のアルファ勾配 | `softParticles` |

コマ割り検出は**境界のまたぎ差を 1 画素内側の差で正規化**する（生の差分だと絵の細かさで
値が変わる）。4x4 が正しいアトラスは 2x2 でも境界が立つため、素点をコマ数で割って
「分割数が大きいほど勝つ」偏りを消している。空コマがあるグリッドは候補から外す。

統計は縮小画像で取り、コマ割り判定だけは原寸で行う（縮小すると境界の不連続が平均化されて
消える）。DDS がメタデータで事前乗算を宣言している場合は、画素からの推測より優先する。

同じ関数を **Editor の VFX Inspector（Texture Analysis パネル）と AI（`vfx.textureAnalyze`）の
両方が呼ぶ**。人が見る面と AI が読む面を別実装にすると必ずドリフトするため、
`CollectVFXGraphWarnings` と同じ方針で単一の正本に寄せてある。推奨は Inspector 上で
1 クリック（または Apply All）で適用でき、AI 側は `recommendations[].schemaPath` / `.value` を
`vfx.node.setField` へそのまま流せる。

### マテリアルまで含めた突き合わせ（`AnalyzeMaterial`）

テクスチャ単体の解析には穴があった。`ParticleEmitter` に `materialPath` があると、
`ParticlePass` は **`.mat` の `blend_mode` で `emitter.blendMode` を上書きする**
（`materialPath` を描画設定の単一の信頼元にするため）。つまり `.mat` を割り当てた時点で
Inspector や AI が設定した `blendMode` は実行時に使われない。しかも `.mat` が指す
テクスチャは `emitter.texturePath` とは別物なので、テクスチャ単体の推奨は
実際の描画と噛み合わない。

`AnalyzeMaterial` は `.mat` を読み、albedo テクスチャも解析して突き合わせる。
返す情報は**直すべき場所で 2 つに分けてある**:

- `findings` — `.mat` 自体を直すべき問題。`blend_mode` が Opaque、`render_path` が
  `particle` でない、albedo 未設定、テクスチャの中身が要求するブレンドとの食い違い。
- `recommendations` — `.mat` では表現できず Emitter 側にしか無い設定。
  `alphaSource` / `spriteColumns` / `spriteRows` / `sortMode`。

この分離をしないと「blendMode を直せ」と言われた AI が Emitter 側を書き換えて、
保存はされるが効かない、という往復に入る。`particle.blendMode` は「実行時に上書きされる値」
として recommendations に含めるが、reason で効かないことを明示している。

### 素材の棚卸し（`vfx.assetSurvey`）

`vfx.guide` の recipe は層構成を示すが、その層を作れる素材が手元にあるかは別問題。
AI は素材を 1 枚ずつ解析して初めて種類が判るため、何を持っているか知らないまま
recipe に沿おうとして「無い素材を前提にしたグラフ」を組んでしまう。

survey はプロジェクトのテクスチャを分類し、`core`(glow) / `body`(smoke) /
`sparks`(spark) / `animated`(flipbook) の 4 役割に対する充足を返す。
役割の語彙は guide の recipe と揃えてある（別に持つと「guide は煙を要求するが
survey は要求しない」という食い違いが生まれる）。`missingRoles` には代替案を添える
(core が無ければ body 素材を Additive で小さく使う、等)。

解析は 1 枚あたり数十 ms かかるため、統計用の縮小を 128px まで強め、既定 120 枚で
打ち切る（`truncated` で判る）。全走査で分単位かかると AI が最初の 1 手で詰まる。

Editor の VFX Inspector も `.mat` 割り当て時は Material Analysis パネルへ切り替わり、
先頭で「Blend Mode は .mat 由来で Emitter の設定は無視される」を警告として出す。

### シェーダー変数の目録（`shader.inspect`）

素材とマテリアルまで読めるようになっても、最後にシェーダー側が残っていた。
`.mat` の `params` も VFX Mesh ノードの `animatedParam` も「シェーダー変数名」を要求するが、
**その一覧を知る手段が無かった**。存在しない名前を書いても保存は通り、実行時は
`Material` が名前で束縛できずに黙って無視する。Dissolve の `alphaCutoff` を動かすつもりが
綴り違いで何も起きない、という失敗はプレビューからは「変化しない」としか見えない。

HLSL を読ませるのは現実的でないうえ、**未使用変数はコンパイル時に消える**ため
ソース上の宣言は正本にならない。実際に効くのは PS バイトコードのリフレクション結果
(`renderer::ShaderDescriptor`) だけで、これは `DX11Shader::Init()` が構築し
`IShader::GetDescriptor()` で取れる。`ResourceManager::LoadShader` 経由で読み、
変数名・要素数 (`columns`)・型とテクスチャスロットを返す。

この目録は 3 箇所で使う:

- `shader.inspect` — 目録そのもの。書ける名前を確認する入口。
- `vfx.materialAnalyze` — `.mat` の `params` を照合し、存在しないキーと
  要素数不足（float3 に 1 個だけ渡すと残りが 0 になり、色が黒くなる典型例）を `findings` へ。
- `vfx.lint` — Mesh ノードの `animatedParam` を照合し `UNKNOWN_SHADER_PARAM` を出す。
  マテリアル未割当時は `VFX_MESH_FALLBACK_MATERIAL` のシェーダーを見る。

これで AI が持つ情報が「素材が何か」「どう描かれるか」「何が足りないか」「何を書けるか」で
一巡した。設定に関する推測はほぼ観測へ置き換わっている。

EditorMcp の TypeScript テストは 35 件成功。
