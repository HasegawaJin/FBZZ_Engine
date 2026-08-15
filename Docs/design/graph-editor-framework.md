# Graph Editor Framework — ノードグラフツールの共通基盤

`AnimationGraphPanel` / `VFXGraphCanvas` / (予定) `BehaviorTreePanel` が
それぞれ独自に実装しているノードグラフ編集の「土台」を 1 箇所へ集約する。

## 背景 — 何が重複しているか (実測)

| ファイル | 行数 |
|---------|------|
| `Projects/Editor/src/Panels/AnimationGraphPanel.cpp` | 3648 |
| `Projects/Editor/src/VFXEditor/Views/VFXGraphCanvas.cpp` | 1749 |

同じ ImNodes API を両方が独立に叩いている (出現回数 A=Animation / V=VFX):

| API | A | V |
|-----|---|---|
| `SetNodeGridSpacePos` | 7 | 3 |
| `EditorContextGetPanning` | 7 | 3 |
| `PushColorStyle` | 40 | 8 |
| `PushStyleVar` | 17 | 4 |
| `IsLinkCreated` / `IsLinkDestroyed` / `IsLinkDropped` | 各 1 | 各 1 |
| `NumSelectedNodes` | 3 | 1 |
| `MiniMap` | 2 | 4 |

### 単なる重複ではなく「挙動が食い違っている」

**カーソル基準ズーム**が別々の数式で 2 回実装されている。

`VFXGraphCanvas.cpp:834-876` — カーソル下の論理座標を逆算して復元する方式。
倍率は `std::pow(1.12f, wheel)`、範囲は **0.45〜1.80**。
スタイルは `ImNodesStyle` の各フィールドを直接書き換える。

`AnimationGraphPanel.cpp:1485-1508` — 新旧倍率の比 `ratio` からパンを補正する方式。
倍率は `oldZoom + wheel * ZOOM_STEP` (Ctrl で 1.5 倍速)、範囲は `ClampZoom` 依存。
スタイルは `ImNodes::PushStyleVar` を 8 回。

**結果として、同じエディタ製品の中で「グラフの拡大操作」がツールごとに違う。**
Ctrl の意味も、ズーム上限も、スタイルの拡縮対象も一致していない。
ツールが 3 つ目・4 つ目と増えるたびにこの分岐が増える。

### そのほか両方が持っている実装

ImNodes コンテキストの生成/破棄、テーマ適用、ID 空間の規約 (node/pin/link)、
選択の吸い上げ、リンク作成の双方向ドラッグ解決、リンク削除、Delete キー処理、
右クリックメニューとスポーン座標変換、Asset Browser からの D&D 受け、
ミニマップ、自動整列、Undo、Play 中の編集禁止ガード、
実行中ノードのハイライト、`AssetDirtyRegistry` 連携。

## 設計方針

### D1. 「イミディエイトモードのビュー記述」方式にする

継承ベースの `IGraphModel` を各ツールに実装させる方式は採らない。

**WHY**: 3 つのグラフはデータモデルが根本的に違う。
- Animation = ステートマシン (状態 + 遷移。遷移が独自の属性を大量に持つ)
- VFX = DAG (トポロジカル評価順が意味を持つ)
- BehaviorTree = 木 (親 1 つ・子順序が優先度)

共通の抽象データモデルを定義しようとすると、3 つの和集合になって
どのツールにとっても使いにくい型ができる。実際 `GraphLayout.hpp` が
`nodePositions` を `map<string, ImVec2>` にしているのは Animation の
「状態名がキー」という都合で、VFX はノード自身が `editorX/editorY` を持つ、
という食い違いが既に起きている。

代わりに**フレームごとにツールが「見た目の記述」を組み立てて渡す**。
フレームワークはデータを所有せず、描画と入力の解釈だけを担う。

