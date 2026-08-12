// FBZZ Engine
// GraphView.hpp | fbzz::editor
// ノードグラフの「見た目の記述」— ツールが毎フレーム組み立ててキャンバスへ渡す
//
// WHY データモデルを共有せず、毎フレーム記述を渡すか:
//   Animation (ステートマシン) / VFX (DAG) / BehaviorTree (木) はデータ構造が
//   根本的に違う。共通の抽象モデルを定義すると 3 つの和集合になり、
//   どのツールにとっても使いにくい型ができる。
//   代わりに ImGui と同じイミディエイトモードにして、キャンバスは
//   「描画と入力の解釈」だけを担い、データは一切所有しない。
//
//   ノード数は実用上 200 未満なので、毎フレームのビュー構築コストは問題にならない。
#pragma once
#include <imgui.h>

#include <functional>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace fbzz::editor {

// ImNodesに依存しないピン形状。共通ビューをツール層・アセット層でも組み立てられるようにする。
enum class GraphPinShape {
    Circle,
    CircleFilled,
    Triangle,
    TriangleFilled,
    Quad,
    QuadFilled,
};

// リンクの線種。描画実装はキャンバスが担当し、ツールは意味に応じた見た目だけを選ぶ。
enum class GraphLinkPattern {
    Solid,
    Dashed,
    Dotted,
};

// ピン 1 本。id はツールが決める任意の整数で、キャンバスは中身を解釈しない。
struct GraphPinView {
    int         id    = 0;
    std::string label;              // 空なら描かない
    ImU32       color = 0;          // 0 なら既定色
    ImU32       hoveredColor = 0;   // 0 なら color を使用
    GraphPinShape shape = GraphPinShape::CircleFilled;
};

struct GraphNodeView {
    int    id       = 0;
    ImVec2 position = {};           // 論理座標 (ズーム倍率を掛ける前)

    std::string title;

    // 色の使い分け規約 (VFXGraphCanvas.cpp:933-946 の方針を全ツールへ広げる):
    //   タイトル帯 = 静的な状態 (ノード種別 / 未接続 / エラー / 検索ヒット)
    //   背景       = 実行中の状態 (Running / アクティブ)
    //   枠線       = 選択・強調
    // WHY 面を分けるか: 3 種類の情報を同じ面へ詰めると、どれか 1 つが変わった瞬間に
    //     他の情報が読めなくなる。「エラーなのか実行中なのか」が判別できなくなる。
    ImU32 titleColor      = 0;
    ImU32 titleTextColor  = 0;       // 0 なら ImGui の既定文字色
    ImU32 backgroundColor = 0;
    ImU32 outlineColor    = 0;
    float outlineThickness = 0.0f;  // 0 なら既定

    // 文字倍率はキャンバスのfontScaleと乗算される。0以下・非有限値は1.0として扱う。
    // WHY 3領域を分けるか: ノード名は見出し、ピンは接続先、本文は補助情報であり、
    //      同じサイズに固定するとグラフの構造と詳細情報の優先順位を変えられない。
    float titleFontScale = 1.0f;
    float pinFontScale   = 1.0f;
    float bodyFontScale  = 1.0f;

    std::vector<GraphPinView> inputs;
    std::vector<GraphPinView> outputs;

    // 標準ピンを使わず、drawBody から属性を組み立てる特殊ノード向け。
    // WHY: VFX の Entry/Reroute は通常の In/Out と異なるため、共通キャンバスが
    //      無条件にピンを追加すると、移行後に同じ属性が二重登録される。
    bool drawDefaultInputs  = true;
    bool drawDefaultOutputs = true;

    // false の場合、drawBody は BeginStaticAttribute の外で呼ばれる。
    // WHY: ImNodes 属性を自前で配置するノードでも、共通キャンバスのノード境界と
    //      ツール固有の描画責務を分離できるようにする。
    bool drawBodyInStaticAttribute = true;

    // タイトル帯の中身をツール固有UIへ差し替える。空なら title を描く。
    // WHY: VFX の有効/無効チェックや Animation の状態バッジを、共通キャンバスの
    // BeginNodeTitleBar 境界を壊さずに保持するため。
    std::function<void()> drawTitle;

    // ImNodes::BeginNode / EndNode の間で呼ばれる。ツールがノード本体を描く。
    //
    // WHY std::function を許容するか: BeginNode / EndNode の間でしか描けないため、
    //     この 1 点だけはツールへ制御を渡す必要がある。キャプチャは this と
    //     ノード ID 程度に収まるので SBO 内で完結し、ヒープ確保は起きない。
    std::function<void()> drawBody;

    std::string tooltip;            // ホバー時。空なら出さない

    // 実行の進捗 [0,1]。負なら描かない。ノード下端へ細い帯として重ねる。
    // WHY 色ではなく別の面か: 「今どこが動いているか」は backgroundColor で表せるが、
    //     「あとどれくらいで終わるか」は表せない。VFX のノード残り時間、
    //     Animation の遷移進捗、BehaviorTree の Running 継続時間は 3 つとも
    //     同じ形の情報なので、ツールごとに drawBody で自作させない。
    float progress = -1.0f;
    ImU32 progressColor = 0;        // 0 なら既定色
};

