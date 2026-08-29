/// @file    AnimationGraphPanel.hpp
/// @brief   AnimatorComponent のステートマシンをノードグラフとして編集するパネル。
/// @author  Hasegawa Jin
/// @date    2026-06-08
///
/// WHY: Inspector の縦リストでは遷移関係が追いづらいため、状態と遷移を同じ画面で直接編集できる UI を提供する。
#pragma once
#include <Editor/GraphEditor/GraphCanvas.hpp>
#include <Editor/GraphEditor/GraphView.hpp>
#include <Editor/Panels/IPanel.hpp>
#include <string>
#include <vector>


namespace fbzz::scene {
class GameObject;
struct AnimatorComponent;
} // namespace fbzz::scene

namespace fbzz::editor {

class AnimationGraphPanel final : public IPanel {
public:
    const char* GetWindowName()        const override { return "Animation Graph"; }
    bool        GetDefaultVisibility() const override { return false; }
    void OnInit(EditorContext& ctx) override;
    void OnShutdown() override;

    // 編集する .animcontroller を明示的に開く (BehaviorTreePanel::RequestOpen と同じ方式)。
    //
    // WHY 明示的に開かせるか:
    //   以前このパネルは ctx.selectedAssetPath に追従していたため、Asset Browser で
    //   別のファイルをクリックしただけで編集対象が切り替わり、未保存の変更が
    //   確認なしで捨てられていた。クリップを Source 欄へドラッグするには
    //   Asset Browser を触る必要があるため、通常の編集作業でこれが必ず起きる。
    //   「開く」を独立した操作にして、選択とドキュメントを切り離す。
    void RequestOpen(const std::string& path) { m_requestedPath = path; }

protected:
    void OnRenderContent(EditorContext& ctx) override;
    // 中身を描かなかったフレーム (閉じている / 非アクティブなタブ) では、
    // ここが公開した選択を取り下げる。
    // WHY: 公開した選択は Inspector の表示対象を最優先で決める。パネルが見えていないのに
    //      残っていると、他の面で何を選んでも Inspector が古いステートを映したままになる。
    void OnAfterEnd(EditorContext& ctx) override;

private:
    // グラフ上のノード種別。
    //
    // WHY 型にするか: 以前は "fromStateIndex == -2 なら Any State / -3 なら Entry" という
    //     符号なしの規約で表現していた。判定を書くたびに読み手がその対応を思い出す必要があり、
    //     実際に Entry (ノード ID 1000010) がコンテキストメニューの分岐から漏れて、
    //     直前の選択のままメニューが開く不具合になっていた。種別は型で持つ。
    enum class NodeKind { None, State, AnyState, Entry };

    struct LinkRef {
        int fromStateIndex = -1;   // -2 = Any State 由来 (ResolveSelectionIndices が設定する)
        int transitionIndex = -1;
    };

    // ── 選択の同一性 ──────────────────────────────────────────────────────
    //
    // WHY index ではなく名前を権威にするか (このパネルの中心的な設計):
    //   データ側は一貫して名前をキーにしている (layout.nodePositions[state.name] /
    //   transition.toStateName / defaultStateName)。一方 UI 側は states[] の添字で
    //   選択を保持していたため、次の食い違いが起きていた。
    //     - ステートを削除すると添字が詰まり、選択が「別のステート」を指したまま残る
    //       (範囲内なので ClearInvalidSelection でも気付けず、Inspector が別物を編集する)
    //     - リネームすると添字は変わらないのに、参照している対象の意味が変わる
    //   名前を権威にし、添字は毎フレーム名前から解決する派生値として扱えば、
    //   削除も並べ替えもリネームも「名前が見つからなければ選択が消える」だけで済む。
    std::vector<std::string> m_selectedStateNames;   // 複数選択 (先頭がプライマリ)
    NodeKind                 m_selectedKind = NodeKind::None;
    // 選択中の遷移の起点。kind=AnyState なら Any State 由来、State なら fromName のステート。
    NodeKind                 m_selectedLinkKind = NodeKind::None;
    std::string              m_selectedLinkFromName;
    std::string              m_openBlendTreeStateName;
    // 遷移作成モードの起点 (Unity の "Make Transition")。
    NodeKind                 m_pendingTransitionKind = NodeKind::None;
    std::string              m_pendingTransitionFromName;

