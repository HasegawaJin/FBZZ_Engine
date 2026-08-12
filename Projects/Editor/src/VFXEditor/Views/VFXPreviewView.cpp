// FBZZ Engine
// VFXPreviewView.cpp | fbzz::editor
// プレビュー表示・参照画像オーバーレイ・連番書き出しダイアログの実装
#include <Editor/VFXEditor/Views/VFXPreviewView.hpp>

#include <Editor/VFXEditor/Application/VFXEditorSession.hpp>
#include <Editor/VFXEditor/Document/VFXGraphOps.hpp>
#include <Editor/VFXEditor/Views/VFXEditorUiCommon.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Panels/StatusBar.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Editor/Util/ParticleEditWidgets.hpp>
#include <Editor/Util/ParticleEmitterModules.hpp>
#include <Editor/Util/SchemaInspector.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/ProceduralVFXTextures.hpp>
#include <Engine/Asset/VFXAuthoringSchema.hpp>
#include <Engine/Asset/VFXGraphAsset.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Renderer/IImGuiRenderer.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/VFXGraphComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Quaternion.hpp>
#include <ImGuizmo.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imnodes.h>
#include <toml++/toml.hpp>
#include <algorithm>
#include <vector>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <Editor/VFXEditor/Views/VFXGraphCanvas.hpp>

namespace fbzz::editor {

// 共有ヘルパー (ノード色・アイコン・パラメーター編集など) は fbzz::editor::vfx に
// 隔離してある。Editor 全体の名前空間を汚さないための境界。
using namespace vfx;

void VFXPreviewView::DrawEnvironmentControls()
{
    static constexpr const char* kPresetNames[] = { "Dark", "Daylight", "Interior", "Neutral Gray" };
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::Combo("Env", &m_session.preview.environmentPreset, kPresetNames, 4))
        m_session.preview.ApplyEnvironmentPreset(m_session.preview.environmentPreset);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("プレビューの背景・ライトを切り替えます。\n"
                          "AI capture には影響しません (評価画の再現性を保つため)。");

    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    if (ImGui::DragInt("Copies", &m_session.preview.instanceCount, 1, 1, 64))
        m_session.preview.instanceCount = std::clamp(m_session.preview.instanceCount, 1, 64);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("同じエフェクトを何個同時に出すか。\n"
                          "実戦密度で重なり・描画順・GPU 時間を確認できます。");
    if (m_session.preview.instanceCount > 1) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        ImGui::DragFloat("Spread", &m_session.preview.instanceSpread, 0.1f, 0.0f, 100.0f, "%.1fm");
    }

    ImGui::SameLine();
    ImGui::Checkbox("Loop Seam", &m_session.preview.showLoopSeam);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("t=0 と t=duration を並べて表示し、ループの継ぎ目を確認します。");

    ImGui::SameLine();
    if (ImGui::Checkbox("Overdraw", &m_session.preview.overdrawView))
        m_session.preview.overdrawReadbackRequested = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("パーティクルの重なり枚数を色で表示します。\n"
                          "青=1-2 / 緑=3-4 / 黄=5-8 / 橙=9-16 / 赤=17+ 枚。\n"
                          "粒子数が budget 内でも赤が広ければ fill rate で重くなります。\n"
                           "(CPU シミュレーションのエミッターのみ計測します)");
    if (m_session.preview.overdrawView) {
        ImGui::SameLine();
        if (ImGui::Checkbox("Models##Overdraw",
                            &m_session.preview.includeModelsInOverdraw))
            m_session.SaveEditorPreferences();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("モデルをヒートマップの背景に残します。Overdraw枚数はParticleのみ計測します。");
    }

    // ── A/B 比較 ──
    ImGui::SameLine();
    if (!m_session.document.hasComparisonSnapshot) {
        if (ImGui::SmallButton("Set B")) {
            m_session.document.comparisonGraph = m_session.document.graph;
            m_session.document.hasComparisonSnapshot = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("今の状態を比較用 B として覚えます。");
    } else {
        if (ImGui::SmallButton(m_session.document.showComparison ? "Showing B" : "Showing A")) {
            // 現在のグラフと B を入れ替える。戻すときも同じ操作なので、
            // 「B を見ている間に編集してしまう」事故は起きるが元へは必ず戻せる。
            std::swap(m_session.document.graph, m_session.document.comparisonGraph);
            m_session.document.showComparison = !m_session.document.showComparison;
            m_session.document.dirty.Touch();
            m_session.document.liveDirtyNodeId = -1;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("A (編集中) と B (記録した状態) を入れ替えて見比べます。");
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear B")) {
            // B を見ている最中に破棄すると編集中の内容を失う。必ず A へ戻してから捨てる。
            if (m_session.document.showComparison) {
                std::swap(m_session.document.graph, m_session.document.comparisonGraph);
                m_session.document.showComparison = false;
                m_session.document.dirty.Touch();
            }
            m_session.document.hasComparisonSnapshot = false;
            m_session.document.comparisonGraph = {};
        }
    }
}


