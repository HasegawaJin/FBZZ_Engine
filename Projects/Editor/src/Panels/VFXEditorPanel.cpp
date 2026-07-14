// FBZZ Engine
// VFXEditorPanel.cpp | fbzz::editor
// VFX (Particle) 専用エディター実装
// プレビューはシーンビューの実レンダリングに任せ、このパネルは
// 「再生制御・タイムライン・エフェクト構造・モジュール編集」に集中する。
#include <Editor/Panels/VFXEditorPanel.hpp>

#include <Editor/EditorContext.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/ParticleEmitterModules.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace fbzz::editor {
namespace {

constexpr float kScrubStep     = 1.0f / 60.0f; // Step ボタン / スクラブの最小時間刻み
constexpr float kTimelineH     = 58.0f;        // タイムラインキャンバスの高さ [px]
constexpr float kHierarchyW    = 230.0f;       // 左カラム (エフェクト構造) の幅 [px]
constexpr float kBurstMarkerR  = 6.0f;         // Burst マーカー (菱形) の半径 [px]

bool Contains(const std::vector<scene::EntityID>& list, scene::EntityID id)
{
    return std::find(list.begin(), list.end(), id) != list.end();
}

// 祖先 GameObject のどれかが ParticleEmitter を持つか (エフェクトルート判定に使う)
bool AncestorHasEmitter(scene::GameObject* go)
{
    for (scene::GameObject* parent = go ? go->GetParent() : nullptr;
         parent; parent = parent->GetParent()) {
        if (parent->GetComponent<scene::ParticleEmitter>())
            return true;
    }
    return false;
}

} // namespace

std::vector<scene::EntityID> VFXEditorPanel::BuildEffectGroup(EditorContext& ctx) const
{
    std::vector<scene::EntityID> group;
    if (!ctx.activeScene)
        return group;
    const scene::EntityID sel = ctx.PrimarySelected();
    if (!sel.IsValid())
        return group;
    auto* selGo = ctx.activeScene->GetGameObject(sel);
    if (!selGo || !selGo->GetComponent<scene::ParticleEmitter>())
        return group;

    // 選択エミッターを起点に、子 GameObject と SubEmitter 名前参照を幅優先で辿る。
    // WHY: SubEmitter は GameObject 名参照なので循環し得る。visited で一度だけ処理する。
    std::vector<scene::EntityID> visited;
    std::vector<scene::EntityID> stack{ sel };
    while (!stack.empty()) {
        const scene::EntityID id = stack.back();
        stack.pop_back();
        if (Contains(visited, id))
            continue;
        visited.push_back(id);
        auto* go = ctx.activeScene->GetGameObject(id);
        if (!go)
            continue;
        auto* emitter = go->GetComponent<scene::ParticleEmitter>();
        if (emitter)
            group.push_back(id);
        // エミッターを持たない中間ノードの下にもエミッターが居られるよう、子は常に辿る
        for (int i = 0; i < go->GetChildCount(); ++i)
            if (auto* child = go->GetChild(i))
                stack.push_back(child->GetID());
        if (emitter) {
            const std::string* refs[] = {
                &emitter->birthSubEmitter, &emitter->deathSubEmitter, &emitter->collisionSubEmitter
            };
            for (const std::string* name : refs) {
                if (name->empty()) continue;
                if (auto* target = ctx.activeScene->Find(*name))
                    stack.push_back(target->GetID());
            }
        }
    }
    return group;
}

void VFXEditorPanel::RequestScrub(EditorContext& ctx,
                                  const std::vector<scene::EntityID>& group, float targetTime)
{
    if (!ctx.activeScene)
        return;
    // グループ全体へ同じ「エフェクト時刻」を要求することで、SubEmitter 連鎖もまとめて巻き戻す。
    for (const scene::EntityID id : group)
        if (auto* emitter = ctx.activeScene->GetComponent<scene::ParticleEmitter>(id))
            emitter->editorScrubTime = (std::max)(targetTime, 0.0f);
}

