// FBZZ Engine
// AnimationGraphPanel.cpp | fbzz::editor
// AnimatorComponent のステートマシンをノードグラフとして編集するパネル
// WHAT: imnodes で State ノードと Transition リンクを描画し、AnimatorComponent を直接更新する。
#include <Editor/Panels/AnimationGraphPanel.hpp>
#include <Editor/Panels/AnimationGraphInspector.hpp>
#include <Editor/Panels/AnimationPreview.hpp>
#include <Editor/PlayModeController.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/AnimationClip.hpp>
#include <Engine/Asset/AnimatorControllerAsset.hpp>
#include <Engine/Asset/AvatarMaskAsset.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Asset/Skeleton.hpp>
#include <Engine/Profiler/ProfileScope.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/GraphEditor/AnimatorGraphOps.hpp>
#include <Editor/GraphEditor/GraphLayoutAlgo.hpp>
#include <Editor/GraphLayout.hpp>
#include <Editor/Util/AnimatorMaskAudit.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/EditorTheme.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Editor/Util/Toast.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <imgui_internal.h>
// NOTE: 以前ここには `#include <imnodes.cpp>` があった。実装を .obj へ同梱することで
//       imnodes.lib のリンク漏れを回避していたが、その代償として「他のどの .cpp も
//       同じことをしてはならない」という不変条件をコメントでしか守れなくなっていた。
//       現在は fbzz_editor が imnodes を PUBLIC リンクするため不要。
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fbzz::editor {

namespace {

std::uint64_t& AnimationGraphEditGeneration()
{
    static std::uint64_t generation = 0;
    return generation;
}

// Play Mode 中はグラフをランタイム監視専用にし、Scene/Undo データを書き換えない。
bool CanEditAnimationGraph(const EditorContext& ctx)
{
    return ctx.playMode == nullptr || ctx.playMode->IsInEditor();
}

constexpr float SIDEBAR_WIDTH = 260.0f;
// WHY: 大きなステートマシンを一望するには Unity 同等の広いズームレンジが必要。
constexpr float MIN_CANVAS_ZOOM = 0.30f;
constexpr float MAX_CANVAS_ZOOM = 2.00f;
constexpr float ZOOM_STEP = 0.10f;
constexpr float BASE_NODE_CARD_WIDTH = 188.0f;
// NOTE: ホイールズーム / パンの定数 (WHEEL_PAN_STEP / CANVAS_HOVER_FLAGS) は
//       旧 ImNodes 経路と一緒に削除した。ズームとパンは GraphCanvas が持つ。

const char* ParamTypeName(scene::ParamType type)
{
    static constexpr const char* NAMES[] = { "Float", "Int", "Bool", "Trigger" };
    return NAMES[std::clamp(static_cast<int>(type), 0, 3)];
}

const char* ConditionOpName(scene::ConditionOp op)
{
    static constexpr const char* NAMES[] = { "Greater", "Less", "Equal", "NotEqual", "True", "False" };
    return NAMES[std::clamp(static_cast<int>(op), 0, 5)];
}

GraphLayout ToEditorGraphLayout(const asset::AnimatorGraphLayout& source)
{
    GraphLayout layout;
    layout.entryPosition = ImVec2(source.entryPosition.x, source.entryPosition.y);
    layout.anyStatePosition = ImVec2(source.anyStatePosition.x, source.anyStatePosition.y);
    for (const auto& [stateName, pos] : source.nodePositions)
        layout.nodePositions[stateName] = ImVec2(pos.x, pos.y);
    for (const auto& [stateName, positions] : source.blendTreeMotionPositions) {
        auto& dst = layout.blendTreeMotionPositions[stateName];
        dst.reserve(positions.size());
        for (const auto& pos : positions)
            dst.emplace_back(pos.x, pos.y);
    }
    return layout;
}

// NOTE: ToAssetGraphLayout / SaveAnimatorControllerWithLayout /
//       dirty 登録の本体は Editor/GraphEditor/AnimatorGraphOps.hpp へ移した。
//       WHY: パネルの private/static に閉じていたため、パネルを描画していないと
//            呼べず、AI からは Animator の構造は編集できるのに**保存できない**
//            (次回起動で編集が消える) 状態だった。
//            Docs/design/editor-operator-model.md

void MarkDirty(EditorContext& ctx)
{
    if (!CanEditAnimationGraph(ctx))
        return;

    ++AnimationGraphEditGeneration();
    // WHY selectedAssetPath ではなく animationControllerEditorPath を見るか (重要):
    //   以前はここが「Asset Browser で今選ばれているファイル」を編集対象と見なしていた。
    //   ところがクリップを Source 欄へドラッグするには Asset Browser を触る必要があり、
    //   その瞬間に selectedAssetPath が .anim / .fbx へ移る。すると
    //     - この分岐が false になり、編集が AssetDirtyRegistry に登録されない
    //     - 保存先も見失う (Save が効いていないように見える)
    //     - Inspector の "Modified" 表示も出ない
    //   という 3 つが同時に起きていた。編集対象は「パネルが開いているドキュメント」であって
    //   ブラウザーの選択ではない。animationControllerEditorPath がその唯一の識別子。
    if (MarkAnimatorControllerDirty(ctx))
        return;
    if (ctx.markSceneDirty) ctx.markSceneDirty();
}

struct AnimationGraphUndoTracker {
    std::string owner;
    ImGuiID activeId = 0;
    scene::EntityID entityId;
    std::shared_ptr<scene::AnimatorComponent> assetAnimator;
    scene::AnimatorComponent beforeAnimator;
    GraphLayout beforeLayout;
    bool active = false;
};

// Animation Graph が編集する定義だけを複製し、巨大なClipトラックや骨行列をUndoへ含めない。
// WHY: AnimatorComponent全体の毎フレーム深いコピーは、Clip読込後にEditor描画を大幅に重くする。
scene::AnimatorComponent MakeAnimationGraphSnapshot(
    const scene::AnimatorComponent& source)
{
    scene::AnimatorComponent snapshot;
    snapshot.enabled = source.enabled;
    snapshot.controllerPath = source.controllerPath;
    snapshot.defaultStateName = source.defaultStateName;
    snapshot.states = source.states;
    snapshot.anyStateTransitions = source.anyStateTransitions;
    snapshot.parameters = source.parameters;
    // レイヤーもグラフ定義の一部。含めないとレイヤー編集だけ Undo が効かなくなる。
    snapshot.layers = source.layers;
    snapshot.baseLayerMask.path = source.baseLayerMask.path;
    snapshot.playing = source.playing;
    return snapshot;
}

// ── レイヤーグラフの一時差し替え ─────────────────────────────────────────────
// WHY: このパネルは 130 箇所以上で animator.states / anyStateTransitions /
//      defaultStateName を直接触っている。レイヤー対応のために全箇所を
//      「今どのレイヤーか」で分岐させると、描画・選択・Undo・ノード配置の
//      すべてに条件が散り、既存の動作を壊すリスクが高い。
//
//      AnimationLayer は AnimatorComponent と同じ型のステート配列を持つので、
//      描画の前後で中身を入れ替えれば、パネル側は「常に自分のグラフを見ている」
//      ままで良い。入れ替えは 1 フレーム内で完結し、デストラクタで必ず戻す。
struct LayerGraphScope {
    scene::AnimatorComponent* animator = nullptr;
    scene::AnimationLayer*    layer    = nullptr;
    scene::AnimatorComponent* ownerAnimator = nullptr;
    scene::AnimationLayer*    ownerLayer = nullptr;

    LayerGraphScope(scene::AnimatorComponent& a, const std::string& layerName)
    {
        if (layerName.empty()) return;             // Base Layer は入れ替え不要
        layer = a.FindLayer(layerName);
        if (layer == nullptr) return;              // 消えたレイヤーは Base Layer 扱い
        ownerAnimator = &a;
        ownerLayer = layer;
        animator = &a;
        Swap();
    }
    ~LayerGraphScope() { Restore(); }

    LayerGraphScope(const LayerGraphScope&) = delete;
    LayerGraphScope& operator=(const LayerGraphScope&) = delete;

    [[nodiscard]] bool Active() const { return animator != nullptr; }

    // 差し替えを元へ戻す。二重呼び出ししても安全 (デストラクタと明示呼び出しの両立)。
    // WHY: Undo のスナップショット比較は「元に戻した状態」で行う必要があるため、
    //      スコープ終端を待たずに明示的に戻せる入口を用意する。
    void Restore()
    {
        if (animator == nullptr) return;
        Swap();
        animator = nullptr;
        layer = nullptr;
    }

    // 保存など、AnimatorComponent 全体の定義を読む処理の前に一時的に元へ戻す。
    // WHY: 差し替え中の animator.states は選択 Layer の内容なので、そのまま保存すると
    //      選択 Layer が Base Layer として書き出される。保存後は表示を継続するため再適用する。
    void Reapply()
    {
        if (animator != nullptr || ownerAnimator == nullptr || ownerLayer == nullptr) return;
        animator = ownerAnimator;
        layer = ownerLayer;
        Swap();
    }

private:
    void Swap()
    {
        std::swap(animator->states, layer->states);
        std::swap(animator->anyStateTransitions, layer->anyStateTransitions);
        std::swap(animator->defaultStateName, layer->defaultStateName);
        // ランタイム表示 (現在ステートのハイライト) もレイヤー側を見せる。
        std::swap(animator->currentStateName, layer->runtime.currentStateName);
        std::swap(animator->blendToState, layer->runtime.blendToState);
        std::swap(animator->blendWeight, layer->runtime.blendWeight);
        std::swap(animator->stateTime, layer->runtime.stateTime);
    }
};

// Undo適用時はランタイム資源を保持し、Graph定義だけを書き戻す。
void ApplyAnimationGraphSnapshot(
    scene::AnimatorComponent& target,
    const scene::AnimatorComponent& snapshot)
{
    target.enabled = snapshot.enabled;
    target.controllerPath = snapshot.controllerPath;
    target.defaultStateName = snapshot.defaultStateName;
    target.states = snapshot.states;
    target.anyStateTransitions = snapshot.anyStateTransitions;
    target.parameters = snapshot.parameters;
    target.layers = snapshot.layers;
    target.baseLayerMask.path = snapshot.baseLayerMask.path;
    target.baseLayerMask.Invalidate();
    target.playing = snapshot.playing;
    target.currentStateName.clear();
    target.blendToState.clear();
    target.stateTime = 0.0f;
    target.blendWeight = 0.0f;
}

void PushAnimationGraphCommand(EditorContext& ctx,
                               const std::string& owner,
                               scene::EntityID entityId,
                               const std::shared_ptr<scene::AnimatorComponent>& assetAnimator,
                               const scene::AnimatorComponent& beforeAnimator,
                               const scene::AnimatorComponent& afterAnimator,
                               const GraphLayout& beforeLayout,
                               const GraphLayout& afterLayout)
{
    if (!ctx.undoStack) return;

    scene::Scene* scene = ctx.activeScene;
    std::string targetInstanceId;
    if (entityId.IsValid() && scene) {
        if (auto* go = scene->GetGameObject(entityId))
            targetInstanceId = go->instanceId;
    }
    EditorContext* context = &ctx;
    const auto markDirty = ctx.markSceneDirty;
    auto apply = [scene, context, owner, targetInstanceId, assetAnimator, markDirty](
                     const scene::AnimatorComponent& animator,
                     const GraphLayout& layout) {
        if (!targetInstanceId.empty() && scene) {
            if (auto* go = scene->FindByGuid(targetInstanceId)) {
                if (auto* target = go->GetComponent<scene::AnimatorComponent>())
                    ApplyAnimationGraphSnapshot(*target, animator);
            }
            if (markDirty) markDirty();
        } else if (assetAnimator) {
            ApplyAnimationGraphSnapshot(*assetAnimator, animator);
            context->animationControllerDirty = true;
        }
        context->graphLayouts[owner] = layout;
        context->animationGraphSelection.Clear();
    };

    ctx.undoStack->Push(std::make_unique<LambdaCommand>(
        "Edit Animation Graph",
        [apply, afterAnimator, afterLayout]() { apply(afterAnimator, afterLayout); },
        [apply, beforeAnimator, beforeLayout]() { apply(beforeAnimator, beforeLayout); }));
}

void TrackAnimationGraphUndo(EditorContext& ctx,
                             const std::string& owner,
                             scene::EntityID entityId,
                             const std::shared_ptr<scene::AnimatorComponent>& assetAnimator,
                             scene::AnimatorComponent& animator,
                             const scene::AnimatorComponent& beforeDraw,
                             const GraphLayout& beforeLayout,
                             std::uint64_t generationBefore)
{
    static AnimationGraphUndoTracker tracker;
    const ImGuiID activeId = ImGui::GetActiveID();
    const bool changed = AnimationGraphEditGeneration() != generationBefore;

    if (tracker.active && tracker.owner != owner) {
        tracker.active = false;
    } else if (tracker.active && activeId != tracker.activeId) {
        PushAnimationGraphCommand(
            ctx, tracker.owner, tracker.entityId, tracker.assetAnimator,
            tracker.beforeAnimator, MakeAnimationGraphSnapshot(animator),
            tracker.beforeLayout, ctx.graphLayouts[owner]);
        tracker.active = false;
    }

    if (!changed) return;
    if (activeId != 0) {
        if (!tracker.active) {
            tracker.owner = owner;
            tracker.activeId = activeId;
            tracker.entityId = entityId;
            tracker.assetAnimator = assetAnimator;
            tracker.beforeAnimator = beforeDraw;
            tracker.beforeLayout = beforeLayout;
            tracker.active = true;
        }
        return;
    }

    PushAnimationGraphCommand(
        ctx, owner, entityId, assetAnimator,
        beforeDraw, MakeAnimationGraphSnapshot(animator),
        beforeLayout, ctx.graphLayouts[owner]);
}

std::string MakeUniqueStateName(const scene::AnimatorComponent& animator, const char* baseName)
{
    const std::string base = (baseName && baseName[0] != '\0') ? baseName : "NewState";
    auto exists = [&](const std::string& name) {
        return std::any_of(animator.states.begin(), animator.states.end(),
            [&](const scene::AnimationState& state) { return state.name == name; });
    };

    if (!exists(base)) return base;
    for (int i = 1; i < 10000; ++i) {
        std::string candidate = base + std::to_string(i);
        if (!exists(candidate)) return candidate;
    }
    return base + "_";
}

// State / Motion の Source として受け付けられるアセットか。
// .anim = クリップ単体、.fbx / .fzasset / .asset = クリップを内包するモデルコンテナ。
// AnimatorSystem::LoadClips が実際に読める形だけを許可し、UI 側で無効な参照を作らせない。
bool IsAnimationSourceAsset(const std::string& path)
{
    const std::string lower = util::StringUtils::ToLower(path);
    return util::StringUtils::EndsWith(lower, ".anim")
        || util::StringUtils::EndsWith(lower, ".fbx")
        || util::StringUtils::EndsWith(lower, ".fzasset")
        || util::StringUtils::EndsWith(lower, ".asset");
}

// ドロップされたアセットパスから State の初期名を作る。
// WHY "@" 以降を採るか: FBX から焼かれたクリップは "<Model>@<Clip>.anim" 命名なので、
//   ファイル名そのままだとどの State も "Player@..." で始まり、グラフ上で見分けられない。
std::string StateNameFromAssetPath(const std::string& path)
{
    std::string stem = util::FileSystem::PathToUtf8(
        util::FileSystem::PathFromUtf8(path).stem());
    if (const auto at = stem.find_last_of('@'); at != std::string::npos && at + 1 < stem.size())
        stem = stem.substr(at + 1);
    return stem.empty() ? std::string("NewState") : stem;
}

int FindStateIndexByName(const scene::AnimatorComponent& animator, const std::string& name)
{
    for (int i = 0; i < static_cast<int>(animator.states.size()); ++i) {
        if (animator.states[static_cast<size_t>(i)].name == name) return i;
    }
    return -1;
}

bool IsFloatCondition(scene::ConditionOp op)
{
    return static_cast<int>(op) <= static_cast<int>(scene::ConditionOp::NotEqual);
}

float ClampZoom(float zoom)
{
    return std::clamp(zoom, MIN_CANVAS_ZOOM, MAX_CANVAS_ZOOM);
}

const char* StateModeName(scene::AnimationStateMode mode)
{
    switch (mode) {
    case scene::AnimationStateMode::Clip:        return "CLIP";
    case scene::AnimationStateMode::BlendTree1D: return "BLEND TREE 1D";
    case scene::AnimationStateMode::BlendTree2D: return "BLEND TREE 2D";
    }
    return "UNKNOWN";
}

ImVec4 StateModeTextColor(scene::AnimationStateMode mode)
{
    switch (mode) {
    case scene::AnimationStateMode::Clip:        return ImVec4(0.52f, 0.72f, 1.00f, 1.0f);
    case scene::AnimationStateMode::BlendTree1D: return ImVec4(0.35f, 0.88f, 0.94f, 1.0f);
    case scene::AnimationStateMode::BlendTree2D: return ImVec4(0.78f, 0.58f, 1.00f, 1.0f);
    }
    return ImVec4(0.75f, 0.75f, 0.75f, 1.0f);
}

// ステートノードの論理幅。ImNodes はノードの幅を「本体で一番広い項目」で決めるため、
// クリップの絶対パスをそのまま流すとノード 1 個が画面幅を超える。ここを唯一の基準にし、
// 収まらない文字列は省略してツールチップへ逃がす。
constexpr float STATE_NODE_WIDTH = 178.0f;

// ノード本体の実効テキスト幅 (スクリーンピクセル)。CalcTextSize はズーム後の
// ピクセルを返すので、論理幅にも同じズームを掛けて同じ空間で比較する。
float NodeTextBudget(float zoom)
{
    constexpr float PADDING = 18.0f;
    return (std::max)((STATE_NODE_WIDTH - PADDING) * (std::max)(zoom, 0.05f), 24.0f);
}

// 収まらない分だけ末尾を省略する。ImGui::TextUnformatted と違い改行も折り返しもしない。
// WHY 折り返しにしないか: ImNodes は折り返し幅を知らないため、TextWrapped でも
//     ノードは長い方の行幅まで広がる。省略しないと幅は縮まらない。
std::string ElideToWidth(const std::string& text, float budget)
{
    if (text.empty() || ImGui::CalcTextSize(text.c_str()).x <= budget) return text;
    const float ellipsisWidth = ImGui::CalcTextSize("...").x;
    std::size_t fit = 0;
    for (std::size_t i = 1; i <= text.size(); ++i) {
        // UTF-8 の途中で切ると豆腐になる。次のバイトが継続バイトなら文字の途中なので、
        // 幅の判定も打ち切りもここでは行わない (次の文字境界まで進める)。
        if (i < text.size() && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) continue;
        if (ImGui::CalcTextSize(text.c_str(), text.c_str() + i).x + ellipsisWidth > budget)
            break;
        fit = i;
    }
    return text.substr(0, fit) + "...";
}

void TextElided(const std::string& text, float budget)
{
    ImGui::TextUnformatted(ElideToWidth(text, budget).c_str());
}

void TextElidedDisabled(const std::string& text, float budget)
{
    ImGui::TextDisabled("%s", ElideToWidth(text, budget).c_str());
}

// 合成ビューが使うスケルトン。
// WHY 3 経路あるか: .animcontroller 単体を開いているときは GameObject が無い。
//     マスクは作成元 FBX を覚えているので、それを最後の頼りにする。これが無いと
//     「シーンに Player を置いてからでないとマスクを検証できない」になる。
const asset::Skeleton* ResolveCompositionSkeleton(EditorContext& ctx,
                                                  const scene::AnimatorComponent& animator)
{
    const auto skeletonOfObject = [](scene::GameObject* object) -> const asset::Skeleton* {
        if (!object) return nullptr;
        auto* smr = object->GetComponent<scene::SkinnedMeshRenderer>();
        if (!smr) {
            for (int i = 0, n = object->GetChildCount(); i < n && !smr; ++i)
                if (scene::GameObject* child = object->GetChild(i))
                    smr = child->GetComponent<scene::SkinnedMeshRenderer>();
        }
        if (!smr) return nullptr;
        if (!smr->model && !smr->modelPath.empty())
            smr->model = asset::AssetManager::LoadModel(smr->modelPath);
        return smr->model ? smr->model->skeleton.get() : nullptr;
    };

    if (const asset::Skeleton* fromSelection = skeletonOfObject(ctx.GetSelectedGO()))
        return fromSelection;

    const auto skeletonOfMask = [](const std::string& maskPath) -> const asset::Skeleton* {
        if (maskPath.empty()) return nullptr;
        asset::AvatarMaskAsset mask;
        if (!asset::LoadAvatarMaskAsset(asset::AssetManager::ResolveAssetPath(maskPath), mask))
            return nullptr;
        if (mask.skeletonSourcePath.empty()) return nullptr;
        const auto model = asset::AssetManager::LoadModel(mask.skeletonSourcePath);
        return model ? model->skeleton.get() : nullptr;
    };

    if (const asset::Skeleton* fromBase = skeletonOfMask(animator.baseLayerMask.path))
        return fromBase;
    for (const auto& layer : animator.layers)
        if (const asset::Skeleton* fromLayer = skeletonOfMask(layer.mask.path))
            return fromLayer;
    return nullptr;
}

// バーの内訳を数字で読む用。Additive は取り分を持たないので倍率として別に並べる。
std::string BuildCompositionTooltip(const std::vector<maskaudit::LayerInfo>& layers,
                                    const maskaudit::BoneContribution& contribution)
{
    char line[128];
    std::snprintf(line, sizeof(line), "Base  %.0f%%", contribution.baseShare * 100.0f);
    std::string tooltip = line;
    for (std::size_t i = 0; i < layers.size(); ++i) {
        if (layers[i].additive) {
            if (contribution.additiveGain[i] <= 0.001f) continue;
            std::snprintf(line, sizeof(line), "\n%s  +%.2fx (additive)",
                          layers[i].name.c_str(), contribution.additiveGain[i]);
        } else {
            if (contribution.share[i] <= 0.001f) continue;
            std::snprintf(line, sizeof(line), "\n%s  %.0f%%",
                          layers[i].name.c_str(), contribution.share[i] * 100.0f);
        }
        tooltip += line;
    }
    return tooltip;
}

// 絶対パスからファイル名だけを残す。
// WHY: Library/Baked 配下の .anim は "Library/Baked/<32桁ハッシュ>/anims/Walk.anim" で、
//      途中のハッシュは読み手に何も伝えない。全文はツールチップに残す。
std::string SourceFileName(const std::string& sourcePath)
{
    if (sourcePath.empty()) return {};
    const std::size_t slash = sourcePath.find_last_of("/\\");
    return slash == std::string::npos ? sourcePath : sourcePath.substr(slash + 1);
}

// ノード本体から外した情報の置き場。ホバーしたときだけ全部出す。
std::string BuildStateTooltip(const scene::AnimationState& state)
{
    std::string tooltip = state.name.empty() ? "(Unnamed)" : state.name;
    tooltip += "\n";
    tooltip += StateModeName(state.mode);

    switch (state.mode) {
    case scene::AnimationStateMode::Clip:
        tooltip += "\nClip: ";
        tooltip += state.clipName.empty() ? "(none)" : state.clipName;
        tooltip += "\nSource: ";
        tooltip += state.sourcePath.empty() ? "(none)" : state.sourcePath;
        break;
    case scene::AnimationStateMode::BlendTree1D:
        tooltip += "\nParam: ";
        tooltip += state.blendTree1D.paramName.empty() ? "<none>" : state.blendTree1D.paramName;
        tooltip += "\n" + std::to_string(state.blendTree1D.motions.size())
                 + " motions (double-click to open)";
        break;
    case scene::AnimationStateMode::BlendTree2D:
        tooltip += "\nParams: ";
        tooltip += (state.blendTree2D.paramX.empty() ? "<none>" : state.blendTree2D.paramX);
        tooltip += " / ";
        tooltip += (state.blendTree2D.paramY.empty() ? "<none>" : state.blendTree2D.paramY);
        tooltip += "\n" + std::to_string(state.blendTree2D.motions.size())
                 + " motions (double-click to open)";
        break;
    }

    char meta[96];
    std::snprintf(meta, sizeof(meta), "\n%s | Speed %.2f | IK %.2f | %d transition%s",
                  state.loop ? "LOOP" : "ONCE", state.speed, state.ikWeight,
                  static_cast<int>(state.transitions.size()),
                  state.transitions.size() == 1 ? "" : "s");
    tooltip += meta;
    return tooltip;
}

const char* ConditionOpSymbol(scene::ConditionOp op)
{
    switch (op) {
    case scene::ConditionOp::Greater:  return ">";
    case scene::ConditionOp::Less:     return "<";
    case scene::ConditionOp::Equal:    return "==";
    case scene::ConditionOp::NotEqual: return "!=";
    case scene::ConditionOp::True:     return "is true";
    case scene::ConditionOp::False:    return "is false";
    }
    return "?";
}

std::string BuildTransitionBadge(const scene::AnimationTransition& transition)
{
    std::string result;
    if (transition.hasExitTime) {
        char exitBuffer[32]{};
        std::snprintf(
            exitBuffer,
            sizeof(exitBuffer),
            "Exit %.0f%%",
            transition.exitTime * 100.0f);
        result = exitBuffer;
    }

    if (!transition.conditions.empty()) {
        if (!result.empty()) result += "  |  ";
        const auto& condition = transition.conditions.front();
        result += condition.paramName;
        result += " ";
        result += ConditionOpSymbol(condition.op);
        if (IsFloatCondition(condition.op)) {
            char thresholdBuffer[32]{};
            std::snprintf(
                thresholdBuffer,
                sizeof(thresholdBuffer),
                " %.2f",
                condition.threshold);
            result += thresholdBuffer;
        }
        if (transition.conditions.size() > 1)
            result += " +" + std::to_string(transition.conditions.size() - 1);
    }

    return result.empty() ? "Immediate" : result;
}

unsigned int NodeBackgroundColor(scene::AnimationStateMode mode,
                                 bool isCurrent,
                                 bool isDefault)
{
    if (isCurrent) return IM_COL32(24, 72, 48, 255);
    if (isDefault) return IM_COL32(74, 57, 22, 255);
    if (mode == scene::AnimationStateMode::BlendTree1D)
        return IM_COL32(27, 44, 52, 255);
    if (mode == scene::AnimationStateMode::BlendTree2D)
        return IM_COL32(42, 34, 54, 255);
    return IM_COL32(34, 39, 47, 255);
}

unsigned int NodeTitleColor(scene::AnimationStateMode mode,
                            bool isCurrent,
                            bool isDefault)
{
    if (isCurrent) return IM_COL32(41, 138, 82, 255);
    if (isDefault) return IM_COL32(156, 112, 32, 255);
    if (mode == scene::AnimationStateMode::BlendTree1D)
        return IM_COL32(35, 91, 105, 255);
    if (mode == scene::AnimationStateMode::BlendTree2D)
        return IM_COL32(91, 57, 112, 255);
    return IM_COL32(54, 62, 72, 255);
}

unsigned int NodeOutlineColor(bool isCurrent, bool isDefault)
{
    if (isCurrent) return IM_COL32(78, 226, 136, 255);
    if (isDefault) return IM_COL32(237, 184, 67, 255);
    return IM_COL32(108, 118, 132, 255);
}

bool DrawFloatParameterCombo(EditorContext& ctx,
                             const char* label,
                             const scene::AnimatorComponent& animator,
                             std::string& parameterName)
{
    const char* preview = parameterName.empty() ? "<Select Float Parameter>" : parameterName.c_str();
    bool changed = false;
    if (ImGui::BeginCombo(label, preview)) {
        for (const auto& parameter : animator.parameters) {
            if (parameter.type != scene::ParamType::Float) continue;
            const bool selected = parameter.name == parameterName;
            if (ImGui::Selectable(parameter.name.c_str(), selected)) {
                parameterName = parameter.name;
                changed = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (changed) MarkDirty(ctx);
    return changed;
}

// クリップの取得元アセットを選ぶ欄。
//
// WHY 共通ウィジェットを使うか:
//   ここは以前 ImGui::InputText と AcceptDragDropPayload("ASSET_PATH") を手書きしていた。
//   その結果、(1) "..." の検索ピッカーが無く目的のクリップを Asset Browser で
//   探し回るしかない、(2) 拡張子を検証しないので .png でもフォルダでも受け付けて
//   静かに壊れる、(3) パスの表示規則が Inspector の他のアセット欄と揃わない、
//   という 3 つが同時に起きていた。widgets::AssetPathField は検索・型フィルター・
//   Ping・右クリックメニュー・D&D を 1 箇所で持っているので、そちらへ寄せる。
//   Inspector 側の "Ref Source" (加算レイヤーの基準ポーズ) は既にこれを使っており、
//   同じ種類の値をパネルごとに違う UI で編集している状態を解消する意味もある。
bool DrawAnimationSource(EditorContext& ctx,
                         const char* label,
                         scene::AnimatorComponent& animator,
                         std::string& sourcePath)
{
    // .anim = クリップ単体、.fbx = インポート元、.asset/.fzasset = インポート済みモデル。
    //
    // WHY .anim を受けるか (不具合修正):
    //   ランタイム (AnimatorSystem::LoadClips) は sourcePath が .anim のとき
    //   AnimationClip として直接ロードする経路を持っており、「1 クリップ = 1 .anim」が
    //   FBZZ の標準的な指定方法になっている。にもかかわらずこの欄のフィルターが
    //   ".asset,.fbx" のままだったため、AcceptAssetPathDrop が .anim を無言で捨てていた。
    //   結果、FBX の展開先 (Library/Baked/<guid>/anims/) から Assets へ取り出した .anim を
    //   ドラッグしても「何も起きない」= アタッチできない状態になっていた。
    const bool changed = widgets::AssetPathField(
        label, sourcePath, ".anim,.asset,.fzasset,.fbx", ctx.projectRoot);
    if (changed) {
        animator.clips.clear();
        animator.clipSourcePaths.clear();
        animator.clipsLoaded = false;
        MarkDirty(ctx);
    }
    return changed;
}

// 指定された Source / Clip の実再生秒数を返し、Graph UI の Length 表示に使用する。
// Source / Clip 名 / index から実体のクリップを引く。
// WHY 切り出すか: Length 表示と Loop Time の引き継ぎが同じ探索を必要とする。
//     片方だけ規則を変えると「表示している尺と、参照しているクリップが別」になる。
const asset::AnimationClip* FindClip(const scene::AnimatorComponent& animator,
                                     const std::string& sourcePath,
                                     const std::string& clipName,
                                     int clipIndex)
{
    const asset::AnimationClip* found = nullptr;
    for (size_t i = 0; i < animator.clips.size(); ++i) {
        if (!sourcePath.empty() &&
            (i >= animator.clipSourcePaths.size() ||
             animator.clipSourcePaths[i] != sourcePath))
            continue;
        if (!found) found = &animator.clips[i];
        if (!clipName.empty() && animator.clips[i].name == clipName) {
            found = &animator.clips[i];
            break;
        }
    }
    if (!found && clipIndex >= 0 &&
        clipIndex < static_cast<int>(animator.clips.size()))
        found = &animator.clips[static_cast<size_t>(clipIndex)];
    return found;
}

float GetClipLength(const scene::AnimatorComponent& animator,
                    const std::string& sourcePath,
                    const std::string& clipName,
                    int clipIndex)
{
    const asset::AnimationClip* clip = FindClip(animator, sourcePath, clipName, clipIndex);
    return clip ? static_cast<float>(clip->GetDurationSeconds()) : 0.0f;
}

// 単一 Clip ステートの Length を返す。BlendTree は実行時 Weight 依存のため 0 を返す。
float GetStateClipLength(const scene::AnimatorComponent& animator,
                         const scene::AnimationState* state)
{
    if (!state || state->mode != scene::AnimationStateMode::Clip) return 0.0f;
    return GetClipLength(
        animator, state->sourcePath, state->clipName, state->clipIndex);
}

// Unity の Transition Preview と同様に、遷移元・遷移先・ブレンド区間を時間軸で表示する。
// WHY: 数値だけでは Clip Length に対する Duration の大きさを判断しづらいため。
void DrawTransitionTimeline(const scene::AnimatorComponent& animator,
                            const scene::AnimationState* sourceState,
                            const scene::AnimationState* destinationState,
                            const scene::AnimationTransition& transition,
                            float sourceLength,
                            float destinationLength)
{
    const float displaySourceLength = sourceLength > 0.0f ? sourceLength : 1.0f;
    const float displayDestinationLength =
        destinationLength > 0.0f ? destinationLength : displaySourceLength;
    const float blendSeconds = transition.fixedDuration
        ? transition.transitionDuration
        : transition.transitionDuration * displaySourceLength;
    const float transitionStart = transition.hasExitTime
        ? transition.exitTime * displaySourceLength
        : (std::max)(displaySourceLength - blendSeconds, 0.0f);
    const float destinationStart = transitionStart;
    const float timelineLength = (std::max)(
        (std::max)(displaySourceLength, transitionStart + blendSeconds),
        destinationStart + displayDestinationLength);

    ImGui::SeparatorText("Transition Preview");
    const float width = (std::max)(ImGui::GetContentRegionAvail().x, 220.0f);
    constexpr float HEIGHT = 94.0f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##TransitionTimeline", ImVec2(width, HEIGHT));

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 max(origin.x + width, origin.y + HEIGHT);
    drawList->AddRectFilled(origin, max, IM_COL32(27, 30, 35, 255), 4.0f);
    drawList->AddRect(origin, max, IM_COL32(76, 83, 94, 255), 4.0f);

    constexpr float LABEL_WIDTH = 54.0f;
    constexpr float RIGHT_PADDING = 10.0f;
    const float trackLeft = origin.x + LABEL_WIDTH;
    const float trackWidth = (std::max)(width - LABEL_WIDTH - RIGHT_PADDING, 1.0f);
    const auto timeToX = [&](float seconds) {
        return trackLeft +
            std::clamp(seconds / (std::max)(timelineLength, 0.0001f), 0.0f, 1.0f) *
                trackWidth;
    };

    drawList->AddText(
        ImVec2(origin.x + 8.0f, origin.y + 20.0f),
        IM_COL32(155, 188, 235, 255), "Source");
    drawList->AddText(
        ImVec2(origin.x + 8.0f, origin.y + 54.0f),
        IM_COL32(242, 177, 96, 255), "Dest");

    const ImVec2 sourceMin(trackLeft, origin.y + 18.0f);
    const ImVec2 sourceMax(timeToX(displaySourceLength), origin.y + 38.0f);
    const ImVec2 destinationMin(timeToX(destinationStart), origin.y + 52.0f);
    const ImVec2 destinationMax(
        timeToX(destinationStart + displayDestinationLength), origin.y + 72.0f);
    drawList->AddRectFilled(
        sourceMin, sourceMax, IM_COL32(67, 122, 190, 255), 3.0f);
    drawList->AddRectFilled(
        destinationMin, destinationMax, IM_COL32(202, 126, 53, 255), 3.0f);
    if (sourceState && sourceMax.x - sourceMin.x > 48.0f)
        drawList->AddText(
            ImVec2(sourceMin.x + 6.0f, sourceMin.y + 2.0f),
            IM_COL32(235, 242, 252, 255), sourceState->name.c_str());
    if (destinationState && destinationMax.x - destinationMin.x > 48.0f)
        drawList->AddText(
            ImVec2(destinationMin.x + 6.0f, destinationMin.y + 2.0f),
            IM_COL32(255, 241, 220, 255), destinationState->name.c_str());

    const float blendStartX = timeToX(transitionStart);
    const float blendEndX = timeToX(transitionStart + blendSeconds);
    drawList->AddRectFilled(
        ImVec2(blendStartX, origin.y + 15.0f),
        ImVec2(blendEndX, origin.y + 75.0f),
        IM_COL32(230, 210, 105, 52));
    drawList->AddLine(
        ImVec2(blendStartX, origin.y + 13.0f),
        ImVec2(blendStartX, origin.y + 78.0f),
        IM_COL32(244, 214, 104, 255), 2.0f);
    drawList->AddLine(
        ImVec2(blendEndX, origin.y + 13.0f),
        ImVec2(blendEndX, origin.y + 78.0f),
        IM_COL32(244, 214, 104, 180), 1.0f);

    if (sourceState && destinationState &&
        animator.currentStateName == sourceState->name &&
        animator.blendToState == destinationState->name) {
        const float runtimeTime =
            transitionStart + blendSeconds * std::clamp(animator.blendWeight, 0.0f, 1.0f);
        const float runtimeX = timeToX(runtimeTime);
        drawList->AddLine(
            ImVec2(runtimeX, origin.y + 8.0f),
            ImVec2(runtimeX, origin.y + 82.0f),
            IM_COL32(245, 245, 245, 255), 2.0f);
    }

    char durationText[96]{};
    std::snprintf(
        durationText, sizeof(durationText),
        "Blend %.3f s  |  Timeline %.3f s", blendSeconds, timelineLength);
    drawList->AddText(
        ImVec2(trackLeft, origin.y + 78.0f),
        IM_COL32(166, 172, 182, 255), durationText);

    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Blue: source clip\nOrange: destination clip\nYellow: transition blend");
    }
}

// sourcePath が指すクリップを animator.clips へ読み込む (未読込のときだけ)。
//
// WHY 関数に切り出すか: Clip コンボとキャンバスへのアセットドロップが同じ読み込みを必要とする。
// WHY .anim を分岐するか (不具合修正):
//   .anim は「1 ファイル = 1 クリップ」のバイナリで、モデルコンテナではない。
//   LoadModel に渡すと ModelImporter まで落ちて必ず失敗するため、以前はここで
//   何も積まれず、.anim を Source に指定しても Clip コンボが空・Clip Length が
//   "unavailable" のままだった (実行時は AnimatorSystem::LoadClips が同じ分岐を
//   持っているので再生自体はできる、というエディターとランタイムの食い違い)。
void EnsureSourceClipsLoaded(scene::AnimatorComponent& animator, const std::string& sourcePath)
{
    if (sourcePath.empty()) return;
    const bool alreadyLoaded = std::any_of(
        animator.clipSourcePaths.begin(),
        animator.clipSourcePaths.end(),
        [&sourcePath](const std::string& loadedSource) {
            return loadedSource == sourcePath;
        });
    if (alreadyLoaded) return;

    if (util::StringUtils::EndsWith(util::StringUtils::ToLower(sourcePath), ".anim")) {
        const auto handle = asset::AssetManager::Load<asset::AnimationClip>(sourcePath);
        if (const asset::AnimationClip* clip = asset::AssetManager::Get(handle)) {
            animator.clips.push_back(*clip);
            animator.clipSourcePaths.push_back(sourcePath);
        }
        return;
    }

    if (auto model = asset::AssetManager::LoadModel(sourcePath)) {
        for (const auto& clip : model->clips) {
            animator.clips.push_back(clip);
            animator.clipSourcePaths.push_back(sourcePath);
        }
    }
}

bool DrawClipCombo(EditorContext& ctx,
                   const char* label,
                   scene::AnimatorComponent& animator,
                   const std::string& sourcePath,
                   std::string& clipName,
                   int& clipIndex)
{
    EnsureSourceClipsLoaded(animator, sourcePath);

    const char* preview = clipName.empty() ? "<Auto / First Clip>" : clipName.c_str();
    bool changed = false;
    if (ImGui::BeginCombo(label, preview)) {
        if (ImGui::Selectable("<Auto / First Clip>", clipName.empty())) {
            clipName.clear();
            clipIndex = -1;
            changed = true;
        }
        for (int i = 0; i < static_cast<int>(animator.clips.size()); ++i) {
            if (!sourcePath.empty() &&
                (i >= static_cast<int>(animator.clipSourcePaths.size()) ||
                 animator.clipSourcePaths[static_cast<size_t>(i)] != sourcePath))
                continue;
            const auto& clip = animator.clips[static_cast<size_t>(i)];
            const bool selected = clipName == clip.name;
            if (ImGui::Selectable(clip.name.c_str(), selected)) {
                clipName = clip.name;
                clipIndex = i;
                changed = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (changed) MarkDirty(ctx);
    const float clipLength =
        GetClipLength(animator, sourcePath, clipName, clipIndex);
    if (clipLength > 0.0f)
        ImGui::TextDisabled("Clip Length: %.3f s", clipLength);
    else
        ImGui::TextDisabled("Clip Length: unavailable");
    return changed;
}

bool HasFloatParameter(const scene::AnimatorComponent& animator, const std::string& name)
{
    return std::any_of(
        animator.parameters.begin(),
        animator.parameters.end(),
        [&name](const scene::AnimatorParameter& parameter) {
            return parameter.type == scene::ParamType::Float && parameter.name == name;
        });
}

void DrawBlendTreeWarnings(const scene::AnimatorComponent& animator,
                           const scene::AnimationState& state)
{
    const auto warning = [](const char* text) {
        ImGui::TextColored(ImVec4(1.0f, 0.68f, 0.25f, 1.0f), "Warning: %s", text);
    };

    if (state.mode == scene::AnimationStateMode::BlendTree1D) {
        if (!HasFloatParameter(animator, state.blendTree1D.paramName))
            warning("Select an existing Float parameter.");
        if (state.blendTree1D.motions.empty())
            warning("Add at least one motion.");
        for (size_t i = 0; i < state.blendTree1D.motions.size(); ++i) {
            const auto& motion = state.blendTree1D.motions[i];
            if (motion.sourcePath.empty())
                warning("A motion has no source.");
            for (size_t j = i + 1; j < state.blendTree1D.motions.size(); ++j) {
                if (std::abs(
                        motion.threshold -
                        state.blendTree1D.motions[j].threshold) <= 0.0001f) {
                    warning("Duplicate thresholds produce an abrupt selection.");
                    return;
                }
            }
        }
        return;
    }

    if (!HasFloatParameter(animator, state.blendTree2D.paramX) ||
        !HasFloatParameter(animator, state.blendTree2D.paramY))
        warning("Select existing Float parameters for both axes.");
    if (state.blendTree2D.paramX == state.blendTree2D.paramY &&
        !state.blendTree2D.paramX.empty())
        warning("X and Y use the same parameter.");
    if (state.blendTree2D.motions.empty())
        warning("Add at least one motion.");
    for (size_t i = 0; i < state.blendTree2D.motions.size(); ++i) {
        const auto& motion = state.blendTree2D.motions[i];
        if (motion.sourcePath.empty()) warning("A motion has no source.");
        for (size_t j = i + 1; j < state.blendTree2D.motions.size(); ++j) {
            const auto& other = state.blendTree2D.motions[j];
            if (std::abs(motion.posX - other.posX) <= 0.0001f &&
                std::abs(motion.posY - other.posY) <= 0.0001f) {
                warning("Duplicate 2D positions cannot be blended reliably.");
                return;
            }
        }
    }
}

void DrawBlendTreeEditor(EditorContext& ctx,
                         scene::AnimatorComponent& animator,
                         scene::AnimationState& state)
{
    static constexpr const char* MODE_NAMES[] = { "Clip", "Blend Tree 1D", "Blend Tree 2D" };
    int mode = static_cast<int>(state.mode);
    if (ImGui::Combo("Mode", &mode, MODE_NAMES, 3)) {
        state.mode = static_cast<scene::AnimationStateMode>(mode);
        MarkDirty(ctx);
    }

    if (state.mode == scene::AnimationStateMode::Clip) {
        DrawAnimationSource(ctx, "Animation (.anim)##state_source", animator, state.sourcePath);
        if (DrawClipCombo(
                ctx,
                "Clip",
                animator,
                state.sourcePath,
                state.clipName,
                state.clipIndex)) {
            // クリップを選び直したら、そのクリップの Loop Time を State の既定値にする。
            //
            // WHY 実行時の権威を State のままにするか (設計判断 A):
            //   AnimatorSystem は一貫して state.loop を見ており、既存の .animcontroller は
            //   すべて loop を保存済み (既定 true)。クリップ側を権威にすると、
            //   今動いているコントローラーの再生が黙って変わる。
            //   クリップは「オーサリング時の初期値の供給元」に留め、
            //   ステートごとの例外は従来どおり Inspector で作れるようにする。
            if (const asset::AnimationClip* clip =
                    FindClip(animator, state.sourcePath, state.clipName, state.clipIndex)) {
                state.loop = clip->loop;
            }
        }
        return;
    }

    DrawBlendTreeWarnings(animator, state);

    auto drawMotion = [&](scene::BlendTreeMotion& motion, bool is2D) {
        DrawAnimationSource(ctx, "Source", animator, motion.sourcePath);
        DrawClipCombo(
            ctx,
            "Clip",
            animator,
            motion.sourcePath,
            motion.clipName,
            motion.clipIndex);
        if (is2D) {
            ImGui::SetNextItemWidth(110.0f);
            if (ImGui::DragFloat("X", &motion.posX, 0.01f)) MarkDirty(ctx);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110.0f);
            if (ImGui::DragFloat("Y", &motion.posY, 0.01f)) MarkDirty(ctx);
        } else if (ImGui::DragFloat("Threshold", &motion.threshold, 0.01f)) {
            MarkDirty(ctx);
        }
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::DragFloat("Motion Speed", &motion.speed, 0.01f, -10.0f, 10.0f))
            MarkDirty(ctx);
        if (ImGui::DragFloat("Motion IK", &motion.ikWeight, 0.01f, 0.0f, 1.0f))
            MarkDirty(ctx);
    };

    if (state.mode == scene::AnimationStateMode::BlendTree1D) {
        DrawFloatParameterCombo(ctx, "Parameter", animator, state.blendTree1D.paramName);
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::DragFloat(
                "Damp Time", &state.blendTree1D.dampTime,
                0.01f, 0.0f, 2.0f, "%.2f s"))
            MarkDirty(ctx);
        if (ImGui::Checkbox("Sync Normalized Time", &state.blendTree1D.syncNormalizedTime))
            MarkDirty(ctx);
        ImGui::TextDisabled(
            "Runtime: Raw %.3f  |  Blended %.3f",
            animator.GetFloat(state.blendTree1D.paramName),
            state.blendTree1D.dampedValueInitialized
                ? state.blendTree1D.dampedValue
                : animator.GetFloat(state.blendTree1D.paramName));
        int removeIndex = -1;
        int duplicateIndex = -1;
        for (int i = 0; i < static_cast<int>(state.blendTree1D.motions.size()); ++i) {
            ImGui::PushID(i);
            auto& motion = state.blendTree1D.motions[static_cast<size_t>(i)];
            char header[160]{};
            std::snprintf(
                header, sizeof(header), "Motion %d  |  %.3f  |  %s",
                i + 1, motion.threshold,
                motion.clipName.empty() ? "<No Clip>" : motion.clipName.c_str());
            const bool open = ImGui::CollapsingHeader(
                header, ImGuiTreeNodeFlags_DefaultOpen);
            if (open) drawMotion(motion, false);
            if (open && ImGui::SmallButton("Duplicate")) duplicateIndex = i;
            if (open) ImGui::SameLine();
            if (ImGui::SmallButton("Remove")) removeIndex = i;
            ImGui::PopID();
        }
        if (duplicateIndex >= 0) {
            auto copy = state.blendTree1D.motions[static_cast<size_t>(duplicateIndex)];
            copy.threshold += 0.1f;
            state.blendTree1D.motions.insert(
                state.blendTree1D.motions.begin() + duplicateIndex + 1, copy);
            MarkDirty(ctx);
        }
        if (removeIndex >= 0) {
            state.blendTree1D.motions.erase(
                state.blendTree1D.motions.begin() + removeIndex);
            MarkDirty(ctx);
        }
        if (ImGui::Button("+ Motion")) {
            scene::BlendTreeMotion motion;
            if (!animator.clips.empty()) {
                motion.clipName = animator.clips.front().name;
                motion.clipIndex = 0;
            }
            if (!state.blendTree1D.motions.empty())
                motion.threshold = state.blendTree1D.motions.back().threshold + 1.0f;
            state.blendTree1D.motions.push_back(std::move(motion));
            MarkDirty(ctx);
        }
        ImGui::SameLine();
        if (ImGui::Button("Sort Thresholds")) {
            std::sort(state.blendTree1D.motions.begin(), state.blendTree1D.motions.end(),
                [](const auto& a, const auto& b) { return a.threshold < b.threshold; });
            MarkDirty(ctx);
        }
    } else {
        DrawFloatParameterCombo(ctx, "Parameter X", animator, state.blendTree2D.paramX);
        DrawFloatParameterCombo(ctx, "Parameter Y", animator, state.blendTree2D.paramY);
        static constexpr const char* TYPE_NAMES[] = {
            "Simple Directional", "Freeform Cartesian"
        };
        int type = static_cast<int>(state.blendTree2D.type);
        if (ImGui::Combo("2D Type", &type, TYPE_NAMES, 2)) {
            state.blendTree2D.type = static_cast<scene::BlendTree2DType>(type);
            MarkDirty(ctx);
        }
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::DragFloat(
                "Damp Time##2D", &state.blendTree2D.dampTime,
                0.01f, 0.0f, 2.0f, "%.2f s"))
            MarkDirty(ctx);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Smooths both axes. 0 makes direction changes snap.");
        if (ImGui::Checkbox(
                "Sync Normalized Time##2D", &state.blendTree2D.syncNormalizedTime))
            MarkDirty(ctx);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Share one 0..1 phase across motions.\n"
                "Needed when rings of different clip lengths overlap (walk vs run).");

        const ImVec2 previewSize(
            std::clamp(ImGui::GetContentRegionAvail().x, 220.0f, 420.0f),
            220.0f);
        const ImVec2 previewMin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##BlendTree2DPreview", previewSize);
        const ImVec2 previewEnd = ImGui::GetCursorScreenPos();
        const ImVec2 previewMax(
            previewMin.x + previewSize.x, previewMin.y + previewSize.y);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(previewMin, previewMax, IM_COL32(24, 27, 31, 255));
        drawList->AddRect(previewMin, previewMax, IM_COL32(95, 105, 118, 255));
        const ImVec2 center(
            (previewMin.x + previewMax.x) * 0.5f,
            (previewMin.y + previewMax.y) * 0.5f);
        drawList->AddLine(
            ImVec2(previewMin.x, center.y), ImVec2(previewMax.x, center.y),
            IM_COL32(65, 72, 82, 255));
        drawList->AddLine(
            ImVec2(center.x, previewMin.y), ImVec2(center.x, previewMax.y),
            IM_COL32(65, 72, 82, 255));

        float range = 1.0f;
        for (const auto& motion : state.blendTree2D.motions)
            range = (std::max)(
                range, (std::max)(std::abs(motion.posX), std::abs(motion.posY)));
        const float scale = ((std::min)(previewSize.x, previewSize.y) * 0.42f) / range;
        for (int i = 0; i < static_cast<int>(state.blendTree2D.motions.size()); ++i) {
            auto& motion = state.blendTree2D.motions[static_cast<size_t>(i)];
            const ImVec2 point(
                center.x + motion.posX * scale,
                center.y - motion.posY * scale);
            ImGui::PushID(i);
            ImGui::SetCursorScreenPos(ImVec2(point.x - 8.0f, point.y - 8.0f));
            ImGui::InvisibleButton("##MotionPoint", ImVec2(16.0f, 16.0f));
            const bool hovered = ImGui::IsItemHovered();
            const bool active = ImGui::IsItemActive();
            if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                const ImVec2 mouse = ImGui::GetIO().MousePos;
                motion.posX = (mouse.x - center.x) / scale;
                motion.posY = (center.y - mouse.y) / scale;
                MarkDirty(ctx);
            }
            drawList->AddCircleFilled(
                point, active ? 7.0f : 5.0f,
                hovered ? IM_COL32(255, 218, 132, 255)
                        : IM_COL32(255, 174, 72, 255));
            if (hovered)
                ImGui::SetTooltip(
                    "%s\nX %.3f  Y %.3f\nDrag to move",
                    motion.clipName.empty() ? "<No Clip>" : motion.clipName.c_str(),
                    motion.posX, motion.posY);
            ImGui::PopID();
        }
        ImGui::SetCursorScreenPos(previewEnd);
        const ImVec2 runtimePoint(
            center.x + animator.GetFloat(state.blendTree2D.paramX) * scale,
            center.y - animator.GetFloat(state.blendTree2D.paramY) * scale);
        drawList->AddCircle(
            runtimePoint, 7.0f, IM_COL32(78, 208, 255, 255), 0, 2.0f);
        ImGui::TextDisabled(
            "Runtime: X %.3f  Y %.3f | Drag orange points to edit",
            animator.GetFloat(state.blendTree2D.paramX),
            animator.GetFloat(state.blendTree2D.paramY));

        int removeIndex = -1;
        int duplicateIndex = -1;
        for (int i = 0; i < static_cast<int>(state.blendTree2D.motions.size()); ++i) {
            ImGui::PushID(i);
            auto& motion = state.blendTree2D.motions[static_cast<size_t>(i)];
            char header[192]{};
            std::snprintf(
                header, sizeof(header), "Motion %d  |  (%.2f, %.2f)  |  %s",
                i + 1, motion.posX, motion.posY,
                motion.clipName.empty() ? "<No Clip>" : motion.clipName.c_str());
            const bool open = ImGui::CollapsingHeader(
                header, ImGuiTreeNodeFlags_DefaultOpen);
            if (open) drawMotion(motion, true);
            if (open && ImGui::SmallButton("Duplicate")) duplicateIndex = i;
            if (open) ImGui::SameLine();
            if (ImGui::SmallButton("Remove")) removeIndex = i;
            ImGui::PopID();
        }
        if (duplicateIndex >= 0) {
            auto copy = state.blendTree2D.motions[static_cast<size_t>(duplicateIndex)];
            copy.posX += 0.1f;
            copy.posY += 0.1f;
            state.blendTree2D.motions.insert(
                state.blendTree2D.motions.begin() + duplicateIndex + 1, copy);
            MarkDirty(ctx);
        }
        if (removeIndex >= 0) {
            state.blendTree2D.motions.erase(
                state.blendTree2D.motions.begin() + removeIndex);
            MarkDirty(ctx);
        }
        if (ImGui::Button("+ Motion")) {
            scene::BlendTreeMotion motion;
            if (!animator.clips.empty()) {
                motion.clipName = animator.clips.front().name;
                motion.clipIndex = 0;
            }
            motion.posX = static_cast<float>(state.blendTree2D.motions.size());
            state.blendTree2D.motions.push_back(std::move(motion));
            MarkDirty(ctx);
        }
    }

    if (!animator.currentBlendWeights.empty()) {
        ImGui::SeparatorText("Runtime Weights");
        for (const auto& [clipName, weight] : animator.currentBlendWeights) {
            char overlay[192]{};
            std::snprintf(
                overlay, sizeof(overlay), "%s  %.1f%%",
                clipName.c_str(), weight * 100.0f);
            ImGui::ProgressBar(weight, ImVec2(-1.0f, 0.0f), overlay);
        }
    }
}

// ステート名の変更をグラフデータ全体へ波及させる。
//
// WHY 自由関数にするか: 名前は states[].name だけでなく、遷移 (toStateName)・
//     defaultStateName・ノード配置マップのキーでもある。リネームの入口が
//     グラフパネル (F2 / 右クリック) と Inspector の Name 欄の 2 つある以上、
//     「参照を全部張り替える」責務を 1 箇所に集めておかないと、
//     片方の入口だけ張り替え漏れを起こすという壊れ方をする。
//     パネル固有の選択追従は呼び出し側 (AnimationGraphPanel) の仕事として分ける。
//
// 戻り値: 実際に改名したら true。空名・重複名・添字範囲外は何もせず false。
bool RenameStateInGraph(EditorContext& ctx,
                        scene::AnimatorComponent& animator,
                        int stateIndex,
                        const std::string& oldName,
                        const std::string& newName,
                        const std::string& instanceId)
{
    if (stateIndex < 0 || stateIndex >= static_cast<int>(animator.states.size())) return false;
    if (newName.empty() || oldName == newName) return false;

    for (int i = 0; i < static_cast<int>(animator.states.size()); ++i) {
        if (i != stateIndex && animator.states[static_cast<std::size_t>(i)].name == newName)
            return false;
    }

    animator.states[static_cast<std::size_t>(stateIndex)].name = newName;
    for (auto& state : animator.states) {
        for (auto& transition : state.transitions) {
            if (transition.toStateName == oldName) transition.toStateName = newName;
        }
    }
    for (auto& transition : animator.anyStateTransitions)
        if (transition.toStateName == oldName) transition.toStateName = newName;

    if (animator.defaultStateName == oldName) animator.defaultStateName = newName;
    if (animator.currentStateName == oldName) animator.currentStateName = newName;
    if (animator.blendToState == oldName)     animator.blendToState = newName;

    auto& positions = ctx.graphLayouts[instanceId].nodePositions;
    if (auto it = positions.find(oldName); it != positions.end()) {
        const ImVec2 renamedPosition = it->second;
        positions.erase(it);
        positions[newName] = renamedPosition;
    }
    auto& blendPositions = ctx.graphLayouts[instanceId].blendTreeMotionPositions;
    if (auto it = blendPositions.find(oldName); it != blendPositions.end()) {
        auto renamedPositions = std::move(it->second);
        blendPositions.erase(it);
        blendPositions[newName] = std::move(renamedPositions);
    }

    // グラフパネルは「名前で覚えている選択」を持つので、Inspector から改名したときは
    // その追従を依頼する。放置すると次フレームの ResolveSelectionIndices が
    // 旧名を「消えたステート」と判定し、改名した瞬間に選択が外れる。
    ctx.animationGraphRenamedFrom = oldName;
    ctx.animationGraphRenamedTo   = newName;

    MarkDirty(ctx);
    return true;
}

} // namespace