// ノードの親チェーンを辿り、親側までのワールド行列を組み立てる。
// WHY: 生成済み GameObject の world 行列をそのまま使うと、Mesh ノードの膨張スケールのような
//      「実行時エンベロープ」が混ざり、ギズモを離した瞬間に値が跳ねる。オーサリング値だけから
//      組み直すことで、掴んだ位置と保存される値が常に一致する。
static math::Matrix4 BuildVFXParentWorld(const asset::VFXGraphAsset& graph,
                                         const asset::VFXGraphNode& node,
                                         const math::Matrix4& ownerWorld)
{
    constexpr float DEG_TO_RAD = 0.01745329251994329577f;
    // 親から子の順に掛けたいので、まず根まで遡ってから逆順に積む。
    std::vector<const asset::VFXGraphNode*> chain;
    for (int cursor = node.parentNodeId, guard = 0;
         cursor != -1 && guard <= static_cast<int>(graph.nodes.size()); ++guard) {
        const auto parent = std::find_if(graph.nodes.begin(), graph.nodes.end(),
            [&](const asset::VFXGraphNode& candidate) { return candidate.id == cursor; });
        if (parent == graph.nodes.end()) break;
        // Entry / Delay は実体を持たず、ランタイムでも owner 直下へ縮退する。
        // ここでも同じ扱いにしないと、ギズモの基準がプレビューの見た目とずれる。
        if (parent->type == asset::VFXNodeType::Entry
            || parent->type == asset::VFXNodeType::Delay) break;
        chain.push_back(&*parent);
        cursor = parent->parentNodeId;
    }
    math::Matrix4 world = ownerWorld;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        const asset::VFXGraphNode& parent = **it;
        world = world * math::Matrix4::TRS(
            parent.localPosition,
            math::Quaternion::FromEuler(parent.localRotationDegrees * DEG_TO_RAD),
            parent.localScale);
    }
    return world;
}