```
ツール (データ所有)                  フレームワーク (描画・入力)
   │                                        │
   │  GraphView を組み立てて渡す ─────────→ │  ImNodes へ描画
   │                                        │  入力を解釈
   │ ←──────── GraphInteraction を返す ──── │
   │  自分のモデルへ適用 + Undo             │
```

これは ImGui 自体の哲学と同じで、既存コードの書き味とも揃う。
ノード数は実用上 200 未満なので、毎フレームのビュー構築コストは問題にならない。

### D2. リンクの検証はフレームワークに持たせない

`VFXGraphCanvas.cpp:1589-1632` は「仮追加 → 検証 → 失敗ならロールバック」を
フレームワーク側でやろうとすると、検証関数をコールバックで渡すことになる。

代わりに **フレームワークは「リンクが作られようとした」という事実だけを返し**、
ツールが自分のルールで判断して適用する。失敗時は
`canvas.ReportError(message, {nodeIds})` を呼べば、フレームワークが
一定秒だけ該当ノードを赤く光らせる (`SetTransientGraphError` 相当)。

**WHY**: 検証ルールはツール固有 (BT は「リーフに子」「循環」、VFX は
トポロジカルソート可能性、Animation は遷移の重複)。コールバックにすると
「いつ呼ばれるか」がフレームワーク側の都合になり、Undo のタイミングと
噛み合わせるのが難しくなる。

### D3. ノード本体の描画だけはコールバック

`ImNodes::BeginNode` / `EndNode` の間でしか描けないため、これだけは
ツールへ制御を渡す必要がある。`GraphNodeView::drawBody` に
`std::function<void()>` を持たせる。

**WHY `std::function` を許容するか**: 毎フレーム最大 200 個の
`std::function` 構築が発生するが、キャプチャは
ポインタ 1〜2 個 (`this` とノード ID) に収まるので SBO 内で完結し、
ヒープ確保は起きない。

### D4. Undo はフレームワークに持たせない

`VFXEditorSession` のスナップショット方式と `AnimationGraphPanel` の
`LambdaCommand` + `ActiveID` 追跡方式は、どちらもツールのデータ構造に
密結合している。

フレームワークが提供するのは **「ユーザー操作の境界」の通知**だけにする。

```cpp
// ドラッグ中は毎フレーム位置が変わる。1 操作 = 1 Undo にまとめるため、
// 「操作が始まった / 終わった」だけをフレームワークが教える。
bool interaction.dragStarted;   // このフレームでドラッグが始まった
bool interaction.dragEnded;     // このフレームでドラッグが終わった
```

ツールはこれを見て `PushUndo()` を 1 回だけ呼ぶ。
これで `VFXGraphCanvas.cpp:1575-1586` の `graphEditInProgress` フラグ管理が
各ツールから消える。

### D5. 既存グラフの共通キャンバス移行状態 (2026-08-09 更新 — 移行完了)

3 ツールすべてが `GraphCanvas` 経由になり、**旧 ImNodes 描画経路は削除済み**。

- `VFXGraphCanvas`: `GraphView` / `GraphInteraction` 経由。VFX 固有の検証・Undo・Inspector・SubGraph は Session 側。
- `AnimationGraphPanel`: ステートマシンと Blend Tree の両方。レイヤー差し替え・遷移条件・保存は Panel / Inspector の責務。
- `BehaviorTreePanel`: 新規。フレームワークの 3 つ目の利用者で、木構造なので自動整列がそのまま効く。

**旧経路を残さない理由**: 当初は「比較用・安全なロールバック用」として `m_useGenericCanvas`
フラグ付きで両方を残していたが、既定が共通経路である以上、旧経路は**誰も実行しないコード**で、
壊れても気付けない。同じ事実を書く面が 2 つある状態そのもので、このリポジトリが
VFX のスキーマ設計でわざわざ潰した構造と同型だった。削除して片面へ畳んだ。