    // 名前 → 現在の添字。states[] が変わるたびに解決し直す。
    [[nodiscard]] static int IndexOfState(const scene::AnimatorComponent& animator,
                                          const std::string& name);
    // 権威 (名前) から派生値 (添字) を作り直す。描画の先頭で必ず 1 回呼ぶ。
    void ResolveSelectionIndices(const scene::AnimatorComponent& animator);
    // キャンバスが報告した選択を名前として取り込む。
    void CaptureCanvasSelection(const scene::AnimatorComponent& animator,
                                const std::vector<int>& selectedNodes,
                                const std::vector<int>& selectedLinks);
    // 単一ステートを選択し直し、キャンバス側のハイライトも合わせる。
    void SelectStateByName(const scene::AnimatorComponent& animator, const std::string& name);
    void ClearSelectionState();

    static int NodeId(int stateIndex);
    static int InputPinId(int stateIndex);
    static int OutputPinId(int stateIndex);
    static int LinkId(int fromStateIndex, int transitionIndex);
    static int AnyStateNodeId();
    static int AnyStateOutputPinId();
    static int AnyStateLinkId(int transitionIndex);
    static int EntryNodeId();
    static int EntryOutputPinId();
    static int EntryLinkId();

    // 編集対象レイヤー (Base Layer / 各 AnimationLayer) を切り替えるツールバー。
    void DrawLayerSelector(EditorContext& ctx, scene::AnimatorComponent& animator,
                           bool allowEditing);
    void DrawToolbar(EditorContext& ctx, scene::AnimatorComponent& animator);
    // Base Layer と各レイヤーのマスクを重ねた結果を、ボーンごとの持ち分として見せる。
    // WHY パネルにするか: マスクを 1 枚ずつ開いても「Base が何 % 残るか」は出てこない。
    //      合成後の数字は、レイヤーを並べているこの画面にしか置き場がない。
    void DrawLayerComposition(EditorContext& ctx, scene::AnimatorComponent& animator);
    void DrawZoomControls();
    // ホイールズーム (カーソル位置固定) + Shift/Alt ホイールパン。
    // メインキャンバスと Blend Tree キャンバスで同じ操作感を共有する。
    void DrawParameterSidebar(EditorContext& ctx, scene::AnimatorComponent& animator);
    void DrawNodeCanvas(EditorContext& ctx, scene::AnimatorComponent& animator, const std::string& instanceId);
    void DrawBlendTreeCanvas(EditorContext& ctx,
                             scene::AnimatorComponent& animator,
                             const std::string& instanceId);
    void PublishSelection(EditorContext& ctx, const scene::GameObject& gameObject) const;
    void AddState(EditorContext& ctx, scene::AnimatorComponent& animator, const char* baseName);
    // 右クリック位置やドロップ位置など、論理グリッド座標を指定してステートを作成する。
    void AddStateAt(EditorContext& ctx,
                    scene::AnimatorComponent& animator,
                    const char* baseName,
                    const std::string& instanceId,
                    float spawnX,
                    float spawnY);
    // 複製元の隣へ配置する。AutoLayout で全体を崩さないための専用経路。
    void DuplicateState(EditorContext& ctx,
                        scene::AnimatorComponent& animator,
                        int stateIndex,
                        const std::string& instanceId);
    void AddTransition(EditorContext& ctx, scene::AnimatorComponent& animator, int fromStateIndex, int toStateIndex);
    void AutoLayoutStates(EditorContext& ctx, scene::AnimatorComponent& animator, const std::string& instanceId);
    void DeleteState(EditorContext& ctx, scene::AnimatorComponent& animator, int stateIndex, const std::string& instanceId);
    void RenameState(EditorContext& ctx,
                     scene::AnimatorComponent& animator,
                     int stateIndex,
                     const std::string& oldName,
                     const std::string& newName,
                     const std::string& instanceId);
    // Layer 名と、その Layer を名前で参照する Graph/Inspector の選択状態を同期する。
    void RenameLayer(EditorContext& ctx,
                     scene::AnimatorComponent& animator,
                     const std::string& oldName,
                     const std::string& newName);
    // 起きた改名を、名前で覚えている選択・編集状態へ反映する。
    // パネル自身の RenameState からも、Inspector の Name 欄からの改名通知
    // (ctx.animationGraphRenamedFrom/To) からも通る。
    void AdoptStateRename(const std::string& oldName, const std::string& newName);
    // Inspector の Layers 欄で起きた改名を、Graph が名前で保持する編集対象へ反映する。
    void AdoptLayerRename(const std::string& oldName, const std::string& newName);
    void ClearInvalidSelection(const scene::AnimatorComponent& animator);
    LinkRef ResolveLink(int linkId, const scene::AnimatorComponent& animator) const;