bool VFXPreviewView::DrawNodeGizmo(EditorContext& ctx,
                                   const ImVec2& viewportMin,
                                   const ImVec2& viewportSize)
{
    constexpr float DEG_TO_RAD = 0.01745329251994329577f;
    if (!m_session.graphMode || !m_session.preview.cameraValid) return false;
    auto* node = FindGraphNode(m_session.document.graph, m_session.selectedNodeId);
    // Entry は時間の起点、Delay は待ち時間だけのノードで、どちらも空間上の実体を持たない。
    if (node == nullptr || node->type == asset::VFXNodeType::Entry
        || node->type == asset::VFXNodeType::Delay) return false;

    // Graph を載せている Preview オブジェクトの world。プレビューでは基本的に原点だが、
    // N 体同時プレビューでは複製が散らばるため、代表インスタンスの位置を基準にする。
    math::Matrix4 ownerWorld = math::Matrix4::Identity();
    if (ctx.vfxPreviewScene != nullptr)
        if (auto* owner = ctx.vfxPreviewScene->GetGameObject(m_session.preview.graphEntity))
            ownerWorld = owner->transform.GetWorldMatrix();

    const math::Matrix4 parentWorld =
        BuildVFXParentWorld(m_session.document.graph, *node, ownerWorld);
    const math::Matrix4 localMatrix = math::Matrix4::TRS(
        node->localPosition,
        math::Quaternion::FromEuler(node->localRotationDegrees * DEG_TO_RAD),
        node->localScale);

    const renderer::Camera& camera = m_session.preview.camera;
    math::Matrix4 viewColumn = math::Matrix4::Transpose(camera.GetViewMatrix());
    math::Matrix4 projColumn = math::Matrix4::Transpose(camera.GetProjectionMatrix());
    math::Matrix4 worldColumn = math::Matrix4::Transpose(parentWorld * localMatrix);

    // Scene View のギズモと内部状態を共有させない。埋め込み版では同じフレームに
    // 両方が動くため、ID を分けないと片方のドラッグがもう片方へ漏れる。
    ImGuizmo::SetID(1);
    ImGuizmo::SetDrawlist();
    ImGuizmo::Enable(true);
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetRect(viewportMin.x, viewportMin.y, viewportSize.x, viewportSize.y);

    // 操作モードと Snap は Scene View と同じ EditorContext を読む。
    // WHY: VFX Editor だけ別の割り当てにすると、W/E/R を押すたびに「どちらの流儀か」を
    //      思い出す必要が出る。ショートカットの意味はエディタ全体で 1 つに保つ。
    // ゲート条件も Scene View と同じ: 文字入力中は取らない / Preview をホバー中だけ /
    // 右ドラッグ (カメラフライの WASD) 中は取らない。
    if (!ImGui::GetIO().WantTextInput && m_session.preview.hovered
        && !ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
        if (ImGui::IsKeyPressed(ImGuiKey_W, false)) ctx.gizmoMode = EditorContext::GizmoMode::Translate;
        if (ImGui::IsKeyPressed(ImGuiKey_E, false)) ctx.gizmoMode = EditorContext::GizmoMode::Rotate;
        if (ImGui::IsKeyPressed(ImGuiKey_R, false)) ctx.gizmoMode = EditorContext::GizmoMode::Scale;
        if (ImGui::IsKeyPressed(ImGuiKey_Q, false))
            ctx.gizmoSpace = (ctx.gizmoSpace == EditorContext::GizmoSpace::World)
                ? EditorContext::GizmoSpace::Local : EditorContext::GizmoSpace::World;
    }
    ImGuizmo::OPERATION operation = ImGuizmo::TRANSLATE;
    if (ctx.gizmoMode == EditorContext::GizmoMode::Rotate) operation = ImGuizmo::ROTATE;
    if (ctx.gizmoMode == EditorContext::GizmoMode::Scale) operation = ImGuizmo::SCALE;
    const ImGuizmo::MODE mode = (ctx.gizmoSpace == EditorContext::GizmoSpace::World)
        ? ImGuizmo::WORLD : ImGuizmo::LOCAL;
    const float snapValue = (ctx.gizmoMode == EditorContext::GizmoMode::Rotate) ? ctx.snapRot
                          : (ctx.gizmoMode == EditorContext::GizmoMode::Scale) ? ctx.snapScale
                          : ctx.snapPos;
    float snap[3] = { snapValue, snapValue, snapValue };
    const bool snapActive = ctx.snapEnabled || ImGui::GetIO().KeyCtrl;

    ImGuizmo::Manipulate(&viewColumn.m[0][0], &projColumn.m[0][0], operation, mode,
                         &worldColumn.m[0][0], nullptr, snapActive ? snap : nullptr);

    const bool using_ = ImGuizmo::IsUsing();
    // ドラッグ開始の 1 回だけ Undo を積む。毎フレーム積むと Ctrl+Z がフレーム数ぶん必要になる。
    if (using_ && !m_gizmoDragging) {
        m_session.PushUndo();
        m_session.graphEditInProgress = true;
    }
    if (!using_ && m_gizmoDragging) m_session.graphEditInProgress = false;
    m_gizmoDragging = using_;

    if (using_) {
        // ワールドへ戻した結果から親の寄与を外し、このノードのローカル値を取り出す。
        const math::Matrix4 newWorld = math::Matrix4::Transpose(worldColumn);
        math::Matrix4 newLocal = math::Matrix4::Inverse(parentWorld) * newWorld;
        math::Matrix4 newLocalColumn = math::Matrix4::Transpose(newLocal);
        float translation[3] = {};
        float rotation[3] = {};
        float scale[3] = {};
        ImGuizmo::DecomposeMatrixToComponents(&newLocalColumn.m[0][0], translation, rotation, scale);
        // 現在の操作に対応する成分だけを書く。
        // WHY: 分解と再合成には必ず誤差が出るため、触っていない成分まで毎フレーム上書きすると
        //      移動しただけで回転がじわじわ回る。編集した軸だけに書き込みを限定する。
        switch (ctx.gizmoMode) {
        case EditorContext::GizmoMode::Rotate:
            node->localRotationDegrees = { rotation[0], rotation[1], rotation[2] };
            break;
        case EditorContext::GizmoMode::Scale:
            node->localScale = { scale[0], scale[1], scale[2] };
            break;
        default:
            node->localPosition = { translation[0], translation[1], translation[2] };
            break;
        }
        m_session.document.dirty = true;
        m_session.document.liveDirtyNodeId = node->id;
    }

    // ImGuizmo の ID をこの View の外へ持ち出さない (次に描く側が既定の 0 を前提にしている)。
    ImGuizmo::SetID(0);
    return using_ || ImGuizmo::IsOver();
}