- `VFXGraphCanvas.cpp` から旧 `Draw` (883 行) と旧グループ描画ヘルパー (112 行) を削除
- `AnimationGraphPanel.cpp` から旧 `DrawNodeCanvas` / `DrawBlendTreeCanvas` / `HandleCanvasWheel` を削除 (計約 1600 行)
- 両者が持っていた**自前の ImNodes コンテキスト**も削除。パン・ズーム・選択が
  共通キャンバス側と二重に存在していた状態を解消

### D6. imnodes のリンク構成 (2026-08-09)

`fbzz_editor` は `imnodes` ターゲットを **PUBLIC リンク**する。

以前は `imnodes.lib` をリンクせず、`AnimationGraphPanel.cpp` が `#include <imnodes.cpp>` で
実装を自分の `.obj` へ抱え込んでいた。そのため「他のどの .cpp も同じことをしてはならない」
という不変条件を `GraphCanvas.hpp` のコメントでしか守れず、破ったときの合図が
リンクエラーだけだった。正式リンクにすれば二重定義はリンカが機械的に弾く。
`Projects/Editor/CMakeLists.txt` の手動 include パス (`EDITOR_IMNODES_INCLUDE`) も削除した
— パスだけが通っていると、同じ回避策をいつでも再発明できてしまう。

---

## API

### ファイル構成

```
Projects/Editor/include/Editor/GraphEditor/GraphView.hpp        ビュー記述の型
Projects/Editor/include/Editor/GraphEditor/GraphInteraction.hpp 入力結果の型
Projects/Editor/include/Editor/GraphEditor/GraphCanvas.hpp      本体
Projects/Editor/include/Editor/GraphEditor/GraphLayoutAlgo.hpp  自動整列
Projects/Editor/src/GraphEditor/GraphCanvas.cpp
Projects/Editor/src/GraphEditor/GraphLayoutAlgo.cpp
```

### ビュー記述

```cpp
enum class GraphPinShape { Circle, CircleFilled, Triangle, TriangleFilled, Quad, QuadFilled };
enum class GraphLinkPattern { Solid, Dashed, Dotted };

// ピン 1 本。id はツールが決める任意の整数 (フレームワークは中身を解釈しない)。
struct GraphPinView {
    int         id    = 0;
    const char* label = nullptr;   // 空なら描かない
    ImU32       color = 0;         // 0 なら既定色
    ImU32       hoveredColor = 0;
    GraphPinShape shape = GraphPinShape::CircleFilled;
};

struct GraphNodeView {
    int    id       = 0;
    ImVec2 position = {};          // 論理座標 (ズーム倍率を掛ける前)

    std::string title;
    ImU32 titleColor      = 0;     // 静的な状態 (種別 / エラー / 検索ヒット)
    ImU32 titleTextColor  = 0;
    ImU32 backgroundColor = 0;     // 実行中の状態。面を分ける規約 (下記)
    ImU32 outlineColor    = 0;
    float outlineThickness = 0.0f;
    float titleFontScale = 1.0f;
    float pinFontScale   = 1.0f;
    float bodyFontScale  = 1.0f;

    std::vector<GraphPinView> inputs;
    std::vector<GraphPinView> outputs;
    bool drawDefaultInputs  = true;
    bool drawDefaultOutputs = true;
    bool drawBodyInStaticAttribute = true;
    std::function<void()> drawTitle; // タイトル帯内のツール固有UI (空なら title)

    // BeginNode / EndNode の間で呼ばれる。ツールが本体を描く。
    std::function<void()> drawBody;

    // ツールチップ (ホバー時)。空なら出さない。
    std::string tooltip;
};

struct GraphLinkView {
    int   id      = 0;
    int   fromPin = 0;
    int   toPin   = 0;
    ImU32 color   = 0;
    ImU32 hoveredColor = 0;
    ImU32 selectedColor = 0;
    GraphLinkPattern pattern = GraphLinkPattern::Solid;
    float thickness = -1.0f;
    float curveStrength = -1.0f;
    float curveMaxTangent = -1.0f;
    float lineSegmentsPerLength = -1.0f;
    float arrowSize = -1.0f;
    float arrowPosition = -1.0f;
    float hoverDistance = -1.0f;
};

// 注釈枠 (VFX の Group 相当)。使わないツールは空で良い。
struct GraphGroupView {
    int         id = 0;
    ImVec2      min = {}, max = {};
    std::string title;
    ImU32       color = 0;
    bool        selected = false;
    float       rounding = 6.0f;
    float       borderThickness = 1.5f;
    float       titleHeight = 22.0f;
    float       fillAlpha = 0.16f;
    float       borderAlpha = 0.78f;
    ImU32       titleTextColor = IM_COL32(240, 244, 250, 255);
};

struct GraphView {
    std::vector<GraphNodeView>  nodes;
    std::vector<GraphLinkView>  links;
    std::vector<GraphGroupView> groups;
    std::function<void()> drawOverlay; // EndNodeEditor 後の選択枠・実行状態など
};
```

