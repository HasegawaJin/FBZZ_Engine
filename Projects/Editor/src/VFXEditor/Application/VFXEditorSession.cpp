// FBZZ Engine
// VFXEditorSession.cpp | fbzz::editor
// VFX 編集セッション (ドキュメント・サービス・共有状態) の実装
#include <Editor/VFXEditor/Application/VFXEditorSession.hpp>

#include <Editor/EditorContext.hpp>
#include <Engine/Asset/VFXGraphAsset.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/VFXGraphComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <Editor/VFXEditor/Document/VFXGraphOps.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <toml++/toml.hpp>
#include <sstream>

namespace fbzz::editor {

// 共有ヘルパー (ノード色・アイコン・パラメーター編集など) は fbzz::editor::vfx に
// 隔離してある。Editor 全体の名前空間を汚さないための境界。
using namespace vfx;

void VFXEditorSession::LoadEditorPreferences(const EditorContext& ctx)
{
    if (ctx.projectRoot.empty()) return;
    const std::filesystem::path directory = util::FileSystem::PathFromUtf8(ctx.projectRoot)
        / L"Assets" / L"EditorConfig";
    (void)util::FileSystem::EnsureDirectory(directory);
    preferencesPath = util::FileSystem::PathToUtf8(directory / L"vfx_editor_settings.toml");

    std::string text;
    if (!util::FileSystem::ReadText(preferencesPath, text)) return;
    const toml::parse_result parsed = toml::parse(text);
    if (!parsed) return;
    showGraphGrid = parsed["view"]["graphGrid"].value_or(showGraphGrid);
    preview.showFloorGrid = parsed["view"]["previewFloorGrid"].value_or(preview.showFloorGrid);
    preview.showGizmos = parsed["view"]["previewVFXGizmos"].value_or(preview.showGizmos);
    preview.includeModelsInOverdraw =
        parsed["view"]["overdrawIncludeModels"].value_or(preview.includeModelsInOverdraw);
    preview.actorModelPath = parsed["previewActor"]["model"].value_or(std::string{});
    preview.actorControllerPath = parsed["previewActor"]["controller"].value_or(std::string{});
    preview.actorClipPath = parsed["previewActor"]["clip"].value_or(std::string{});
    preview.actorMaterialPath = parsed["previewActor"]["material"].value_or(std::string{});
    preview.actorState = parsed["previewActor"]["state"].value_or(std::string{});
    preview.selectedBone = parsed["previewActor"]["bone"].value_or(std::string{});
    preview.attachGraphToBone = parsed["previewActor"]["attachToBone"].value_or(false);
    preview.syncActorToVFX = parsed["previewActor"]["syncToVFX"].value_or(true);
    preview.restorePending = !preview.actorModelPath.empty();
    showMiniMap = parsed["view"]["miniMap"].value_or(showMiniMap);
    graphZoom = std::clamp(
        static_cast<float>(parsed["view"]["graphZoom"].value_or(static_cast<double>(graphZoom))),
        0.45f, 1.80f);
    showTimeline = parsed["view"]["timeline"].value_or(showTimeline);
    timelineSnap = parsed["editing"]["timelineSnap"].value_or(timelineSnap);
    timelineSnapStep = static_cast<float>(
        parsed["editing"]["timelineSnapStep"].value_or(static_cast<double>(timelineSnapStep)));
    liveEditEnabled = parsed["editing"]["liveEdit"].value_or(liveEditEnabled);
    autoConnectNewNodes = parsed["editing"]["autoConnectNewNodes"].value_or(autoConnectNewNodes);
}


void VFXEditorSession::SaveEditorPreferences() const
{
    if (preferencesPath.empty()) return;
    toml::table root;
    toml::table view;
    view.insert("graphGrid", showGraphGrid);
    view.insert("previewFloorGrid", preview.showFloorGrid);
    view.insert("previewVFXGizmos", preview.showGizmos);
    view.insert("overdrawIncludeModels", preview.includeModelsInOverdraw);
    view.insert("miniMap", showMiniMap);
    view.insert("graphZoom", graphZoom);
    view.insert("timeline", showTimeline);
    root.insert("view", std::move(view));
    toml::table editing;
    editing.insert("liveEdit", liveEditEnabled);
    editing.insert("autoConnectNewNodes", autoConnectNewNodes);
    editing.insert("timelineSnap", timelineSnap);
    editing.insert("timelineSnapStep", timelineSnapStep);
    root.insert("editing", std::move(editing));
    toml::table actor;
    actor.insert("model", preview.actorModelPath);
    actor.insert("controller", preview.actorControllerPath);
    actor.insert("clip", preview.actorClipPath);
    actor.insert("material", preview.actorMaterialPath);
    actor.insert("state", preview.actorState);
    actor.insert("bone", preview.selectedBone);
    actor.insert("attachToBone", preview.attachGraphToBone);
    actor.insert("syncToVFX", preview.syncActorToVFX);
    root.insert("previewActor", std::move(actor));
    std::ostringstream stream;
    stream << root;
    (void)util::FileSystem::WriteText(preferencesPath, stream.str());
}


bool VFXEditorSession::LoadGraph(const std::string& path)
{
    document.path = path;
    document.error.clear();
    asset::VFXGraphAsset loaded;
    if (!asset::ParseVFXGraphAsset(path, loaded, &document.error)) {
        document.graph = {};
        selectedNodeId = -1;
        return false;
    }
    document.graph = std::move(loaded);
    // 内容がまるごと入れ替わったので、未保存扱いにはせず版数だけ進めてライブ反映させる。
    document.dirty.Touch();
    document.liveDirtyNodeId = -1;
    // 前のGraphに対する一時エラー表示・ハイライトを持ち越さない。
    // Canvas 固有の一時状態は View 側にしかないため、フック経由で破棄させる。
    if (onGraphReplaced) onGraphReplaced(true);
    document.unreachableNodes.clear();
    // 構文が読めれば不正DAGも保持し、Inspector/Canvasから修復できるようにする。
    // 検証結果は document.error の設定が目的なので戻り値は使わない。
    (void)asset::ValidateVFXGraphAsset(document.graph, &document.error);
    document.dirty = false;
    document.history.Clear();
    positionsPending = true;
    selectedNodeId = -1;
    selectedLinkIndex = -1;
    return true;
}


bool VFXEditorSession::SaveGraph()
{
    document.error.clear();
    if (!asset::SaveVFXGraphAsset(document.path, document.graph, &document.error)) return false;
    document.dirty = false;
    document.history.savedStateId = document.history.currentStateId;
    AssetDirtyRegistry::MarkClean(document.path);
    return true;
}


void VFXEditorSession::PushUndo()
{
    PushUndo(document.graph);
}


void VFXEditorSession::PushUndo(const asset::VFXGraphAsset& before)
{
    constexpr std::size_t MAX_HISTORY = 96;
    document.history.undoStack.push_back(before);
    document.history.undoStateIds.push_back(document.history.currentStateId);
    document.history.currentStateId = document.history.nextStateId++;
    if (document.history.undoStack.size() > MAX_HISTORY) {
        document.history.undoStack.erase(document.history.undoStack.begin());
        document.history.undoStateIds.erase(document.history.undoStateIds.begin());
    }
    document.history.redoStack.clear();
    document.history.redoStateIds.clear();
}


void VFXEditorSession::Undo()
{
    if (document.history.undoStack.empty()) return;
    document.history.redoStack.push_back(document.graph);
    document.history.redoStateIds.push_back(document.history.currentStateId);
    document.graph = std::move(document.history.undoStack.back());
    document.history.undoStack.pop_back();
    document.history.currentStateId = document.history.undoStateIds.back();
    document.history.undoStateIds.pop_back();
    // グラフ全体が入れ替わるので、ノード限定の再適用を解除して全ノードを対象へ戻す。
    // WHY: 直前に触ったノード以外を巻き戻した Undo が、絞り込みのせいで
    //      プレビューへ反映されない取りこぼしを防ぐ。
    document.liveDirtyNodeId = -1;
    document.dirty = document.history.currentStateId != document.history.savedStateId;
    if (!document.dirty.IsDirty()) AssetDirtyRegistry::MarkClean(document.path);
    positionsPending = true;
    selectedNodeId = -1;
    selectedLinkIndex = -1;
    selectedGroupId = -1;
    if (onGraphReplaced) onGraphReplaced(false);
    preview.restartRequested = true;
}


void VFXEditorSession::Redo()
{
    if (document.history.redoStack.empty()) return;
    document.history.undoStack.push_back(document.graph);
    document.history.undoStateIds.push_back(document.history.currentStateId);
    document.graph = std::move(document.history.redoStack.back());
    document.history.redoStack.pop_back();
    document.history.currentStateId = document.history.redoStateIds.back();
    document.history.redoStateIds.pop_back();
    document.liveDirtyNodeId = -1; // Undo と同じ理由でノード限定を解除する
    document.dirty = document.history.currentStateId != document.history.savedStateId;
    if (!document.dirty.IsDirty()) AssetDirtyRegistry::MarkClean(document.path);
    positionsPending = true;
    selectedNodeId = -1;
    selectedLinkIndex = -1;
    selectedGroupId = -1;
    if (onGraphReplaced) onGraphReplaced(false);
    preview.restartRequested = true;
}


// Template を複製せず参照として置く。Merge との違いは「更新が伝播するか」だけで、
// 使い分けはユーザーが決める。ここは Sub Graph ノードを 1 個作って配線するだけ。
bool VFXEditorSession::LinkTemplateAsSubGraph(const GraphTemplateEntry& entry,
                                              const vfx::TemplateMergeOptions& options,
                                              std::string* outError)
{
    const asset::VFXGraphAsset before = document.graph;
    const auto fail = [&](const std::string& message) {
        document.graph = before;
        if (outError != nullptr) *outError = message;
        return false;
    };

    const std::string graphPath = NormalizeAssetPath(entry.path);
    // 自分自身を参照すると実行時に深度制限へ当たるまで再帰する。保存前に止める。
    if (!document.path.empty() && graphPath == NormalizeAssetPath(document.path))
        return fail("自分自身を Sub Graph として参照することはできません");

    int nextNodeId = 0;
    for (const auto& node : document.graph.nodes) nextNodeId = (std::max)(nextNodeId, node.id);

    const auto entryNode = std::find_if(document.graph.nodes.begin(), document.graph.nodes.end(),
        [](const asset::VFXGraphNode& node) { return node.type == asset::VFXNodeType::Entry; });
    int anchorId = entryNode != document.graph.nodes.end() ? entryNode->id : -1;
    const asset::VFXGraphNode* anchor = nullptr;
    if (options.anchorNodeId >= 0) {
        anchor = FindGraphNode(document.graph, options.anchorNodeId);
        if (anchor == nullptr) return fail("接続先ノードが見つかりません");
        anchorId = anchor->id;
    }

    asset::VFXGraphNode node;
    node.id = nextNodeId + 1;
    node.type = asset::VFXNodeType::SubGraph;
    node.name = entry.name;
    // Sub Graph の生存時間は子グラフの全長。0 のままだと即座に終了扱いになる。
    node.duration = entry.duration > 0.0f ? entry.duration : 1.0f;
    node.subGraph.graphPath = graphPath;
    node.parentNodeId = options.parentNodeId;
    if (anchor != nullptr) {
        node.editorX = anchor->editorX + 280.0f;
        node.editorY = anchor->editorY;
    } else {
        float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;
        const bool hasBounds = ComputeGraphNodeBounds(document.graph, minX, minY, maxX, maxY);
        node.editorX = hasBounds ? maxX + 80.0f : 260.0f;
        node.editorY = hasBounds ? minY : 80.0f;
    }
    const int addedId = node.id;
    document.graph.nodes.push_back(std::move(node));

    if (anchorId > 0) {
        asset::VFXGraphLink link;
        link.fromNode = anchorId;
        link.toNode = addedId;
        link.trigger = options.anchorNodeId >= 0 ? options.anchorTrigger
                                                 : asset::VFXLinkTrigger::OnStart;
        link.delay = options.anchorDelay;
        document.graph.links.push_back(link);
    }

    std::vector<float> starts;
    float duration = 0.0f;
    std::string error;
    if (!asset::BuildVFXGraphSchedule(document.graph, starts, duration, &error))
        return fail(error.empty() ? "スケジュールを構築できない形になりました" : error);

    lastMergeReport = vfx::TemplateMergeReport{};
    lastMergeReport.addedNodes.push_back(addedId);
    // 子グラフの budget は子アセット側で管理されるため、親の上限は動かさない。
    // ただし「置いても何も出ない」原因になる参照切れだけは同じ面で伝える。
    lastMergeReport.missingAssets = entry.missingAssets;
    lastMergeReport.usageAfter = asset::CalculateVFXGraphBudget(document.graph);
    lastMergeReport.budgetBefore[0] = lastMergeReport.budgetAfter[0] = document.graph.maxParticles;
    lastMergeReport.budgetBefore[1] = lastMergeReport.budgetAfter[1] = document.graph.maxLights;
    lastMergeReport.budgetBefore[2] = lastMergeReport.budgetAfter[2] = document.graph.maxAudioVoices;
    return true;
}


void VFXEditorSession::ApplyVariantAsDefaults(const std::string& variantName)
{
    const auto variant = std::find_if(document.graph.variants.begin(), document.graph.variants.end(),
        [&variantName](const asset::VFXVariantSet& item) { return item.name == variantName; });
    if (variant == document.graph.variants.end()) return;
    for (const auto& overrideValue : variant->overrides) {
        auto parameter = std::find_if(document.graph.parameters.begin(),
            document.graph.parameters.end(), [&overrideValue](const asset::VFXParamDefinition& item) {
                return item.name == overrideValue.paramName;
            });
        if (parameter != document.graph.parameters.end())
            parameter->defaultValue = overrideValue.value;
    }
}


bool VFXEditorSession::ApplyTemplate(EditorContext&, const GraphTemplateEntry& entry,
                                        TemplateApplyMode mode,
                                        const vfx::TemplateMergeOptions& options,
                                        bool saveImmediately)
{
    asset::VFXGraphAsset templateGraph;
    std::string error;
    // 不正DAGのTemplateでも構造だけは取り込んでCanvas上で直せるよう、Parse側を使う。
    if (!asset::ParseVFXGraphAsset(entry.path, templateGraph, &error)) {
        document.error = error.empty() ? "VFX Templateを読み込めませんでした: " + entry.name
                                     : std::move(error);
        return false;
    }

    PushUndo();
    document.liveDirtyNodeId = -1; // Template 適用はグラフ全体が変わる
    hasMergeReport = false;
    const bool replaces = mode == TemplateApplyMode::Replace;
    if (mode == TemplateApplyMode::Merge) {
        // 追記は既存の作業を一切壊さない。id・座標を衝突しないよう再割り当てして配置し、
        // 取り込んだ範囲を Template 名のグループ枠で囲って出所を残す。
        if (!MergeGraphTemplateInto(document.graph, templateGraph, entry.name, options,
                                    lastMergeReport, &error)) {
            document.history.undoStack.pop_back(); // 取り込めなかったので履歴も戻す
            document.error = error;
            return false;
        }
        hasMergeReport = true;
        // 取り込んだノードは次フレームに描画されてから選択する (View 側で予約する)。
        if (onSelectNodes) onSelectNodes(lastMergeReport.addedNodes);
    } else if (mode == TemplateApplyMode::SubGraph) {
        if (!LinkTemplateAsSubGraph(entry, options, &error)) {
            document.history.undoStack.pop_back();
            document.error = error;
            return false;
        }
        hasMergeReport = true;
        if (onSelectNodes) onSelectNodes(lastMergeReport.addedNodes);
    } else {
        document.graph = std::move(templateGraph);
        // Replace ではTemplateの名前をそのまま使うと全ての .vfx が同名になるため、
        // 現在編集中のアセット名(ファイル名)を維持する。
        const std::string stem = std::filesystem::path(document.path).stem().generic_string();
        if (!stem.empty()) document.graph.name = stem;
        // Template の説明は Template のものであってこの .vfx のものではない。
        // 引き継ぐと「全ての .vfx が Explosion テンプレートの説明を持つ」状態になる。
        document.graph.description.clear();
        document.graph.tags.clear();
        // 適用時に Variant を選んでいれば、その値を既定値として焼き込む。
        if (!options.variantName.empty()) ApplyVariantAsDefaults(options.variantName);
        selectedNodeId = -1;
        selectedLinkIndex = -1;
        selectedGroupId = -1;
    }
    document.dirty = true;
    positionsPending = true;
    // Merge では取り込んだノードを選び直すため、パン位置までは戻さない。
    if (onGraphReplaced) onGraphReplaced(replaces);
    document.error.clear();

    // Preview runtimeはdocument.pathをディスクから再読込するため、即時プレビューは保存を伴う。
    // WHY: 以前は無条件に保存していたが、Templateを覗くつもりの操作でユーザーの .vfx が
    //      問答無用に上書きされて戻せなかった。保存するかどうかは呼び出し側の明示選択に委ねる。
    if (saveImmediately) {
        if (!SaveGraph()) {
            document.dirty = true;
            return false;
        }
        preview.restartRequested = true;
    }
    return true;
}


void VFXEditorSession::ToggleSolo(int nodeId)
{
    document.soloNodeId = (document.soloNodeId == nodeId) ? -1 : nodeId;
    // Solo は値の変更ではないので dirty は触らない。プレビューへの再送出だけ促す。
    preview.restartRequested = true;
}


// NOTE: 実際の読み込みは requestedAssetPath 経由で Panel が次フレーム先頭に行う。
//       ここで LoadGraph を直接呼ぶと document.path と ctx.selectedAssetPath がずれ、
//       Panel が「別のアセットが選ばれた」と誤認して親グラフへ即座に戻してしまう。
bool VFXEditorSession::EnterSubGraph(const std::string& subGraphPath)
{
    const std::string target = NormalizeAssetPath(subGraphPath);
    if (target.empty() || target == document.path) return false;
    // 親を dirty のまま置き去りにすると、戻ってきたときにディスクと編集内容のどちらが正か
    // 決められなくなる。中へ入る前に必ず確定させる。
    if (document.dirty && !SaveGraph()) return false;

    subGraphBreadcrumb.push_back(document.path);
    subGraphNavigationPending = true;
    requestedAssetPath = target;
    return true;
}


bool VFXEditorSession::LeaveSubGraph(int targetDepth)
{
    if (subGraphBreadcrumb.empty()) return false;
    if (document.dirty && !SaveGraph()) return false;

    // targetDepth はパン屑の index。-1 なら 1 段だけ戻る。
    const int depth = targetDepth < 0
        ? static_cast<int>(subGraphBreadcrumb.size()) - 1
        : std::clamp(targetDepth, 0, static_cast<int>(subGraphBreadcrumb.size()) - 1);
    subGraphNavigationPending = true;
    requestedAssetPath = subGraphBreadcrumb[static_cast<std::size_t>(depth)];
    subGraphBreadcrumb.resize(static_cast<std::size_t>(depth));
    return true;
}


void VFXEditorSession::CreatePreviewEmitterFromAsset(EditorContext& ctx, const std::string& sourcePath)
{
    if (ctx.activeScene == nullptr) return;
    const std::string path = NormalizeAssetPath(sourcePath);
    if (EndsWithInsensitive(path, ".vfx")) {
        // .vfxはEmitterのtextureとして扱わず、独立Graph Editorで開く。
        requestedAssetPath = path;
        return;
    }
    scene::EntityID parentId = scene::EntityID::INVALID;
    if (auto* selected = ctx.GetSelectedGO();
        selected != nullptr && selected->GetComponent<scene::ParticleEmitter>() != nullptr)
        parentId = selected->GetID();

    auto& gameObject = ctx.activeScene->CreateGameObject("VFX Emitter");
    if (ctx.activeScene->IsValid(parentId))
        if (auto* parent = ctx.activeScene->GetGameObject(parentId)) gameObject.SetParent(*parent);
    auto& emitter = gameObject.AddComponent<scene::ParticleEmitter>();
    if (EndsWithInsensitive(path, ".mat")) emitter.materialPath = path;
    else if (EndsWithInsensitive(path, ".mesh") || EndsWithInsensitive(path, ".fbx")
             || EndsWithInsensitive(path, ".obj")) emitter.meshShapePath = path;
    else {
        // 描画テクスチャは ParticleEmitter へ直接保持せず、.mat の albedo スロットで指定する。
        // テクスチャ単体のドロップはマテリアル作成後に割り当てる。
    }
    ctx.selectedEntities = { gameObject.GetID() };
    preview.selectedEntity = gameObject.GetID();
}

} // namespace fbzz::editor