static void DrawTransitionEditor(EditorContext& ctx,
                                 scene::AnimatorComponent& animator,
                                 const scene::AnimationState* sourceState,
                                 scene::AnimationTransition& transition);

// ImNodes のコンテキストは GraphCanvas が所有する。以前は旧描画経路のために
// パネル側でも 1 つ作っており、パン・ズーム・選択が二重に存在していた。
void AnimationGraphPanel::OnInit(EditorContext&) { m_graphCanvas.CreateContexts(); }

void AnimationGraphPanel::OnShutdown() { m_graphCanvas.DestroyContexts(); }

// ── 選択の同一性 (名前が権威 / 添字は派生値) ────────────────────────────────

int AnimationGraphPanel::IndexOfState(const scene::AnimatorComponent& animator,
                                      const std::string& name)
{
    if (name.empty()) return -1;
    for (int i = 0; i < static_cast<int>(animator.states.size()); ++i)
        if (animator.states[static_cast<std::size_t>(i)].name == name) return i;
    return -1;
}

void AnimationGraphPanel::ClearSelectionState()
{
    m_selectedStateNames.clear();
    m_selectedKind = NodeKind::None;
    m_selectedNode = -1;
    m_selectedAnyState = false;
    m_selectedLink = {};
    m_selectedLinkKind = NodeKind::None;
    m_selectedLinkFromName.clear();
}