**色の使い分け規約** (`VFXGraphCanvas.cpp:933-946` の方針を全ツールへ広げる):
- **タイトル帯 = 静的状態** (ノード種別 / 未接続 / エラー / 検索ヒット)
- **背景 = 実行中の状態** (Running / アクティブ)
- **枠線 = 選択・強調**

WHY: 3 つの面に 3 種類の情報を割り当てると、どれか 1 つが変わっても
他の情報が読めなくなることがない。同じ面に詰め込むと
「エラーなのか実行中なのか」が判別できなくなる。

### 文字サイズ

`GraphCanvas::Config::fontScale` を基準に、各 `GraphNodeView` の
`titleFontScale` / `pinFontScale` / `bodyFontScale` を乗算する。
倍率は `0.25〜4.0` に制限し、非有限値は `1.0` として扱う。

**WHY 領域ごとに分けるか**: ノード名は構造把握、ピン名は接続確認、本文は詳細確認を担う。
同じ倍率に固定せず、グラフの密度やツールの用途に合わせて情報の優先順位を調整できるようにする。

### リンクの外観

リンクは `GraphLinkView` 単位で、色・選択色・ホバー色・実線/破線/点線・太さ・曲率・
矢印サイズ/位置・ホバー判定距離を変更できる。値が `-1` の項目はキャンバスの既定値を使う。

**WHY リンク単位で持つか**: VFX のイベント線、Animation の遷移線、BehaviorTree の制御線は
同じ「接続」でも意味が異なる。キャンバス全体のテーマだけにすると、実行順・イベント・警告を
色だけで表現することになり、視認性とアクセシビリティが落ちる。

破線・点線は ImNodes のBezier曲線を短い線分へ分解して描く。通常の実線は従来のBezier描画を
維持するため、既存グラフの描画コストと見た目を変えない。

### 入力結果

```cpp
struct GraphNodeMove { int nodeId; ImVec2 position; };

struct GraphGroupMove { int groupId; ImVec2 delta; };
struct GraphGroupResize { int groupId; ImVec2 min, max; };

struct GraphInteraction {
    // ── リンク ──
    bool linkCreated  = false;
    int  createdFromPin = 0, createdToPin = 0;
    // ピンをノード本体へドロップした場合 (Unity 相当の操作性)
    bool linkDroppedOnNode = false;
    int  droppedFromPin = 0, droppedOnNode = 0;
    std::vector<int> destroyedLinks;

    // ── 選択 ──
    bool selectionChanged = false;
    std::vector<int> selectedNodes;
    std::vector<int> selectedLinks;

    // ── 移動 ──
    std::vector<GraphNodeMove> movedNodes;
    bool dragStarted = false;   // 1 操作 = 1 Undo の境界
    bool dragEnded   = false;

    // ── コマンド ──
    bool deleteRequested    = false;   // Delete キー
    bool duplicateRequested = false;   // Ctrl+D
    bool copyRequested      = false;
    bool cutRequested       = false;
    bool pasteRequested     = false;
    bool selectAllRequested = false;   // Ctrl+A
    bool clearSelectionRequested = false; // Escape
    bool frameAllRequested = false;   // Home / A
    bool frameSelectionRequested = false; // F

    // ── 空白の右クリック ──
    bool   contextMenuRequested = false;
    ImVec2 contextSpawnPosition = {};  // 論理座標へ変換済み

    // ── ピン ──
    int hoveredPin = -1;
    bool pinContextMenuRequested = false;
    int contextMenuPin = -1;
    bool nodeContextMenuRequested = false;
    int contextMenuNode = -1;
    bool groupContextMenuRequested = false;
    int contextMenuGroup = -1;

    // ── Asset Browser からの D&D ──
    bool        assetDropped = false;
    std::string droppedAssetPath;
    ImVec2      dropPosition = {};     // 論理座標へ変換済み

    int hoveredNode = -1;
    int hoveredGroup = -1;
    bool groupClicked = false;
    int clickedGroup = -1;

    std::vector<GraphGroupMove> movedGroups;
    std::vector<GraphGroupResize> resizedGroups;
};
```