void VFXPreviewView::DrawViewport(EditorContext& ctx, bool fillAvailable)
{
    ImGui::TextUnformatted("Preview");
    ImGui::SameLine();
    ImGui::TextDisabled("Isolated World");
    ImGui::SameLine();
    if (ImGui::Checkbox("Grid", &m_session.preview.showFloorGrid)) m_session.SaveEditorPreferences();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("Show a floor grid in the preview to gauge scale");
    ImGui::SameLine();
    if (ImGui::Checkbox("Gizmos", &m_session.preview.showGizmos)) m_session.SaveEditorPreferences();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("力場の半径・向きとエミッター形状/初速をワイヤーで表示");

    if (ctx.vfxPreviewScene != nullptr
        && ctx.vfxPreviewScene->IsValid(m_session.preview.actorEntity)
        && ImGui::CollapsingHeader("Preview Animator",
                                   ImGuiTreeNodeFlags_DefaultOpen)) {
        auto* actor = ctx.vfxPreviewScene->GetGameObject(m_session.preview.actorEntity);
        auto* animator = actor != nullptr
            ? actor->GetComponent<scene::AnimatorComponent>() : nullptr;
        if (animator != nullptr) {
            auto* previewGraph = ctx.vfxPreviewScene->GetComponent<scene::VFXGraphComponent>(
                m_session.preview.graphEntity);
            if (ImGui::Checkbox("Sync To VFX Time##Actor",
                                &m_session.preview.syncActorToVFX)) {
                if (auto* graph = ctx.vfxPreviewScene->GetComponent<scene::VFXGraphComponent>(
                        m_session.preview.graphEntity))
                    graph->syncParentAnimator = m_session.preview.syncActorToVFX;
                animator->playing = !m_session.preview.syncActorToVFX
                    && m_session.preview.actorPlaying;
                m_session.SaveEditorPreferences();
            }
            if (m_session.preview.actorPlaying)
                m_session.preview.actorTime = animator->currentStateName.empty()
                    ? animator->time : animator->stateTime;
            if (ImGui::Button(m_session.preview.actorPlaying ? "Pause##Actor" : "Play##Actor")) {
                m_session.preview.actorPlaying = !m_session.preview.actorPlaying;
                if (m_session.preview.syncActorToVFX && previewGraph != nullptr) {
                    if (m_session.preview.actorPlaying) previewGraph->Resume();
                    else previewGraph->Pause();
                } else {
                    animator->playing = m_session.preview.actorPlaying;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Restart##Actor")) {
                m_session.preview.actorTime = 0.0f;
                animator->time = 0.0f;
                animator->stateTime = 0.0f;
                if (m_session.preview.syncActorToVFX && previewGraph != nullptr)
                    previewGraph->Restart();
                else
                    animator->playing = m_session.preview.actorPlaying;
            }
            ImGui::SameLine();
            if (ImGui::DragFloat("Speed##Actor", &m_session.preview.actorSpeed,
                                 0.01f, -8.0f, 8.0f)) {
                animator->speed = m_session.preview.actorSpeed;
            }
            ImGui::SameLine();
            if (ImGui::Checkbox("Loop##Actor", &m_session.preview.actorLoop))
                animator->loop = m_session.preview.actorLoop;
            if (!animator->states.empty()) {
                const char* current = animator->currentStateName.empty()
                    ? "(Controller Default)" : animator->currentStateName.c_str();
                if (ImGui::BeginCombo("State##Actor", current)) {
                    for (const auto& state : animator->states) {
                        const bool selected = animator->currentStateName == state.name;
                        if (ImGui::Selectable(state.name.c_str(), selected)) {
                            animator->currentStateName = state.name;
                            animator->stateTime = 0.0f;
                            m_session.preview.actorState = state.name;
                            m_session.SaveEditorPreferences();
                        }
                    }
                    ImGui::EndCombo();
                }
            } else if (!animator->clips.empty()) {
                const char* current = animator->clipName.empty()
                    ? animator->clips[static_cast<std::size_t>(std::clamp(
                        animator->clipIndex, 0,
                        static_cast<int>(animator->clips.size()) - 1))].name.c_str()
                    : animator->clipName.c_str();
                if (ImGui::BeginCombo("Clip##Actor", current)) {
                    for (std::size_t index = 0; index < animator->clips.size(); ++index) {
                        const bool selected = animator->clipIndex == static_cast<int>(index);
                        if (ImGui::Selectable(animator->clips[index].name.c_str(), selected)) {
                            animator->clipIndex = static_cast<int>(index);
                            animator->clipName = animator->clips[index].name;
                            animator->time = 0.0f;
                        }
                    }
                    ImGui::EndCombo();
                }
            }
            if (ImGui::DragFloat("Animation Time##Actor", &m_session.preview.actorTime,
                                 0.01f, 0.0f, 3600.0f, "%.3fs")) {
                animator->playing = false;
                animator->time = m_session.preview.actorTime;
                animator->stateTime = m_session.preview.actorTime;
            }
            if (m_session.preview.actorBones.empty())
                m_session.preview.RefreshActorBones(ctx);
            const char* selectedBone = m_session.preview.selectedBone.empty()
                ? "(Actor Root)" : m_session.preview.selectedBone.c_str();
            if (ImGui::BeginCombo("Bone / Socket##Actor", selectedBone)) {
                if (ImGui::Selectable("(Actor Root)",
                                      m_session.preview.selectedBone.empty())) {
                    m_session.preview.selectedBone.clear();
                    m_session.preview.ApplyActorAttachment(ctx);
                    m_session.SaveEditorPreferences();
                }
                for (const auto& bone : m_session.preview.actorBones)
                    if (ImGui::Selectable(bone.c_str(),
                                          m_session.preview.selectedBone == bone)) {
                        m_session.preview.selectedBone = bone;
                        m_session.preview.ApplyActorAttachment(ctx);
                        m_session.SaveEditorPreferences();
                    }
                ImGui::EndCombo();
            }
            if (ImGui::Checkbox("Attach VFX To Bone",
                                &m_session.preview.attachGraphToBone)) {
                m_session.preview.ApplyActorAttachment(ctx);
                m_session.SaveEditorPreferences();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Preview Graphのrootを選択Boneへ追従させます");
        }
    }

    const ImVec2 available = ImGui::GetContentRegionAvail();
    // 狭いウィンドウでも横スクロールを発生させず、Inspector のリサイズ操作へ追従する。
    const float width = (std::max)(available.x, 1.0f);
    const float maxHeight = (std::max)(available.y, 90.0f);
    // Graph専用Viewport列では縦方向も全て使い、Canvasと同じ高さで結果を見続けられるようにする。
    const float height = fillAvailable
        ? maxHeight
        : std::clamp(width * (9.0f / 16.0f), 90.0f, maxHeight);
    m_session.preview.width = std::floor(width);
    m_session.preview.height = std::floor(height);

    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 end{ start.x + width, start.y + height };
    ImGui::InvisibleButton("##VFXPreview", { width, height });
    const bool viewportHovered = ImGui::IsItemHovered();
    m_session.preview.hovered = m_session.preview.hovered || viewportHovered;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && ImGui::BeginTooltip()) {
        ImGui::TextUnformatted("RMB + WASD: Fly   MMB: Pan   Wheel: Dolly");
        ImGui::TextUnformatted("Alt + LMB: Orbit   Drop asset: Create effect");
        ImGui::EndTooltip();
    }
    if (ImGui::BeginDragDropTarget()) {
        std::string path;
        if (ReadAssetPayload(ImGui::AcceptDragDropPayload("ASSET_PATH"), path)) {
            const bool actorAsset = EndsWithInsensitive(path, ".fbx")
                || EndsWithInsensitive(path, ".gltf") || EndsWithInsensitive(path, ".glb")
                || EndsWithInsensitive(path, ".anim")
                || EndsWithInsensitive(path, ".animcontroller")
                || EndsWithInsensitive(path, ".animctrl")
                || EndsWithInsensitive(path, ".mat");
            if (actorAsset) {
                std::string error;
                if (!m_session.preview.LoadPreviewAsset(ctx, path, &error))
                    m_session.document.error = std::move(error);
                else
                    m_session.SaveEditorPreferences();
            } else if (m_session.graphMode) {
                m_canvas.AddNodeFromAsset(path);
            } else {
                m_session.CreatePreviewEmitterFromAsset(ctx, path);
            }
        }
        ImGui::EndDragDropTarget();
    }
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(start, end, IM_COL32(5, 6, 9, 255));
    if (m_session.preview.renderTarget.IsValid() && ctx.imguiRenderer && ctx.resources) {
        void* nativeId = ctx.imguiRenderer->GetImTextureID(m_session.preview.renderTarget, *ctx.resources, 0);
        if (nativeId != nullptr) {
            const ImTextureID textureId = static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(nativeId));
            drawList->AddImage(textureId, start, end);
        }
    } else {
        drawList->AddText({ start.x + 12.0f, start.y + 12.0f },
                          IM_COL32(150, 155, 165, 255), "Preview RenderTarget unavailable");
    }
    // 参照画像オーバーレイ。ImGui の描画リストへ重ねるだけで、Preview World にも
    // AI capture 用 World にも一切触れない。評価用の静止画が担当者の表示設定で
    // 変わってしまうと視覚判断が再現しなくなるため、この分離は崩さないこと。
    if (m_referenceImageVisible && !m_referenceImagePath.empty()
        && ctx.imguiRenderer != nullptr && ctx.resources != nullptr) {
        // AssetPathField は "Assets/..." 相対を返すため、GPU ロード前に実体パスへ解決する。
        const auto handle = ctx.resources->LoadTexture(
            asset::AssetManager::ResolveAssetPath(m_referenceImagePath));
        if (handle.IsValid()) {
            if (void* rawId = ctx.imguiRenderer->GetImTextureID(handle, *ctx.resources)) {
                const ImTextureID referenceId =
                    static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(rawId));
                // Preview 全体を基準にスケールし、中心合わせ + オフセットで置く。
                // 参照写真とエフェクトの「大きさ」を合わせる作業がこのウィジェットの主目的。
                const float w = width * m_referenceImageScale;
                const float h = height * m_referenceImageScale;
                const ImVec2 center{ (start.x + end.x) * 0.5f + m_referenceImageOffsetX * width,
                                     (start.y + end.y) * 0.5f + m_referenceImageOffsetY * height };
                const ImVec2 refMin{ center.x - w * 0.5f, center.y - h * 0.5f };
                const ImVec2 refMax{ center.x + w * 0.5f, center.y + h * 0.5f };
                const ImU32 tint = IM_COL32(255, 255, 255,
                    static_cast<int>(std::clamp(m_referenceImageOpacity, 0.0f, 1.0f) * 255.0f));
                // Behind は「参照の上にエフェクトを重ねて形を追う」用途。
                // ただし ImGui は後から積んだものが必ず前に来るので、下敷きは
                // 参照を不透明側で敷き直すのではなく、Preview 画像を薄く見せる意味になる。
                // ここでは実装を単純に保ち、どちらも参照側の不透明度で調整させる。
                drawList->PushClipRect(start, end, true);
                drawList->AddImage(referenceId, refMin, refMax, { 0.0f, 0.0f }, { 1.0f, 1.0f }, tint);
                drawList->PopClipRect();
            }
        }
    }
    // 選択輪郭はRenderTarget内でSelection Maskから合成済み。ImGui側はTransformギズモだけを重ねる。
    const bool gizmoUsedMouse = DrawNodeGizmo(ctx, start, { width, height });
    // ギズモを掴んでいる間はカメラ操作へ入力を渡さない (掴んだまま視点が回ると位置が決められない)。
    if (gizmoUsedMouse) m_session.preview.hovered = false;

    drawList->AddRect(start, end, IM_COL32(70, 76, 90, 255));
    drawList->AddRectFilled({ start.x + 10.0f, start.y + 10.0f },
                            { start.x + 128.0f, start.y + 31.0f }, IM_COL32(10, 14, 20, 190), 4.0f);
    drawList->AddText({ start.x + 17.0f, start.y + 13.0f }, IM_COL32(112, 210, 168, 255),
                      "PREVIEW WORLD");
    // 現在のギズモモードを右上へ出す。キー割り当ては表に出ていないと発見されないので、
    // 「今どのモードか」と「切り替えキー」を同じ場所で示す。
    if (m_session.graphMode && m_session.selectedNodeId > 0) {
        const char* modeLabel = ctx.gizmoMode == EditorContext::GizmoMode::Rotate ? "ROTATE  (E)"
                              : ctx.gizmoMode == EditorContext::GizmoMode::Scale  ? "SCALE  (R)"
                              : "MOVE  (W)";
        const char* spaceLabel = ctx.gizmoSpace == EditorContext::GizmoSpace::World
            ? "World (Q)" : "Local (Q)";
        char gizmoText[64];
        std::snprintf(gizmoText, sizeof(gizmoText), "%s   %s", modeLabel, spaceLabel);
        const ImVec2 textSize = ImGui::CalcTextSize(gizmoText);
        const ImVec2 boxMax{ end.x - 10.0f, start.y + 31.0f };
        const ImVec2 boxMin{ boxMax.x - textSize.x - 14.0f, start.y + 10.0f };
        drawList->AddRectFilled(boxMin, boxMax, IM_COL32(10, 14, 20, 190), 4.0f);
        drawList->AddText({ boxMin.x + 7.0f, boxMin.y + 3.0f },
                          IM_COL32(190, 198, 212, 255), gizmoText);
    }
    // 書き出し中はプレビューが自動でスクラブされる。操作を受け付けている最中と
    // 区別が付かないと「勝手に動いた」に見えるため、状態を必ず出す。
    if (m_session.sequenceExport.running) {
        drawList->AddRectFilled({ start.x + 10.0f, start.y + 37.0f },
                                { start.x + 210.0f, start.y + 58.0f },
                                IM_COL32(60, 30, 10, 210), 4.0f);
        char label[64];
        std::snprintf(label, sizeof(label), "EXPORTING  %d / %d",
                      m_session.sequenceExport.frame, m_session.sequenceExport.total);
        drawList->AddText({ start.x + 17.0f, start.y + 40.0f },
                          IM_COL32(255, 190, 110, 255), label);
    }
}