struct GraphLinkView {
    int   id      = 0;
    int   fromPin = 0;
    int   toPin   = 0;
    ImU32 color   = 0;              // 0 なら既定色
    ImU32 hoveredColor = 0;         // 0 なら既定色
    ImU32 selectedColor = 0;        // 0 なら既定色
    GraphLinkPattern pattern = GraphLinkPattern::Solid;

    // 0 未満ならキャンバスの既定値を使う。単位は太さ・矢印・ホバー距離がpx。
    float thickness = -1.0f;
    float curveStrength = -1.0f;
    float curveMaxTangent = -1.0f;
    float lineSegmentsPerLength = -1.0f;
    float arrowSize = -1.0f;        // 0 なら矢印を無効化
    float arrowPosition = -1.0f;    // 0.1〜1.0
    float hoverDistance = -1.0f;

    // 実行の進捗 [0,1]。負なら描かない。リンク上を進む点として重ねる。
    // NOTE: ImNodes はリンクのベジェ制御点を公開しないため、始点ノードの右端中央と
    //       終点ノードの左端中央を結ぶ直線上で近似する。曲線から少しずれるが、
    //       「どの遷移がどこまで進んだか」を読むには十分。
    float progress = -1.0f;
    ImU32 progressColor = 0;
};

// 注釈枠 (VFX の Group 相当)。使わないツールは空のままでよい。
struct GraphGroupView {
    int         id  = 0;
    ImVec2      min = {};           // 論理座標
    ImVec2      max = {};
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
    // EndNodeEditor 後に描くツール固有オーバーレイ (選択枠・実行リング等)。
    // ImNodes の座標問い合わせが必要な描画を共通キャンバスの外へ漏らさない。
    std::function<void()> drawOverlay;

    // 接続ドラッグ中に「このピンへ繋げてよいか」を答える。空なら全ピンが等価。
    //
    // WHY キャンバスが受け取るか (検証はツール、という原則との関係):
    //   判定そのものはツールが持ったままで、キャンバスは**見た目にしか使わない**。
    //   繋げない先を減光するには BeginNodeEditor 〜 EndNodeEditor の内側で
    //   ピンの色を積む必要があり、これはツール側では原理的に書けない。
    //   「繋いでから弾かれる」を「繋げない先が最初から暗い」へ変えるための最小の口。
    // NOTE: 掴んだ最初の 1 フレームは fromPin が未確定なので減光は次フレームから効く
    //       (ImNodes::IsLinkStarted は EndNodeEditor の後でしか取れないため)。
    std::function<bool(int fromPin, int toPin)> linkDragFilter;
};

// ピン ID → ノード ID の逆引き表を view から作る。
// WHY キャンバスがこれを持つか: GraphView の各ピンは既にノードの中にあるのだから、
//     逆引きは view だけで決まる。以前はツール側が「偶奇規約」(Animation) と
//     「出力ピン ID の総当り」(VFX) で別々に解決しており、GraphIds の規約を
//     使うツールと使わないツールに分かれていた。表を作れば規約は任意でよくなる。
[[nodiscard]] std::unordered_map<int, int> BuildPinOwnerMap(const GraphView& view);

// ── ピン ID の規約ヘルパー ──────────────────────────────────────────────────
//
// WHY キャンバスが ID 規約を強制しないか:
//   AnimationGraphPanel は「ピン ID の偶奇で入出力を判別」する技を使い、
//   VFXGraphCanvas は OutputPinId(node.id) の総当りで解決している。
//   どちらもツール固有の都合で、キャンバスが規約を決めると片方が必ず不自然になる。
//   キャンバスは受け取った ID をそのまま ImNodes へ渡し、そのまま返す。
//
// ただし GraphInteraction はピン ID しか返さないため、ツール側に
// 「ピン ID → ノード ID」の逆引きが要る。単純な規約で済むツール向けに用意する。
namespace GraphIds {

// ノード ID は 1 以上とし、ImNodes の int ID 空間を超える値は無効にする。
// WHY ここで上限を持つか: 2 倍してピン ID を作るため、無検証だと大きな
// ノード ID が符号反転し、別ノードのピンと衝突する。無効値は 0 へ畳み込む。
inline constexpr int MAX_NODE_ID = (std::numeric_limits<int>::max() - 2) / 2;

[[nodiscard]] constexpr bool IsValidNodeId(int nodeId)
{
    return nodeId > 0 && nodeId <= MAX_NODE_ID;
}

[[nodiscard]] constexpr int InputPin(int nodeId)
{
    return IsValidNodeId(nodeId) ? nodeId * 2 + 1 : 0; // 奇数
}

[[nodiscard]] constexpr int OutputPin(int nodeId)
{
    return IsValidNodeId(nodeId) ? nodeId * 2 + 2 : 0; // 偶数
}

[[nodiscard]] constexpr bool IsOutputPin(int pinId)
{
    return pinId >= 2 && (pinId % 2) == 0;
}

[[nodiscard]] constexpr bool IsInputPin(int pinId)
{
    return pinId >= 3 && (pinId % 2) != 0;
}

[[nodiscard]] constexpr int NodeOfPin(int pinId)
{
    if (IsOutputPin(pinId)) return (pinId - 2) / 2;
    if (IsInputPin(pinId)) return (pinId - 1) / 2;
    return 0;
}

} // namespace GraphIds

} // namespace fbzz::editor