void VFXEditorPanel::OnRenderContent(EditorContext& ctx)
{
    const std::vector<scene::EntityID> group = BuildEffectGroup(ctx);
    scene::ParticleEmitter* root = nullptr;
    if (!group.empty() && ctx.activeScene)
        root = ctx.activeScene->GetComponent<scene::ParticleEmitter>(group.front());

    // プレビュー速度の注入。毎フレーム書き込み、書き込みが止まると Engine 側が自動で 1.0 へ戻す。
    // Play Mode 中はゲーム本来の再生を邪魔しないよう注入しない。
    const bool inPlayMode = ctx.playMode && !ctx.playMode->IsInEditor();
    if (!inPlayMode && ctx.activeScene) {
        for (const scene::EntityID id : group) {
            if (auto* emitter = ctx.activeScene->GetComponent<scene::ParticleEmitter>(id)) {
                emitter->editorTimeScale      = m_paused ? 0.0f : (std::max)(m_previewSpeed, 0.0f);
                emitter->editorTimeScaleFrame = Time::frameCount;
            }
        }
    }

    // Space で再生 / 一時停止 (パネルフォーカス中のみ。テキスト入力中は無視)
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
        && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Space, false) && root)
        m_paused = !m_paused;

    DrawTransport(ctx, group, root);
    ImGui::Separator();

    ImGui::BeginChild("##VFXBody", ImVec2(0, 0), false);

    ImGui::BeginChild("##VFXHierarchy", ImVec2(kHierarchyW, 0), true);
    DrawHierarchy(ctx);
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##VFXMain", ImVec2(0, 0), false);
    if (root) {
        DrawTimeline(ctx, group, root);
        DrawStats(ctx, group, root);
        ImGui::Separator();
        // モジュールスタック (Inspector と同じ実装を共有)
        ImGui::BeginChild("##VFXModules", ImVec2(0, 0), false);
        if (DrawParticleEmitterModules(*root, ctx) && ctx.markSceneDirty)
            ctx.markSceneDirty();
        ImGui::EndChild();
    } else {
        ImGui::Spacing();
        ImGui::TextDisabled("Select a Particle Emitter, or create one from the left panel.");
        ImGui::TextDisabled("The effect preview renders live in the Scene viewport.");
    }
    ImGui::EndChild();

    ImGui::EndChild();
}