// ── 参照画像オーバーレイの操作 UI ───────────────────────────────────────────
// 設計書の「参照画像ガイドオーサリング」は AI 側の面しか無かったが、
// 参照を見ながら値を詰める作業は人間がやる場合も同じだけ有効なので、
// 決定論プレビューの上に素朴なオーバーレイとして用意する。

void VFXPreviewView::DrawReferenceOverlayControls(EditorContext& ctx)
{
    (void)ctx;
    if (!ImGui::CollapsingHeader("Reference Image")) return;
    AssetPathField("Image", m_referenceImagePath, ".png,.jpg,.jpeg,.tga,.bmp");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("参照スクショ / 実写を Preview へ重ねます。\n"
                          "Preview World には影響しないため、AI capture の評価画は変わりません。");
    ImGui::Checkbox("Show##reference", &m_referenceImageVisible);
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset Transform")) {
        m_referenceImageScale = 1.0f;
        m_referenceImageOffsetX = 0.0f;
        m_referenceImageOffsetY = 0.0f;
        m_referenceImageOpacity = 0.45f;
    }
    ImGui::SliderFloat("Opacity", &m_referenceImageOpacity, 0.0f, 1.0f);
    ImGui::SliderFloat("Scale", &m_referenceImageScale, 0.1f, 3.0f, "%.2fx");
    ImGui::SliderFloat("Offset X", &m_referenceImageOffsetX, -1.0f, 1.0f);
    ImGui::SliderFloat("Offset Y", &m_referenceImageOffsetY, -1.0f, 1.0f);
}