// 名前から添字を作り直す。states[] を触った直後と、描画の先頭で必ず通る。
//
// WHY 毎フレームやるか: 削除・並べ替え・リネームのたびに個別へ添字を直して回ると、
//     直し漏れた 1 箇所が「選択が別のステートを指す」という気付きにくい不具合になる。
//     解決を 1 箇所に集めれば、名前が消えた = 選択が消える、が自動的に保証される。
void AnimationGraphPanel::ResolveSelectionIndices(const scene::AnimatorComponent& animator)
{
    // 存在しなくなった名前を落とす (削除されたステートの選択はここで消える)。
    std::erase_if(m_selectedStateNames, [&](const std::string& name) {
        return IndexOfState(animator, name) < 0;
    });
    if (m_selectedStateNames.empty() && m_selectedKind == NodeKind::State)
        m_selectedKind = NodeKind::None;

    m_selectedNode = (m_selectedKind == NodeKind::State && !m_selectedStateNames.empty())
        ? IndexOfState(animator, m_selectedStateNames.front())
        : -1;
    m_selectedAnyState = (m_selectedKind == NodeKind::AnyState);

    // 遷移の選択。起点ステートが消えていたら選択ごと落とす。
    switch (m_selectedLinkKind) {
    case NodeKind::AnyState:
        m_selectedLink.fromStateIndex = -2;
        break;
    case NodeKind::State: {
        const int from = IndexOfState(animator, m_selectedLinkFromName);
        if (from < 0) { m_selectedLink = {}; m_selectedLinkKind = NodeKind::None; }
        else          { m_selectedLink.fromStateIndex = from; }
        break;
    }
    default:
        m_selectedLink = {};
        m_selectedLinkKind = NodeKind::None;
        m_selectedLinkFromName.clear();
        break;
    }

    m_openBlendTreeState = IndexOfState(animator, m_openBlendTreeStateName);
    if (m_openBlendTreeState < 0) m_openBlendTreeStateName.clear();

    m_renamingNode = IndexOfState(animator, m_renamingStateName);

    switch (m_pendingTransitionKind) {
    case NodeKind::Entry:    m_pendingTransitionFrom = -3; break;
    case NodeKind::AnyState: m_pendingTransitionFrom = -2; break;
    case NodeKind::State: {
        const int from = IndexOfState(animator, m_pendingTransitionFromName);
        m_pendingTransitionFrom = from;
        if (from < 0) m_pendingTransitionKind = NodeKind::None;
        break;
    }
    default: m_pendingTransitionFrom = -1; break;
    }
}

void AnimationGraphPanel::CaptureCanvasSelection(const scene::AnimatorComponent& animator,
                                                 const std::vector<int>& selectedNodes,
                                                 const std::vector<int>& selectedLinks)
{
    ClearSelectionState();

    if (!selectedLinks.empty()) {
        const LinkRef link = ResolveLink(selectedLinks.front(), animator);
        m_selectedLink = link;
        if (link.fromStateIndex == -2) {
            m_selectedLinkKind = NodeKind::AnyState;
        } else if (link.fromStateIndex >= 0 &&
                   link.fromStateIndex < static_cast<int>(animator.states.size())) {
            m_selectedLinkKind = NodeKind::State;
            m_selectedLinkFromName =
                animator.states[static_cast<std::size_t>(link.fromStateIndex)].name;
        }
        return;
    }

    // WHY front() だけでなく全部を取るか: 矩形選択で複数掴んでも 1 個しか
    //     覚えていなかったため、「見えている選択」と Delete が消す対象が食い違っていた。
    for (const int nodeId : selectedNodes) {
        if (nodeId == AnyStateNodeId()) {
            if (m_selectedKind == NodeKind::None) m_selectedKind = NodeKind::AnyState;
            continue;
        }
        if (nodeId == EntryNodeId()) {
            if (m_selectedKind == NodeKind::None) m_selectedKind = NodeKind::Entry;
            continue;
        }
        for (int i = 0; i < static_cast<int>(animator.states.size()); ++i) {
            if (NodeId(i) != nodeId) continue;
            m_selectedKind = NodeKind::State;   // ステートが 1 つでもあればステート選択とみなす
            m_selectedStateNames.push_back(animator.states[static_cast<std::size_t>(i)].name);
            break;
        }
    }
}