void VFXEditorPanel::DrawTransport(EditorContext& ctx,
                                   const std::vector<scene::EntityID>& group,
                                   scene::ParticleEmitter* root)
{
    const bool hasEffect = root != nullptr;
    ImGui::BeginDisabled(!hasEffect);

    const bool isPlaying = hasEffect && root->playing && !m_paused;
    if (ImGui::Button(isPlaying ? "Pause" : "Play", { 64.0f, 0.0f })) {
        if (isPlaying) {
            m_paused = true;
        } else {
            m_paused = false;
            if (ctx.activeScene)
                for (const scene::EntityID id : group)
                    if (auto* emitter = ctx.activeScene->GetComponent<scene::ParticleEmitter>(id))
                        emitter->playing = true;
        }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("Space");

    ImGui::SameLine();
    if (ImGui::Button("Restart") && ctx.activeScene) {
        for (const scene::EntityID id : group)
            if (auto* emitter = ctx.activeScene->GetComponent<scene::ParticleEmitter>(id))
                emitter->ResetPlayback();
        m_paused = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop") && ctx.activeScene) {
        // 放出だけ止め、既存粒子は寿命で消える (Unity の Stop 相当)
        for (const scene::EntityID id : group)
            if (auto* emitter = ctx.activeScene->GetComponent<scene::ParticleEmitter>(id))
                emitter->playing = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Step") && hasEffect) {
        // 一時停止のまま 1/60s だけ進める。決定論スクラブなので何度押しても再現する。
        RequestScrub(ctx, group, root->playTime + kScrubStep);
        m_paused = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("Advance one frame (1/60s) while paused");
    ImGui::SameLine();
    if (ImGui::Button("Burst") && hasEffect)
        root->burstPending += 10;

    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    ImGui::DragFloat("##vfx_speed", &m_previewSpeed, 0.01f, 0.05f, 4.0f, "Speed %.2fx");
    ImGui::SameLine();
    if (ImGui::SmallButton("0.25x")) m_previewSpeed = 0.25f;
    ImGui::SameLine();
    if (ImGui::SmallButton("1x")) m_previewSpeed = 1.0f;
    ImGui::SameLine();
    if (ImGui::SmallButton("2x")) m_previewSpeed = 2.0f;

    if (hasEffect) {
        ImGui::SameLine();
        ImGui::Text("  %.2fs / %.2fs", root->playTime, root->duration);
    }
    ImGui::EndDisabled();

    // 右端: プレビューの場所を明示する (旧パネルの偽 2D プレビュー廃止に伴う導線)
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 150.0f);
    ImGui::TextDisabled("Preview: Scene View");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("The selected effect simulates and renders live in the Scene viewport");
}

void VFXEditorPanel::DrawHierarchy(EditorContext& ctx)
{
    ImGui::TextUnformatted("Effects");
    ImGui::Separator();
    if (!ctx.activeScene) {
        ImGui::TextDisabled("No active scene");
        return;
    }

    // 新規エミッター作成。エミッター選択中ならその子として作り、エフェクトの部品追加を1クリックにする。
    if (ImGui::Button("+ New Emitter", { -1.0f, 0.0f })) {
        scene::EntityID parentId = scene::EntityID::INVALID;
        if (auto* selGo = ctx.GetSelectedGO(); selGo && selGo->GetComponent<scene::ParticleEmitter>())
            parentId = selGo->GetID();
        auto& go = ctx.activeScene->CreateGameObject("VFX Emitter");
        // CreateGameObject で内部ストレージが再配置され得るため、親は EntityID から引き直す
        if (ctx.activeScene->IsValid(parentId))
            if (auto* parentGo = ctx.activeScene->GetGameObject(parentId))
                go.SetParent(*parentGo);
        go.AddComponent<scene::ParticleEmitter>();
        ctx.selectedEntities = { go.GetID() };
        if (ctx.markSceneDirty) ctx.markSceneDirty();
    }
    ImGui::Spacing();

    // 他エミッターから SubEmitter として名前参照されているものはルートに並べない
    std::vector<std::string> referencedNames;
    for (const scene::EntityID id : ctx.activeScene->GetEntities<scene::ParticleEmitter>()) {
        const auto* emitter = ctx.activeScene->GetComponent<scene::ParticleEmitter>(id);
        if (!emitter) continue;
        if (!emitter->birthSubEmitter.empty())     referencedNames.push_back(emitter->birthSubEmitter);
        if (!emitter->deathSubEmitter.empty())     referencedNames.push_back(emitter->deathSubEmitter);
        if (!emitter->collisionSubEmitter.empty()) referencedNames.push_back(emitter->collisionSubEmitter);
    }

    bool anyRoot = false;
    std::vector<scene::EntityID> visited;
    for (const scene::EntityID id : ctx.activeScene->GetEntities<scene::ParticleEmitter>()) {
        auto* go = ctx.activeScene->GetGameObject(id);
        if (!go) continue;
        const bool isReferenced =
            std::find(referencedNames.begin(), referencedNames.end(), go->name) != referencedNames.end();
        if (isReferenced || AncestorHasEmitter(go))
            continue; // ルートではない → 親ノードの下に表示される
        anyRoot = true;
        visited.clear();
        DrawEmitterNode(ctx, id, 0, visited);
    }
    if (!anyRoot)
        ImGui::TextDisabled("No Particle Emitters in scene");
}

void VFXEditorPanel::DrawEmitterNode(EditorContext& ctx, scene::EntityID id, int depth,
                                     std::vector<scene::EntityID>& visited)
{
    // SubEmitter の名前参照は循環し得るため、visited と深さ上限の両方で打ち切る
    if (depth > 8 || Contains(visited, id) || !ctx.activeScene)
        return;
    visited.push_back(id);

    auto* go = ctx.activeScene->GetGameObject(id);
    auto* emitter = go ? go->GetComponent<scene::ParticleEmitter>() : nullptr;
    if (!go || !emitter)
        return;

    // 子ノード = 子 GameObject のエミッター + SubEmitter 名前参照
    std::vector<scene::EntityID> childEmitters;
    for (int i = 0; i < go->GetChildCount(); ++i)
        if (auto* child = go->GetChild(i))
            if (child->GetComponent<scene::ParticleEmitter>())
                childEmitters.push_back(child->GetID());
    struct SubRef { const char* eventName; scene::EntityID id; };
    std::vector<SubRef> subRefs;
    const std::pair<const char*, const std::string*> refs[] = {
        { "Birth", &emitter->birthSubEmitter },
        { "Death", &emitter->deathSubEmitter },
        { "Collision", &emitter->collisionSubEmitter },
    };
    for (const auto& [eventName, name] : refs) {
        if (name->empty()) continue;
        if (auto* target = ctx.activeScene->Find(*name))
            if (target->GetComponent<scene::ParticleEmitter>())
                subRefs.push_back({ eventName, target->GetID() });
    }

    const bool selected = ctx.PrimarySelected() == id;
    const bool leaf = childEmitters.empty() && subRefs.empty();
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow
                             | ImGuiTreeNodeFlags_SpanAvailWidth
                             | ImGuiTreeNodeFlags_DefaultOpen;
    if (leaf)     flags |= ImGuiTreeNodeFlags_Leaf;
    if (selected) flags |= ImGuiTreeNodeFlags_Selected;

    char label[160];
    std::snprintf(label, sizeof(label), "%s  (%d)%s",
                  go->name.c_str(),
                  static_cast<int>(emitter->particles.size()),
                  emitter->playing ? "" : "  [stopped]");
    ImGui::PushID(static_cast<int>(id.index));
    const bool open = ImGui::TreeNodeEx(label, flags);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
        ctx.selectedEntities = { id };
    if (open) {
        for (const scene::EntityID child : childEmitters)
            DrawEmitterNode(ctx, child, depth + 1, visited);
        for (const SubRef& ref : subRefs) {
            // イベント名バッジ + 参照先ノード。参照先は独立した GO なので同じ描画を使う
            ImGui::TextDisabled("  [%s]", ref.eventName);
            ImGui::SameLine();
            DrawEmitterNode(ctx, ref.id, depth + 1, visited);
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void VFXEditorPanel::DrawTimeline(EditorContext& ctx,
                                  const std::vector<scene::EntityID>& group,
                                  scene::ParticleEmitter* root)
{
    // 横軸スパン: duration。0 (無限再生) のときは lifetime を目安に表示だけ行う
    const bool  continuous = root->duration <= 0.0f;
    const float span = continuous ? (std::max)(root->lifetime, 1.0f) : root->duration;

    const float  width = (std::max)(ImGui::GetContentRegionAvail().x, 160.0f);
    ImGui::InvisibleButton("##vfx_timeline", ImVec2(width, kTimelineH));
    const ImVec2 rectMin = ImGui::GetItemRectMin();
    const ImVec2 rectMax = ImGui::GetItemRectMax();
    const bool   hovered = ImGui::IsItemHovered();
    ImDrawList*  draw  = ImGui::GetWindowDrawList();
    const ImVec2 mouse = ImGui::GetMousePos();

    const float rulerBottom  = rectMin.y + 22.0f;         // 上段: 目盛り + 再生ヘッド操作ゾーン
    const float markerCenter = rectMax.y - 12.0f;         // 下段: Burst マーカーゾーン

    auto timeToX = [&](float t) {
        return rectMin.x + std::clamp(t / span, 0.0f, 1.0f) * width;
    };
    auto xToTime = [&](float x) {
        return std::clamp((x - rectMin.x) / width, 0.0f, 1.0f) * span;
    };

    // 背景と目盛り
    draw->AddRectFilled(rectMin, rectMax, IM_COL32(22, 25, 32, 255), 3.0f);
    draw->AddLine({ rectMin.x, rulerBottom }, { rectMax.x, rulerBottom }, IM_COL32(60, 66, 80, 255));
    for (int i = 0; i <= 10; ++i) {
        const float x = rectMin.x + width * (static_cast<float>(i) / 10.0f);
        const bool major = i % 5 == 0;
        draw->AddLine({ x, rectMin.y }, { x, rectMin.y + (major ? 12.0f : 6.0f) },
                      IM_COL32(120, 128, 145, major ? 200 : 110));
        if (major) {
            char tick[16];
            std::snprintf(tick, sizeof(tick), "%.1fs", span * (static_cast<float>(i) / 10.0f));
            draw->AddText({ x + 3.0f, rectMin.y + 8.0f }, IM_COL32(150, 158, 175, 255), tick);
        }
    }
    draw->AddRect(rectMin, rectMax, IM_COL32(70, 76, 90, 255), 3.0f);

    // ループ / 遅延の注記
    if (root->loop)
        draw->AddText({ rectMax.x - 44.0f, rectMin.y + 2.0f }, IM_COL32(120, 200, 160, 255), "loop");
    if (root->startDelay > 0.0f) {
        char delayText[32];
        std::snprintf(delayText, sizeof(delayText), "delay %.2fs", root->startDelay);
        draw->AddText({ rectMin.x + 4.0f, rectMin.y + 2.0f }, IM_COL32(230, 180, 90, 255), delayText);
    }

    ImGuiStorage* storage    = ImGui::GetStateStorage();
    const ImGuiID scrubId    = ImGui::GetID("##vfx_scrubbing");
    const ImGuiID burstDragId = ImGui::GetID("##vfx_burst_drag");
    const ImGuiID burstEditId = ImGui::GetID("##vfx_burst_edit");

    // ── Burst マーカー (下段の菱形。ドラッグで時刻変更 / 右クリックで詳細編集) ──
    int hoveredBurst = -1;
    for (size_t i = 0; i < root->bursts.size(); ++i) {
        const float x = timeToX(root->bursts[i].time);
        const ImVec2 c(x, markerCenter);
        const bool hot = hovered
            && std::fabs(mouse.x - c.x) <= kBurstMarkerR + 3.0f
            && std::fabs(mouse.y - c.y) <= kBurstMarkerR + 3.0f;
        if (hot) hoveredBurst = static_cast<int>(i);
        const ImU32 fill = hot ? IM_COL32(255, 220, 120, 255) : IM_COL32(235, 160, 70, 255);
        draw->AddQuadFilled({ c.x, c.y - kBurstMarkerR }, { c.x + kBurstMarkerR, c.y },
                            { c.x, c.y + kBurstMarkerR }, { c.x - kBurstMarkerR, c.y }, fill);
        draw->AddQuad({ c.x, c.y - kBurstMarkerR }, { c.x + kBurstMarkerR, c.y },
                      { c.x, c.y + kBurstMarkerR }, { c.x - kBurstMarkerR, c.y },
                      IM_COL32(30, 34, 44, 255), 1.0f);
        if (hot)
            ImGui::SetTooltip("Burst: %d particles @ %.2fs (x%d)\nDrag: move / Right-click: edit",
                              root->bursts[i].count, root->bursts[i].time,
                              (std::max)(root->bursts[i].cycles, 1));
    }

    // マーカーのドラッグ
    if (hovered && hoveredBurst >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        storage->SetInt(burstDragId, hoveredBurst);
    const int burstDrag = storage->GetInt(burstDragId, -1);
    if (burstDrag >= 0 && burstDrag < static_cast<int>(root->bursts.size())
        && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        root->bursts[static_cast<size_t>(burstDrag)].time = xToTime(mouse.x);
        ImGui::SetTooltip("%.2fs", root->bursts[static_cast<size_t>(burstDrag)].time);
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && burstDrag >= 0) {
        storage->SetInt(burstDragId, -1);
        if (ctx.markSceneDirty) ctx.markSceneDirty();
    }

    // マーカー右クリック → 詳細編集ポップアップ
    if (hovered && hoveredBurst >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        storage->SetInt(burstEditId, hoveredBurst);
        ImGui::OpenPopup("##vfx_burst_popup");
    }
    if (ImGui::BeginPopup("##vfx_burst_popup")) {
        const int editIndex = storage->GetInt(burstEditId, -1);
        if (editIndex >= 0 && editIndex < static_cast<int>(root->bursts.size())) {
            auto& burst = root->bursts[static_cast<size_t>(editIndex)];
            bool burstChanged = false;
            ImGui::TextDisabled("Burst %d", editIndex + 1);
            burstChanged |= ImGui::DragFloat("Time", &burst.time, 0.01f, 0.0f, span);
            burstChanged |= ImGui::DragInt("Count", &burst.count, 1, 0, 100000);
            burstChanged |= ImGui::DragInt("Cycles", &burst.cycles, 1, 1, 1000);
            burstChanged |= ImGui::DragFloat("Interval", &burst.interval, 0.01f, 0.0f, 300.0f);
            burstChanged |= ImGui::DragFloat("Probability", &burst.probability, 0.01f, 0.0f, 1.0f);
            ImGui::Separator();
            if (ImGui::MenuItem("Delete Burst")) {
                root->bursts.erase(root->bursts.begin() + editIndex);
                burstChanged = true;
                ImGui::CloseCurrentPopup();
            }
            if (burstChanged && ctx.markSceneDirty)
                ctx.markSceneDirty();
        } else {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // 下段の空きをダブルクリック → その時刻に Burst 追加
    if (hovered && hoveredBurst < 0 && mouse.y > rulerBottom
        && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        scene::ParticleBurst burst;
        burst.time = xToTime(mouse.x);
        root->bursts.push_back(burst);
        if (ctx.markSceneDirty) ctx.markSceneDirty();
    }

    // ── 再生ヘッド (上段クリック / ドラッグで決定論スクラブ) ──
    const bool scrubbing = storage->GetBool(scrubId, false);
    if (hovered && hoveredBurst < 0 && mouse.y <= rulerBottom
        && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        storage->SetBool(scrubId, true);
    if (storage->GetBool(scrubId, false)) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const float t = xToTime(mouse.x);
            if (!continuous) {
                RequestScrub(ctx, group, t);
                m_paused = true; // Unity 同様、スクラブ中は一時停止して時刻を固定する
            }
            ImGui::SetTooltip("%.2fs", t);
        } else {
            storage->SetBool(scrubId, false);
        }
    }
    if (continuous && hovered && mouse.y <= rulerBottom)
        ImGui::SetTooltip("Duration is 0 (continuous) - set a Duration to enable scrubbing");

    // 再生ヘッド描画は最後 (マーカーの上に重ねる)
    {
        const float x = timeToX(root->playTime);
        draw->AddLine({ x, rectMin.y }, { x, rectMax.y }, IM_COL32(120, 200, 255, 255), 2.0f);
        draw->AddTriangleFilled({ x - 5.0f, rectMin.y }, { x + 5.0f, rectMin.y },
                                { x, rectMin.y + 7.0f }, IM_COL32(120, 200, 255, 255));
    }
    (void)scrubbing;
}

void VFXEditorPanel::DrawStats(EditorContext& ctx,
                               const std::vector<scene::EntityID>& group,
                               scene::ParticleEmitter* root)
{
    // グループ合計とルート詳細を1行で。パフォーマンス確認をパネル内で完結させる。
    int totalParticles = 0;
    int totalVisible = 0;
    if (ctx.activeScene) {
        for (const scene::EntityID id : group) {
            if (const auto* emitter = ctx.activeScene->GetComponent<scene::ParticleEmitter>(id)) {
                totalParticles += emitter->simulationMode == scene::ParticleSimulationMode::Gpu
                    ? emitter->visibleParticleCount
                    : static_cast<int>(emitter->particles.size());
                totalVisible += emitter->visibleParticleCount;
            }
        }
    }
    ImGui::TextDisabled("Emitters: %d   Particles: %d (visible %d)   Root: %s / %s%s",
                        static_cast<int>(group.size()),
                        totalParticles, totalVisible,
                        root->simulationMode == scene::ParticleSimulationMode::Gpu ? "GPU" : "CPU",
                        root->simulationSpace == scene::ParticleSimulationSpace::Local ? "Local" : "World",
                        root->isCulledThisFrame ? "   [CULLED]" : "");
}

} // namespace fbzz::editor