// ── 連番 PNG 書き出し ──────────────────────────────────────────────────────

void VFXPreviewView::DrawSequenceExportDialog(EditorContext& ctx)
{
    if (sequenceDialogOpen) {
        ImGui::OpenPopup("Export Frame Sequence##VFX");
        sequenceDialogOpen = false;
    }
    ImGui::SetNextWindowSize({ 470.0f, 0.0f }, ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Export Frame Sequence##VFX", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::TextWrapped("決定論スクラブで全区間を等間隔にレンダリングし、連番 PNG を書き出します。"
                       "randomSeed から再シミュレートするため、何度実行しても同じ絵になります。");
    ImGui::Separator();
    ImGui::SliderInt("FPS", &m_session.sequenceExport.fps, 6, 60);
    ImGui::DragFloat("Duration", &m_session.sequenceExport.duration, 0.05f, 0.1f, 60.0f, "%.2fs");
    InputString("Output Directory", m_session.sequenceExport.directory, 512);
    const int total = (std::max)(1,
        static_cast<int>(m_session.sequenceExport.duration * static_cast<float>(m_session.sequenceExport.fps)));
    ImGui::TextDisabled("%d frames  (%.0fx%.0f)", total, m_session.preview.width, m_session.preview.height);
    ImGui::Checkbox("Bake Flipbook Atlas", &m_session.sequenceExport.bakeAtlas);
    bool atlasFits = true;
    if (m_session.sequenceExport.bakeAtlas) {
        InputString("Atlas File Name", m_session.sequenceExport.atlasFileName, 256);
        ImGui::DragInt("Atlas Columns", &m_session.sequenceExport.atlasColumns, 1.0f, 0, total);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("0 = フレーム数から正方形に近い列数を自動選択");
        ImGui::Checkbox("Overwrite Existing Atlas", &m_session.sequenceExport.overwriteAtlas);
        const int columns = m_session.sequenceExport.atlasColumns > 0
            ? (std::min)(m_session.sequenceExport.atlasColumns, total)
            : static_cast<int>(std::ceil(std::sqrt(static_cast<double>(total))));
        const int rows = (total + columns - 1) / columns;
        const int atlasWidth = static_cast<int>(m_session.preview.width) * columns;
        const int atlasHeight = static_cast<int>(m_session.preview.height) * rows;
        atlasFits = atlasWidth <= 16384 && atlasHeight <= 16384;
        ImGui::TextDisabled("Atlas grid %dx%d  (%dx%d px)", columns, rows,
            atlasWidth, atlasHeight);
        if (!atlasFits)
            ImGui::TextColored({ 1.0f, 0.42f, 0.32f, 1.0f },
                               "Atlas exceeds the 16384px texture limit.");
    }
    if (!m_session.sequenceExport.message.empty()) ImGui::TextWrapped("%s", m_session.sequenceExport.message.c_str());
    ImGui::Separator();
    const bool canExport = ctx.renderer != nullptr && ctx.resources != nullptr
                        && m_session.preview.renderTarget.IsValid()
                        && !m_session.sequenceExport.directory.empty()
                        && (!m_session.sequenceExport.bakeAtlas
                            || (!m_session.sequenceExport.atlasFileName.empty() && atlasFits));
    if (!canExport) ImGui::BeginDisabled();
    if (ImGui::Button("Export", { 140.0f, 0.0f })) {
        std::error_code code;
        std::filesystem::create_directories(m_session.sequenceExport.directory, code);
        if (code) {
            m_session.sequenceExport.message = "出力先を作成できません: " + code.message();
        } else {
            m_session.sequenceExport.running = true;
            m_session.sequenceExport.frame = 0;
            m_session.sequenceExport.total = total;
            m_session.sequenceExport.settleFrames = 0;
            m_session.sequenceExport.message.clear();
            ImGui::CloseCurrentPopup();
        }
    }
    if (!canExport) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", { 120.0f, 0.0f })) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

} // namespace fbzz::editor