### 本体

```cpp
class GraphCanvas {
public:
    struct Config {
        const char* id = "##graph";      // ImGui / ImNodes の ID
        float minZoom  = 0.45f;
        float maxZoom  = 1.80f;
        bool  showMiniMap = true;
        bool  showGrid    = true;
        float fontScale  = 1.0f;
        // 編集を許可するか。false なら移動・接続・削除を受け付けない。
        // WHY: Play 中はランタイム監視専用にする (AnimationGraphPanel.cpp:48-52 の規約)。
        bool  editable = true;
        bool  applyNodePositions = true;
    };

    // IPanel::OnInit / OnShutdown から呼ぶ。ImNodes コンテキストを専有する。
    void CreateContexts();
    void DestroyContexts();

    // 1 フレーム描画して、ユーザー操作の結果を返す。
    [[nodiscard]] GraphInteraction Draw(const GraphView& view, const Config& config);

    // ツールが検証に失敗したときに呼ぶ。指定ノードを一定秒だけ赤く光らせる。
    void ReportError(std::string message, std::vector<int> nodeIds);

    // 次フレームで選択させる (新規ノードは ImNodes のプールにまだ無いため)。
    void RequestSelection(std::vector<int> nodeIds);
    // 全ノードが収まるようパン・ズームを合わせる。
    void RequestFrameAll();
    // 現在選択中のノードだけを画面へ収める。
    void RequestFrameSelection();
    void ResetView();

    [[nodiscard]] float Zoom() const;
    void SetZoom(float zoom);
    [[nodiscard]] ImVec2 Panning() const;
    void SetPanning(ImVec2 panning);

    // 画面座標 → 論理座標。ツールが独自のドロップ処理を書くとき用。
    [[nodiscard]] ImVec2 ScreenToLogical(ImVec2 screenPos) const;
    [[nodiscard]] ImVec2 LogicalToScreen(ImVec2 logicalPos) const;
};
```

### ID 空間

**フレームワークは ID を解釈しない。** ツールが渡した `node.id` / `pin.id` /
`link.id` をそのまま ImNodes へ渡し、そのまま返す。

**WHY 規約を強制しないか**: `AnimationGraphPanel.cpp:1054-1082` は
「ピン ID の偶奇で入出力を判別」という技を使い、`VFXGraphCanvas` は
`OutputPinId(node.id)` の総当りで解決している。どちらもツール固有の都合で、
フレームワークが規約を決めると片方が必ず不自然になる。

ただし `GraphInteraction` は **ピン ID しか返さない**ので、
ツール側に「ピン ID → ノード ID」の逆引きが要る。
これを楽にするヘルパーだけ提供する。

