// FBZZ Engine
// AnimationGraphPanel.hpp | fbzz::editor
// AnimatorComponent のステートマシンをノードグラフとして編集するパネル
// WHY: Inspector の縦リストでは遷移関係が追いづらいため、状態と遷移を同じ画面で直接編集できる UI を提供する。
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <string>

struct ImNodesContext;
struct ImNodesEditorContext;

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

protected:
    void OnRenderContent(EditorContext& ctx) override;

private:
    struct LinkRef {
        int fromStateIndex = -1;
        int transitionIndex = -1;
    };

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

    void DrawToolbar(EditorContext& ctx, scene::AnimatorComponent& animator);
    void DrawZoomControls();
    // ホイールズーム (カーソル位置固定) + Shift/Alt ホイールパン。
    // メインキャンバスと Blend Tree キャンバスで同じ操作感を共有する。
    void HandleCanvasWheel(float canvasOriginX, float canvasOriginY);
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
    void ClearInvalidSelection(const scene::AnimatorComponent& animator);
    LinkRef ResolveLink(int linkId, const scene::AnimatorComponent& animator) const;

    ImNodesContext*       m_nodesContext = nullptr;
    ImNodesEditorContext* m_editorContext = nullptr;
    LinkRef               m_selectedLink;
    int                   m_selectedNode = -1;
    int                   m_openBlendTreeState = -1;
    int                   m_selectedMotion = -1;
    bool                  m_selectedAnyState = false;
    float                 m_canvasZoom = 1.0f;
    std::string           m_selectionOwnerInstanceId;
    int                   m_renamingNode = -1;
    char                  m_renameBuffer[128] = {};
    // Unity の "Make Transition" モード。-1: 非アクティブ / -2: Any State / -3: Entry / 0以上: ステート index。
    // アクティブ中はソースノードからマウスへ矢印付きプレビュー線を描き、ノードクリックで遷移を確定する。
    int                   m_pendingTransitionFrom = -1;
    // 右クリックメニューを開いた瞬間の論理グリッド座標。"New State" をカーソル位置に生成するために保持する。
    float                 m_contextSpawnX = 80.0f;
    float                 m_contextSpawnY = 80.0f;
};

} // namespace fbzz::editor
