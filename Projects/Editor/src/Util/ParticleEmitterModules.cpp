/// @file    ParticleEmitterModules.cpp
/// @brief   ParticleEmitter のモジュールスタック UI (Emission / Shape / Flows ほか)。
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
#include <Engine/Scene/Components/WaterComponent.hpp>
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

/// @brief チェックボックス付き折りたたみモジュールヘッダー (Unity Shuriken 風)。
/// @brief enabled == nullptr のモジュールは常時有効でチェックボックスを出さない。
/// @brief 開いている間は PushID(name) が生きているため、モジュール間でフィールドラベルが重複しても安全。
/// @brief 必ず true を返した場合のみ EndModule() を呼ぶこと。
bool BeginModule(const char* name, bool* enabled, bool defaultOpen, bool& changed)
{
    ImGui::PushID(name);
    if (enabled) {
        changed |= ImGui::Checkbox("##module_en", enabled);
        ImGui::SameLine();
    }
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_AllowOverlap;
    if (defaultOpen) flags |= ImGuiTreeNodeFlags_DefaultOpen;
    /// @note 無効モジュールはヘッダー文字を落として「使われていない」ことを見せる (Unity 同様)
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

/// @brief 流れ 1 本ぶんの編集。型によって意味のある項目だけを出す。
/// @note シーン FlowField とエミッター内蔵の流れの両方から呼ばれる共通 UI。
/// @note 別々に実装すると欄の食い違いが起きる。
bool DrawFlowFieldSettings(scene::FlowFieldSettings& force, EditorContext& ctx,
                                    bool showSpace)
{
    using Type  = scene::FlowFieldType;
    using Space = scene::FlowFieldSpace;

    /// @note 添字 = FlowFieldType の値。LegacyDrag (5) は読み込み専用なので選択肢へ出さない
    /// @note (kSelectableTypes がその «出してよい型» の並び)。
    static const char* kTypeLabels[] = {
        "Uniform (constant flow)", "Sink", "Source", "Vortex", "Curl (turbulence)",
        "Drag (legacy)", "Baked Field"
    };
    static_assert(static_cast<int>(std::size(kTypeLabels)) == scene::kFlowFieldTypeCount,
                  "流れの種類を足したらラベルも足すこと");
    static const Type kSelectableTypes[] = {
        Type::Uniform, Type::Sink, Type::Source, Type::Vortex, Type::Curl, Type::Baked
    };

    bool changed = false;
    changed |= ImGui::Checkbox("Enabled", &force.enabled);

    {
        const char* preview = kTypeLabels[static_cast<int>(force.fieldType)];
        if (ImGui::BeginCombo("Type", preview)) {
            for (Type candidate : kSelectableTypes) {
                const bool selected = force.fieldType == candidate;
                if (ImGui::Selectable(kTypeLabels[static_cast<int>(candidate)], selected)
                    && !selected) {
                    /// @note direction は Uniform では流れの向き、Vortex では回転軸を兼ねる。既定の
                    /// @note (1,0,0) は風にしか合わず、そのまま Vortex にすると軸が寝た «ローラー» になる。
                    if (candidate == Type::Vortex) force.direction = { 0.0f, 1.0f, 0.0f };
                    force.fieldType = candidate;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
    }
    if (force.fieldType == Type::LegacyDrag) {
        ImGui::TextDisabled("旧 Drag は空気抵抗であって流れの種類ではありません。\n"
                            "Main の Flow Coupling へ移してこの 1 本は削除してください。");
    }

    const bool needsOrigin = force.fieldType == Type::Sink || force.fieldType == Type::Source
                          || force.fieldType == Type::Vortex  || force.fieldType == Type::Baked;
    if (showSpace && needsOrigin) {
        int spaceValue = static_cast<int>(force.space);
        static const char* kSpaceLabels[] = { "World", "Follow Emitter" };
        if (ImGui::Combo("Space", &spaceValue, kSpaceLabels, 2)) {
            force.space = static_cast<Space>(spaceValue);
            changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Follow Emitter = 原点と向きがエミッターに追従します。\n"
                              "渦や吸い込みはこちら。乱流は World のままにしてください。");
    }

    const char* speedLabel = force.fieldType == Type::Baked ? "Field Scale"
                           : force.fieldType == Type::LegacyDrag ? "Damping [1/s]"
                                                                 : "Speed [m/s]";
    changed |= ImGui::DragFloat(speedLabel, &force.strength, 0.05f, -1000.0f, 1000.0f);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(force.fieldType == Type::Baked
            ? "焼いた場の値 [m/s] に掛ける無次元の倍率。"
            : "その点で媒質がどう動いているか [m/s]。\n"
              "粒子がどれだけ乗るかは Main の Flow Coupling が決めます。");
    }

    if (force.fieldType == Type::Uniform || force.fieldType == Type::Vortex)
        changed |= widgets::DragVec3(force.fieldType == Type::Vortex ? "Axis" : "Direction",
                                     force.direction, 0.01f);
    if (force.fieldType == Type::Vortex) {
        const float axisLength = force.direction.Length();
        const float upright = axisLength > 1.0e-4f ? std::abs(force.direction.y) / axisLength : 0.0f;
        if (upright < scene::kWaterVortexUprightDot)
            ImGui::TextDisabled("軸が寝ています。水面に渦潮 (穴) を作るのは軸がほぼ鉛直の渦だけです。\n"
                                "Axis を (0, 1, 0) にすると掘ります (GameObject の回転も効きます)。");
    }

    if (force.fieldType != Type::Baked) {
        changed |= ImGui::DragFloat("Radius [m]", &force.radius, 0.05f, 0.0f, 1000.0f);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("0 = 減衰なしで全体に効きます。\n"
                              "シーン全体の «環境流» は Scene Environment 側にあります。");
        if (force.radius > 0.0f)
            changed |= ImGui::DragFloat("Falloff Power", &force.falloffPower, 0.01f, 0.01f, 16.0f);
    }

    if (force.fieldType == Type::Curl) {
        changed |= ImGui::DragFloat("Noise Frequency [1/m]", &force.noiseFrequency, 0.01f, 0.001f, 100.0f);
        changed |= ImGui::DragFloat("Noise Scroll Speed", &force.noiseSpeed, 0.01f, -100.0f, 100.0f);
    }

    if (force.fieldType == Type::Baked) {
        changed |= widgets::AssetPathField("Field", force.vectorFieldPath, ".png,.fga",
                                           ctx.projectRoot);
        changed |= widgets::DragVec3("Extents [m]", force.vectorFieldExtents, 0.05f);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("焼いた場をワールドのどの寸法へ貼るか (半径)。\n"
                              "同じ 1 枚を «部屋いっぱいの渦» と «手のひらの渦» に使い回せます。");
        if (force.vectorFieldPath.empty())
            ImGui::TextDisabled("Baked Field が未設定のため、この流れは何もしません。");
        else
            ImGui::TextDisabled("CPU / GPU どちらのシミュレーションでも効きます\n"
                                "(常駐できる場は %u 枚まで)。",
                                asset::VelocityFieldAtlas::kMaxTiles);
    }
    return changed;
}

/// @brief 流れのリスト。追加・削除を持つ。エミッター内蔵の流れとシーンの場が共用する。
bool DrawFlowFieldList(std::vector<scene::FlowFieldSettings>& forces,
                               EditorContext& ctx, bool showSpace)
{
    using Type = scene::FlowFieldType;
    /// @note 添字 = FlowFieldType の値。LegacyDrag は «Add Flow» に出さない (kAddableTypes)。
    static const char* kShortNames[] = {
        "Uniform", "Sink", "Source", "Vortex", "Curl", "Drag (legacy)", "Baked"
    };
    static const Type kAddableTypes[] = {
        Type::Uniform, Type::Sink, Type::Source, Type::Vortex, Type::Curl, Type::Baked
    };

    bool changed = false;
    int removeIndex = -1;
    for (std::size_t index = 0; index < forces.size(); ++index) {
        ImGui::PushID(static_cast<int>(index));
        auto& force = forces[index];
        char header[96];
        std::snprintf(header, sizeof(header), "%zu. %s###force", index,
                      kShortNames[static_cast<int>(force.fieldType)]);
        const bool open = ImGui::TreeNodeEx(header, ImGuiTreeNodeFlags_DefaultOpen
                                                  | ImGuiTreeNodeFlags_AllowOverlap);
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 18.0f);
        if (ImGui::SmallButton("x")) removeIndex = static_cast<int>(index);
        if (open) {
            changed |= DrawFlowFieldSettings(force, ctx, showSpace);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (removeIndex >= 0) {
        forces.erase(forces.begin() + removeIndex);
        changed = true;
    }

    if (ImGui::Button("Add Flow")) ImGui::OpenPopup("add_force");
    if (ImGui::BeginPopup("add_force")) {
        for (Type type : kAddableTypes) {
            if (!ImGui::MenuItem(kShortNames[static_cast<int>(type)])) continue;
            /// @note EnsureLocalForce は «同じ型が既にあれば足さない»。ここでは «2 本目の Uniform» を
            /// @note 作れる必要がある (別方向の流れ、別半径の吸い込み) ので直接足す。
            scene::FlowFieldSettings force;
            force.fieldType = type;
            force.radius = 0.0f;
            if (type == Type::Vortex) force.direction = { 0.0f, 1.0f, 0.0f };
            if (type == Type::Vortex || type == Type::Source || type == Type::Sink)
                force.space = scene::FlowFieldSpace::Emitter;
            forces.push_back(force);
            changed = true;
        }
        ImGui::EndPopup();
    }
    if (forces.empty())
        ImGui::TextDisabled("流れがありません。粒子は初速と重力だけで飛びます。");
    return changed;
}

/// @brief シーン内の ParticleEmitter 持ち GameObject 名から選ぶ SubEmitter 用コンボ。
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
            /// @note 同名の GameObject は珍しくないので、名前ではなく実体で ID を分ける。
            ImGui::PushID(&go);
            if (ImGui::Selectable(go.name.c_str(), selected) && !selected) {
                target = go.name;
                changed = true;
            }
            ImGui::PopID();
        }
    }
    ImGui::EndCombo();
    return changed;
}

namespace {

/// @brief Shape のプレビュー付き 2D ギズモ。ドラッグで半径 / Extents を直接編集できる。
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

/// @brief モジュールを «オフ» にしたときに捨てられる値の控え (編集セッション限定、保存しない)。
/// @note Noise/Collision は専用の有効フラグを持たず、強度・モードの値自体を «有効/無効» として
/// @note 扱う。オフにすると値が消えるため、ここに控えて再度オンにしたとき復元する。
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
    /// @note 実体が無い (VFX グラフのノードを編集している) ときは、再生状態のリセットを捨て先へ流す。
    /// @note 分岐を書かずに済ませることで、以降のモジュール実装が実体の有無を意識しなくてよくなる。
    scene::ParticleRuntime discardedRuntime;
    scene::ParticleRuntime& runtime =
        liveEmitter != nullptr ? liveEmitter->runtime : discardedRuntime;
    bool changed = false;

    /// @note GPU 縮退の判定にはフリップブック補間・MV・自己影も要るが、それらは .mat 側にある。
    /// @note 実体の有無に関わらず同じ答えを出すため、runtime の解決結果ではなく素材から直接読む。
    const asset::ParticleMaterialSettings* look = ResolveParticleMaterialSettings(pe.materialPath);

    /// @name Main: 初期値・再生設定・シミュレーション方式
    if (BeginModule("Main", nullptr, /*defaultOpen=*/true, changed)) {
        /// @note 再生制御は実体があるときだけ。アセット上のノードには «再生» が無い。
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

        /// @note 重力と結合係数は «流れ» ではないのでリストに入らない。最も触る 2 つなので
        /// @note Main へ直に出す。
        changed |= widgets::DragVec3("Gravity", pe.gravity, 0.05f);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("粒子だけに掛かる加速度 [m/s^2]。\n"
                              "物理の重力 (ProjectSettings) とは別で、絵として落ち方を決めます。");
        changed |= ImGui::DragFloat("Flow Coupling [1/s]", &pe.flowCoupling, 0.01f, 0.0f, 100.0f);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("流れにどれだけ乗るか。v += (流速 - v) * この値 * dt。\n"
                              "大きいほど素早く流れに乗り、0 で流れを完全に無視します。\n"
                              "旧 Velocity Damping と同じ枠・同じ単位です。");
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
        /// @note GPU を選んでも条件を 1 つでも外すと黙って CPU へ落ちる。原因をコンボの真下に
        /// @note 表示し、縮退に気付けるようにする。
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
            /// @note Local Space は CPU のみ対応 (GPU CS はワールド固定レイアウトのため)
            if (pe.simulationSpace == scene::ParticleSimulationSpace::Local)
                pe.simulationMode = scene::ParticleSimulationMode::Cpu;
            runtime.gpuClearPending = true;
            changed = true;
        }
        EndModule();
    }

    /// @name Emission: 連続レート・距離レート・Burst
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

    /// @name Shape: 発生形状と初速
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

    /// @name Velocity over Lifetime: 寿命に沿った速度スケールカーブ
    if (BeginModule("Velocity over Lifetime", &pe.useVelocityCurve, /*defaultOpen=*/false, changed)) {
        if (pe.useVelocityCurve) {
            changed |= widgets::CurveEditor("Speed Multiplier", pe.velocityCurve, 10.0f, 96.0f, &ctx.projectRoot);
        } else {
            ImGui::TextDisabled("Enable the checkbox to scale particle speed over lifetime.");
        }
        EndModule();
    }

    /// @name Size over Lifetime: カーブ or べき乗フォールバック
    if (BeginModule("Size over Lifetime", &pe.useSizeCurve, /*defaultOpen=*/false, changed)) {
        if (pe.useSizeCurve) {
            changed |= widgets::CurveEditor("Size (Start -> End blend)", pe.sizeCurve, 1.0f, 96.0f, &ctx.projectRoot);
        } else {
            /// @note カーブ無効時は Start->End の補間カーブ形状をべき乗で調整する既定動作
            changed |= ImGui::DragFloat("Size Curve Power", &pe.sizeCurvePower, 0.01f, 0.01f, 10.0f);
            ImGui::TextDisabled("pow(t, power) blend. Enable the checkbox for a custom curve.");
        }
        EndModule();
    }

    /// @name Color over Lifetime: グラデーション or べき乗フォールバック
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
            /// @note 実際に焼き込まれる色を出す。数値だけでは温度と見た目が結び付かない。
            scene::RefreshParticleRuntimeGradient(pe, runtime);
            ImGui::TextDisabled("Baked");
            ImGui::SameLine();
            for (uint32_t i = 0; i < runtime.runtimeGradient.keyCount; ++i) {
                if (i > 0) ImGui::SameLine();
                const auto& color = runtime.runtimeGradient.keys[i].color;
                ImGui::PushID(static_cast<int>(i));
                ImGui::ColorButton("##baked",
                    ImVec4(color.x, color.y, color.z, 1.0f),
                    ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                    ImVec2(28.0f, 16.0f));
                ImGui::PopID();
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

    /// @name Rotation over Lifetime
    if (BeginModule("Rotation over Lifetime", nullptr, /*defaultOpen=*/false, changed)) {
        changed |= ImGui::Checkbox("Random Start Rotation", &pe.randomStartRotation);
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

    /// @name Drag over Lifetime: flowCoupling への時間倍率
    if (BeginModule("Drag over Lifetime", &pe.useDragCurve, /*defaultOpen=*/false, changed)) {
        if (pe.useDragCurve) {
            changed |= widgets::CurveEditor("Drag Multiplier", pe.dragCurve, 4.0f, 96.0f, &ctx.projectRoot);
            ImGui::TextDisabled("Main の Flow Coupling に掛かります。"
                                "後半を高くすると噴き出した後で急に空気抵抗が効きます。");
        } else {
            ImGui::TextDisabled("Enable to modulate Flow Coupling over lifetime.");
        }
        EndModule();
    }

    /// @name Flows: 内蔵の流れ (乱流・渦・湧き出し・焼いた場)
    if (BeginModule("Flows", nullptr, /*defaultOpen=*/false, changed)) {
        changed |= DrawFlowFieldList(pe.localForces, ctx, /*showSpace=*/true);
        EndModule();
    }

    /// @name Velocity Modules: 移動の引き継ぎ
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

    /// @name Trails: 粒子1つ1つに尾を付ける (火の粉・魔法の軌跡)
    if (BeginModule("Trails", &pe.trail.trailEnabled, /*defaultOpen=*/false, changed)) {
        if (pe.trail.trailEnabled) {
            changed |= ImGui::DragInt("Trail Points", &pe.trail.trailPointCount, 1, 1,
                                      scene::kMaxParticleTrailPoints);
            changed |= ImGui::DragFloat("Sample Interval", &pe.trail.trailSampleInterval,
                                        0.001f, 0.001f, 1.0f, "%.3fs");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("履歴を刻む間隔。短いほど尾が滑らかになりますが、"
                                  "同じ点数でも尾は短くなります。");
            changed |= ImGui::SliderFloat("Tip Width", &pe.trail.trailWidthScale, 0.0f, 1.0f);
            changed |= ImGui::SliderFloat("Tip Alpha", &pe.trail.trailAlphaScale, 0.0f, 1.0f);
            float tint[4] = { pe.trail.trailColorTint.x, pe.trail.trailColorTint.y,
                              pe.trail.trailColorTint.z, pe.trail.trailColorTint.w };
            if (ImGui::ColorEdit4("Trail Tint", tint)) {
                pe.trail.trailColorTint = { tint[0], tint[1], tint[2], tint[3] };
                changed = true;
            }
            changed |= ImGui::Checkbox("Continuous Ribbon", &pe.trail.trailRibbon);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "履歴点をポリラインとみなし、1 枚の連続した帯として描きます。\n"
                    "剣閃・魔法の軌跡のように「幅のある帯」が主役の表現に使います。\n"
                    "ビルボード方式は太くすると粒の連なりが露見するため、太い帯には向きません。");
            }
            if (pe.trail.trailRibbon) {
                changed |= ImGui::DragFloat("Ribbon Width", &pe.trail.trailRibbonWidth, 0.01f, 0.0f, 20.0f,
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

    /// @note Noise モジュールは Forces の Curl 1 本になった (専用の枠を持たない)。

    /// @name External Flow: シーンの FlowField と環境流を受けるか
    if (BeginModule("External Flow", &pe.receiveFlowFields, /*defaultOpen=*/false, changed)) {
        ImGui::TextDisabled("Receives Flow Fields and the scene's ambient wind.");
        if (widgets::FlowFieldChannelMask("Channels", pe.flowFieldChannels)) changed = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Only flow fields sharing a bit with this mask are applied.");
        EndModule();
    }

    /// @name Collision: mode != None で有効
    {
        bool collisionEnabled = pe.collisionMode != scene::ParticleCollisionMode::None;
        bool moduleChanged = false;
        auto& rememberedMode = RememberedModuleValue<scene::ParticleCollisionMode>(
            &pe.collisionMode, scene::ParticleCollisionMode::Physics);
        if (collisionEnabled) rememberedMode = pe.collisionMode;
        const bool open = BeginModule("Collision", &collisionEnabled, /*defaultOpen=*/false, moduleChanged);
        if (moduleChanged) {
            /// @note 戻したときは «外す直前の方式»。常に Physics へ倒すと Plane / Depth の選択が消える。
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
                if (pe.collisionMode == scene::ParticleCollisionMode::Physics) {
                    if (widgets::FlowFieldChannelMask("Collision Layers", pe.collisionLayerMask))
                        changed = true;
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("当たってよいコライダーのレイヤーです (ビット n = レイヤー n)。\n"
                                          "既定は全レイヤー。地面だけに当てたいときはここで絞ります。");
                    ImGui::TextDisabled("トリガーには当たりません (通過を検知する体積で、跳ね返る面ではないため)");
                }
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

    /// @name Sub Emitters: Birth / Death / Collision イベント連鎖
    if (BeginModule("Sub Emitters", nullptr, /*defaultOpen=*/false, changed)) {
        changed |= SubEmitterCombo("Birth", pe.birthSubEmitter, ctx);
        changed |= SubEmitterCombo("Death", pe.deathSubEmitter, ctx);
        changed |= SubEmitterCombo("Collision", pe.collisionSubEmitter, ctx);
        changed |= ImGui::DragInt("Burst Count", &pe.subEmitterBurstCount, 1, 1, 10000);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("Particles queued on the target emitter per event");
        ImGui::TextDisabled("イベントの «発生した位置» が対象エミッターへ渡ります "
                            "(当たった点 / 消えた点)");
        ImGui::Separator();
        changed |= ImGui::SliderFloat("Inherit Velocity##subEmitter",
                                      &pe.subEmitterInheritVelocity, 0.0f, 1.0f);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("このエミッターが «発火元の粒子» に呼ばれたとき、その速度を初速へ継ぐ割合です。\n"
                              "0 (既定) なら継ぎません。丸ごと継ぐと発火元と同じ向きへ流れるだけになるので、\n"
                              "当たりの火花は 0.1〜0.3 程度から試してください。\n"
                              "エミッター自身の移動を継ぐ Emission の Inherit Velocity とは別の値です。");
        EndModule();
    }

    /// @name Renderer: 描画方式とマテリアル参照
    /// @note 見た目 (ブレンド・フリップブック・歪み・煙・影) は .mat の [particle] が
    /// @note 正本になったため、ここには無い。Material を選んで Inspector で編集する。
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

        /// @note .mat 参照。変更時はキャッシュを無効化してレンダーパスに再ロードさせる。
        /// @note テクスチャを直接落とした場合は加算の .mat を用意して差し替える。
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
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("«メッシュ粒子として描くか» のスイッチです。空でなければ有効。\n"
                              "指定したパスは読み込まれません — 形は同じ GameObject の\n"
                              "MeshRenderer が持ちます (互換のため型は文字列のまま)。");
        if (!pe.meshParticlePath.empty()) {
            /// @note «別のモデルを指定したのに変わらない» / «何も出ない» の 2 つが、この欄で
            /// @note 一番多い詰まり方。要求を画面に出しておく (どちらも黙って失敗する)。
            ImGui::TextColored({ 1.0f, 0.78f, 0.35f, 1.0f },
                "パスは使われません: 形は同じ GameObject の MeshRenderer が正本です");
            ImGui::TextColored({ 1.0f, 0.78f, 0.35f, 1.0f },
                "CPU シミュレーションでは MeshTrailComponent も必須 (無いと 1 粒も描かれません)");
        }
        EndModule();
    }

    /// @name Culling & LOD
    if (BeginModule("Culling & LOD", nullptr, /*defaultOpen=*/false, changed)) {
        changed |= ImGui::Checkbox("Culling Enabled", &pe.culling.cullingEnabled);
        changed |= ImGui::DragFloat("Bounds Padding", &pe.culling.cullingBoundsPadding, 0.01f, 0.0f, 100.0f);
        changed |= ImGui::Checkbox("Pause When Culled", &pe.culling.pauseWhenCulled);
        changed |= ImGui::DragFloat("Screen Coverage Threshold", &pe.culling.screenCoverageThreshold, 0.0001f, 0.0f, 1.0f, "%.4f");
        ImGui::Separator();
        changed |= ImGui::Checkbox("LOD Enabled", &pe.culling.lodEnabled);
        changed |= ImGui::DragFloat("LOD Near Distance", &pe.culling.lodNearDistance, 0.1f, 0.0f, 10000.0f);
        changed |= ImGui::DragFloat("LOD Far Distance", &pe.culling.lodFarDistance, 0.1f, 0.0f, 10000.0f);
        changed |= ImGui::DragFloat("LOD Near Rate", &pe.culling.lodNearRateScale, 0.01f, 0.0f, 1.0f);
        changed |= ImGui::DragFloat("LOD Far Rate", &pe.culling.lodFarRateScale, 0.01f, 0.0f, 1.0f);
        ImGui::Text("Bounds: %.2f  Visible: %d  Culled: %s",
                    runtime.boundsRadius, runtime.visibleParticleCount, runtime.isCulledThisFrame ? "Yes" : "No");
        EndModule();
    }

    /// @name Lights: 粒子を点光源にする
    if (BeginModule("Lights", &pe.light.lightEnabled, /*defaultOpen=*/false, changed)) {
        auto& light = pe.light;
        changed |= ImGui::SliderFloat("Ratio", &light.lightRatio, 0.0f, 1.0f);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("光らせる粒子の割合です。粒子ごとに固定で、寿命の間は変わりません。");
        changed |= ImGui::DragInt("Max Lights", &light.lightMaxCount, 1.0f, 0, 64);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("このエミッターから出す本数の上限です。明るい順に選びます。\n"
                              "ライトの枠は LightComponent と共有です (LightComponent が優先)。");
        changed |= ImGui::DragFloat("Range", &light.lightRange, 0.05f, 0.0f, 100.0f, "%.2f m");
        changed |= ImGui::Checkbox("Range x Particle Size", &light.lightRangeFromSize);
        changed |= ImGui::DragFloat("Intensity", &light.lightIntensity, 0.05f, 0.0f, 1000.0f);
        changed |= ImGui::ColorEdit3("Color", &light.lightColor.x, ImGuiColorEditFlags_Float);
        changed |= ImGui::Checkbox("Multiply Particle Color", &light.lightUseParticleColor);
        changed |= ImGui::Checkbox("Fade With Alpha", &light.lightFadeWithAlpha);
        if (pe.simulationMode == scene::ParticleSimulationMode::Gpu)
            ImGui::TextColored({ 1.0f, 0.65f, 0.3f, 1.0f },
                               "Lights を入れると CPU シミュレーションへ縮退します "
                               "(粒子の位置が CPU に無いと光源にできないため)");
        EndModule();
    }

    /// @name Presets: よく使う見た目のワンクリック設定
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
            /// @note 連続放出ではなく t=0 の Burst 一発で構成する例。タイムライン編集のデモも兼ねる。
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