```cpp
// 単純な規約を使いたいツール向け。使わなくてよい。
namespace GraphIds {
inline int InputPin (int nodeId) { return nodeId * 2 + 1; }   // 奇数
inline int OutputPin(int nodeId) { return nodeId * 2 + 2; }   // 偶数
inline bool IsOutputPin(int pinId) { return (pinId % 2) == 0; }
inline int  NodeOfPin (int pinId) { return IsOutputPin(pinId) ? (pinId - 2) / 2 : (pinId - 1) / 2; }
}
```

### 自動整列

```cpp
// 深さベースの列配置。有向グラフなら DAG でも木でも使える。
// VFXGraphCanvas.cpp:399-453 の実装を一般化したもの。
struct GraphLayoutOptions {
    float columnStep = 300.0f;
    float rowStep    = 190.0f;
    bool  centerColumns = true;   // 列ごとに縦センタリング
};

struct GraphLayoutEdge { int from; int to; };

// rootIds から到達不能なノードは最終列の右へ隔離する。
[[nodiscard]] std::unordered_map<int, ImVec2> ComputeGraphLayout(
    std::span<const int> nodeIds,
    std::span<const GraphLayoutEdge> edges,
    std::span<const int> rootIds,
    const GraphLayoutOptions& options = {});
```

**WHY 座標だけ返すか**: ノードの位置をどこに保存するかはツールごとに違う
(VFX はノード自身、Animation は `EditorContext::graphLayouts`)。
フレームワークは計算だけして、書き込みはツールに任せる。

---

## ビルド上の注意 (最重要)

**新しいファイルで `#include <imnodes.cpp>` を書いてはならない。**

`fbzz_editor` は `imnodes.lib` をリンクしておらず
(`Projects/Editor/CMakeLists.txt` の `target_link_libraries` に imnodes が無い)、
`AnimationGraphPanel.cpp:23-28` が `#include <imnodes.cpp>` して
実装を .obj へ同梱している。

`GraphCanvas.cpp` は `#include <imnodes.h>` のみにする。
二重に含めると `LNK2005` (重複シンボル) になる。

**将来的な整理**: `ThirdParty/CMakeLists.txt:29-42` に `imnodes` ターゲットは
既にあるので、`Projects/Editor/CMakeLists.txt` の `target_link_libraries` へ
`imnodes` を足し、`AnimationGraphPanel.cpp` の `#include <imnodes.cpp>` を
削除するのが本来の形。ただしコメントに「古い VS プロジェクトで LNK2019 を
出さないため」とあるので、**この変更は単独で行い、単独でビルド確認する**。
フレームワーク導入と混ぜない。

---

## 段階分け

### 段階 1 — フレームワーク本体

新規: `GraphView.hpp` / `GraphInteraction.hpp` / `GraphCanvas.hpp` / `GraphCanvas.cpp`

- ImNodes コンテキスト生成/破棄 + `EditorTheme::ApplyImNodes()`
- 統一されたカーソル基準ズーム + パン + スタイル拡縮
- ノード / リンク / グループの描画
- 選択・移動・リンク作成/削除・Delete/コピー系ショートカットの解釈
- 右クリックメニュー要求 + 座標変換
- Asset D&D 受け
- ミニマップ / グリッド / Frame All
- `ReportError` の一時ハイライト

**完了時**: まだ誰も使っていない。既存の 2 ツールは無変更で動く。

### 段階 2 — 自動整列

新規: `GraphLayoutAlgo.hpp` / `.cpp`
`VFXGraphCanvas.cpp:399-453` のアルゴリズムを一般化して切り出す。

### 段階 3 — `BehaviorTreePanel` を フレームワークの上に作る

これがフレームワークの最初の実利用者であり検証台。
木構造なので自動整列がそのまま綺麗に効く (VFX の DAG より条件が良い)。

**完了時**: `.behaviortree` をダブルクリックしてグラフが開き、
ノードを組んで保存でき、Play 中は Running の枝が光る。

### 段階 4 — `VFXGraphCanvas` の移行 (必須)