void AnimationGraphPanel::SelectStateByName(const scene::AnimatorComponent& animator,
                                            const std::string& name)
{
    ClearSelectionState();
    const int index = IndexOfState(animator, name);
    if (index < 0) return;

    m_selectedKind = NodeKind::State;
    m_selectedStateNames.push_back(name);
    m_selectedNode = index;
    // WHY キャンバスへも伝えるか: 以前はパネル側の選択だけを更新していたため、
    //     ステートを追加・複製した直後に Inspector には新ステートが出るのに
    //     グラフ上はどこも光っていない、という食い違いが起きていた。
    //     VFX エディタは既に RequestSelection で揃えている。同じ規約へ寄せる。
    m_graphCanvas.RequestSelection({ NodeId(index) });
}

int AnimationGraphPanel::NodeId(int stateIndex)
{
    return stateIndex + 1;
}

int AnimationGraphPanel::InputPinId(int stateIndex)
{
    return stateIndex * 2 + 1;
}

int AnimationGraphPanel::OutputPinId(int stateIndex)
{
    return stateIndex * 2 + 2;
}

int AnimationGraphPanel::LinkId(int fromStateIndex, int transitionIndex)
{
    return (fromStateIndex << 16) | transitionIndex;
}

int AnimationGraphPanel::AnyStateNodeId() { return 1000000; }
int AnimationGraphPanel::AnyStateOutputPinId() { return 1000002; }
int AnimationGraphPanel::AnyStateLinkId(int transitionIndex)
{
    return 0x70000000 | transitionIndex;
}
int AnimationGraphPanel::EntryNodeId() { return 1000010; }
int AnimationGraphPanel::EntryOutputPinId() { return 1000012; }
int AnimationGraphPanel::EntryLinkId() { return 0x60000000; }