    // VFX と Animation のパン・ズーム・選択・リンク操作を同じ実装へ集約する。
    // 旧コンテキストは移行中の比較用フォールバックとして残し、通常経路では使わない。
    GraphCanvas           m_graphCanvas;
    // ── 以下は ResolveSelectionIndices() が毎フレーム作り直す派生値 ────────
    // 直接代入しないこと。選択を変えるときは名前側 (m_selectedStateNames 等) を更新する。
    LinkRef               m_selectedLink;
    int                   m_selectedNode = -1;
    int                   m_openBlendTreeState = -1;
    // 編集中のレイヤー名。空 = Base Layer。
    // WHY: 上半身レイヤーに独自の遷移グラフを組むには、どのグラフを見ているかを
    //      パネル側で保持する必要がある。描画時に LayerGraphScope で差し替える。
    std::string           m_editingLayer;
    int                   m_selectedMotion = -1;
    bool                  m_selectedAnyState = false;
    float                 m_canvasZoom = 1.0f;
    std::string           m_selectionOwnerInstanceId;
    int                   m_renamingNode = -1;
    char                  m_renameBuffer[128] = {};
    // リネーム対象のステート名 (権威)。m_renamingNode はここから解決する派生値。
    std::string           m_renamingStateName;
    // 次フレームの描画先頭で ImGui::OpenPopup を呼ぶための 1 ショット。
    //
    // WHY 直接 OpenPopup しないか: 右クリックメニューの MenuItem ハンドラから
    //     OpenPopup を呼ぶと、ImGui はそれを「今開いているポップアップの子」として扱う。
    //     MenuItem のクリックで親メニューが閉じると子も一緒に閉じるため、
    //     コンテキストメニューの "Rename" は構造上ぜったいに開かなかった。
    //     フラグを立てて、ポップアップの外側にいるフレーム先頭で開く。
    bool                  m_renameRequested = false;
    // 入力欄へフォーカスを移すのは開いた最初のフレームだけ。
    bool                  m_renameFocusPending = false;
    // 重複名・空名で弾いた理由。無言で失敗しないための表示用。
    std::string           m_renameError;
    // Layer の名前変更は入力中に states[] を差し替えないよう、確定時だけ反映する。
    char                  m_layerRenameBuffer[128] = {};
    bool                  m_layerRenameActive = false;
    bool                  m_layerRenameFocusPending = false;
    std::string           m_layerRenameError;
    // Unity の "Make Transition" モード。起点は m_pendingTransitionKind /
    // m_pendingTransitionFromName が権威で、これはその派生値
    // (-1: 非アクティブ / -2: Any State / -3: Entry / 0以上: ステート index)。
    int                   m_pendingTransitionFrom = -1;
    // 右クリックメニューを開いた瞬間の論理グリッド座標。"New State" をカーソル位置に生成するために保持する。
    float                 m_contextSpawnX = 80.0f;
    float                 m_contextSpawnY = 80.0f;
    // レイヤー合成ビューの表示状態と絞り込み。
    bool                  m_showComposition = false;
    bool                  m_compositionIssuesOnly = false;
    char                  m_compositionFilter[64] = {};
    // RequestOpen() で積まれた「次に開くパス」。描画の先頭で 1 回だけ消費する。
    std::string           m_requestedPath;
};

} // namespace fbzz::editor