既に Session/Canvas/Inspector に分離済みなので移行しやすい。
移行後に `VFXGraphCanvas.cpp` から消えるのは、ズーム / パン / 選択 /
リンク解決 / ミニマップ / D&D / 一時エラー表示のおよそ 400〜500 行の見込み。

#### 移行契約 (確定)

- VFX は `GraphView` を毎フレーム構築し、`GraphNodeView::drawTitle` へ有効/無効チェックと
  実行状態バッジ、`drawBody` へノード概要を渡す。
- `GraphView::drawOverlay` へ選択ブラケット・実行リングを渡し、ImNodes の座標問い合わせは
  共通キャンバスの `EndNodeEditor()` 後に限定する。
- グループの移動/リサイズは `GraphInteraction::movedGroups` / `resizedGroups` で返し、
  VFX側が内包ノードの連動移動・Undo・dirtyを適用する。
- VFX固有のDAG検証、リンクトリガー編集、SubGraph遷移、Inspectorは移行後もSession/Inspectorへ残す。

この境界なら、見た目と入力の重複だけを共通化し、VFXの意味論を汎用キャンバスへ漏らさずに済む。

### 段階 5 — `AnimationGraphPanel` の移行 (必須・最後)

3648 行の単一ファイル。移行と同時に Session/Canvas/Inspector へ分割する。
**フレームワークが 2 ツールで実証されてから着手する。**

---

## リスク

| # | リスク | 対策 |
|---|---|---|
| R1 | `#include <imnodes.cpp>` の二重化で `LNK2005` | 新規ファイルは `imnodes.h` のみ。リンク構成の整理は別変更として単独で行う |
| R2 | 抽象化が 3 ツールの和集合になり、どれにとっても使いにくくなる | ビュー記述方式にしてデータモデルを持たない。検証も Undo もツール側に残す |
| R3 | 既存 2 ツールの移行で動作が変わる | 新規ツールで API を固めてから、VFX → Animation の順で移行し、各段階で回帰確認する |
| R4 | ズーム挙動の統一で既存ユーザーの操作感が変わる | 移行時に明示。統一自体が目的なので受容する |
| R5 | `std::function` の毎フレーム構築 | キャプチャは `this` + ノード ID で SBO 内。ノード数 200 未満 |
| R6 | `ImNodes::IsEditorHovered()` は `EndNodeEditor()` の後だと常に false | `VFXGraphCanvas.cpp:1136` の既知の罠。フレームワーク内で正しい位置に置き、コメントを残す |

---

## テスト

`Projects/Tests/GraphEditor/main.cpp` を新設する。ImGui / ImNodes の
コンテキストを必要としない純粋な部分のみを対象にする。

- [ ] `ComputeGraphLayout`: 木の深さが列に対応する
- [ ] `ComputeGraphLayout`: DAG で最長経路の深さが使われる
- [ ] `ComputeGraphLayout`: 到達不能ノードが最終列の右へ隔離される
- [ ] `ComputeGraphLayout`: 空グラフ / 単一ノード / 循環を含む入力で落ちない
- [ ] `GraphIds`: `NodeOfPin(InputPin(n)) == n` / `NodeOfPin(OutputPin(n)) == n`
- [ ] `GraphIds`: 入出力の判別が全ノード ID で正しい

**WHY 描画をテストしないか**: ImNodes は描画コンテキストと ImGui のフレームを
要求するため、ヘッドレスのテスト実行ファイルでは動かせない。
描画・入力解釈は実機確認に委ねる。

## 追加された共通面 (2026-08-09)

### ピン ID → ノード ID の逆引きはキャンバスが返す

`GraphView` の各ピンは既にノードの中にあるので、逆引き表は view だけで決まる。
`BuildPinOwnerMap()` を毎フレーム作り、`GraphInteraction` が
`createdFromNode` / `createdToNode` / `droppedFromNode` / `hoveredPinNode` を返す。

