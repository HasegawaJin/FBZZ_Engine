/// @file    ParticleEmitterModules.cpp
/// @brief   ParticleEmitter のモジュールスタック UI (Emission / Shape / Forces ほか)。
/// @author  Hasegawa Jin
/// @date    2026-07-15

#include <Editor/Util/ParticleEmitterModules.hpp>

#include <Editor/EditorContext.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Editor/Util/ParticleEditWidgets.hpp>
#include <Editor/Util/ParticleMaterialFactory.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Asset/VelocityFieldAtlas.hpp>
#include <Engine/Scene/Components/ParticleGpuSimulation.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <imgui.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <string>
#include <unordered_map>

namespace fbzz::editor {
namespace {

// チェックボックス付き折りたたみモジュールヘッダー (Unity Shuriken 風)。
// enabled == nullptr のモジュールは常時有効でチェックボックスを出さない。
// 開いている間は PushID(name) が生きているため、モジュール間でフィールドラベルが重複しても安全。
// 必ず true を返した場合のみ EndModule() を呼ぶこと。
bool BeginModule(const char* name, bool* enabled, bool defaultOpen, bool& changed)
{
    ImGui::PushID(name);
    if (enabled) {
        changed |= ImGui::Checkbox("##module_en", enabled);
        ImGui::SameLine();
    }
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_AllowOverlap;
    if (defaultOpen) flags |= ImGuiTreeNodeFlags_DefaultOpen;
    // 無効モジュールはヘッダー文字を落として「使われていない」ことを見せる (Unity 同様)
    const bool disabled = enabled && !*enabled;
    if (disabled)
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    const bool open = ImGui::CollapsingHeader(name, flags);
    if (disabled)
        ImGui::PopStyleColor();
    if (!open) {
        ImGui::PopID();
        return false;
    }
    ImGui::Indent(8.0f);
    return true;
}

void EndModule()
{
    ImGui::Unindent(8.0f);
    ImGui::Spacing();
    ImGui::PopID();
}

} // namespace

// 力場 1 本ぶんの編集。型によって意味のある項目だけを出す。
// WHY 型で出し分けるか: 12 項目すべてを常に出すと、Wind に noiseFrequency が、
//     Turbulence に direction が並ぶ。効かない値が編集できると «設定したのに変わらない» になる。
// WHY 公開するか: 同じ設定型がシーンの ParticleForceField とエミッター内蔵の力の
//     両方で使われる。UI が 2 本あると «片方にだけ Vector Field 欄が無い» が起きる。
bool DrawParticleForceFieldSettings(scene::ParticleForceFieldSettings& force, EditorContext& ctx,
                                    bool showSpace)
{
    using Type  = scene::ParticleForceFieldType;
    using Space = scene::ParticleForceFieldSpace;

    static const char* kTypeLabels[] = {
        "Wind (constant)", "Attract", "Repulse", "Vortex", "Turbulence", "Drag", "Vector Field"
    };
    static_assert(static_cast<int>(std::size(kTypeLabels)) == scene::kParticleForceFieldTypeCount,
                  "力の種類を足したらラベルも足すこと");

    bool changed = false;
    changed |= ImGui::Checkbox("Enabled", &force.enabled);

    int typeValue = static_cast<int>(force.fieldType);
    if (ImGui::Combo("Type", &typeValue, kTypeLabels, scene::kParticleForceFieldTypeCount)) {
        force.fieldType = static_cast<Type>(typeValue);
        changed = true;
    }

    const bool needsOrigin = force.fieldType == Type::Attract || force.fieldType == Type::Repulse
                          || force.fieldType == Type::Vortex  || force.fieldType == Type::VectorField;
    if (showSpace && needsOrigin) {
        int spaceValue = static_cast<int>(force.space);
        static const char* kSpaceLabels[] = { "World", "Follow Emitter" };
        if (ImGui::Combo("Space", &spaceValue, kSpaceLabels, 2)) {
            force.space = static_cast<Space>(spaceValue);
            changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Follow Emitter = 原点と向きがエミッターに追従します。\n"
                              "渦や吸い込みはこちら。重力や乱流は World のままにしてください。");
    }

    const char* strengthLabel = force.fieldType == Type::Drag ? "Damping [1/s]" : "Strength [m/s^2]";
    changed |= ImGui::DragFloat(strengthLabel, &force.strength, 0.05f, -1000.0f, 1000.0f);

    if (force.fieldType == Type::Wind || force.fieldType == Type::Vortex)
        changed |= widgets::DragVec3(force.fieldType == Type::Vortex ? "Axis" : "Direction",
                                     force.direction, 0.01f);

    if (force.fieldType != Type::VectorField) {
        changed |= ImGui::DragFloat("Radius [m]", &force.radius, 0.05f, 0.0f, 1000.0f);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("0 = 減衰なしで全体に効きます (重力・環境風はこちら)。");
        if (force.radius > 0.0f)
            changed |= ImGui::DragFloat("Falloff Power", &force.falloffPower, 0.01f, 0.01f, 16.0f);
    }

    if (force.fieldType == Type::Turbulence) {
        changed |= ImGui::DragFloat("Noise Frequency [1/m]", &force.noiseFrequency, 0.01f, 0.001f, 100.0f);
        changed |= ImGui::DragFloat("Noise Scroll Speed", &force.noiseSpeed, 0.01f, -100.0f, 100.0f);
    }

    if (force.fieldType == Type::VectorField) {
        changed |= widgets::AssetPathField("Field", force.vectorFieldPath, ".vfield,.fga",
                                           ctx.projectRoot);
        changed |= widgets::DragVec3("Extents [m]", force.vectorFieldExtents, 0.05f);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("焼いた場をワールドのどの寸法へ貼るか (半径)。\n"
                              "同じ 1 枚を «部屋いっぱいの渦» と «手のひらの渦» に使い回せます。");
        changed |= ImGui::SliderFloat("Tightness", &force.vectorFieldTightness, 0.0f, 1.0f);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("0 = 場を加速度として足すだけ。\n"
                              "1 に寄せるほど初速も重力も無視して流れに乗ります (レール表現)。");
        if (force.vectorFieldPath.empty())
            ImGui::TextDisabled("Vector Field が未設定のため、この力は何もしません。");
        else
            ImGui::TextDisabled("CPU / GPU どちらのシミュレーションでも効きます\n"
                                "(常駐できる場は %u 枚まで)。",
                                asset::VelocityFieldAtlas::kMaxTiles);
    }
    return changed;
}

namespace {

// 内蔵の力のリスト。追加・削除を持つ。
bool DrawLocalForceList(scene::ParticleEmitterSettings& pe, EditorContext& ctx)
{
    using Type = scene::ParticleForceFieldType;
    static const char* kShortNames[] = {
        "Wind", "Attract", "Repulse", "Vortex", "Turbulence", "Drag", "Vector Field"
    };

    bool changed = false;
    int removeIndex = -1;
    for (std::size_t index = 0; index < pe.localForces.size(); ++index) {
        ImGui::PushID(static_cast<int>(index));
        auto& force = pe.localForces[index];
        char header[96];
        std::snprintf(header, sizeof(header), "%zu. %s###force", index,
                      kShortNames[static_cast<int>(force.fieldType)]);
        const bool open = ImGui::TreeNodeEx(header, ImGuiTreeNodeFlags_DefaultOpen
                                                  | ImGuiTreeNodeFlags_AllowOverlap);
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 18.0f);
        if (ImGui::SmallButton("x")) removeIndex = static_cast<int>(index);
        if (open) {
            changed |= DrawParticleForceFieldSettings(force, ctx);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (removeIndex >= 0) {
        pe.localForces.erase(pe.localForces.begin() + removeIndex);
        changed = true;
    }

    if (ImGui::Button("Add Force")) ImGui::OpenPopup("add_force");
    if (ImGui::BeginPopup("add_force")) {
        for (int type = 0; type < scene::kParticleForceFieldTypeCount; ++type) {
            if (!ImGui::MenuItem(kShortNames[type])) continue;
            // EnsureLocalForce は «同じ型が既にあれば足さない»。ここでは «2 本目の Wind» を
            // 作れる必要がある (別方向の風、別半径の吸引) ので直接足す。
            scene::ParticleForceFieldSettings force;
            force.fieldType = static_cast<Type>(type);
            force.radius = 0.0f;
            if (type == static_cast<int>(Type::Vortex) || type == static_cast<int>(Type::Repulse)
                || type == static_cast<int>(Type::Attract))
                force.space = scene::ParticleForceFieldSpace::Emitter;
            pe.localForces.push_back(force);
            changed = true;
        }
        ImGui::EndPopup();
    }
    if (pe.localForces.empty())
        ImGui::TextDisabled("力がありません。粒子は初速のまま等速で飛びます。");
    return changed;
}

// シーン内の ParticleEmitter 持ち GameObject 名から選ぶ SubEmitter 用コンボ。
// WHY: 旧 UI は手打ちの InputText で、GameObject 名のタイポに気付けなかった。
bool SubEmitterCombo(const char* label, std::string& target, EditorContext& ctx)
{
    bool changed = false;
    const char* preview = target.empty() ? "(None)" : target.c_str();
    if (!ImGui::BeginCombo(label, preview))
        return false;
    if (ImGui::Selectable("(None)", target.empty()) && !target.empty()) {
        target.clear();
        changed = true;
    }
    if (ctx.activeScene) {
        for (auto& go : ctx.activeScene->GameObjects()) {
            if (!go.GetComponent<scene::ParticleEmitter>()) continue;
            const bool selected = target == go.name;
            if (ImGui::Selectable(go.name.c_str(), selected) && !selected) {
                target = go.name;
                changed = true;
            }
        }
    }
    ImGui::EndCombo();
    return changed;
}

// Shape のプレビュー付き 2D ギズモ。ドラッグで半径 / Extents を直接編集できる。
bool DrawShapeGizmo(scene::ParticleEmitterSettings& pe)
{
    bool changed = false;
    const ImVec2 gizmoSize(180.0f, 120.0f);
    ImGui::InvisibleButton("##shape_gizmo", gizmoSize);
    const ImVec2 gizmoMin = ImGui::GetItemRectMin();
    const ImVec2 gizmoMax = ImGui::GetItemRectMax();
    const ImVec2 center((gizmoMin.x + gizmoMax.x) * 0.5f, (gizmoMin.y + gizmoMax.y) * 0.5f);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(gizmoMin, gizmoMax, IM_COL32(24, 28, 36, 255), 3.0f);
    draw->AddLine({ center.x - 70.0f, center.y }, { center.x + 70.0f, center.y }, IM_COL32(90, 90, 100, 255));
    draw->AddLine({ center.x, center.y - 45.0f }, { center.x, center.y + 45.0f }, IM_COL32(90, 90, 100, 255));
    if (pe.shape == scene::ParticleEmitterShape::Sphere) {
        draw->AddCircle(center, (std::min)(pe.sphereRadius * 35.0f, 55.0f), IM_COL32(80, 190, 255, 255), 32, 2.0f);
    } else if (pe.shape == scene::ParticleEmitterShape::Box) {
        const ImVec2 ext((std::min)(pe.boxExtents.x * 25.0f, 70.0f), (std::min)(pe.boxExtents.y * 25.0f, 45.0f));
        draw->AddRect({ center.x - ext.x, center.y - ext.y }, { center.x + ext.x, center.y + ext.y },
                      IM_COL32(120, 230, 150, 255), 0.0f, 0, 2.0f);
    } else if (pe.shape == scene::ParticleEmitterShape::Cone) {
        const float radius = (std::min)(pe.coneRadius * 25.0f, 55.0f);
        draw->AddTriangle({ center.x, center.y - 40.0f }, { center.x - radius, center.y + 40.0f },
                          { center.x + radius, center.y + 40.0f }, IM_COL32(255, 190, 80, 255), 2.0f);
    } else {
        draw->AddCircleFilled(center, 4.0f, IM_COL32(255, 220, 100, 255));
    }
    if (ImGui::IsItemActive()) {
        const ImVec2 delta = ImGui::GetIO().MouseDelta;
        if (delta.x != 0.0f || delta.y != 0.0f) changed = true;
        if (pe.shape == scene::ParticleEmitterShape::Sphere)
            pe.sphereRadius = (std::max)(0.0f, pe.sphereRadius + delta.x * 0.01f);
        else if (pe.shape == scene::ParticleEmitterShape::Cone)
            pe.coneRadius = (std::max)(0.0f, pe.coneRadius + delta.x * 0.01f);
        else if (pe.shape == scene::ParticleEmitterShape::Box) {
            pe.boxExtents.x = (std::max)(0.0f, pe.boxExtents.x + delta.x * 0.01f);
            pe.boxExtents.y = (std::max)(0.0f, pe.boxExtents.y - delta.y * 0.01f);
        }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("Drag to resize the emitter shape");
    return changed;
}

// モジュールを «オフ» にしたときに捨てられてしまう値の控え。
//
// WHY 要るか:
//   Noise と Collision には専用の有効フラグが無く、noiseStrength > 0 /
//   collisionMode != None を «有効» と読み替えている。そのためチェックを外すと
//   調整した強度や Plane / Depth の選択がその場で消え、入れ直すと既定値に戻る。
//   編集セッションの間だけ控えておけば «うっかり外した» が元に戻せる。
// WHY 設定のアドレスで引くか: この控えは UI の利便のためだけのもので、保存もしないし
//     残っていなくても正しく動く。エディタを開き直せば消えて構わない。
template<typename T>
[[nodiscard]] T& RememberedModuleValue(const void* key, T fallback)
{
    static std::unordered_map<const void*, T> remembered;
    const auto [it, inserted] = remembered.try_emplace(key, fallback);
    return it->second;
}

} // namespace

void DrawParticleEmitterPlaybackButtons(scene::ParticleEmitter& pe)
{
    if (ImGui::Button(pe.settings.playing ? "Pause" : "Play")) {
        if (pe.settings.playing) pe.Pause();
        else                     pe.Play();
    }
    ImGui::SameLine();
    if (ImGui::Button("Restart")) pe.Play(true);
    ImGui::SameLine();
    if (ImGui::Button("Stop")) pe.Stop();
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        pe.ClearParticles();
    }
    ImGui::SameLine();
    if (ImGui::Button("Burst")) pe.Burst(10);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("Emit 10 particles immediately");
}

bool DrawParticleEmitterModules(scene::ParticleEmitterSettings& pe, EditorContext& ctx,
                                scene::ParticleEmitter* liveEmitter)
{
    // 実体が無い (VFX グラフのノードを編集している) ときは、再生状態のリセットを捨て先へ流す。
    // 分岐を書かずに済ませることで、以降のモジュール実装が実体の有無を意識しなくてよくなる。
    scene::ParticleRuntime discardedRuntime;
    scene::ParticleRuntime& runtime =
        liveEmitter != nullptr ? liveEmitter->runtime : discardedRuntime;
    bool changed = false;

    // GPU 縮退の判定にはフリップブック補間・MV・自己影も要るが、それらは .mat 側にある。
    // 実体の有無に関わらず同じ答えを出すため、runtime の解決結果ではなく素材から直接読む。
    const asset::ParticleMaterialSettings* look = ResolveParticleMaterialSettings(pe.materialPath);

    // ── Main: 初期値・再生設定・シミュレーション方式 ──────────────────────────
    if (BeginModule("Main", nullptr, /*defaultOpen=*/true, changed)) {
        // 再生制御は実体があるときだけ。アセット上のノードには «再生» が無い。
        if (liveEmitter != nullptr) {
            DrawParticleEmitterPlaybackButtons(*liveEmitter);
            ImGui::Spacing();
        }

        changed |= ImGui::DragFloat("Duration", &pe.duration, 0.05f, 0.0f, 300.0f);
        changed |= ImGui::Checkbox("Loop", &pe.loop);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Clear On Stop", &pe.clearOnStop);
        changed |= ImGui::DragFloat("Start Delay", &pe.startDelay, 0.05f, 0.0f, 300.0f);
        changed |= ImGui::DragFloat("Lifetime", &pe.lifetime, 0.05f, 0.1f, 30.0f);
        changed |= ImGui::DragFloat("Lifetime Random", &pe.lifetimeRandom, 0.01f, 0.0f, 1.0f);
        changed |= ImGui::DragFloat("Size Start", &pe.sizeStart, 0.005f, 0.0f, 10.0f);
        changed |= ImGui::DragFloat("Size End", &pe.sizeEnd, 0.005f, 0.0f, 10.0f);
        changed |= widgets::DragVec3("Size Axis Scale", pe.sizeAxisScale, 0.01f, 0.0f, 100.0f);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("軸ごとのサイズ倍率。X<Y で縦長 (立ち上る炎)、X>Y で平たい (衝撃波)。\n"
                              "Billboard は XY のみ、Mesh Particle は Z も使います。");
        if (ImGui::SmallButton("Uniform##SizeAxis")) { pe.sizeAxisScale = { 1, 1, 1 }; changed = true; }
        ImGui::SameLine();
        if (ImGui::SmallButton("Tall##SizeAxis")) { pe.sizeAxisScale = { 0.5f, 2.0f, 1 }; changed = true; }
        ImGui::SameLine();
        if (ImGui::SmallButton("Flat##SizeAxis")) { pe.sizeAxisScale = { 2.0f, 0.4f, 1 }; changed = true; }

        float cs[4] = { pe.colorStart.x, pe.colorStart.y, pe.colorStart.z, pe.colorStart.w };
        if (ImGui::ColorEdit4("Color Start", cs)) {
            pe.colorStart = { cs[0], cs[1], cs[2], cs[3] };
            changed = true;
        }
        float ce[4] = { pe.colorEnd.x, pe.colorEnd.y, pe.colorEnd.z, pe.colorEnd.w };
        if (ImGui::ColorEdit4("Color End", ce)) {
            pe.colorEnd = { ce[0], ce[1], ce[2], ce[3] };
            changed = true;
        }

        changed |= ImGui::SliderFloat("Color Variation", &pe.colorVariation, 0.0f, 1.0f);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("粒子ごとに RGB を独立してばらつかせます。\n"
                              "同色の粒子が集まって一枚のベタ塗りに見えるのを防ぎ、\n"
                              "炎・火花に明度差と軽い色相差が出ます。");

        // 重力と空気抵抗は内蔵の力リストの 1 本ずつだが、ここでは «いつもの 2 項目» のまま出す。
        // WHY: 保存形式がリストになったことは担当者の関心事ではない。最も触る 2 つを
        //      リストの中に沈めると、粒子を置くたびに «力を追加» から始めることになる。
        math::Vector3 gravity = pe.GravityAcceleration();
        if (widgets::DragVec3("Gravity", gravity, 0.05f)) {
            pe.SetGravityAcceleration(gravity);
            changed = true;
        }
        {
            scene::ParticleForceFieldSettings* drag =
                pe.FindLocalForce(scene::ParticleForceFieldType::Drag);
            float damping = drag ? drag->strength : 0.0f;
            if (ImGui::DragFloat("Velocity Damping", &damping, 0.01f, 0.0f, 100.0f)) {
                // 0 のときに Drag を足さないのは、力リストが «効いていない 1 本» で
                // 埋まらないようにするため。0 へ戻したら消す。
                if (damping > 0.0f)
                    pe.EnsureLocalForce(scene::ParticleForceFieldType::Drag).strength = damping;
                else if (drag)
                    pe.RemoveLocalForce(scene::ParticleForceFieldType::Drag);
                changed = true;
            }
        }
        changed |= ImGui::DragInt("Max Particles", &pe.maxParticles, 1, 1, 100000);

        int seed = static_cast<int>(pe.randomSeed);
        if (ImGui::DragInt("Random Seed", &seed, 1, 1, 2147483647)) {
            pe.randomSeed = static_cast<decltype(pe.randomSeed)>(seed);
            runtime.randomState = pe.randomSeed;
            runtime.emitAccum = 0.0f;
            changed = true;
        }

        const char* simItems[] = { "CPU", "GPU" };
        int sim = static_cast<int>(pe.simulationMode);
        if (ImGui::Combo("Simulation", &sim, simItems, 2)) {
            pe.simulationMode = static_cast<scene::ParticleSimulationMode>(sim);
            runtime.gpuClearPending = true;
            changed = true;
        }
        // GPU を選んでも、条件のどれか 1 つを外すと黙って CPU へ落ちる。
        // WHY: これが見えないと、縮退したまま粒子数だけ増やし続けることになる。
        //      原因の設定名をコンボの真下に出し、設定を触ったその場で気づけるようにする。
        if (const auto fallback = scene::GetParticleGpuFallbackReason(pe, look);
            fallback != scene::ParticleGpuFallbackReason::None
            && fallback != scene::ParticleGpuFallbackReason::NotRequested) {
            ImGui::TextColored({ 1.0f, 0.65f, 0.3f, 1.0f },
                               "GPU 指定ですが %s のため CPU で実行されます",
                               scene::ParticleGpuFallbackFieldName(fallback));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", scene::ParticleGpuFallbackDescription(fallback));
        }
        const char* spaceItems[] = { "World", "Local" };
        int simulationSpace = static_cast<int>(pe.simulationSpace);
        if (ImGui::Combo("Simulation Space", &simulationSpace, spaceItems, 2)) {
            pe.simulationSpace = static_cast<scene::ParticleSimulationSpace>(simulationSpace);
            // Local Space は CPU のみ対応 (GPU CS はワールド固定レイアウトのため)
            if (pe.simulationSpace == scene::ParticleSimulationSpace::Local)
                pe.simulationMode = scene::ParticleSimulationMode::Cpu;
            runtime.gpuClearPending = true;
            changed = true;
        }
        EndModule();
    }

    // ── Emission: 連続レート・距離レート・Burst ─────────────────────────────
    if (BeginModule("Emission", nullptr, /*defaultOpen=*/true, changed)) {
        changed |= ImGui::DragFloat("Rate over Time", &pe.emitRate, 1.0f, 0.0f, 10000.0f);
        changed |= ImGui::DragFloat("Rate over Distance", &pe.rateOverDistance, 0.1f, 0.0f, 10000.0f);
        if (ImGui::Checkbox("Prewarm", &pe.prewarm)) {
            runtime.prewarmed = false;
            runtime.prewarmSpawnPending = 0;
            changed = true;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("Loop 時に定常状態の粒子を最初から投入する");

        ImGui::SeparatorText("Bursts");
        ImGui::TextDisabled("Bursts are also editable on the VFX Editor timeline");
        if (ImGui::SmallButton("+ Add Burst")) {
            pe.bursts.push_back({});
            changed = true;
        }
        int removeBurst = -1;
        for (size_t i = 0; i < pe.bursts.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            char header[32];
            std::snprintf(header, sizeof(header), "Burst %d", static_cast<int>(i) + 1);
            ImGui::SeparatorText(header);
            changed |= ImGui::DragFloat("Time", &pe.bursts[i].time, 0.01f, 0.0f, 300.0f);
            changed |= ImGui::DragInt("Count", &pe.bursts[i].count, 1, 0, 100000);
            changed |= ImGui::DragInt("Cycles", &pe.bursts[i].cycles, 1, 1, 1000);
            changed |= ImGui::DragFloat("Interval", &pe.bursts[i].interval, 0.01f, 0.0f, 300.0f);
            changed |= ImGui::DragFloat("Probability", &pe.bursts[i].probability, 0.01f, 0.0f, 1.0f);
            if (ImGui::SmallButton("Remove")) removeBurst = static_cast<int>(i);
            ImGui::PopID();
        }
        if (removeBurst >= 0) {
            pe.bursts.erase(pe.bursts.begin() + removeBurst);
            changed = true;
        }
        EndModule();
    }

    // ── Shape: 発生形状と初速 ────────────────────────────────────────────
    if (BeginModule("Shape", nullptr, /*defaultOpen=*/false, changed)) {
        const char* shapeItems[] = { "Point", "Sphere", "Cone", "Box", "Mesh Surface (FBX)" };
        int shape = static_cast<int>(pe.shape);
        if (ImGui::Combo("Shape", &shape, shapeItems, 5)) {
            pe.shape = static_cast<scene::ParticleEmitterShape>(shape);
            changed = true;
        }
        if (pe.shape == scene::ParticleEmitterShape::Sphere)
            changed |= ImGui::DragFloat("Sphere Radius", &pe.sphereRadius, 0.01f, 0.0f, 100.0f);
        if (pe.shape == scene::ParticleEmitterShape::Cone) {
            changed |= ImGui::DragFloat("Cone Angle", &pe.coneAngleDegrees, 0.1f, 0.0f, 180.0f);
            changed |= ImGui::DragFloat("Cone Radius", &pe.coneRadius, 0.01f, 0.0f, 100.0f);
        }
        if (pe.shape == scene::ParticleEmitterShape::Box)
            changed |= widgets::DragVec3("Box Extents", pe.boxExtents, 0.01f);
        if (pe.shape == scene::ParticleEmitterShape::MeshSurface) {
            if (widgets::AssetPathField("Shape Model (FBX)", pe.meshShapePath, ".fbx,.fzasset", ctx.projectRoot)) {
                runtime.loadedMeshShapePath.clear();
                runtime.loadedMeshShapeIndex = -2;
                runtime.meshShapeVertices.clear();
                runtime.meshShapeTriangles.clear();
                changed = true;
            }
            if (ImGui::DragInt("Shape Mesh Index", &pe.meshShapeIndex, 1, -1, 1024)) {
                runtime.loadedMeshShapeIndex = -2;
                runtime.meshShapeVertices.clear();
                runtime.meshShapeTriangles.clear();
                changed = true;
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip("-1 uses every sub-mesh from the FBX");
            changed |= ImGui::DragFloat("Shape Scale", &pe.meshShapeScale, 0.001f, 0.0001f, 1000.0f);
            changed |= ImGui::Checkbox("Follow Skinned Animation", &pe.meshShapeFollowSkinnedAnimation);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip("Uses the Animator on this GameObject to deform particle spawn points");
            changed |= ImGui::DragFloat("Normal Velocity", &pe.meshShapeNormalVelocity, 0.01f, -100.0f, 100.0f);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip("Initial speed along the surface normal (m/s). Negative pulls inward");
        }

        ImGui::SeparatorText("Initial Velocity");
        changed |= widgets::DragVec3("Emit Position", pe.emitPosition);
        changed |= widgets::DragVec3("Emit Velocity", pe.emitVelocity);
        changed |= ImGui::DragFloat("Velocity Spread", &pe.velocitySpread, 0.01f, 0.0f, 20.0f);

        changed |= DrawShapeGizmo(pe);
        EndModule();
    }

    // ── Velocity over Lifetime: 寿命に沿った速度スケールカーブ ───────────────
    if (BeginModule("Velocity over Lifetime", &pe.useVelocityCurve, /*defaultOpen=*/false, changed)) {
        if (pe.useVelocityCurve) {
            changed |= widgets::CurveEditor("Speed Multiplier", pe.velocityCurve, 10.0f, 96.0f, &ctx.projectRoot);
        } else {
            ImGui::TextDisabled("Enable the checkbox to scale particle speed over lifetime.");
        }
        EndModule();
    }

    // ── Size over Lifetime: カーブ or べき乗フォールバック ────────────────────
    if (BeginModule("Size over Lifetime", &pe.useSizeCurve, /*defaultOpen=*/false, changed)) {
        if (pe.useSizeCurve) {
            changed |= widgets::CurveEditor("Size (Start -> End blend)", pe.sizeCurve, 1.0f, 96.0f, &ctx.projectRoot);
        } else {
            // カーブ無効時は Start->End の補間カーブ形状をべき乗で調整する既定動作
            changed |= ImGui::DragFloat("Size Curve Power", &pe.sizeCurvePower, 0.01f, 0.01f, 10.0f);
            ImGui::TextDisabled("pow(t, power) blend. Enable the checkbox for a custom curve.");
        }
        EndModule();
    }

    // ── Color over Lifetime: グラデーション or べき乗フォールバック ────────────
    if (BeginModule("Color over Lifetime", &pe.useColorGradient, /*defaultOpen=*/false, changed)) {
        if (pe.useColorGradient) {
            changed |= widgets::GradientEditor("Color Gradient", pe.colorGradient, &ctx.projectRoot);
            if (pe.blackbodyEnabled)
                ImGui::TextColored({ 1.0f, 0.78f, 0.35f, 1.0f },
                    "Blackbody 有効: RGB は色温度から作られ、ここでの RGB は使われません (アルファのみ有効)");
        } else {
            changed |= ImGui::DragFloat("Color Curve Power", &pe.colorCurvePower, 0.01f, 0.01f, 10.0f);
            ImGui::TextDisabled("Start/End color blend. Enable the checkbox for a gradient.");
        }
        EndModule();
    }

    if (BeginModule("Blackbody (Temperature)", &pe.blackbodyEnabled, /*defaultOpen=*/false, changed)) {
        if (pe.blackbodyEnabled) {
            if (!pe.useColorGradient)
                ImGui::TextColored({ 1.0f, 0.78f, 0.35f, 1.0f },
                    "Color over Lifetime を有効にしてください (アルファはグラデーション側が持ちます)");
            changed |= widgets::CurveEditor("Temperature (K)", pe.temperatureCurve, 3000.0f, 96.0f, &ctx.projectRoot);
            ImGui::TextDisabled("炎 1000-1600K / 溶鉄 1800K / 爆轟閃光 3000K+ / 落雷 20000K");
            changed |= ImGui::DragFloat("Reference (K)", &pe.blackbodyReferenceTemperature,
                                        10.0f, 500.0f, 12000.0f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("この温度で Intensity どおりの明るさになります。\n"
                                  "輝度は T^4 に比例するので、これより高い部分は HDR で白熱し、\n"
                                  "低い部分は彩度を保ったまま暗く沈みます。");
            changed |= ImGui::DragFloat("Intensity", &pe.blackbodyIntensity, 0.01f, 0.0f, 100.0f);
            // 実際に焼き込まれる色を出す。数値だけでは温度と見た目が結び付かない。
            scene::RefreshParticleRuntimeGradient(pe, runtime);
            ImGui::TextDisabled("Baked");
            ImGui::SameLine();
            for (uint32_t i = 0; i < runtime.runtimeGradient.keyCount; ++i) {
                if (i > 0) ImGui::SameLine();
                const auto& color = runtime.runtimeGradient.keys[i].color;
                ImGui::ColorButton("##baked",
                    ImVec4(color.x, color.y, color.z, 1.0f),
                    ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                    ImVec2(28.0f, 16.0f));
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("t=%.2f  %.0fK  rgb(%.2f, %.2f, %.2f)",
                        runtime.runtimeGradient.keys[i].time,
                        pe.temperatureCurve.Evaluate(runtime.runtimeGradient.keys[i].time),
                        color.x, color.y, color.z);
                }
            }
        }
        EndModule();
    }

    // ── Rotation over Lifetime ─────────────────────────────────────────
    if (BeginModule("Rotation over Lifetime", nullptr, /*defaultOpen=*/false, changed)) {
        changed |= ImGui::DragFloat("Angular Velocity Min", &pe.angularVelocityMin, 0.01f, -100.0f, 100.0f);
        changed |= ImGui::DragFloat("Angular Velocity Max", &pe.angularVelocityMax, 0.01f, -100.0f, 100.0f);
        changed |= ImGui::Checkbox("Use Rotation Curve", &pe.useRotationCurve);
        if (pe.useRotationCurve) {
            changed |= widgets::CurveEditor("Spin Multiplier", pe.rotationCurve, 2.0f, 96.0f, &ctx.projectRoot);
            ImGui::TextDisabled("角速度への時間倍率。頭を高く末尾を 0 にすると"
                                "「勢いよく回り始めて止まる」破片になります。");
        }
        EndModule();
    }

    // ── Drag over Lifetime: velocityDamping への時間倍率 ────────────────────
    if (BeginModule("Drag over Lifetime", &pe.useDragCurve, /*defaultOpen=*/false, changed)) {
        if (pe.useDragCurve) {
            changed |= widgets::CurveEditor("Drag Multiplier", pe.dragCurve, 4.0f, 96.0f, &ctx.projectRoot);
            ImGui::TextDisabled("Main の Velocity Damping に掛かります。"
                                "後半を高くすると噴き出した後で急に空気抵抗が効きます。");
        } else {
            ImGui::TextDisabled("Enable to modulate Velocity Damping over lifetime.");
        }
        EndModule();
    }

    // ── Forces: 内蔵の力そのもの (重力・空気抵抗も含む) ─────────────────────
    if (BeginModule("Forces", nullptr, /*defaultOpen=*/false, changed)) {
        changed |= DrawLocalForceList(pe, ctx);
        EndModule();
    }

    // ── Velocity Modules: 移動の引き継ぎ ─────────────────────────────────
    if (BeginModule("Velocity Modules", nullptr, /*defaultOpen=*/false, changed)) {
        changed |= ImGui::SliderFloat("Inherit Velocity", &pe.inheritVelocity, 0.0f, 1.0f);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("発生時にエミッター自身の移動速度を初速へ加算する割合。\n"
                              "移動する剣・ロケットの火花が引きずられて見えます。\n"
                              "Local space シミュレーションでは二重に効くため無効です。");
        if (pe.inheritVelocity > 0.0f && pe.simulationSpace == scene::ParticleSimulationSpace::Local)
            ImGui::TextDisabled("Simulation Space が Local のため Inherit Velocity は効きません。");
        EndModule();
    }

    // ── Trails: 粒子1つ1つに尾を付ける (火の粉・魔法の軌跡) ────────────────────
    if (BeginModule("Trails", &pe.trailEnabled, /*defaultOpen=*/false, changed)) {
        if (pe.trailEnabled) {
            changed |= ImGui::DragInt("Trail Points", &pe.trailPointCount, 1, 1,
                                      scene::kMaxParticleTrailPoints);
            changed |= ImGui::DragFloat("Sample Interval", &pe.trailSampleInterval,
                                        0.001f, 0.001f, 1.0f, "%.3fs");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("履歴を刻む間隔。短いほど尾が滑らかになりますが、"
                                  "同じ点数でも尾は短くなります。");
            changed |= ImGui::SliderFloat("Tip Width", &pe.trailWidthScale, 0.0f, 1.0f);
            changed |= ImGui::SliderFloat("Tip Alpha", &pe.trailAlphaScale, 0.0f, 1.0f);
            float tint[4] = { pe.trailColorTint.x, pe.trailColorTint.y,
                              pe.trailColorTint.z, pe.trailColorTint.w };
            if (ImGui::ColorEdit4("Trail Tint", tint)) {
                pe.trailColorTint = { tint[0], tint[1], tint[2], tint[3] };
                changed = true;
            }
            changed |= ImGui::Checkbox("Continuous Ribbon", &pe.trailRibbon);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "履歴点をポリラインとみなし、1 枚の連続した帯として描きます。\n"
                    "剣閃・魔法の軌跡のように「幅のある帯」が主役の表現に使います。\n"
                    "ビルボード方式は太くすると粒の連なりが露見するため、太い帯には向きません。");
            }
            if (pe.trailRibbon) {
                changed |= ImGui::DragFloat("Ribbon Width", &pe.trailRibbonWidth, 0.01f, 0.0f, 20.0f,
                                            "%.3f m");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("0 のときは粒子サイズをそのまま帯の幅として使います。");
                ImGui::TextDisabled("帯は 1 エミッターぶんをまとめて 1 DrawCall で描くため、"
                                    "色は粒子ごとではなく Color Start / End を帯の長さ方向へ配ります。");
                ImGui::TextDisabled("粒子ごとの色ゆらぎを尾へ乗せたい場合はビルボード方式のままにしてください。");
            } else {
                ImGui::TextDisabled("尾は履歴点へビルボードを連ねて描きます"
                                    "(本体と同じマテリアル・ブレンドが自動的に適用されます)。");
            }
            if (pe.simulationMode == scene::ParticleSimulationMode::Gpu)
                ImGui::TextDisabled("GPU シミュレーションは履歴を保持できないため、"
                                    "Trail 有効時は CPU で実行されます。");
        } else {
            ImGui::TextDisabled("Enable to give each particle its own trail.");
        }
        EndModule();
    }

    // Noise モジュールは Forces の Turbulence 1 本になった (専用の枠を持たない)。

    // ── External Forces: シーンの ParticleForceField を受けるか ───────────────
    if (BeginModule("External Forces", &pe.receiveForceFields, /*defaultOpen=*/false, changed)) {
        ImGui::TextDisabled("Receives Wind / Attract / Vortex / Turbulence force fields in the scene.");
        if (widgets::ForceFieldChannelMask("Channels", pe.forceFieldChannels)) changed = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Only force fields sharing a bit with this mask are applied.");
        EndModule();
    }

    // ── Collision: mode != None で有効 ─────────────────────────────────
    {
        bool collisionEnabled = pe.collisionMode != scene::ParticleCollisionMode::None;
        bool moduleChanged = false;
        auto& rememberedMode = RememberedModuleValue<scene::ParticleCollisionMode>(
            &pe.collisionMode, scene::ParticleCollisionMode::Physics);
        if (collisionEnabled) rememberedMode = pe.collisionMode;
        const bool open = BeginModule("Collision", &collisionEnabled, /*defaultOpen=*/false, moduleChanged);
        if (moduleChanged) {
            // 戻したときは «外す直前の方式»。常に Physics へ倒すと Plane / Depth の選択が消える。
            pe.collisionMode = collisionEnabled
                ? rememberedMode
                : scene::ParticleCollisionMode::None;
            changed = true;
        }
        if (open) {
            if (pe.collisionMode != scene::ParticleCollisionMode::None) {
                const char* collisionModeItems[] = { "None", "Physics", "Plane", "GPU Depth" };
                int collisionMode = static_cast<int>(pe.collisionMode);
                if (ImGui::Combo("Mode", &collisionMode, collisionModeItems, 4)) {
                    pe.collisionMode = static_cast<scene::ParticleCollisionMode>(collisionMode);
                    changed = true;
                }
                const char* collisionResponseItems[] = { "Bounce", "Kill", "Stop" };
                int response = static_cast<int>(pe.collisionResponse);
                if (ImGui::Combo("Response", &response, collisionResponseItems, 3)) {
                    pe.collisionResponse = static_cast<scene::ParticleCollisionResponse>(response);
                    changed = true;
                }
                changed |= ImGui::DragFloat("Radius", &pe.collisionRadius, 0.001f, 0.0f, 10.0f);
                changed |= ImGui::DragFloat("Bounciness", &pe.collisionBounciness, 0.01f, 0.0f, 1.0f);
                changed |= ImGui::DragFloat("Damping", &pe.collisionDamping, 0.01f, 0.0f, 1.0f);
                if (pe.collisionMode == scene::ParticleCollisionMode::Plane)
                    changed |= ImGui::DragFloat("Plane Y", &pe.collisionPlaneY, 0.01f, -10000.0f, 10000.0f);
                if (pe.collisionMode == scene::ParticleCollisionMode::Depth)
                    ImGui::TextDisabled("GPU simulation only. Uses opaque scene depth without CPU readback.");
                ImGui::Text("Collisions this frame: %d", runtime.collisionCountThisFrame);
            } else {
                ImGui::TextDisabled("Enable to collide particles with Physics World or a ground plane.");
            }
            EndModule();
        }
    }

    // ── Sub Emitters: Birth / Death / Collision イベント連鎖 ─────────────────
    if (BeginModule("Sub Emitters", nullptr, /*defaultOpen=*/false, changed)) {
        changed |= SubEmitterCombo("Birth", pe.birthSubEmitter, ctx);
        changed |= SubEmitterCombo("Death", pe.deathSubEmitter, ctx);
        changed |= SubEmitterCombo("Collision", pe.collisionSubEmitter, ctx);
        changed |= ImGui::DragInt("Burst Count", &pe.subEmitterBurstCount, 1, 1, 10000);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("Particles queued on the target emitter per event");
        EndModule();
    }

    // ── Renderer: 描画方式とマテリアル参照 ────────────────────────────────────
    // NOTE: 見た目 (ブレンド・フリップブック・歪み・煙・影) は .mat の [particle] が
    //       正本になったため、ここには無い。Material を選んで Inspector で編集する。
    if (BeginModule("Renderer", nullptr, /*defaultOpen=*/true, changed)) {
        const char* renderModeItems[] = { "Billboard", "Stretched", "Horizontal", "Vertical" };
        int renderMode = static_cast<int>(pe.renderMode);
        if (ImGui::Combo("Render Mode", &renderMode, renderModeItems, 4)) {
            pe.renderMode = static_cast<scene::ParticleRenderMode>(renderMode);
            changed = true;
        }
        if (pe.renderMode == scene::ParticleRenderMode::StretchedBillboard) {
            changed |= ImGui::DragFloat("Stretch Velocity", &pe.stretchedVelocityScale, 0.01f, 0.0f, 100.0f);
            changed |= ImGui::DragFloat("Stretch Length", &pe.stretchedLengthScale, 0.01f, 0.0f, 100.0f);
        }
        const char* sortItems[] = { "None", "Back To Front" };
        int sort = static_cast<int>(pe.sortMode);
        if (ImGui::Combo("Sort Mode", &sort, sortItems, 2)) {
            pe.sortMode = static_cast<scene::ParticleSortMode>(sort);
            changed = true;
        }
        changed |= ImGui::DragInt("Render Priority", &pe.renderPriority, 1, -1000, 1000);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("エミッター間の描画順。小さいほど先に描かれ、奥に見えます。\n"
                              "炎(小さい値)と煙(大きい値)のように重ねると前後が安定します。\n"
                              "同値のときはカメラから遠い順に描画されます。");

        // .mat 参照。変更時はキャッシュを無効化してレンダーパスに再ロードさせる。
        // テクスチャを直接落とした場合は加算の .mat を用意して差し替える。
        if (ParticleMaterialField("Material", pe.materialPath, ctx.projectRoot)) {
            runtime.loadedMaterialPath.clear();
            runtime.texture = {};
            runtime.loadedTexturePath.clear();
            changed = true;
        }
        ImGui::TextDisabled(".png/.tga/.dds もドロップできます (加算の .mat を作成)");
        if (pe.materialPath.empty())
            ImGui::TextColored({ 1.0f, 0.78f, 0.35f, 1.0f },
                "Material 未設定: ParticleFallback.mat (加算の丸い光) で描かれます");
        else
            ImGui::TextDisabled("ブレンド・フリップブック・歪み・煙・影は Material の Inspector で編集します");
        changed |= widgets::AssetPathField("Mesh Particle (optional)", pe.meshParticlePath,
                                           ".fbx,.obj,.mesh,.fzasset", ctx.projectRoot);
        if (!pe.meshParticlePath.empty())
            ImGui::TextDisabled("Mesh Particle uses deterministic CPU simulation.");
        EndModule();
    }

    // ── Culling & LOD ───────────────────────────────────────────────
    if (BeginModule("Culling & LOD", nullptr, /*defaultOpen=*/false, changed)) {
        changed |= ImGui::Checkbox("Culling Enabled", &pe.cullingEnabled);
        changed |= ImGui::DragFloat("Bounds Padding", &pe.cullingBoundsPadding, 0.01f, 0.0f, 100.0f);
        changed |= ImGui::Checkbox("Pause When Culled", &pe.pauseWhenCulled);
        changed |= ImGui::DragFloat("Screen Coverage Threshold", &pe.screenCoverageThreshold, 0.0001f, 0.0f, 1.0f, "%.4f");
        ImGui::Separator();
        changed |= ImGui::Checkbox("LOD Enabled", &pe.lodEnabled);
        changed |= ImGui::DragFloat("LOD Near Distance", &pe.lodNearDistance, 0.1f, 0.0f, 10000.0f);
        changed |= ImGui::DragFloat("LOD Far Distance", &pe.lodFarDistance, 0.1f, 0.0f, 10000.0f);
        changed |= ImGui::DragFloat("LOD Near Rate", &pe.lodNearRateScale, 0.01f, 0.0f, 1.0f);
        changed |= ImGui::DragFloat("LOD Far Rate", &pe.lodFarRateScale, 0.01f, 0.0f, 1.0f);
        ImGui::Text("Bounds: %.2f  Visible: %d  Culled: %s",
                    runtime.boundsRadius, runtime.visibleParticleCount, runtime.isCulledThisFrame ? "Yes" : "No");
        EndModule();
    }

    // ── Presets: よく使う見た目のワンクリック設定 ─────────────────────────────
    if (BeginModule("Presets", nullptr, /*defaultOpen=*/false, changed)) {
        if (ImGui::Button("Fire")) {
            pe.colorStart = { 1.0f, 0.35f, 0.05f, 1.0f };
            pe.colorEnd   = { 1.0f, 0.02f, 0.0f, 0.0f };
            pe.SetGravityAcceleration({ 0.0f, 1.5f, 0.0f });
            pe.emitRate   = 80.0f;
            pe.lifetime   = 1.2f;
            pe.shape      = scene::ParticleEmitterShape::Cone;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Smoke")) {
            pe.colorStart = { 0.25f, 0.25f, 0.25f, 0.65f };
            pe.colorEnd   = { 0.05f, 0.05f, 0.05f, 0.0f };
            pe.SetGravityAcceleration({ 0.0f, 0.3f, 0.0f });
            pe.emitRate   = 25.0f;
            pe.lifetime   = 3.0f;
            pe.shape      = scene::ParticleEmitterShape::Sphere;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Sparks")) {
            pe.colorStart = { 1.0f, 0.8f, 0.2f, 1.0f };
            pe.colorEnd   = { 1.0f, 0.1f, 0.0f, 0.0f };
            pe.SetGravityAcceleration({ 0.0f, -9.8f, 0.0f });
            pe.emitRate   = 120.0f;
            pe.lifetime   = 0.8f;
            pe.shape      = scene::ParticleEmitterShape::Cone;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Explosion")) {
            // 連続放出ではなく t=0 の Burst 一発で構成する例。タイムライン編集のデモも兼ねる。
            pe.colorStart = { 1.0f, 0.6f, 0.15f, 1.0f };
            pe.colorEnd   = { 0.4f, 0.05f, 0.0f, 0.0f };
            pe.SetGravityAcceleration({ 0.0f, -3.0f, 0.0f });
            pe.emitRate   = 0.0f;
            pe.lifetime   = 0.9f;
            pe.velocitySpread = 6.0f;
            pe.shape      = scene::ParticleEmitterShape::Sphere;
            pe.sphereRadius = 0.3f;
            pe.loop       = false;
            pe.duration   = 1.5f;
            pe.bursts.clear();
            pe.bursts.push_back({ 0.0f, 60, 1, 0.1f, 1.0f });
            if (liveEmitter != nullptr) liveEmitter->ResetPlayback();
            else                        pe.playing = true;
            changed = true;
        }
        EndModule();
    }

    return changed;
}

} // namespace fbzz::editor