void AnimationGraphPanel::DrawNodeCanvas(
    EditorContext& ctx,
    scene::AnimatorComponent& animator,
    const std::string& instanceId)
{
    ImGui::BeginChild("##AnimationGenericGraph", ImVec2(0.0f, 0.0f), true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    // 名前 (権威) から添字 (派生値) を作り直す。以降のコードは添字で書けるが、
    // その添字は常に「今の states[]」と一致していることがここで保証される。
    ResolveSelectionIndices(animator);

    // リネームのポップアップは、どのポップアップの内側でもないここで開く。
    // WHY: MenuItem のハンドラから OpenPopup すると親メニューの子として開かれ、
    //      親が閉じると同時に消える (コンテキストメニューの Rename が動かなかった原因)。
    if (m_renameRequested) {
        m_renameRequested = false;
        m_renameError.clear();
        m_renameFocusPending = true;
        ImGui::OpenPopup("##AnimationGenericRename");
    }

    auto& layout = ctx.graphLayouts[instanceId];
    for (int i = 0; i < static_cast<int>(animator.states.size()); ++i) {
        const auto& state = animator.states[static_cast<std::size_t>(i)];
        if (!layout.nodePositions.contains(state.name))
            layout.nodePositions[state.name] = { 80.0f + 260.0f * static_cast<float>(i), 80.0f };
    }

    GraphView view;
    GraphNodeView entry;
    entry.id = EntryNodeId();
    entry.position = layout.entryPosition;
    entry.title = "Entry";
    entry.titleColor = IM_COL32(50, 145, 82, 255);
    entry.backgroundColor = IM_COL32(35, 82, 55, 255);
    entry.inputs = {};
    entry.outputs.push_back({ EntryOutputPinId(), "START", IM_COL32(92, 220, 132, 255),
                              IM_COL32(150, 255, 178, 255), GraphPinShape::TriangleFilled });
    entry.drawBody = [this, &animator]() {
        const char* target = "<none>";
        if (!animator.defaultStateName.empty()) target = animator.defaultStateName.c_str();
        else if (!animator.states.empty()) target = animator.states.front().name.c_str();
        ImGui::TextDisabled("DEFAULT FLOW");
        ImGui::Text("Target: %s", target);
    };
    view.nodes.push_back(std::move(entry));

    GraphNodeView anyState;
    anyState.id = AnyStateNodeId();
    anyState.position = layout.anyStatePosition;
    anyState.title = "Any State";
    anyState.titleColor = IM_COL32(125, 65, 145, 255);
    anyState.backgroundColor = IM_COL32(75, 45, 85, 255);
    anyState.inputs = {};
    anyState.outputs.push_back({ AnyStateOutputPinId(), "OUT", IM_COL32(210, 118, 232, 255),
                                 IM_COL32(242, 176, 255, 255), GraphPinShape::TriangleFilled });
    anyState.drawBody = [&animator]() {
        ImGui::TextDisabled("GLOBAL TRANSITIONS");
        ImGui::Text("%d transition%s", static_cast<int>(animator.anyStateTransitions.size()),
                     animator.anyStateTransitions.size() == 1 ? "" : "s");
    };
    view.nodes.push_back(std::move(anyState));

    for (int i = 0; i < static_cast<int>(animator.states.size()); ++i) {
        auto* state = &animator.states[static_cast<std::size_t>(i)];
        GraphNodeView node;
        node.id = NodeId(i);
        node.position = layout.nodePositions[state->name];
        node.title = state->name;
        node.titleColor = state->name == animator.currentStateName
            ? IM_COL32(50, 145, 82, 255)
            : (state->name == animator.defaultStateName
                ? IM_COL32(154, 107, 40, 255) : IM_COL32(72, 82, 98, 255));
        node.backgroundColor = IM_COL32(52, 58, 69, 255);
        node.outlineColor = i == m_selectedNode ? IM_COL32(255, 198, 92, 255) : 0;
        node.outlineThickness = i == m_selectedNode ? 2.5f : 0.0f;
        node.inputs.push_back({ InputPinId(i), "IN", IM_COL32(82, 164, 255, 255),
                                IM_COL32(132, 210, 255, 255), GraphPinShape::CircleFilled });
        node.outputs.push_back({ OutputPinId(i), "OUT", IM_COL32(255, 156, 72, 255),
                                 IM_COL32(255, 202, 118, 255), GraphPinShape::CircleFilled });
        node.minWidth = STATE_NODE_WIDTH;
        // 本文から外した情報 (絶対パス・全パラメーター) はここへ集約する。
        node.tooltip = BuildStateTooltip(*state);
        node.titleFontScale = 1.05f;
        node.drawTitle = [this, state, &animator]() {
            const bool current = state->name == animator.currentStateName;
            const bool isDefault = !current
                && (state->name == animator.defaultStateName
                    || (animator.defaultStateName.empty() && state == &animator.states.front()));
            // バッジのぶんだけ名前の取り分を削る。名前が長くてもノードは広がらない。
            const char* badge = current ? "  \xe2\x97\x8f" : (isDefault ? "  \xe2\x96\xb6" : "");
            const float budget = NodeTextBudget(m_canvasZoom)
                - (badge[0] != '\0' ? ImGui::CalcTextSize(badge).x : 0.0f);
            TextElided(state->name.empty() ? "(Unnamed)" : state->name, budget);
            if (current) {
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::TextColored({ 0.45f, 1.0f, 0.62f, 1.0f }, "%s", badge);
            } else if (isDefault) {
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::TextColored({ 1.0f, 0.76f, 0.28f, 1.0f }, "%s", badge);
            }
        };
        node.drawBody = [this, state, &animator]() {
            const float budget = NodeTextBudget(m_canvasZoom);
            switch (state->mode) {
            case scene::AnimationStateMode::Clip:
                // Clip Name 未設定でも Source だけは入っていることが多い (FBX に 1 テイク)。
                // "(no clip)" と出すと本当に未接続なのか区別できないので、実体名へ落とす。
                TextElided(!state->clipName.empty()
                    ? state->clipName
                    : (state->sourcePath.empty()
                        ? std::string("(no clip)") : SourceFileName(state->sourcePath)),
                    budget);
                break;
            case scene::AnimationStateMode::BlendTree1D:
                TextElided(state->blendTree1D.paramName.empty()
                    ? "Blend Tree 1D" : ("Blend Tree  " + state->blendTree1D.paramName), budget);
                break;
            case scene::AnimationStateMode::BlendTree2D:
                TextElided(state->blendTree2D.paramX.empty() && state->blendTree2D.paramY.empty()
                    ? "Blend Tree 2D"
                    : ("Blend Tree  " + state->blendTree2D.paramX + " / " + state->blendTree2D.paramY),
                    budget);
                break;
            }

            // 1 行に畳む。ノードで知りたいのは「ループするか」「等速か」「遷移が有るか」
            // の 3 つで、正確な数値は Inspector 側が持つ。
            std::string meta = state->loop ? "loop" : "once";
            if (std::abs(state->speed - 1.0f) > 0.001f) {
                char speedText[32];
                std::snprintf(speedText, sizeof(speedText), "  %.2gx", state->speed);
                meta += speedText;
            }
            const int transitionCount = static_cast<int>(state->transitions.size());
            if (transitionCount > 0) {
                char transitionText[32];
                std::snprintf(transitionText, sizeof(transitionText), "  \xe2\x86\x92%d", transitionCount);
                meta += transitionText;
            }
            TextElidedDisabled(meta, budget);

            if (state->name == animator.currentStateName)
                ImGui::ProgressBar(animator.GetNormalizedTime(), { -1.0f, 4.0f }, "");
        };
        view.nodes.push_back(std::move(node));
    }

    for (int from = 0; from < static_cast<int>(animator.states.size()); ++from) {
        const auto& state = animator.states[static_cast<std::size_t>(from)];
        for (int ti = 0; ti < static_cast<int>(state.transitions.size()); ++ti) {
            const auto& transition = state.transitions[static_cast<std::size_t>(ti)];
            const int to = FindStateIndexByName(animator, transition.toStateName);
            if (to < 0) continue;
            GraphLinkView link;
            link.id = LinkId(from, ti);
            link.fromPin = OutputPinId(from);
            link.toPin = InputPinId(to);
            link.color = transition.hasExitTime ? IM_COL32(228, 169, 73, 225) : IM_COL32(96, 164, 224, 220);
            link.hoveredColor = IM_COL32(140, 213, 255, 255);
            link.selectedColor = IM_COL32(255, 224, 125, 255);
            link.arrowSize = 7.0f;
            view.links.push_back(link);
        }
    }
    for (int ti = 0; ti < static_cast<int>(animator.anyStateTransitions.size()); ++ti) {
        const int to = FindStateIndexByName(animator, animator.anyStateTransitions[static_cast<std::size_t>(ti)].toStateName);
        if (to < 0) continue;
        GraphLinkView link;
        link.id = AnyStateLinkId(ti);
        link.fromPin = AnyStateOutputPinId();
        link.toPin = InputPinId(to);
        link.color = IM_COL32(177, 96, 214, 225);
        link.hoveredColor = IM_COL32(222, 144, 255, 255);
        link.selectedColor = IM_COL32(255, 224, 125, 255);
        link.pattern = GraphLinkPattern::Dashed;
        link.arrowSize = 7.0f;
        view.links.push_back(link);
    }
    int entryTarget = FindStateIndexByName(animator, animator.defaultStateName);
    if (entryTarget < 0 && !animator.states.empty()) entryTarget = 0;
    if (entryTarget >= 0) {
        GraphLinkView link;
        link.id = EntryLinkId();
        link.fromPin = EntryOutputPinId();
        link.toPin = InputPinId(entryTarget);
        link.color = IM_COL32(80, 210, 128, 230);
        link.hoveredColor = IM_COL32(128, 244, 166, 255);
        link.selectedColor = IM_COL32(255, 198, 92, 255);
        link.arrowSize = 7.0f;
        view.links.push_back(link);
    }

    m_graphCanvas.SetZoom(m_canvasZoom);
    GraphCanvas::Config config;
    config.id = "##AnimationGenericGraphCanvas";
    config.showMiniMap = true;
    config.showGrid = true;
    config.editable = CanEditAnimationGraph(ctx);
    const GraphInteraction interaction = m_graphCanvas.Draw(view, config);
    m_canvasZoom = m_graphCanvas.Zoom();

    if (interaction.selectionChanged) {
        CaptureCanvasSelection(animator, interaction.selectedNodes, interaction.selectedLinks);
        ResolveSelectionIndices(animator);
        m_selectionOwnerInstanceId = instanceId;
    }
    // リネームを開始する共通経路。実際に開くのは次フレームの先頭 (m_renameRequested)。
    const auto beginRename = [this, &animator](int stateIndex) {
        if (stateIndex < 0 || stateIndex >= static_cast<int>(animator.states.size())) return;
        m_renamingStateName = animator.states[static_cast<std::size_t>(stateIndex)].name;
        std::snprintf(m_renameBuffer, sizeof(m_renameBuffer), "%s",
                      m_renamingStateName.c_str());
        m_renameRequested = true;
    };

    // WHY ダブルクリックでリネームしないか: 以前は Clip ステートならリネーム、
    //     Blend Tree なら中へ入る、と同じ操作の意味がノードの型で変わっていた。
    //     ダブルクリック =「中へ入る」に統一し、リネームは F2 と右クリックに寄せる
    //     (Blend Tree を持たないステートでは何も起きないのが正しい)。
    if (interaction.nodeDoubleClicked) {
        for (int i = 0; i < static_cast<int>(animator.states.size()); ++i) {
            if (NodeId(i) != interaction.doubleClickedNode) continue;
            const auto mode = animator.states[static_cast<std::size_t>(i)].mode;
            if (mode == scene::AnimationStateMode::BlendTree1D
                || mode == scene::AnimationStateMode::BlendTree2D) {
                m_openBlendTreeStateName = animator.states[static_cast<std::size_t>(i)].name;
                m_openBlendTreeState = i;
                m_selectedMotion = -1;
                m_graphCanvas.ClearSelection();
            }
            break;
        }
    }
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
        && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F2))
        beginRename(m_selectedNode);

    bool moved = false;
    for (const auto& move : interaction.movedNodes) {
        if (move.nodeId == EntryNodeId()) layout.entryPosition = move.position;
        else if (move.nodeId == AnyStateNodeId()) layout.anyStatePosition = move.position;
        else {
            for (int i = 0; i < static_cast<int>(animator.states.size()); ++i) {
                if (NodeId(i) == move.nodeId) {
                    layout.nodePositions[animator.states[static_cast<std::size_t>(i)].name] = move.position;
                    moved = true;
                    break;
                }
            }
        }
        moved = true;
    }
    if (moved) MarkDirty(ctx);

    auto stateIndexFromInput = [&](int pin) {
        for (int i = 0; i < static_cast<int>(animator.states.size()); ++i)
            if (InputPinId(i) == pin) return i;
        return -1;
    };
    auto sourceFromOutput = [&](int pin) {
        if (pin == EntryOutputPinId()) return -3;
        if (pin == AnyStateOutputPinId()) return -2;
        for (int i = 0; i < static_cast<int>(animator.states.size()); ++i)
            if (OutputPinId(i) == pin) return i;
        return -1;
    };
    if (interaction.linkCreated) {
        const int source = sourceFromOutput(interaction.createdFromPin);
        const int target = stateIndexFromInput(interaction.createdToPin);
        if (source == -3 && target >= 0) {
            animator.defaultStateName = animator.states[static_cast<std::size_t>(target)].name;
            MarkDirty(ctx);
        } else if (source == -2 && target >= 0) {
            const auto& targetName = animator.states[static_cast<std::size_t>(target)].name;
            const bool exists = std::any_of(animator.anyStateTransitions.begin(), animator.anyStateTransitions.end(),
                [&targetName](const scene::AnimationTransition& t) { return t.toStateName == targetName; });
            if (!exists) {
                animator.anyStateTransitions.push_back({});
                animator.anyStateTransitions.back().toStateName = targetName;
                m_selectedLink = { -2, static_cast<int>(animator.anyStateTransitions.size()) - 1 };
                m_selectedLinkKind = NodeKind::AnyState;
                m_selectedLinkFromName.clear();
                MarkDirty(ctx);
            }
        } else if (source >= 0 && target >= 0) {
            AddTransition(ctx, animator, source, target);
        }
    }
    for (const int linkId : interaction.destroyedLinks) {
        const LinkRef link = ResolveLink(linkId, animator);
        if (link.fromStateIndex == -2 && link.transitionIndex >= 0) {
            animator.anyStateTransitions.erase(animator.anyStateTransitions.begin() + link.transitionIndex);
            m_selectedLink = {};
            m_selectedLinkKind = NodeKind::None;
            m_selectedLinkFromName.clear();
            MarkDirty(ctx);
        } else if (link.fromStateIndex >= 0 && link.transitionIndex >= 0) {
            auto& transitions = animator.states[static_cast<std::size_t>(link.fromStateIndex)].transitions;
            transitions.erase(transitions.begin() + link.transitionIndex);
            m_selectedLink = {};
            m_selectedLinkKind = NodeKind::None;
            m_selectedLinkFromName.clear();
            MarkDirty(ctx);
        }
    }
    // 選択中のステートを「すべて」消す。
    //
    // WHY 名前で回すか: DeleteState は erase で添字を詰めるため、添字のリストを
    //     順に渡すと 2 件目以降が別のステートを指す。名前は消しても他へずれないので、
    //     1 件ずつ引き直して消せば取り違えが起きない。
    const auto deleteSelectedStates = [&]() {
        if (m_selectedStateNames.empty()) return;
        const std::vector<std::string> targets = m_selectedStateNames;
        for (const std::string& name : targets) {
            const int index = IndexOfState(animator, name);
            if (index >= 0) DeleteState(ctx, animator, index, instanceId);
        }
        ClearSelectionState();
        m_graphCanvas.ClearSelection();
    };

    if (interaction.deleteRequested) {
        if (!m_selectedStateNames.empty()) deleteSelectedStates();
        else if (m_selectedLink.fromStateIndex == -2 && m_selectedLink.transitionIndex >= 0) {
            animator.anyStateTransitions.erase(animator.anyStateTransitions.begin() + m_selectedLink.transitionIndex);
            m_selectedLink = {};
            m_selectedLinkKind = NodeKind::None;
            m_selectedLinkFromName.clear();
            MarkDirty(ctx);
        } else if (m_selectedLink.fromStateIndex >= 0 && m_selectedLink.transitionIndex >= 0) {
            auto& transitions = animator.states[static_cast<std::size_t>(m_selectedLink.fromStateIndex)].transitions;
            transitions.erase(transitions.begin() + m_selectedLink.transitionIndex);
            m_selectedLink = {};
            m_selectedLinkKind = NodeKind::None;
            m_selectedLinkFromName.clear();
            MarkDirty(ctx);
        }
    }
    if (interaction.duplicateRequested && m_selectedNode >= 0)
        DuplicateState(ctx, animator, m_selectedNode, instanceId);

    // Asset Browser からクリップを落として State を作る (Unity の Animator と同じ操作)。
    //
    // WHY 追加するか (不具合修正): これまでステートマシンのキャンバスには
    //   ドロップの受け皿が一切なく、.anim を持ってきても落とせなかった。
    //   State を作ってから Inspector の Source 欄で選び直すしかなく、
    //   「クリップをグラフへ置く」という一番自然な導線が存在しなかった。
    if (interaction.assetDropped) {
        const std::string dropped = NormalizeAssetPath(interaction.droppedAssetPath);
        if (IsAnimationSourceAsset(dropped)) {
            AddStateAt(ctx, animator, StateNameFromAssetPath(dropped).c_str(),
                       instanceId, interaction.dropPosition.x, interaction.dropPosition.y);
            auto& created = animator.states.back();
            created.mode       = scene::AnimationStateMode::Clip;
            created.sourcePath = dropped;
            // AddStateAt は「既存クリップの先頭」を初期値に入れる。落としたアセットとは
            // 無関係な名前なので必ず捨て、Source 側の解決 (<Auto / First Clip>) に任せる。
            created.clipName.clear();
            created.clipIndex = -1;
            // Loop Time はクリップに焼かれた値を State の初期値として引き継ぐ
            // (Clip コンボで選び直したときと同じ規則)。
            EnsureSourceClipsLoaded(animator, dropped);
            if (const asset::AnimationClip* clip =
                    FindClip(animator, dropped, created.clipName, created.clipIndex))
                created.loop = clip->loop;
            MarkDirty(ctx);
        }
    }

    if (interaction.contextMenuRequested) {
        m_contextSpawnX = interaction.contextSpawnPosition.x;
        m_contextSpawnY = interaction.contextSpawnPosition.y;
        ImGui::OpenPopup("##AnimationGenericCanvasMenu");
    }
    if (interaction.nodeContextMenuRequested) {
        // WHY 総当りで引くか: 以前は "id >= 1 && id < 1000000 なら id-1 が添字" という
        //     ID 体系への直接依存で、Entry (1000010) がどちらの分岐にも入らず
        //     「直前の選択のままメニューが開く」不具合になっていた。
        //     ノードは種別で判定し、ステートは NodeId() の逆引きで確実に特定する。
        const int contextNode = interaction.contextMenuNode;
        if (contextNode == AnyStateNodeId()) {
            ClearSelectionState();
            m_selectedKind = NodeKind::AnyState;
            m_selectedAnyState = true;
        } else if (contextNode == EntryNodeId()) {
            ClearSelectionState();
            m_selectedKind = NodeKind::Entry;
        } else {
            for (int i = 0; i < static_cast<int>(animator.states.size()); ++i) {
                if (NodeId(i) != contextNode) continue;
                // 右クリックしたノードが選択外なら、そのノードだけを選び直す。
                // 選択済みなら複数選択を保つ (まとめて Delete したい流れを壊さない)。
                const std::string& name = animator.states[static_cast<std::size_t>(i)].name;
                const bool alreadySelected =
                    std::find(m_selectedStateNames.begin(), m_selectedStateNames.end(), name)
                        != m_selectedStateNames.end();
                if (!alreadySelected) SelectStateByName(animator, name);
                else m_selectedNode = i;
                break;
            }
        }
        ImGui::OpenPopup("##AnimationGenericNodeMenu");
    }
    if (ImGui::BeginPopup("##AnimationGenericCanvasMenu")) {
        if (ImGui::MenuItem("+ Empty Node"))
            AddStateAt(ctx, animator, "NewNode", instanceId, m_contextSpawnX, m_contextSpawnY);
        if (ImGui::MenuItem("Auto Layout")) AutoLayoutStates(ctx, animator, instanceId);
        if (ImGui::MenuItem("Center View")) m_graphCanvas.ResetView();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##AnimationGenericNodeMenu")) {
        if (m_selectedNode >= 0 && m_selectedNode < static_cast<int>(animator.states.size())) {
            auto& state = animator.states[static_cast<std::size_t>(m_selectedNode)];
            if (ImGui::MenuItem("Set as Default")) {
                animator.defaultStateName = state.name;
                MarkDirty(ctx);
            }
            if ((state.mode == scene::AnimationStateMode::BlendTree1D || state.mode == scene::AnimationStateMode::BlendTree2D)
                && ImGui::MenuItem("Open Blend Tree")) {
                m_openBlendTreeStateName = state.name;
                m_openBlendTreeState = m_selectedNode;
                m_selectedMotion = -1;
                m_graphCanvas.ClearSelection();
            }
            if (ImGui::MenuItem("Duplicate")) DuplicateState(ctx, animator, m_selectedNode, instanceId);
            if (ImGui::MenuItem("Delete")) deleteSelectedStates();
            // OpenPopup はここで呼ばない (親メニューの子として開かれ、すぐ閉じてしまう)。
            // 次フレームの先頭で開く。
            if (ImGui::MenuItem("Rename", "F2")) beginRename(m_selectedNode);
        } else if (m_selectedAnyState) {
            ImGui::TextDisabled("Any State");
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup("##AnimationGenericRename")) {
        // 開いた最初のフレームだけ入力欄へフォーカスを移す。
        // WHY: これが無いと F2 や右クリックで開いても欄をクリックするまで打てず、
        //      「リネームできない」という体験になっていた。
        if (m_renameFocusPending) {
            ImGui::SetKeyboardFocusHere();
            m_renameFocusPending = false;
        }
        const bool committed = ImGui::InputText(
            "Name", m_renameBuffer, sizeof(m_renameBuffer),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);

        if (!m_renameError.empty())
            ImGui::TextColored(EditorTheme::Color(ThemeColor::Danger), "%s", m_renameError.c_str());

        // Enter で確定。閉じるのは成功したときだけで、弾かれたら理由を出したまま残す。
        // WHY 残すか: 以前は無言で失敗して閉じていたため、
        //      「打って Enter したのに名前が変わらない」理由がどこにも出なかった。
        if (committed && m_renamingNode >= 0) {
            const std::string oldName =
                animator.states[static_cast<std::size_t>(m_renamingNode)].name;
            const std::string newName = m_renameBuffer;
            if (newName.empty()) {
                m_renameError = "Name cannot be empty.";
            } else if (newName != oldName && IndexOfState(animator, newName) >= 0) {
                m_renameError = "A state named '" + newName + "' already exists.";
            } else {
                RenameState(ctx, animator, m_renamingNode, oldName, newName, instanceId);
                m_renamingStateName.clear();
                m_renameError.clear();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::TextDisabled("Enter to apply / Esc to cancel");
        ImGui::EndPopup();
    } else if (!m_renamingStateName.empty() && !m_renameRequested) {
        // ポップアップ外クリックや Esc で閉じられた。状態を残すと次回の F2 が
        // 「開いているつもり」で始まってしまうため、必ず片付ける。
        m_renamingStateName.clear();
        m_renameError.clear();
    }
    ImGui::EndChild();
}

void AnimationGraphPanel::DrawBlendTreeCanvas(
    EditorContext& ctx,
    scene::AnimatorComponent& animator,
    const std::string& instanceId)
{
    // Blend Tree を開いている間は DrawNodeCanvas が呼ばれないため、
    // 添字の解決はここでも行う (開いているステートが削除・リネームされた場合に追従する)。
    ResolveSelectionIndices(animator);

    if (m_openBlendTreeState < 0 || m_openBlendTreeState >= static_cast<int>(animator.states.size())) {
        m_openBlendTreeStateName.clear();
        m_openBlendTreeState = -1;
        m_selectedMotion = -1;
        return;
    }
    auto& state = animator.states[static_cast<std::size_t>(m_openBlendTreeState)];
    if (state.mode != scene::AnimationStateMode::BlendTree1D
        && state.mode != scene::AnimationStateMode::BlendTree2D) {
        m_openBlendTreeStateName.clear();
        m_openBlendTreeState = -1;
        m_selectedMotion = -1;
        return;
    }
    auto& motions = state.mode == scene::AnimationStateMode::BlendTree1D
        ? state.blendTree1D.motions : state.blendTree2D.motions;
    auto& positions = ctx.graphLayouts[instanceId].blendTreeMotionPositions[state.name];
    positions.resize(motions.size());
    for (std::size_t i = 0; i < positions.size(); ++i) {
        if (positions[i].x == 0.0f && positions[i].y == 0.0f)
            positions[i] = { 160.0f + static_cast<float>(i % 3) * 280.0f,
                              50.0f + static_cast<float>(i / 3) * 230.0f };
    }

    ImGui::BeginChild("##BlendTreeGeneric", ImVec2(0.0f, 0.0f), true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::TextColored({ 0.52f, 0.78f, 1.0f, 1.0f }, "%s",
        state.mode == scene::AnimationStateMode::BlendTree1D ? "Blend Tree 1D" : "Blend Tree 2D");
    ImGui::SameLine();
    if (state.mode == scene::AnimationStateMode::BlendTree1D) {
        ImGui::SetNextItemWidth(180.0f);
        DrawFloatParameterCombo(ctx, "Parameter", animator, state.blendTree1D.paramName);
    } else {
        ImGui::SetNextItemWidth(150.0f);
        DrawFloatParameterCombo(ctx, "Parameter X", animator, state.blendTree2D.paramX);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150.0f);
        DrawFloatParameterCombo(ctx, "Parameter Y", animator, state.blendTree2D.paramY);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Motion")) {
        scene::BlendTreeMotion motion;
        if (!animator.clips.empty()) { motion.clipName = animator.clips.front().name; motion.clipIndex = 0; }
        motions.push_back(std::move(motion));
        positions.push_back({ 160.0f, 50.0f });
        m_selectedMotion = static_cast<int>(motions.size()) - 1;
        MarkDirty(ctx);
    }
    ImGui::Separator();

    constexpr int ROOT_NODE_ID = 2000000;
    constexpr int ROOT_OUTPUT_ID = 2000002;
    constexpr int MOTION_NODE_BASE = 2010000;
    constexpr int MOTION_INPUT_BASE = 2020000;
    constexpr int MOTION_LINK_BASE = 2030000;
    GraphView view;
    GraphNodeView root;
    root.id = ROOT_NODE_ID;
    root.position = { -220.0f, 100.0f };
    root.title = "Blend Parameter";
    root.titleColor = IM_COL32(39, 112, 145, 255);
    root.backgroundColor = IM_COL32(34, 77, 98, 255);
    root.inputs = {};
    root.outputs.push_back({ ROOT_OUTPUT_ID, "MOTIONS", IM_COL32(104, 218, 255, 255),
                             IM_COL32(164, 240, 255, 255), GraphPinShape::TriangleFilled });
    root.drawBody = [&state, &animator]() {
        if (state.mode == scene::AnimationStateMode::BlendTree1D) {
            ImGui::Text("%s", state.blendTree1D.paramName.empty() ? "<Select Float Parameter>" : state.blendTree1D.paramName.c_str());
            ImGui::TextDisabled("Raw %.3f", animator.GetFloat(state.blendTree1D.paramName));
        } else {
            ImGui::Text("X: %s", state.blendTree2D.paramX.empty() ? "<none>" : state.blendTree2D.paramX.c_str());
            ImGui::Text("Y: %s", state.blendTree2D.paramY.empty() ? "<none>" : state.blendTree2D.paramY.c_str());
        }
    };
    view.nodes.push_back(std::move(root));

    for (int i = 0; i < static_cast<int>(motions.size()); ++i) {
        auto* motion = &motions[static_cast<std::size_t>(i)];
        GraphNodeView node;
        node.id = MOTION_NODE_BASE + i;
        node.position = positions[static_cast<std::size_t>(i)];
        node.title = "Motion " + std::to_string(i + 1);
        node.titleColor = m_selectedMotion == i ? IM_COL32(151, 103, 38, 255) : IM_COL32(72, 82, 98, 255);
        node.backgroundColor = IM_COL32(52, 58, 69, 255);
        node.outlineColor = m_selectedMotion == i ? IM_COL32(255, 198, 92, 255) : 0;
        node.outlineThickness = m_selectedMotion == i ? 2.5f : 0.0f;
        node.inputs.push_back({ MOTION_INPUT_BASE + i, "WEIGHT", IM_COL32(82, 164, 255, 255),
                                IM_COL32(132, 210, 255, 255), GraphPinShape::CircleFilled });
        node.drawBody = [this, &ctx, &animator, &state, motion]() {
            ImGui::SetNextItemWidth(220.0f);
            DrawAnimationSource(ctx, "Source", animator, motion->sourcePath);
            ImGui::SetNextItemWidth(220.0f);
            DrawClipCombo(ctx, "Clip", animator, motion->sourcePath, motion->clipName, motion->clipIndex);
            if (state.mode == scene::AnimationStateMode::BlendTree1D) {
                ImGui::SetNextItemWidth(110.0f);
                if (ImGui::DragFloat("Threshold", &motion->threshold, 0.01f)) MarkDirty(ctx);
            } else {
                ImGui::SetNextItemWidth(95.0f);
                if (ImGui::DragFloat("X", &motion->posX, 0.01f)) MarkDirty(ctx);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(95.0f);
                if (ImGui::DragFloat("Y", &motion->posY, 0.01f)) MarkDirty(ctx);
            }
            ImGui::TextDisabled("Speed %.2f | IK %.2f", motion->speed, motion->ikWeight);
        };
        view.nodes.push_back(std::move(node));
        view.links.push_back({ MOTION_LINK_BASE + i, ROOT_OUTPUT_ID, MOTION_INPUT_BASE + i,
                               IM_COL32(104, 170, 210, 220), IM_COL32(140, 213, 255, 255),
                               IM_COL32(255, 224, 125, 255), GraphLinkPattern::Solid,
                               -1.0f, -1.0f, -1.0f, -1.0f, 7.0f });
    }

    m_graphCanvas.SetZoom(m_canvasZoom);
    GraphCanvas::Config config;
    config.id = "##BlendTreeGenericCanvas";
    config.showMiniMap = true;
    config.showGrid = true;
    config.editable = CanEditAnimationGraph(ctx);
    const GraphInteraction interaction = m_graphCanvas.Draw(view, config);
    m_canvasZoom = m_graphCanvas.Zoom();

    if (interaction.selectionChanged) {
        m_selectedMotion = -1;
        if (!interaction.selectedNodes.empty()) {
            const int id = interaction.selectedNodes.front();
            if (id >= MOTION_NODE_BASE && id < MOTION_NODE_BASE + static_cast<int>(motions.size()))
                m_selectedMotion = id - MOTION_NODE_BASE;
        }
    }
    for (const auto& move : interaction.movedNodes) {
        if (move.nodeId >= MOTION_NODE_BASE && move.nodeId < MOTION_NODE_BASE + static_cast<int>(positions.size())) {
            positions[static_cast<std::size_t>(move.nodeId - MOTION_NODE_BASE)] = move.position;
            MarkDirty(ctx);
        }
    }

    int pendingDelete = -1;
    int pendingDuplicate = -1;
    if (interaction.deleteRequested) pendingDelete = m_selectedMotion;
    if (interaction.duplicateRequested) pendingDuplicate = m_selectedMotion;
    if (interaction.nodeContextMenuRequested) {
        const int id = interaction.contextMenuNode;
        if (id >= MOTION_NODE_BASE && id < MOTION_NODE_BASE + static_cast<int>(motions.size())) {
            m_selectedMotion = id - MOTION_NODE_BASE;
            ImGui::OpenPopup("##BlendGenericMotionMenu");
        }
    }
    if (ImGui::BeginPopup("##BlendGenericMotionMenu")) {
        if (ImGui::MenuItem("Duplicate")) pendingDuplicate = m_selectedMotion;
        if (ImGui::MenuItem("Delete")) pendingDelete = m_selectedMotion;
        ImGui::EndPopup();
    }
    if (interaction.contextMenuRequested) {
        ImGui::OpenPopup("##BlendGenericCanvasMenu");
    }
    if (ImGui::BeginPopup("##BlendGenericCanvasMenu")) {
        if (ImGui::MenuItem("+ New Motion")) {
            scene::BlendTreeMotion motion;
            motions.push_back(std::move(motion));
            positions.push_back(interaction.contextSpawnPosition);
            m_selectedMotion = static_cast<int>(motions.size()) - 1;
            MarkDirty(ctx);
        }
        if (ImGui::MenuItem("Back to Base Layer")) {
            m_openBlendTreeStateName.clear();
            m_openBlendTreeState = -1;
            m_selectedMotion = -1;
            m_graphCanvas.ResetView();
        }
        ImGui::EndPopup();
    }
    if (interaction.assetDropped) {
        const std::string path = NormalizeAssetPath(interaction.droppedAssetPath);
        // .anim もここで受ける。ブレンドツリーの各モーションは 1 クリップなので、
        // むしろ .anim が本来の指定形。以前は弾いていたため、クリップを落としても
        // Motion が生えなかった。
        if (IsAnimationSourceAsset(path)) {
            scene::BlendTreeMotion motion;
            motion.sourcePath = path;
            motions.push_back(std::move(motion));
            positions.push_back(interaction.dropPosition);
            m_selectedMotion = static_cast<int>(motions.size()) - 1;
            MarkDirty(ctx);
        }
    }
    if (pendingDuplicate >= 0 && pendingDuplicate < static_cast<int>(motions.size())) {
        motions.insert(motions.begin() + pendingDuplicate + 1, motions[static_cast<std::size_t>(pendingDuplicate)]);
        positions.insert(positions.begin() + pendingDuplicate + 1, positions[static_cast<std::size_t>(pendingDuplicate)] + ImVec2(40.0f, 40.0f));
        m_selectedMotion = pendingDuplicate + 1;
        MarkDirty(ctx);
    }
    if (pendingDelete >= 0 && pendingDelete < static_cast<int>(motions.size())) {
        motions.erase(motions.begin() + pendingDelete);
        positions.erase(positions.begin() + pendingDelete);
        m_selectedMotion = -1;
        MarkDirty(ctx);
    }
    ImGui::EndChild();
}

void AnimationGraphPanel::OnRenderContent(EditorContext& ctx)
{
    FBZZ_PROFILE_SCOPE("AnimationGraphPanel::Render");

    // Inspector の Name 欄で起きた改名を、パネルが名前で覚えている選択へ取り込む。
    // グラフデータ側 (遷移・レイアウト) の張り替えは RenameStateInGraph が済ませている。
    if (!ctx.animationGraphRenamedFrom.empty()) {
        AdoptStateRename(ctx.animationGraphRenamedFrom, ctx.animationGraphRenamedTo);
        ctx.animationGraphRenamedFrom.clear();
        ctx.animationGraphRenamedTo.clear();
    }
    if (!ctx.animationGraphLayerRenamedFrom.empty()) {
        const std::string oldName = ctx.animationGraphLayerRenamedFrom;
        const std::string newName = ctx.animationGraphLayerRenamedTo;
        AdoptLayerRename(oldName, newName);
        if (ctx.animationGraphSelection.layerName == oldName)
            ctx.animationGraphSelection.layerName = newName;
        ctx.animationGraphLayerRenamedFrom.clear();
        ctx.animationGraphLayerRenamedTo.clear();
    }

    // WHY: Play Mode 中は Animator の実行時状態が毎フレーム変化する。
    //      編集用 snapshot と Undo 追跡を続けると、監視表示だけで大きな CPU 負荷になる。
    const bool allowEditing = CanEditAnimationGraph(ctx);

    // ── 編集ドキュメントの決定 ────────────────────────────────────────────
    // WHY 選択追従をやめるか:
    //   以前は ctx.selectedAssetPath をそのまま編集対象にしていた。しかし Source 欄へ
    //   クリップをドラッグするには Asset Browser を触る必要があり、そこで選択が変わると
    //   編集対象ごと切り替わって未保存の変更が確認なしで消えていた。
    //   ここでは「明示的に開いたパス」を保持し、選択が動いても手放さない。
    //   まだ何も開いていないときだけ、選択中の .animcontroller を初回の入口として拾う
    //   (BehaviorTreePanel と同じ規則)。
    if (!m_requestedPath.empty()) {
        // WHY 絶対パスへ正規化するか: 呼び出し元によって "Assets/..." 相対 (Inspector の
        //     controllerPath) と絶対パス (Asset Browser の選択) が混ざる。AssetDirtyRegistry の
        //     キーは絶対パス前提なので、ここで 1 度だけ揃えておかないと
        //     「Modified 表示は出るのに Save All で拾われない」といった食い違いが起きる。
        const std::string requested =
            asset::AssetManager::ResolveAssetPath(m_requestedPath);
        m_requestedPath.clear();
        if (requested != ctx.animationControllerEditorPath) {
            ctx.animationControllerEditorPath = requested;
            ctx.animationControllerEditor.reset();  // 下のロード経路へ落とす
        }
    } else if (ctx.animationControllerEditorPath.empty() &&
               util::StringUtils::EndsWith(ctx.selectedAssetPath, ".animcontroller")) {
        ctx.animationControllerEditorPath = ctx.selectedAssetPath;
    }

    const std::string docPath = ctx.animationControllerEditorPath;
    const bool editingControllerAsset =
        util::StringUtils::EndsWith(docPath, ".animcontroller");
    if (editingControllerAsset) {
        if (!ctx.animationControllerEditor) {
            asset::AnimatorControllerAsset controller;
            if (!asset::LoadAnimatorControllerAsset(docPath, controller)) {
                ImGui::TextDisabled("Failed to load Animator Controller.");
                ImGui::TextDisabled("%s", docPath.c_str());
                if (ImGui::Button("Close")) {
                    ctx.animationControllerEditorPath.clear();
                    ctx.animationControllerDirty = false;
                }
                return;
            }
            ctx.animationControllerEditor =
                std::shared_ptr<scene::AnimatorComponent>(new scene::AnimatorComponent());
            asset::ApplyAnimatorControllerAsset(
                controller, *ctx.animationControllerEditor);
            ctx.animationControllerDirty = false;
            ctx.graphLayouts[docPath] =
                ToEditorGraphLayout(controller.editorLayout);
            m_selectionOwnerInstanceId.clear();
            // 名前側 (権威) を必ず落とす。派生値だけ消しても、次の
            // ResolveSelectionIndices が名前から復元してしまう。
            ClearSelectionState();
            m_openBlendTreeStateName.clear();
            m_openBlendTreeStateName.clear();
            m_openBlendTreeState = -1;
            m_selectedMotion = -1;
            m_pendingTransitionKind = NodeKind::None;
            m_pendingTransitionFromName.clear();
            m_pendingTransitionFrom = -1;
            m_renamingStateName.clear();
            m_renameRequested = false;
            m_renameError.clear();
            m_editingLayer.clear();
            m_graphCanvas.ClearSelection();
            m_graphCanvas.ResetView();
            ctx.animationGraphSelection.Clear();
        }

        auto& animator = *ctx.animationControllerEditor;
        scene::AnimatorComponent undoBeforeAnimator;
        GraphLayout undoBeforeLayout;
        if (allowEditing) {
            // スナップショットは差し替え前 (= Base Layer が入っている状態) で取る。
            // layers も含むため、レイヤー側の編集も Undo で戻せる。
            undoBeforeAnimator = MakeAnimationGraphSnapshot(animator);
            undoBeforeLayout = ctx.graphLayouts[docPath];
        }
        const std::uint64_t undoGenerationBefore = AnimationGraphEditGeneration();

        // 編集対象レイヤーを選ばせ、以降の描画中だけそのグラフへ差し替える。
        DrawLayerSelector(ctx, animator, allowEditing);
        LayerGraphScope layerScope(animator, m_editingLayer);

        ClearInvalidSelection(animator);
        ImGui::BeginDisabled(!allowEditing);
        ImGui::TextUnformatted(
            util::FileSystem::GetFilename(docPath).c_str());
        ImGui::SameLine();
        if (m_openBlendTreeState < 0 && ImGui::Button("+ Node"))
            AddState(ctx, animator, "NewNode");
        if (m_openBlendTreeState >= 0 &&
            m_openBlendTreeState < static_cast<int>(animator.states.size())) {
            const std::string breadcrumbName =
                animator.states[static_cast<size_t>(m_openBlendTreeState)].name;
            ImGui::SameLine();
            if (ImGui::SmallButton("Base Layer")) {
                m_openBlendTreeStateName.clear();
                m_openBlendTreeState = -1;
                m_selectedMotion = -1;
                m_graphCanvas.ClearSelection();
                m_graphCanvas.ResetView();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("> %s", breadcrumbName.c_str());
        }
        // 保存操作は他のアセット型と揃える。
        // WHY Ctrl+S を足すか: .tex / .mask / .terrain / Model Meta は既に
        //     「Apply ボタン または Ctrl+S」で保存できる。Animator Controller だけが
        //     専用ボタンのみで、他と同じつもりで Ctrl+S を押しても何も起きなかった。
        const bool controllerDirty =
            ctx.animationControllerDirty || AssetDirtyRegistry::IsDirty(docPath);
        ImGui::SameLine();
        const bool savePressed = ImGui::Button("Save Controller");
        const bool saveShortcut =
            allowEditing && controllerDirty &&
            ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S);
        if (savePressed || saveShortcut) {
            // LayerGraphScope は描画中の states を選択 Layerへ差し替えているため、
            // 保存時だけ必ず Base + layers の正規形へ戻す。これをしないと空の新規 Layerを
            // 選択して保存した瞬間、Base Layer が空の Controller として書き出される。
            layerScope.Restore();
            const bool saved = SaveAnimatorControllerWithLayout(ctx, docPath, animator);
            layerScope.Reapply();
            if (saved) {
                ctx.animationControllerDirty = false;
                AssetDirtyRegistry::MarkClean(docPath);
                ctx.requestAssetBrowserRefresh = true;
                if (ctx.activeScene) {
                    asset::AnimatorControllerAsset controller;
                    if (asset::LoadAnimatorControllerAsset(docPath, controller)) {
                        const std::string savedPath =
                            NormalizeAssetPath(docPath);
                        for (auto [sceneAnimator] :
                             ctx.activeScene->View<scene::AnimatorComponent>()) {
                            if (NormalizeAssetPath(sceneAnimator.controllerPath) == savedPath) {
                                asset::ApplyAnimatorControllerAsset(
                                    controller, sceneAnimator);
                                sceneAnimator.loadedControllerPath =
                                    sceneAnimator.controllerPath;
                            }
                        }
                    }
                }
            } else {
                // WHY 失敗を必ず言うか: 以前は else が無く、書き込みに失敗しても
                //     ログもトーストも出なかった。ユーザーからは「押しても保存できない」
                //     としか見えず、原因を絞る手がかりが 1 つも残らない。
                FBZZ_LOG_ERROR("Animator Controller save failed: %s", docPath.c_str());
                Toast::Error("Failed to save " +
                             util::FileSystem::GetFilename(docPath));
            }
        }
        // 未保存であることを常に見せる。Material の "Modified" / "Saved" 表示と同じ規約。
        ImGui::SameLine();
        if (controllerDirty)
            ImGui::TextColored(EditorTheme::Color(ThemeColor::Warning), "Modified");
        else
            ImGui::TextDisabled("Saved");

        // 掴んでいるノード数。Delete の対象と一致する。
        if (m_selectedStateNames.size() > 1) {
            ImGui::SameLine();
            ImGui::TextDisabled("| %zu selected", m_selectedStateNames.size());
        }

        // ドキュメントを閉じてシーン内 Animator の編集モードへ戻る。
        // WHY 必要か: 編集対象を選択から切り離した結果、一度アセットを開くと
        //     GameObject の AnimatorComponent を直接いじる従来モードへ戻れなくなる。
        //     「開く」を明示にした以上、「閉じる」も明示の操作として要る。
        ImGui::SameLine();
        if (ImGui::SmallButton("Close")) {
            if (controllerDirty) {
                // 破棄はしない。閉じる前に保存を促す (誤操作で作業を失わせない)。
                Toast::Warning("Save the controller before closing.");
            } else {
                ctx.animationControllerEditorPath.clear();
                ctx.animationControllerEditor.reset();
                ctx.animationControllerDirty = false;
                ctx.animationGraphSelection.Clear();
            }
        }
        DrawZoomControls();
        ImGui::Separator();
        ImGui::BeginChild("##AnimationGraphRoot", ImVec2(0.0f, 0.0f), false);
        DrawParameterSidebar(ctx, animator);
        ImGui::SameLine();
        ImGui::BeginGroup();
        {
            FBZZ_PROFILE_SCOPE("AnimationGraphPanel::Canvas");
            if (m_openBlendTreeState >= 0)
                DrawBlendTreeCanvas(ctx, animator, docPath);
            else
                DrawNodeCanvas(ctx, animator, docPath);
        }
        ImGui::EndGroup();
        ImGui::EndChild();
        ImGui::EndDisabled();

        auto& selection = ctx.animationGraphSelection;
        selection.Clear();
        selection.assetPath = docPath;
        // BlendTree を開いている間は、Motion ノードの選択を State 選択より優先して公開する。
        // WHY: m_selectedMotion はキャンバス内では更新されていたが、従来はここで捨てられ、
        //      Inspector が親 State のままになっていた。
        if (m_openBlendTreeState >= 0 && m_selectedMotion >= 0) {
            selection.type = EditorContext::AnimationGraphSelection::Type::BlendTreeMotion;
            selection.stateIndex = m_openBlendTreeState;
            selection.motionIndex = m_selectedMotion;
        } else if (m_selectedLink.fromStateIndex == -2) {
            selection.type = EditorContext::AnimationGraphSelection::Type::AnyStateTransition;
            selection.transitionIndex = m_selectedLink.transitionIndex;
        } else if (m_selectedLink.fromStateIndex >= 0) {
            selection.type = EditorContext::AnimationGraphSelection::Type::Transition;
            selection.stateIndex = m_selectedLink.fromStateIndex;
            selection.transitionIndex = m_selectedLink.transitionIndex;
        } else if (m_selectedNode >= 0) {
            selection.type = EditorContext::AnimationGraphSelection::Type::State;
            selection.stateIndex = m_selectedNode;
        } else if (m_selectedAnyState) {
            selection.type = EditorContext::AnimationGraphSelection::Type::AnyState;
        }
        // どのレイヤーのグラフに対する選択かを Inspector へ伝える。
        selection.layerName = m_editingLayer;
        // 複数選択中は「何個掴んでいるか」を Inspector 側でも出せるようにする。
        selection.selectedCount = static_cast<int>(m_selectedStateNames.size());

        // Undo 比較の前にレイヤーグラフを元へ戻す。
        // WHY: 差し替えたままスナップショットを比較すると、Base Layer とレイヤーの
        //      ステートが入れ替わった状態が「変更」として記録されてしまう。
        layerScope.Restore();

        if (allowEditing) {
            TrackAnimationGraphUndo(
                ctx,
                docPath,
                {},
                ctx.animationControllerEditor,
                animator,
                undoBeforeAnimator,
                undoBeforeLayout,
                undoGenerationBefore);
        }
        return;
    }

    // ここから下は「アセットを開いていないとき」の経路で、シーン内 GameObject の
    // AnimatorComponent を直接編集する従来モード。
    scene::GameObject* go = ctx.GetSelectedGO();
    if (!go) {
        ImGui::TextDisabled("No Animator Controller is open.");
        ImGui::Spacing();
        ImGui::TextDisabled("Open a .animcontroller from the Asset Browser (double-click),");
        ImGui::TextDisabled("or select a GameObject that has an AnimatorComponent.");
        return;
    }

    auto* animator = go->GetComponent<scene::AnimatorComponent>();
    if (!animator) {
        ImGui::TextDisabled("Selected GameObject has no AnimatorComponent.");
        return;
    }

    if (m_selectionOwnerInstanceId != go->instanceId) {
        m_selectionOwnerInstanceId = go->instanceId;
        ClearSelectionState();
        m_openBlendTreeStateName.clear();
        m_openBlendTreeStateName.clear();
        m_openBlendTreeState = -1;
        m_selectedMotion = -1;
        m_pendingTransitionKind = NodeKind::None;
        m_pendingTransitionFromName.clear();
        m_pendingTransitionFrom = -1;
        m_renamingStateName.clear();
        m_editingLayer.clear();
        ctx.animationGraphSelection.Clear();
    }

    scene::AnimatorComponent undoBeforeAnimator;
    GraphLayout undoBeforeLayout;
    if (allowEditing) {
        // スナップショットは差し替え前 (Base Layer が入っている状態) で取る。
        undoBeforeAnimator = MakeAnimationGraphSnapshot(*animator);
        undoBeforeLayout = ctx.graphLayouts[go->instanceId];
    }
    const std::uint64_t undoGenerationBefore = AnimationGraphEditGeneration();

    // シーン上の Animator でも、編集対象レイヤーを切り替えられるようにする。
    DrawLayerSelector(ctx, *animator, allowEditing);
    LayerGraphScope layerScope(*animator, m_editingLayer);

    ClearInvalidSelection(*animator);
    ImGui::BeginDisabled(!allowEditing);
    DrawToolbar(ctx, *animator);
    if (m_openBlendTreeState >= 0 &&
        m_openBlendTreeState < static_cast<int>(animator->states.size())) {
        const std::string breadcrumbName =
            animator->states[static_cast<size_t>(m_openBlendTreeState)].name;
        ImGui::SameLine();
        if (ImGui::SmallButton("Base Layer")) {
            m_openBlendTreeStateName.clear();
            m_openBlendTreeState = -1;
            m_selectedMotion = -1;
            m_graphCanvas.ClearSelection();
            m_graphCanvas.ResetView();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("> %s", breadcrumbName.c_str());
    }
    ImGui::Separator();

    ImGui::BeginChild("##AnimationGraphRoot", ImVec2(0.0f, 0.0f), false);
    DrawParameterSidebar(ctx, *animator);
    ImGui::SameLine();
    ImGui::BeginGroup();
    {
        FBZZ_PROFILE_SCOPE("AnimationGraphPanel::Canvas");
        if (m_openBlendTreeState >= 0)
            DrawBlendTreeCanvas(ctx, *animator, go->instanceId);
        else
            DrawNodeCanvas(ctx, *animator, go->instanceId);
    }
    ImGui::EndGroup();
    ImGui::EndChild();
    ImGui::EndDisabled();
    PublishSelection(ctx, *go);
    ctx.animationGraphSelection.layerName = m_editingLayer;

    // Undo 比較の前にレイヤーグラフを元へ戻す。
    layerScope.Restore();

    if (allowEditing) {
        TrackAnimationGraphUndo(
            ctx,
            go->instanceId,
            go->GetID(),
            {},
            *animator,
            undoBeforeAnimator,
            undoBeforeLayout,
            undoGenerationBefore);
    }
}

// 編集対象レイヤーを選ぶツールバー。Base Layer と各 AnimationLayer を切り替える。
// WHY: レイヤーごとのステートマシンは、これが無いとノードグラフから一切触れない。
//      切り替え時は選択状態と BlendTree の掘り下げをリセットする。
//      別グラフのインデックスを持ち越すと、存在しないステートを指したまま描画してしまう。
void AnimationGraphPanel::DrawLayerSelector(
    EditorContext& ctx, scene::AnimatorComponent& animator, bool allowEditing)
{
    const auto resetSelection = [&]() {
        ClearSelectionState();
        m_selectedMotion = -1;
        m_openBlendTreeStateName.clear();
        m_openBlendTreeStateName.clear();
        m_openBlendTreeState = -1;
        m_pendingTransitionKind = NodeKind::None;
        m_pendingTransitionFromName.clear();
        m_pendingTransitionFrom = -1;
        m_renamingStateName.clear();
        m_layerRenameActive = false;
        m_layerRenameFocusPending = false;
        m_layerRenameError.clear();
        m_graphCanvas.ClearSelection();
        ctx.animationGraphSelection.Clear();
    };

    if (!ctx.animationGraphLayerRemoved.empty()) {
        if (m_editingLayer == ctx.animationGraphLayerRemoved) {
            m_editingLayer.clear();
            resetSelection();
        }
        ctx.animationGraphLayerRemoved.clear();
    }
    if (!ctx.animationGraphLayerFocus.empty()) {
        const std::string requestedLayer = ctx.animationGraphLayerFocus;
        ctx.animationGraphLayerFocus.clear();
        if (animator.FindLayer(requestedLayer) != nullptr &&
            m_editingLayer != requestedLayer) {
            m_editingLayer = requestedLayer;
            resetSelection();
        }
    }
    // 指しているレイヤーが消えていたら Base Layer へ戻し、古いレイヤーのステート選択も捨てる。
    // WHY ここでリセットするか: 先に名前だけを空にすると、残った stateIndex が Base Layer
    //      の同じ添字へ誤って適用され、Mask 欄も別レイヤーの内容に見えてしまう。
    if (!m_editingLayer.empty() && animator.FindLayer(m_editingLayer) == nullptr) {
        m_editingLayer.clear();
        resetSelection();
    }

    ImGui::TextDisabled("Layer");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200.0f);
    const char* preview = m_editingLayer.empty() ? "Base Layer" : m_editingLayer.c_str();
    if (ImGui::BeginCombo("##graph_layer", preview)) {
        if (ImGui::Selectable("Base Layer", m_editingLayer.empty())) {
            if (!m_editingLayer.empty()) { m_editingLayer.clear(); resetSelection(); }
        }
        for (const auto& layer : animator.layers) {
            const bool selected = (layer.name == m_editingLayer);
            char label[192];
            std::snprintf(label, sizeof(label), "%s  [%s %.0f%%]%s",
                          layer.name.c_str(),
                          layer.mode == scene::AnimationLayerMode::Additive ? "Additive" : "Override",
                          layer.weight * 100.0f,
                          layer.states.empty() ? "  (no graph)" : "");
            if (ImGui::Selectable(label, selected) && !selected) {
                m_editingLayer = layer.name;
                resetSelection();
            }
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(!allowEditing);
    if (ImGui::SmallButton("+ Layer")) {
        scene::AnimationLayer layer;
        // 名前引き API (SetLayerWeight / PlaySlot / MCP) が壊れるため同名は避ける。
        layer.name = "Layer " + std::to_string(animator.layers.size() + 1);
        for (int suffix = 1; animator.FindLayer(layer.name) != nullptr && suffix < 1000; ++suffix)
            layer.name = "Layer " + std::to_string(animator.layers.size() + 1 + suffix);
        const std::string created = layer.name;
        animator.layers.push_back(std::move(layer));
        m_editingLayer = created;
        resetSelection();
        MarkDirty(ctx);
    }
    if (!m_editingLayer.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("- Layer")) {
            const std::string target = m_editingLayer;
            animator.layers.erase(
                std::remove_if(animator.layers.begin(), animator.layers.end(),
                    [&](const scene::AnimationLayer& l) { return l.name == target; }),
                animator.layers.end());
            m_editingLayer.clear();
            resetSelection();
            MarkDirty(ctx);
        }
    }
    ImGui::EndDisabled();

    // Layer 名は Graph の表示対象そのものなので、入力途中に即時変更すると
    // m_editingLayer が古い名前を指し続けて Base Layer へ誤フォールバックする。
    // そのため入力は専用バッファへ保持し、Apply の瞬間だけ定義へ反映する。
    if (!m_editingLayer.empty()) {
        ImGui::SameLine();
        ImGui::BeginDisabled(!allowEditing);
        if (!m_layerRenameActive) {
            if (ImGui::SmallButton("Rename Layer")) {
                if (const scene::AnimationLayer* layer = animator.FindLayer(m_editingLayer)) {
                    std::snprintf(m_layerRenameBuffer,
                                  sizeof(m_layerRenameBuffer),
                                  "%s",
                                  layer->name.c_str());
                    m_layerRenameActive = true;
                    m_layerRenameFocusPending = true;
                    m_layerRenameError.clear();
                }
            }
        } else {
            ImGui::SetNextItemWidth(180.0f);
            if (m_layerRenameFocusPending) {
                ImGui::SetKeyboardFocusHere();
                m_layerRenameFocusPending = false;
            }
            const bool enterRename = ImGui::InputText(
                "##graph_layer_rename_input",
                m_layerRenameBuffer,
                sizeof(m_layerRenameBuffer),
                ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            const bool applyRename =
                enterRename || ImGui::SmallButton("Apply##graph_layer_rename_apply");
            ImGui::SameLine();
            const bool cancelRename = ImGui::SmallButton("Cancel##graph_layer_rename_cancel");
            if (applyRename) {
                const std::string oldName = m_editingLayer;
                RenameLayer(ctx, animator, oldName, m_layerRenameBuffer);
            }
            if (cancelRename) {
                m_layerRenameActive = false;
                m_layerRenameFocusPending = false;
                m_layerRenameError.clear();
            }
        }
        ImGui::EndDisabled();
        if (!m_layerRenameError.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(EditorTheme::Color(ThemeColor::Danger),
                               "%s",
                               m_layerRenameError.c_str());
        }
    }

    // 選択中レイヤーの要点 (weight / mode / mask) をその場で調整できるようにする。
    if (scene::AnimationLayer* layer = animator.FindLayer(m_editingLayer)) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        // Additive だけ 1.0 より上を許す。差分の倍率なので、クリップの振れ幅が
        // 足りないときの誇張がここで完結する。
        const float weightMax = layer->mode == scene::AnimationLayerMode::Additive
            ? scene::MAX_LAYER_WEIGHT : 1.0f;
        if (ImGui::SliderFloat("##layer_weight", &layer->weight, 0.0f, weightMax, "w %.2f"))
            MarkDirty(ctx);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        static constexpr const char* kModeNames[] = { "Override", "Additive" };
        int modeIndex = static_cast<int>(layer->mode);
        if (ImGui::Combo("##layer_mode", &modeIndex, kModeNames, 2)) {
            layer->mode = static_cast<scene::AnimationLayerMode>(modeIndex);
            if (layer->mode == scene::AnimationLayerMode::Override)
                layer->weight = std::min(layer->weight, 1.0f);
            MarkDirty(ctx);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220.0f);
        if (widgets::AssetPathField("##layer_mask", layer->mask.path, ".mask", ctx.projectRoot)) {
            layer->mask.Invalidate();
            MarkDirty(ctx);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("このレイヤーが効くボーンを決める .mask アセット");
    }

    ImGui::SameLine();
    if (ImGui::SmallButton(m_showComposition ? "Composition v" : "Composition >"))
        m_showComposition = !m_showComposition;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("全レイヤーを重ねた後、ボーンごとに誰が何 %% 持っているかを出す");

    DrawLayerComposition(ctx, animator);
    ImGui::Separator();
}

// レイヤーの色。積み上げバーとオーナー表示で同じ色を使う。
// WHY 固定パレットにするか: レイヤーは 2〜5 本しかなく、色が毎フレーム変わると
//     「どの帯がどのレイヤーか」を目で追えない。並び順で決め打つ。
static ImU32 CompositionLayerColor(std::size_t index, bool additive)
{
    static constexpr ImU32 OVERRIDE_COLORS[] = {
        IM_COL32(104, 186, 255, 255), IM_COL32(126, 226, 158, 255),
        IM_COL32(246, 186, 92, 255),  IM_COL32(214, 140, 255, 255),
        IM_COL32(255, 138, 138, 255),
    };
    static constexpr ImU32 ADDITIVE_COLORS[] = {
        IM_COL32(92, 150, 200, 255),  IM_COL32(100, 178, 128, 255),
        IM_COL32(196, 150, 78, 255),  IM_COL32(170, 116, 200, 255),
        IM_COL32(200, 112, 112, 255),
    };
    const auto& palette = additive ? ADDITIVE_COLORS : OVERRIDE_COLORS;
    constexpr std::size_t count = sizeof(OVERRIDE_COLORS) / sizeof(OVERRIDE_COLORS[0]);
    return palette[index % count];
}

void AnimationGraphPanel::DrawLayerComposition(
    EditorContext& ctx, scene::AnimatorComponent& animator)
{
    if (!m_showComposition) return;

    const asset::Skeleton* skeleton = ResolveCompositionSkeleton(ctx, animator);
    if (!skeleton || skeleton->nodes.empty()) {
        ImGui::TextColored(EditorTheme::Color(ThemeColor::Warning),
            "スケルトンを解決できません。SkinnedMeshRenderer を持つ GameObject を選ぶか、"
            ".mask の Skeleton Source を設定してください。");
        return;
    }

    // マスクの読み込みはここで行われる (編集中は AnimatorSystem が読まないため)。
    const std::vector<maskaudit::LayerInfo> layers = maskaudit::CollectLayers(animator);
    if (layers.empty()) {
        ImGui::TextDisabled("レイヤーがありません。Base Layer が全身を 100%% 動かします。");
        return;
    }

    const std::vector<maskaudit::Issue> issues = maskaudit::Audit(*skeleton, layers);

    // ── 検証結果 ──────────────────────────────────────────────────────────
    int warningCount = 0;
    for (const auto& issue : issues)
        if (issue.severity == maskaudit::Severity::Warning) ++warningCount;

    if (issues.empty()) {
        ImGui::TextColored(EditorTheme::Color(ThemeColor::Success),
                           "レイヤー合成の問題は見つかりませんでした。");
    } else {
        char header[96];
        std::snprintf(header, sizeof(header), "Issues  %d warning / %d info###mask_issues",
                      warningCount, static_cast<int>(issues.size()) - warningCount);
        if (ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen)) {
            for (const auto& issue : issues) {
                const bool warning = issue.severity == maskaudit::Severity::Warning;
                ImGui::TextColored(
                    EditorTheme::Color(warning ? ThemeColor::Warning : ThemeColor::TextMuted),
                    "%s", warning ? "[!]" : "[i]");
                ImGui::SameLine();
                ImGui::TextWrapped("%s : %s", issue.layerName.c_str(), issue.message.c_str());
            }
        }
    }

    // ── 凡例 ──────────────────────────────────────────────────────────────
    // 名前をクリックすると、そのレイヤーのマスクを 3D プレビューへ送る。
    ImDrawList* legendList = ImGui::GetWindowDrawList();
    const auto legendSwatch = [&](ImU32 color, const char* label, const std::string& maskPath) {
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float height = ImGui::GetTextLineHeight();
        legendList->AddRectFilled(pos, { pos.x + height, pos.y + height }, color, 2.0f);
        ImGui::Dummy({ height, height });
        ImGui::SameLine(0.0f, 4.0f);
        if (maskPath.empty()) {
            ImGui::TextDisabled("%s", label);
        } else {
            if (ImGui::SmallButton(label))
                RequestAnimationMaskPreview(maskPath);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip("Animation Mask Preview でこのレイヤーのマスクを見る\n%s",
                                  maskPath.c_str());
        }
        ImGui::SameLine(0.0f, 12.0f);
    };
    legendSwatch(IM_COL32(96, 100, 112, 255), "(base)", {});
    for (std::size_t i = 0; i < layers.size(); ++i) {
        if (layers[i].additive) continue;
        // レイヤー名が空でもボタン ID が衝突しないように添字で分ける。
        ImGui::PushID(static_cast<int>(i));
        legendSwatch(CompositionLayerColor(i, false),
                     layers[i].name.empty() ? "(unnamed)" : layers[i].name.c_str(),
                     layers[i].maskPath);
        ImGui::PopID();
    }
    ImGui::NewLine();

    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputTextWithHint("##composition_filter", "Filter bones",
                             m_compositionFilter, sizeof(m_compositionFilter));
    ImGui::SameLine();
    ImGui::Checkbox("Base が残る骨だけ", &m_compositionIssuesOnly);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("どのレイヤーにも 100%% 所有されていないボーンだけを出す");

    const std::string filter = util::StringUtils::ToLower(m_compositionFilter);

    // ── ボーンごとの持ち分 ────────────────────────────────────────────────
    if (ImGui::BeginTable("##composition", 4,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV,
            ImVec2(0.0f, 220.0f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Bone", ImGuiTableColumnFlags_WidthFixed, 190.0f);
        ImGui::TableSetupColumn("Composition", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Base", ImGuiTableColumnFlags_WidthFixed, 52.0f);
        ImGui::TableSetupColumn("Owner", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableHeadersRow();

        for (int i = 0; i < static_cast<int>(skeleton->nodes.size()); ++i) {
            const auto& node = skeleton->nodes[static_cast<std::size_t>(i)];
            const std::string path = asset::BuildSkeletonNodePath(*skeleton, i);
            const maskaudit::BoneContribution contribution =
                maskaudit::Evaluate(layers, path, node.name);

            if (m_compositionIssuesOnly && contribution.baseShare <= 0.05f) continue;
            if (!filter.empty() &&
                util::StringUtils::ToLower(node.name).find(filter) == std::string::npos)
                continue;

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            // 階層の深さぶんだけ字下げして、どこの骨かを読めるようにする。
            const int depth = static_cast<int>(std::count(path.begin(), path.end(), '/'));
            ImGui::Dummy({ static_cast<float>(std::min(depth, 8)) * 8.0f, 0.0f });
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::TextUnformatted(node.name.c_str());

            ImGui::TableNextColumn();
            const ImVec2 barPos = ImGui::GetCursorScreenPos();
            const float barWidth = (std::max)(ImGui::GetContentRegionAvail().x - 4.0f, 24.0f);
            const float barHeight = ImGui::GetTextLineHeight();
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            float cursorX = barPos.x;
            const auto segment = [&](float ratio, ImU32 color) {
                if (ratio <= 0.001f) return;
                const float width = barWidth * ratio;
                drawList->AddRectFilled({ cursorX, barPos.y },
                                        { cursorX + width, barPos.y + barHeight }, color);
                cursorX += width;
            };
            segment(contribution.baseShare, IM_COL32(96, 100, 112, 255));
            for (std::size_t li = 0; li < layers.size(); ++li)
                segment(contribution.share[li], CompositionLayerColor(li, false));
            drawList->AddRect(barPos, { barPos.x + barWidth, barPos.y + barHeight },
                              IM_COL32(24, 26, 32, 180));
            ImGui::Dummy({ barWidth, barHeight });
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", BuildCompositionTooltip(layers, contribution).c_str());

            ImGui::TableNextColumn();
            // Base が残っているほど強く出す。ここが上半身で 0 でなければ設定が効いていない。
            ImGui::TextColored(contribution.baseShare > 0.05f
                    ? EditorTheme::Color(ThemeColor::Warning)
                    : EditorTheme::Color(ThemeColor::TextMuted),
                "%3.0f%%", contribution.baseShare * 100.0f);

            ImGui::TableNextColumn();
            if (contribution.owner < 0) ImGui::TextDisabled("(base)");
            else ImGui::TextUnformatted(layers[static_cast<std::size_t>(contribution.owner)].name.c_str());
        }
        ImGui::EndTable();
    }
}

void AnimationGraphPanel::DrawToolbar(EditorContext& ctx, scene::AnimatorComponent& animator)
{
    scene::GameObject* go = ctx.GetSelectedGO();
    ImGui::TextUnformatted(go ? go->name.c_str() : "Animation Graph");
    ImGui::SameLine();

    if (m_openBlendTreeState < 0 && ImGui::Button("+ Node")) {
        AddState(ctx, animator, "NewNode");
    }
    ImGui::SameLine();
    if (ImGui::Button("+ Param")) {
        scene::AnimatorParameter param;
        param.name = "NewParam";
        param.type = scene::ParamType::Float;
        animator.parameters.push_back(std::move(param));
        MarkDirty(ctx);
    }
    ImGui::SameLine();
    if (ImGui::Button(animator.playing ? "Pause" : "Play")) {
        animator.playing = !animator.playing;
        MarkDirty(ctx);
    }

    if (!animator.currentStateName.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("Current: %s", animator.currentStateName.c_str());
    }
    if (!animator.blendToState.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("-> %s %.0f%%", animator.blendToState.c_str(), animator.blendWeight * 100.0f);
    }

    DrawZoomControls();
}

void AnimationGraphPanel::DrawZoomControls()
{
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (ImGui::SmallButton("-##ZoomOut")) {
        m_canvasZoom = ClampZoom(m_canvasZoom - ZOOM_STEP);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("1:1##ZoomReset")) {
        m_canvasZoom = 1.0f;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("+##ZoomIn")) {
        m_canvasZoom = ClampZoom(m_canvasZoom + ZOOM_STEP);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%d%%", static_cast<int>(std::round(m_canvasZoom * 100.0f)));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Wheel: Zoom | Shift + Wheel: Horizontal pan | Alt + Wheel: Vertical pan");
}

void AnimationGraphPanel::DrawParameterSidebar(EditorContext& ctx, scene::AnimatorComponent& animator)
{
    ImGui::BeginChild("##AnimationGraphParameters", ImVec2(SIDEBAR_WIDTH, 0.0f), true);
    ImGui::TextUnformatted("Parameters");
    ImGui::Separator();

    static constexpr const char* PARAM_TYPES[] = { "Float", "Int", "Bool", "Trigger" };
    int removeIndex = -1;
    for (int i = 0; i < static_cast<int>(animator.parameters.size()); ++i) {
        auto& param = animator.parameters[static_cast<size_t>(i)];
        ImGui::PushID(i);
        ImGui::BeginGroup();

        int typeIndex = static_cast<int>(param.type);
        ImGui::SetNextItemWidth(86.0f);
        if (ImGui::Combo("##Type", &typeIndex, PARAM_TYPES, 4)) {
            param.type = static_cast<scene::ParamType>(typeIndex);
            MarkDirty(ctx);
        }
        ImGui::SameLine();

        char nameBuffer[96]{};
        std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", param.name.c_str());
        ImGui::SetNextItemWidth(112.0f);
        if (ImGui::InputText("##Name", nameBuffer, sizeof(nameBuffer))) {
            param.name = nameBuffer;
            MarkDirty(ctx);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) removeIndex = i;

        switch (param.type) {
        case scene::ParamType::Float:
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::DragFloat("##Value", &param.floatValue, 0.01f)) MarkDirty(ctx);
            break;
        case scene::ParamType::Int:
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::DragInt("##Value", &param.intValue)) MarkDirty(ctx);
            break;
        case scene::ParamType::Bool:
            if (ImGui::Checkbox("Value", &param.boolValue)) MarkDirty(ctx);
            break;
        case scene::ParamType::Trigger:
            // Trigger は Draw/Holster/Jump など実行時入力からのみ発火させる。
            // WHY 手動 Fire を置かないか: ここで true にすると、その値が Controller へ
            //      保存され、再ロード時に「起動直後から遷移する」「別の遷移まで巻き込む」
            //      という非決定的な状態になる。Graph は定義編集、Trigger はランタイム入力
            //      という責務分離にする。
            ImGui::TextDisabled("Runtime Trigger");
            break;
        }

        ImGui::TextDisabled("%s", ParamTypeName(param.type));
        ImGui::EndGroup();
        ImGui::Separator();
        ImGui::PopID();
    }

    if (removeIndex >= 0) {
        animator.parameters.erase(animator.parameters.begin() + removeIndex);
        MarkDirty(ctx);
    }

    if (ImGui::Button("+ Add Parameter", ImVec2(-1.0f, 0.0f))) {
        scene::AnimatorParameter param;
        param.name = "NewParam";
        animator.parameters.push_back(std::move(param));
        MarkDirty(ctx);
    }
    ImGui::EndChild();
}


// 選択中ステートの名前を編集する欄。
//
// WHY Inspector にも置くか: 従来のリネーム導線はグラフ上の F2 と右クリックメニューだけで、
//     Inspector を見ているあいだは名前を変えられること自体に気付けなかった。
//     Unity と同じく「選択したノードの名前は Inspector の一番上で変えられる」に揃える。
//
// WHY 1 文字ごとに反映しないか: 名前は遷移 (toStateName)・defaultStateName・
//     ノード配置マップのキーそのもの。打鍵のたびに改名すると、"Idle" → "Idl" → "Id"…と
//     中間状態で参照を張り替え続けることになり、Undo 履歴もその分だけ刻まれる。
//     Enter またはフォーカスを外したときにだけ確定する。
// ownerKey: ctx.graphLayouts のキー (GameObject なら instanceId、アセットなら .animcontroller パス)。
static void DrawStateNameField(EditorContext& ctx,
                               scene::AnimatorComponent& animator,
                               scene::AnimationState& state,
                               int stateIndex,
                               const std::string& ownerKey,
                               const std::string& layerName)
{
    // 編集中テキストは「確定するまで state.name と食い違う」ため、フレームを跨いで持つ。
    // 対象が変わったら (別ステート選択・別グラフ・改名確定後) 必ず貼り直す。
    static std::string editingKey;
    static std::string editError;
    static char        nameBuffer[128]{};

    const std::string key = ownerKey + "|" + layerName + "|" + state.name;
    if (editingKey != key) {
        editingKey = key;
        editError.clear();
        std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", state.name.c_str());
    }

    const bool allowEditing = CanEditAnimationGraph(ctx);
    ImGui::BeginDisabled(!allowEditing);
    ImGui::SetNextItemWidth(-1.0f);
    // EnterReturnsTrue と IsItemDeactivatedAfterEdit の両方を確定条件にする。
    // WHY 両方か: Enter を押さずに他の欄へ移る操作が普通にあるため、
    //     Enter だけだと「打ったのに変わらない」状態でフォーカスが外れる。
    // WHY AutoSelectAll を付けないか: 常設の欄なので、クリックのたびに全選択されると
    //     語尾だけ直す操作 (Idle → Idle_Loop) ができない。全消しは Ctrl+A で足りる。
    const bool submitted = ImGui::InputText(
        "Name##state_name", nameBuffer, sizeof(nameBuffer),
        ImGuiInputTextFlags_EnterReturnsTrue);
    const bool committed = submitted || ImGui::IsItemDeactivatedAfterEdit();
    ImGui::EndDisabled();

    // 弾いた理由は必ず出す。無言で元の名前へ戻ると原因が何も残らない。
    if (!editError.empty())
        ImGui::TextColored(EditorTheme::Color(ThemeColor::Danger), "%s", editError.c_str());

    if (!committed || !allowEditing) return;

    const std::string oldName = state.name;
    const std::string newName = nameBuffer;
    if (newName == oldName) { editError.clear(); return; }

    if (newName.empty()) {
        editError = "Name cannot be empty.";
        return;
    }
    for (int i = 0; i < static_cast<int>(animator.states.size()); ++i) {
        if (i != stateIndex && animator.states[static_cast<std::size_t>(i)].name == newName) {
            editError = "A state named '" + newName + "' already exists.";
            return;
        }
    }

    if (RenameStateInGraph(ctx, animator, stateIndex, oldName, newName, ownerKey))
        editError.clear();
}

static bool DrawAnimationGraphDetails(EditorContext& ctx,
                                       scene::AnimatorComponent& animator,
                                       const std::string& ownerKey)
{
    auto& selection = ctx.animationGraphSelection;
    if (selection.type == EditorContext::AnimationGraphSelection::Type::None)
        return false;

    const bool isTransitionSelection =
        selection.type == EditorContext::AnimationGraphSelection::Type::Transition ||
        selection.type ==
            EditorContext::AnimationGraphSelection::Type::AnyStateTransition;
    ImGui::TextUnformatted(
        isTransitionSelection ? "Transition Inspector" : "Animation Details");
    // 複数選択中であることを必ず出す。
    // WHY: 編集できるのはプライマリ 1 件だけだが Delete は選択全部に効くため、
    //      件数を隠すと「1 個選んだつもりで複数消えた」ように見える。
    if (selection.type == EditorContext::AnimationGraphSelection::Type::State &&
        selection.selectedCount > 1) {
        ImGui::TextColored(EditorTheme::Color(ThemeColor::Warning),
                           "%d states selected - editing the primary one",
                           selection.selectedCount);
    }
    ImGui::Separator();

    if (selection.type == EditorContext::AnimationGraphSelection::Type::BlendTreeMotion) {
        if (selection.stateIndex < 0 ||
            selection.stateIndex >= static_cast<int>(animator.states.size())) {
            selection.Clear();
            return false;
        }

        auto& state = animator.states[static_cast<size_t>(selection.stateIndex)];
        const bool is1D = state.mode == scene::AnimationStateMode::BlendTree1D;
        const bool is2D = state.mode == scene::AnimationStateMode::BlendTree2D;
        if ((!is1D && !is2D) || selection.motionIndex < 0) {
            selection.Clear();
            return false;
        }

        auto& motions = is1D ? state.blendTree1D.motions : state.blendTree2D.motions;
        if (selection.motionIndex >= static_cast<int>(motions.size())) {
            selection.Clear();
            return false;
        }

        ImGui::Text("Motion %d", selection.motionIndex + 1);
        ImGui::TextDisabled("State: %s", state.name.c_str());
        ImGui::SeparatorText(is1D ? "Blend Tree 1D Motion" : "Blend Tree 2D Motion");

        auto& motion = motions[static_cast<size_t>(selection.motionIndex)];
        DrawAnimationSource(ctx, "Source##motion", animator, motion.sourcePath);
        DrawClipCombo(
            ctx,
            "Clip##motion",
            animator,
            motion.sourcePath,
            motion.clipName,
            motion.clipIndex);

        if (is1D) {
            ImGui::SetNextItemWidth(140.0f);
            if (ImGui::DragFloat("Threshold##motion", &motion.threshold, 0.01f))
                MarkDirty(ctx);
        } else {
            ImGui::SetNextItemWidth(95.0f);
            if (ImGui::DragFloat("X##motion", &motion.posX, 0.01f))
                MarkDirty(ctx);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(95.0f);
            if (ImGui::DragFloat("Y##motion", &motion.posY, 0.01f))
                MarkDirty(ctx);
        }

        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::DragFloat("Motion Speed##motion", &motion.speed, 0.01f, -10.0f, 10.0f))
            MarkDirty(ctx);
        if (ImGui::DragFloat("Motion IK##motion", &motion.ikWeight, 0.01f, 0.0f, 1.0f))
            MarkDirty(ctx);
        return true;
    }

    if (selection.type == EditorContext::AnimationGraphSelection::Type::State) {
        if (selection.stateIndex < 0 ||
            selection.stateIndex >= static_cast<int>(animator.states.size())) {
            selection.Clear();
            return false;
        }
        auto& state = animator.states[static_cast<size_t>(selection.stateIndex)];
        ImGui::TextUnformatted("Animation Node");
        ImGui::TextDisabled("State Node");
        DrawStateNameField(
            ctx, animator, state, selection.stateIndex, ownerKey, selection.layerName);
        DrawBlendTreeEditor(ctx, animator, state);
        if (state.mode == scene::AnimationStateMode::Clip && state.sourcePath.empty()) {
            ImGui::TextColored(EditorTheme::Color(ThemeColor::Warning),
                               "Empty Node - attach an .anim in Animation (.anim).");
        }
        ImGui::SeparatorText("State Settings");
        if (ImGui::DragFloat("IK Weight##det", &state.ikWeight, 0.01f, 0.0f, 1.0f))
            MarkDirty(ctx);
        if (ImGui::DragFloat("Speed##det", &state.speed, 0.01f, -10.0f, 10.0f))
            MarkDirty(ctx);
        if (ImGui::Checkbox("Loop##det", &state.loop))
            MarkDirty(ctx);
        // クリップ側の Loop Time と食い違っているときだけ、その旨を出す。
        //
        // WHY 出すか: 実行時の権威は State (設計 A) なので、FBX で Loop Time を入れても
        //     既存ステートは自動では変わらない。黙って食い違うと
        //     「FBX でループにしたのにループしない」の原因が見えなくなる。
        if (state.mode == scene::AnimationStateMode::Clip) {
            if (const asset::AnimationClip* clip =
                    FindClip(animator, state.sourcePath, state.clipName, state.clipIndex)) {
                if (clip->loop != state.loop) {
                    ImGui::TextColored(EditorTheme::Color(ThemeColor::Warning),
                                       "Clip Loop Time is %s",
                                       clip->loop ? "ON" : "OFF");
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Use Clip")) {
                        state.loop = clip->loop;
                        MarkDirty(ctx);
                    }
                }
            }
        }
        return true;
    }

    if (selection.type == EditorContext::AnimationGraphSelection::Type::AnyState) {
        ImGui::TextDisabled(
            "Any State transitions are created by dragging From to a state.");
        return true;
    }

    if (selection.type == EditorContext::AnimationGraphSelection::Type::AnyStateTransition) {
        if (selection.transitionIndex < 0 ||
            selection.transitionIndex >= static_cast<int>(animator.anyStateTransitions.size())) {
            selection.Clear();
            return false;
        }
        ImGui::TextUnformatted("Any State Transition");
        DrawTransitionEditor(
            ctx,
            animator,
            nullptr,
            animator.anyStateTransitions[static_cast<size_t>(selection.transitionIndex)]);
        return true;
    }

    if (selection.type != EditorContext::AnimationGraphSelection::Type::Transition ||
        selection.stateIndex < 0 ||
        selection.stateIndex >= static_cast<int>(animator.states.size())) {
        selection.Clear();
        return false;
    }

    auto& state = animator.states[static_cast<size_t>(selection.stateIndex)];
    if (selection.transitionIndex < 0 ||
        selection.transitionIndex >= static_cast<int>(state.transitions.size())) {
        selection.Clear();
        return false;
    }

    auto& transition = state.transitions[static_cast<size_t>(selection.transitionIndex)];
    ImGui::Text("%s -> %s", state.name.c_str(), transition.toStateName.c_str());
    DrawTransitionEditor(ctx, animator, &state, transition);
    return true;
}

bool DrawAnimationGraphInspector(EditorContext& ctx, scene::GameObject& gameObject)
{
    auto& selection = ctx.animationGraphSelection;
    if (selection.entityId != gameObject.GetID()) return false;
    auto* animator = gameObject.GetComponent<scene::AnimatorComponent>();
    if (animator == nullptr) return false;
    // selection.stateIndex はグラフごとの添字。パネルと同じレイヤーへ差し替えて解決する。
    // WHY: 差し替えずに描くと、上半身レイヤーのステートを選んだのに
    //      Base Layer の同じ添字のステートを編集してしまう。
    LayerGraphScope layerScope(*animator, selection.layerName);
    // ownerKey は ctx.graphLayouts のキー。パネルが DrawNodeCanvas へ渡すものと同じにする。
    return DrawAnimationGraphDetails(ctx, *animator, gameObject.instanceId);
}

bool DrawAnimationGraphAssetInspector(EditorContext& ctx)
{
    if (!ctx.animationControllerEditor ||
        ctx.animationGraphSelection.assetPath != ctx.animationControllerEditorPath)
        return false;
    LayerGraphScope layerScope(*ctx.animationControllerEditor,
                               ctx.animationGraphSelection.layerName);
    return DrawAnimationGraphDetails(
        ctx, *ctx.animationControllerEditor, ctx.animationControllerEditorPath);
}

static void DrawTransitionEditor(EditorContext& ctx,
                                 scene::AnimatorComponent& animator,
                                 const scene::AnimationState* sourceState,
                                 scene::AnimationTransition& transition)
{
    std::vector<const char*> stateNames;
    stateNames.reserve(animator.states.size());
    int toIndex = 0;
    for (int i = 0; i < static_cast<int>(animator.states.size()); ++i) {
        stateNames.push_back(animator.states[static_cast<size_t>(i)].name.c_str());
        if (animator.states[static_cast<size_t>(i)].name == transition.toStateName) toIndex = i;
    }

    ImGui::SeparatorText("Transition");
    ImGui::SetNextItemWidth(180.0f);
    if (!stateNames.empty() &&
        ImGui::Combo("To State", &toIndex, stateNames.data(), static_cast<int>(stateNames.size()))) {
        transition.toStateName = stateNames[static_cast<size_t>(toIndex)];
        MarkDirty(ctx);
    }

    const scene::AnimationState* destinationState = nullptr;
    if (toIndex >= 0 && toIndex < static_cast<int>(animator.states.size()))
        destinationState = &animator.states[static_cast<size_t>(toIndex)];
    const float sourceLength = GetStateClipLength(animator, sourceState);
    const float destinationLength =
        GetStateClipLength(animator, destinationState);

    DrawTransitionTimeline(
        animator,
        sourceState,
        destinationState,
        transition,
        sourceLength,
        destinationLength);

    ImGui::SeparatorText("Settings");
    if (ImGui::Checkbox("Has Exit Time", &transition.hasExitTime)) MarkDirty(ctx);
    ImGui::BeginDisabled(!transition.hasExitTime);
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::DragFloat(
            "Exit Time", &transition.exitTime, 0.01f, 0.0f, 1.0f, "%.3f"))
        MarkDirty(ctx);
    ImGui::EndDisabled();

    if (sourceLength > 0.0f)
        ImGui::TextDisabled("Source Clip Length: %.3f s", sourceLength);
    else
        ImGui::TextDisabled("Source Clip Length: Blend Tree / unavailable");
    if (destinationLength > 0.0f)
        ImGui::TextDisabled(
            "Destination Clip Length: %.3f s", destinationLength);
    else
        ImGui::TextDisabled(
            "Destination Clip Length: Blend Tree / unavailable");

    if (ImGui::Checkbox("Fixed Duration", &transition.fixedDuration))
        MarkDirty(ctx);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Enabled: seconds\nDisabled: normalized fraction of the source Clip Length");
    if (!sourceState && !transition.fixedDuration) {
        ImGui::TextColored(
            ImVec4(1.0f, 0.68f, 0.25f, 1.0f),
            "Any State has no source Length. Use Fixed Duration.");
    }

    ImGui::SetNextItemWidth(140.0f);
    if (transition.fixedDuration) {
        if (ImGui::DragFloat(
                "Transition Duration (s)",
                &transition.transitionDuration, 0.01f, 0.0f, 5.0f))
            MarkDirty(ctx);
    } else {
        if (ImGui::DragFloat(
                "Transition Duration",
                &transition.transitionDuration, 0.01f, 0.0f, 2.0f))
            MarkDirty(ctx);
        const float effectiveSeconds =
            sourceLength > 0.0f
                ? sourceLength * transition.transitionDuration
                : 0.0f;
        if (sourceLength > 0.0f)
            ImGui::TextDisabled(
                "Normalized: %.1f%% | Effective Blend: %.3f s",
                transition.transitionDuration * 100.0f,
                effectiveSeconds);
        else
            ImGui::TextDisabled(
                "Normalized: %.1f%% | Effective Blend unavailable",
                transition.transitionDuration * 100.0f);
    }

    ImGui::SeparatorText("Conditions");
    static constexpr const char* OP_NAMES[] = { "Greater", "Less", "Equal", "NotEqual", "True", "False" };

    int removeIndex = -1;
    for (int i = 0; i < static_cast<int>(transition.conditions.size()); ++i) {
        auto& condition = transition.conditions[static_cast<size_t>(i)];
        ImGui::PushID(i);

        std::vector<const char*> paramNames;
        paramNames.reserve(animator.parameters.size());
        int paramIndex = 0;
        for (int pi = 0; pi < static_cast<int>(animator.parameters.size()); ++pi) {
            paramNames.push_back(animator.parameters[static_cast<size_t>(pi)].name.c_str());
            if (animator.parameters[static_cast<size_t>(pi)].name == condition.paramName) paramIndex = pi;
        }

        ImGui::SetNextItemWidth(150.0f);
        if (!paramNames.empty() &&
            ImGui::Combo("##Param", &paramIndex, paramNames.data(), static_cast<int>(paramNames.size()))) {
            condition.paramName = paramNames[static_cast<size_t>(paramIndex)];
            MarkDirty(ctx);
        } else if (paramNames.empty()) {
            ImGui::TextDisabled("No parameter");
        }
        ImGui::SameLine();

        int opIndex = static_cast<int>(condition.op);
        ImGui::SetNextItemWidth(110.0f);
        if (ImGui::Combo("##Op", &opIndex, OP_NAMES, 6)) {
            condition.op = static_cast<scene::ConditionOp>(opIndex);
            MarkDirty(ctx);
        }
        ImGui::SameLine();

        if (IsFloatCondition(condition.op)) {
            ImGui::SetNextItemWidth(90.0f);
            if (ImGui::DragFloat("##Threshold", &condition.threshold, 0.01f)) MarkDirty(ctx);
            ImGui::SameLine();
        } else {
            ImGui::TextDisabled("%s", ConditionOpName(condition.op));
            ImGui::SameLine();
        }

        if (ImGui::SmallButton("x")) removeIndex = i;
        ImGui::PopID();
    }

    if (removeIndex >= 0) {
        transition.conditions.erase(transition.conditions.begin() + removeIndex);
        MarkDirty(ctx);
    }

    if (ImGui::Button("+ Condition")) {
        scene::AnimatorCondition condition;
        if (!animator.parameters.empty()) condition.paramName = animator.parameters.front().name;
        transition.conditions.push_back(std::move(condition));
        MarkDirty(ctx);
    }
}

void AnimationGraphPanel::PublishSelection(EditorContext& ctx,
                                           const scene::GameObject& gameObject) const
{
    auto& selection = ctx.animationGraphSelection;
    selection.Clear();
    selection.entityId = gameObject.GetID();

    // BlendTree を開いているときは Motion 選択を Inspector へ公開する。
    if (m_openBlendTreeState >= 0 && m_selectedMotion >= 0) {
        selection.type = EditorContext::AnimationGraphSelection::Type::BlendTreeMotion;
        selection.stateIndex = m_openBlendTreeState;
        selection.motionIndex = m_selectedMotion;
    } else if (m_selectedLink.fromStateIndex == -2) {
        selection.type = EditorContext::AnimationGraphSelection::Type::AnyStateTransition;
        selection.transitionIndex = m_selectedLink.transitionIndex;
    } else if (m_selectedLink.fromStateIndex >= 0) {
        selection.type = EditorContext::AnimationGraphSelection::Type::Transition;
        selection.stateIndex = m_selectedLink.fromStateIndex;
        selection.transitionIndex = m_selectedLink.transitionIndex;
    } else if (m_selectedNode >= 0) {
        selection.type = EditorContext::AnimationGraphSelection::Type::State;
        selection.stateIndex = m_selectedNode;
    } else if (m_selectedAnyState) {
        selection.type = EditorContext::AnimationGraphSelection::Type::AnyState;
    }
}

void AnimationGraphPanel::AddState(EditorContext& ctx, scene::AnimatorComponent& animator, const char* baseName)
{
    scene::AnimationState state;
    state.name = MakeUniqueStateName(animator, baseName);
    if (animator.defaultStateName.empty()) animator.defaultStateName = state.name;
    // 空 Node はアニメーションを自動割り当てしない。
    // WHY: Graph 上の既存クリップ一覧から先頭を勝手に選ぶと、作成した Node が
    //      意図しない .anim を再生し、Inspector で設定する前に意味を持ってしまう。
    //      Unity と同じく、Node の作成と Motion の割り当てを分離する。
    state.sourcePath.clear();
    state.clipName.clear();
    state.clipIndex = -1;
    animator.states.push_back(std::move(state));
    SelectStateByName(animator, animator.states.back().name);
    MarkDirty(ctx);
}

void AnimationGraphPanel::AddStateAt(EditorContext& ctx,
                                     scene::AnimatorComponent& animator,
                                     const char* baseName,
                                     const std::string& instanceId,
                                     float spawnX,
                                     float spawnY)
{
    scene::AnimationState state;
    state.name = MakeUniqueStateName(animator, baseName);
    if (animator.defaultStateName.empty()) animator.defaultStateName = state.name;
    // 右クリック作成もツールバー作成と同じ空 Node にする。
    state.sourcePath.clear();
    state.clipName.clear();
    state.clipIndex = -1;
    // 生成前に位置を確定しておくことで、デフォルトのグリッド整列配置を経由せず即カーソル位置へ出す。
    ctx.graphLayouts[instanceId].nodePositions[state.name] = ImVec2(spawnX, spawnY);
    const std::string createdName = state.name;
    animator.states.push_back(std::move(state));
    // 作った直後はそれだけを選択し、キャンバスのハイライトも合わせる。
    SelectStateByName(animator, createdName);
    MarkDirty(ctx);
}

void AnimationGraphPanel::DuplicateState(EditorContext& ctx,
                                         scene::AnimatorComponent& animator,
                                         int stateIndex,
                                         const std::string& instanceId)
{
    if (stateIndex < 0 || stateIndex >= static_cast<int>(animator.states.size())) return;
    const auto& source = animator.states[static_cast<size_t>(stateIndex)];
    scene::AnimationState copied = source;
    copied.name = MakeUniqueStateName(animator, (source.name + "_Copy").c_str());

    // WHY: AutoLayout で全ノードを並べ直すと手作業のレイアウトが失われる。
    //      Unity と同じく複製元の右下へずらして置くだけに留める。
    auto& positions = ctx.graphLayouts[instanceId].nodePositions;
    ImVec2 spawn(80.0f, 80.0f);
    if (const auto it = positions.find(source.name); it != positions.end())
        spawn = ImVec2(it->second.x + 44.0f, it->second.y + 44.0f);
    positions[copied.name] = spawn;

    const std::string copiedName = copied.name;
    animator.states.push_back(std::move(copied));
    SelectStateByName(animator, copiedName);
    MarkDirty(ctx);
}

void AnimationGraphPanel::AddTransition(EditorContext& ctx,
                                        scene::AnimatorComponent& animator,
                                        int fromStateIndex,
                                        int toStateIndex)
{
    if (fromStateIndex < 0 || fromStateIndex >= static_cast<int>(animator.states.size())) return;
    if (toStateIndex < 0 || toStateIndex >= static_cast<int>(animator.states.size())) return;

    auto& fromState = animator.states[static_cast<size_t>(fromStateIndex)];
    const auto& toState = animator.states[static_cast<size_t>(toStateIndex)];
    for (const auto& transition : fromState.transitions) {
        if (transition.toStateName == toState.name) return;
    }

    scene::AnimationTransition transition;
    transition.toStateName = toState.name;
    fromState.transitions.push_back(std::move(transition));
    // 作った遷移を選択状態にする。起点は名前で覚える (添字は次フレームに解決される)。
    m_selectedStateNames.clear();
    m_selectedKind = NodeKind::None;
    m_selectedLink = { fromStateIndex, static_cast<int>(fromState.transitions.size()) - 1 };
    m_selectedLinkKind = NodeKind::State;
    m_selectedLinkFromName = fromState.name;
    m_graphCanvas.ClearSelection();
    MarkDirty(ctx);
}

void AnimationGraphPanel::AutoLayoutStates(EditorContext& ctx,
                                           scene::AnimatorComponent& animator,
                                           const std::string& instanceId)
{
    // 整列そのものは共有実装 (AnimatorGraphOps) が持つ。
    // WHY: ここに書いたままだと AI 側の animation.auto_layout が別実装になり、
    //      間隔がわずかに違うだけで「AI が整列したグラフを Editor で整列し直すと
    //      座標が動く」= 差分に意味のない座標変更が毎回混ざる状態になる。
    AutoLayoutAnimatorStates(ctx.graphLayouts[instanceId], animator);
    MarkDirty(ctx);
}

void AnimationGraphPanel::DeleteState(EditorContext& ctx,
                                      scene::AnimatorComponent& animator,
                                      int stateIndex,
                                      const std::string& instanceId)
{
    if (stateIndex < 0 || stateIndex >= static_cast<int>(animator.states.size())) return;
    const std::string removedName = animator.states[static_cast<size_t>(stateIndex)].name;
    animator.states.erase(animator.states.begin() + stateIndex);
    ctx.graphLayouts[instanceId].nodePositions.erase(removedName);
    ctx.graphLayouts[instanceId].blendTreeMotionPositions.erase(removedName);

    for (auto& state : animator.states) {
        state.transitions.erase(
            std::remove_if(state.transitions.begin(), state.transitions.end(),
                [&](const scene::AnimationTransition& transition) {
                    return transition.toStateName == removedName;
                }),
            state.transitions.end());
    }
    animator.anyStateTransitions.erase(
        std::remove_if(animator.anyStateTransitions.begin(),
                       animator.anyStateTransitions.end(),
            [&](const scene::AnimationTransition& transition) {
                return transition.toStateName == removedName;
            }),
        animator.anyStateTransitions.end());

    if (animator.defaultStateName == removedName) {
        animator.defaultStateName = animator.states.empty() ? std::string{} : animator.states.front().name;
    }
    if (animator.currentStateName == removedName) animator.currentStateName.clear();
    if (animator.blendToState == removedName) animator.blendToState.clear();
    m_selectedLink = {};
    m_selectedLinkKind = NodeKind::None;
    m_selectedLinkFromName.clear();
    MarkDirty(ctx);
}

void AnimationGraphPanel::RenameState(EditorContext& ctx,
                                      scene::AnimatorComponent& animator,
                                      int stateIndex,
                                      const std::string& oldName,
                                      const std::string& newName,
                                      const std::string& instanceId)
{
    if (!RenameStateInGraph(ctx, animator, stateIndex, oldName, newName, instanceId))
        return;

    // 自分で起こした改名なので、パネル側の追従はここで済ませる。
    // 通知を残したままにすると次フレームに二重適用され (旧名はもう無いので実害は無いが)、
    // 「誰が改名したのか」が曖昧になるため、その場で消費しておく。
    AdoptStateRename(oldName, newName);
    ctx.animationGraphRenamedFrom.clear();
    ctx.animationGraphRenamedTo.clear();
}

void AnimationGraphPanel::RenameLayer(EditorContext& ctx,
                                      scene::AnimatorComponent& animator,
                                      const std::string& oldName,
                                      const std::string& newName)
{
    m_layerRenameError.clear();
    if (oldName.empty() || animator.FindLayer(oldName) == nullptr) {
        m_layerRenameError = "Layer が見つかりません。";
        return;
    }
    if (newName.empty()) {
        m_layerRenameError = "Layer 名を空にできません。";
        return;
    }
    if (oldName == newName) {
        m_layerRenameActive = false;
        m_layerRenameFocusPending = false;
        return;
    }
    if (std::any_of(animator.layers.begin(), animator.layers.end(),
                    [&](const scene::AnimationLayer& layer) {
                        return layer.name == newName;
                    })) {
        m_layerRenameError = "同名の Layer が既に存在します。";
        return;
    }

    animator.FindLayer(oldName)->name = newName;
    if (m_editingLayer == oldName) m_editingLayer = newName;
    if (ctx.animationGraphSelection.layerName == oldName)
        ctx.animationGraphSelection.layerName = newName;
    ctx.animationGraphLayerFocus = newName;
    m_layerRenameActive = false;
    m_layerRenameFocusPending = false;
    MarkDirty(ctx);
}

// 名前が選択の権威なので、リネームしたら選択側も追従させる。
// これを忘れると次の ResolveSelectionIndices で「消えたステート」と判定され、
// リネーム直後に選択が外れる。
// WHY 独立させたか: Inspector の Name 欄からの改名もこの追従を必要とするため、
//     パネル外で起きた改名を取り込む入口としても使う。
void AnimationGraphPanel::AdoptStateRename(const std::string& oldName,
                                            const std::string& newName)
{
    if (oldName.empty() || newName.empty() || oldName == newName) return;

    for (std::string& selected : m_selectedStateNames)
        if (selected == oldName) selected = newName;
    if (m_openBlendTreeStateName == oldName)    m_openBlendTreeStateName = newName;
    if (m_selectedLinkFromName == oldName)      m_selectedLinkFromName = newName;
    if (m_pendingTransitionFromName == oldName) m_pendingTransitionFromName = newName;
    if (m_renamingStateName == oldName)         m_renamingStateName = newName;
}

void AnimationGraphPanel::AdoptLayerRename(const std::string& oldName,
                                           const std::string& newName)
{
    if (oldName.empty() || newName.empty() || oldName == newName) return;
    if (m_editingLayer == oldName) m_editingLayer = newName;
    m_layerRenameActive = false;
    m_layerRenameFocusPending = false;
    m_layerRenameError.clear();
}

void AnimationGraphPanel::ClearInvalidSelection(const scene::AnimatorComponent& animator)
{
    if (m_selectedLink.fromStateIndex == -2) {
        if (m_selectedLink.transitionIndex < 0 ||
            m_selectedLink.transitionIndex >=
                static_cast<int>(animator.anyStateTransitions.size())) {
            m_selectedLink = {};
            m_selectedLinkKind = NodeKind::None;
            m_selectedLinkFromName.clear();
        }
        return;
    }
    if (m_selectedLink.fromStateIndex < 0) return;
    if (m_selectedLink.fromStateIndex >= static_cast<int>(animator.states.size())) {
        m_selectedLink = {};
        m_selectedLinkKind = NodeKind::None;
        m_selectedLinkFromName.clear();
        return;
    }
    const auto& transitions = animator.states[static_cast<size_t>(m_selectedLink.fromStateIndex)].transitions;
    if (m_selectedLink.transitionIndex < 0 ||
        m_selectedLink.transitionIndex >= static_cast<int>(transitions.size())) {
        m_selectedLink = {};
        m_selectedLinkKind = NodeKind::None;
        m_selectedLinkFromName.clear();
    }
}

AnimationGraphPanel::LinkRef AnimationGraphPanel::ResolveLink(int linkId,
                                                              const scene::AnimatorComponent& animator) const
{
    if (linkId == EntryLinkId()) return { -3, 0 };
    if ((linkId & 0x70000000) == 0x70000000) {
        const int transitionIndex = linkId & 0x0fffffff;
        if (transitionIndex >= 0 &&
            transitionIndex < static_cast<int>(animator.anyStateTransitions.size()))
            return { -2, transitionIndex };
        return {};
    }
    const int from = linkId >> 16;
    const int transitionIndex = linkId & 0xffff;
    if (from < 0 || from >= static_cast<int>(animator.states.size())) return {};

    const auto& transitions = animator.states[static_cast<size_t>(from)].transitions;
    if (transitionIndex < 0 || transitionIndex >= static_cast<int>(transitions.size())) return {};
    return { from, transitionIndex };
}

} // namespace fbzz::editor