以前は Animation が「ピン ID の偶奇」、VFX が「出力ピン ID の総当り」で別々に
解決しており、`GraphIds` の規約を使うツールと使わないツールに分かれていた。
キャンバスが答えれば、規約の採否はツールの自由になる。

### 接続ドラッグ中の妥当性プレビュー — `GraphView::linkDragFilter`

繋げないピン・ノードを減光する。`GraphCanvas` は `ImNodes::IsLinkStarted` で
掴んだピンを覚え (`EndNodeEditor` の後でしか取れないので跨いで保持)、
次フレームの `BeginNodeEditor` 内でフィルタを引いて色のアルファを落とす。

**「検証はフレームワークに持たせない」(D2) との関係**: 判定そのものはツールが持ったまま。
キャンバスは結果を**見た目にしか使わない**。減光は `BeginNodeEditor` 〜 `EndNodeEditor` の
内側でしか描けず、ツール側では原理的に書けないので、この 1 点だけ口を開ける。
「繋いでから弾かれる」が「繋げない先が最初から暗い」に変わる。

### 実行進捗 — `GraphNodeView::progress` / `GraphLinkView::progress`

VFX の残り時間・Animation の遷移進捗・BT の Running 継続は 3 つとも同じ形の情報だが、
以前は `backgroundColor` の塗り分けしか表せなかった。`EndNodeEditor` の後に
オーバーレイとして重ねる (ノード内へ描くとノードの高さが変わり、再生中だけ
レイアウトが動く)。リンクは ImNodes がベジェ制御点を公開しないため、
始点ノードの右端中央 → 終点ノードの左端中央の直線で近似する。

### 部分グラフの抽出・整列 — `GraphSubgraphOps`

「id を振り直す / 内部リンクを保つ / 境界リンクを落とす」は
クリップボードの貼り付け・VFX の Template 取り込み・BT の部分木複製で同じ規則。
`ExtractSubgraph` / `CollectReachable` / `AlignNodes` / `DistributeNodes` として
framework へ寄せた。いずれも純粋計算なのでヘッドレスでテストできる。

`AlignNodes` の中央は**重心ではなく外接矩形の中心**。重心だと 1 個だけ離れた
ノードに引っ張られて、揃えたはずの列が斜めのまま残る。

### `ComputeGraphLayout` を実際に使う

作られていたが**呼び出し元が 1 つも無かった**。VFX は自前の深さ計算、
Animation は「4 列の単純グリッド」のままで、同じ Auto Layout という操作なのに
結果の質が違った。両方を共通実装へ差し替え、BT も同じものを使う。

- VFX: Entry を根に指定 (入次数 0 に任せると到達不能ノードまで 1 列目へ並ぶ)
- Animation: 既定ステートを根に指定 (同じ理由)
- BT: `FindRootIds()` を根に指定

## 実装チェックリスト

- [x] `GraphView.hpp` / `GraphInteraction.hpp`
- [x] `GraphCanvas.hpp` / `GraphCanvas.cpp` (コンテキスト・ズーム・描画・入力解釈)
- [x] `ReportError` の一時ハイライト
- [x] `RequestSelection` / `RequestFrameAll` / `RequestFrameSelection` / `ResetView`
- [x] `GraphLayoutAlgo.hpp` / `.cpp`
- [x] `Projects/Tests/GraphEditor/main.cpp` + `Tests/CMakeLists.txt` 登録
- [x] `BehaviorTreePanel` をフレームワークの上に実装
- [x] `VFXGraphCanvas` の移行 (旧経路削除まで完了)
- [x] `AnimationGraphPanel` の移行 (旧経路削除まで完了)
- [x] `imnodes` を `fbzz_editor` へ正式リンクし `#include <imnodes.cpp>` を廃止
- [x] `BuildPinOwnerMap` / `linkDragFilter` / `progress` / `GraphSubgraphOps`
- [x] `ComputeGraphLayout` を 3 ツールすべてで実使用
- [ ] Visual Studio 2022 全体ビルド
