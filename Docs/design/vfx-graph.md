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
| command | `vfx.template.apply` | テンプレート`.vfx`を複製し新規アセット化 |

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
- Graph UI: Graphドキュメントの画面構成を`VFXGraphEditor`へ分離し、`Graph Canvas | Viewport | Inspector`を
  常時同時表示する3列ワークスペースへ変更した。各境界はドラッグでリサイズでき、Viewportは列全体へ追従して
  Graph配線中も実描画結果を隠さない。Canvas空きスペースの右クリックはクリック位置へそのままノードを生成し、
  ノード上の右クリックはDuplicate/Deleteのコンテキストメニューへ切り替わる(Unity Shader/VFX Graph相当)。
  `Ctrl+D`で選択ノードを複製できる。

EditorMcpのTypeScriptテストは25件成功。C++／HLSLの最終コンパイル確認は本リポジトリ規約どおり
Visual Studio 2022のソリューションビルドで行う。
