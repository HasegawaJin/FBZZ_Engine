// FBZZ Engine
// ParticleEmitterModules.cpp | fbzz::editor
// ParticleEmitter の Shuriken 式モジュールスタック UI 実装
#include <Editor/Util/ParticleEmitterModules.hpp>

#include <Editor/EditorContext.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Editor/Util/ParticleEditWidgets.hpp>
#include <Engine/Asset/FlipbookMotionVectors.hpp>
#include <Engine/Asset/ProceduralVFXTextures.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/ParticleGpuSimulation.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <imgui.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

namespace fbzz::editor {
namespace {

// モーションベクター生成の結果メッセージ。生成は数秒かかる同期処理で、
// 押した直後に何が起きたか分からないと不安になるため結果をパネルへ残す。
// Inspector は 1 つしか開かないため static で足りる。
std::string s_motionVectorStatus;
bool s_motionVectorStatusIsError = false;

struct ProceduralFlipbookUiState {
    int preset = 0;
    int frameSize = 128;
    int columns = 8;
    int rows = 2;
    int seed = 1;
    float noiseScale = 4.0f;
    float warpStrength = 0.65f;
    bool generateMotionVectors = false;
    bool clearMaterialOverride = true;
    std::string status;
    bool statusIsError = false;
};

// ProjectごとのGenerated配下へ出し、Engine同梱Assetsを誤って変更しない。
std::string ProceduralVFXOutputDirectory(const std::string& projectRoot)
{
    if (projectRoot.empty()) return "Assets/Textures/Generated/VFX";
    return projectRoot + "/Assets/Textures/Generated/VFX";
}

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
bool DrawShapeGizmo(scene::ParticleEmitter& pe)
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

} // namespace

void DrawParticleEmitterPlaybackButtons(scene::ParticleEmitter& pe)
{
    if (ImGui::Button(pe.playing ? "Pause" : "Play")) {
        if (pe.playing) pe.Pause();
        else            pe.Play();
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

bool DrawParticleEmitterModules(scene::ParticleEmitter& pe, EditorContext& ctx)
{
    bool changed = false;

    // ── Main: 初期値・再生設定・シミュレーション方式 ──────────────────────────
    if (BeginModule("Main", nullptr, /*defaultOpen=*/true, changed)) {
        DrawParticleEmitterPlaybackButtons(pe);
        ImGui::Spacing();

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

        changed |= widgets::DragVec3("Gravity", pe.gravity, 0.05f);
        changed |= ImGui::DragFloat("Velocity Damping", &pe.velocityDamping, 0.01f, 0.0f, 100.0f);
        changed |= ImGui::DragInt("Max Particles", &pe.maxParticles, 1, 1, 100000);

        int seed = static_cast<int>(pe.randomSeed);
        if (ImGui::DragInt("Random Seed", &seed, 1, 1, 2147483647)) {
            pe.randomSeed = static_cast<decltype(pe.randomSeed)>(seed);
            pe.randomState = pe.randomSeed;
            pe.emitAccum = 0.0f;
            changed = true;
        }

        const char* simItems[] = { "CPU", "GPU" };
        int sim = static_cast<int>(pe.simulationMode);
        if (ImGui::Combo("Simulation", &sim, simItems, 2)) {
            pe.simulationMode = static_cast<scene::ParticleSimulationMode>(sim);
            pe.gpuClearPending = true;
            changed = true;
        }
        // GPU を選んでも、条件のどれか 1 つを外すと黙って CPU へ落ちる。
        // WHY: これが見えないと、縮退したまま粒子数だけ増やし続けることになる。
        //      原因の設定名をコンボの真下に出し、設定を触ったその場で気づけるようにする。
        if (const auto fallback = scene::GetParticleGpuFallbackReason(pe);
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
            pe.gpuClearPending = true;
            changed = true;
        }
        EndModule();
    }

    // ── Emission: 連続レート・距離レート・Burst ─────────────────────────────
    if (BeginModule("Emission", nullptr, /*defaultOpen=*/true, changed)) {
        changed |= ImGui::DragFloat("Rate over Time", &pe.emitRate, 1.0f, 0.0f, 10000.0f);
        changed |= ImGui::DragFloat("Rate over Distance", &pe.rateOverDistance, 0.1f, 0.0f, 10000.0f);
        if (ImGui::Checkbox("Prewarm", &pe.prewarm)) {
            pe.prewarmed = false;
            pe.prewarmSpawnPending = 0;
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
                pe.loadedMeshShapePath.clear();
                pe.loadedMeshShapeIndex = -2;
                pe.meshShapeVertices.clear();
                changed = true;
            }
            if (ImGui::DragInt("Shape Mesh Index", &pe.meshShapeIndex, 1, -1, 1024)) {
                pe.loadedMeshShapeIndex = -2;
                pe.meshShapeVertices.clear();
                changed = true;
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip("-1 uses every sub-mesh from the FBX");
            changed |= ImGui::DragFloat("Shape Scale", &pe.meshShapeScale, 0.001f, 0.0001f, 1000.0f);
            changed |= ImGui::Checkbox("Follow Skinned Animation", &pe.meshShapeFollowSkinnedAnimation);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip("Uses the Animator on this GameObject to deform particle spawn points");
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
            changed |= widgets::CurveEditor("Speed Multiplier", pe.velocityCurve, 10.0f);
        } else {
            ImGui::TextDisabled("Enable the checkbox to scale particle speed over lifetime.");
        }
        EndModule();
    }

    // ── Size over Lifetime: カーブ or べき乗フォールバック ────────────────────
    if (BeginModule("Size over Lifetime", &pe.useSizeCurve, /*defaultOpen=*/false, changed)) {
        if (pe.useSizeCurve) {
            changed |= widgets::CurveEditor("Size (Start -> End blend)", pe.sizeCurve, 1.0f);
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
            changed |= widgets::GradientEditor("Color Gradient", pe.colorGradient);
        } else {
            changed |= ImGui::DragFloat("Color Curve Power", &pe.colorCurvePower, 0.01f, 0.01f, 10.0f);
            ImGui::TextDisabled("Start/End color blend. Enable the checkbox for a gradient.");
        }
        EndModule();
    }

    // ── Rotation over Lifetime ─────────────────────────────────────────
    if (BeginModule("Rotation over Lifetime", nullptr, /*defaultOpen=*/false, changed)) {
        changed |= ImGui::DragFloat("Angular Velocity Min", &pe.angularVelocityMin, 0.01f, -100.0f, 100.0f);
        changed |= ImGui::DragFloat("Angular Velocity Max", &pe.angularVelocityMax, 0.01f, -100.0f, 100.0f);
        changed |= ImGui::Checkbox("Use Rotation Curve", &pe.useRotationCurve);
        if (pe.useRotationCurve) {
            changed |= widgets::CurveEditor("Spin Multiplier", pe.rotationCurve, 2.0f);
            ImGui::TextDisabled("角速度への時間倍率。頭を高く末尾を 0 にすると"
                                "「勢いよく回り始めて止まる」破片になります。");
        }
        EndModule();
    }

    // ── Drag over Lifetime: velocityDamping への時間倍率 ────────────────────
    if (BeginModule("Drag over Lifetime", &pe.useDragCurve, /*defaultOpen=*/false, changed)) {
        if (pe.useDragCurve) {
            changed |= widgets::CurveEditor("Drag Multiplier", pe.dragCurve, 4.0f);
            ImGui::TextDisabled("Main の Velocity Damping に掛かります。"
                                "後半を高くすると噴き出した後で急に空気抵抗が効きます。");
        } else {
            ImGui::TextDisabled("Enable to modulate Velocity Damping over lifetime.");
        }
        EndModule();
    }

    // ── Orbital / Radial / Inherit: エミッター原点まわりの運動と移動の引き継ぎ ──
    if (BeginModule("Velocity Modules", nullptr, /*defaultOpen=*/false, changed)) {
        changed |= widgets::DragVec3("Orbital Axis", pe.orbitalAxis, 0.01f);
        changed |= ImGui::DragFloat("Orbital Velocity", &pe.orbitalVelocity, 0.05f, -100.0f, 100.0f);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("軸まわりの接線加速度。渦・竜巻・魔法陣の回転に使います");
        changed |= ImGui::DragFloat("Radial Velocity", &pe.radialVelocity, 0.05f, -100.0f, 100.0f);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("原点から外向きの加速度。負値で吸い込みになります");
        ImGui::Separator();
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

    // ── Noise: エミッター固有のカールノイズ乱流。strength 0 = 無効 ─────────────
    {
        bool noiseEnabled = pe.noiseStrength > 0.0f;
        bool moduleChanged = false;
        const bool open = BeginModule("Noise", &noiseEnabled, /*defaultOpen=*/false, moduleChanged);
        if (moduleChanged) {
            // チェックボックスで有効化したら見た目に変化が出る初期値を入れる
            pe.noiseStrength = noiseEnabled ? 1.0f : 0.0f;
            changed = true;
        }
        if (open) {
            if (noiseEnabled) {
                changed |= ImGui::DragFloat("Strength", &pe.noiseStrength, 0.01f, 0.0f, 100.0f);
                changed |= ImGui::DragFloat("Frequency", &pe.noiseFrequency, 0.01f, 0.001f, 100.0f);
                changed |= ImGui::DragFloat("Scroll Speed", &pe.noiseSpeed, 0.01f, -100.0f, 100.0f);
            } else {
                ImGui::TextDisabled("Curl-noise turbulence for flame flicker / smoke wobble.");
            }
            EndModule();
        }
    }

    // ── External Forces: シーンの ParticleForceField を受けるか ───────────────
    if (BeginModule("External Forces", &pe.receiveForceFields, /*defaultOpen=*/false, changed)) {
        ImGui::TextDisabled("Receives Wind / Attract / Vortex / Turbulence force fields in the scene.");
        EndModule();
    }

    // ── Collision: mode != None で有効 ─────────────────────────────────
    {
        bool collisionEnabled = pe.collisionMode != scene::ParticleCollisionMode::None;
        bool moduleChanged = false;
        const bool open = BeginModule("Collision", &collisionEnabled, /*defaultOpen=*/false, moduleChanged);
        if (moduleChanged) {
            pe.collisionMode = collisionEnabled
                ? scene::ParticleCollisionMode::Physics
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
                ImGui::Text("Collisions this frame: %d", pe.collisionCountThisFrame);
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

    // ── Texture Sheet Animation: スプライトシート / Flipbook ─────────────────
    if (BeginModule("Texture Sheet Animation", nullptr, /*defaultOpen=*/false, changed)) {
        changed |= ImGui::DragInt("Columns", &pe.spriteColumns, 1, 1, 64);
        changed |= ImGui::DragInt("Rows", &pe.spriteRows, 1, 1, 64);
        changed |= ImGui::DragInt("Start Frame", &pe.spriteStartFrame, 1, 0, 4095);
        changed |= ImGui::DragInt("End Frame", &pe.spriteEndFrame, 1, 0, 4095);
        const char* flipbookItems[] = { "Lifetime", "FPS", "Random", "Ping Pong" };
        int flipbookMode = static_cast<int>(pe.flipbookMode);
        if (ImGui::Combo("Mode", &flipbookMode, flipbookItems, 4)) {
            pe.flipbookMode = static_cast<scene::ParticleFlipbookMode>(flipbookMode);
            changed = true;
        }
        if (pe.flipbookMode != scene::ParticleFlipbookMode::Lifetime
            && pe.flipbookMode != scene::ParticleFlipbookMode::RandomFrame)
            changed |= ImGui::DragFloat("FPS", &pe.flipbookFramesPerSecond, 0.1f, 0.0f, 240.0f);
        changed |= ImGui::Checkbox("Frame Blending", &pe.flipbookFrameBlending);
        changed |= ImGui::Checkbox("Random Start Frame", &pe.spriteRandomStartFrame);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("粒子ごとに再生位相をずらします。\n"
                              "同時に湧いた煙が全部同じコマで回って一枚板に見えるのを防ぎます。");
        changed |= ImGui::Checkbox("Random Row", &pe.spriteRandomRow);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("アトラスの各行を別バリエーションとして扱い、粒子ごとに1行を選びます。\n"
                              "1枚のアトラスで見た目の異なる煙・爆炎を混ぜられます。\n"
                              "有効時は Start/End Frame より行の範囲が優先されます。");

        if (ImGui::TreeNode("Procedural Flipbook Generator")) {
            // 関数ローカルstaticならグローバル状態を増やさず、Inspectorを閉じても設定を保持できる。
            static ProceduralFlipbookUiState procedural;
            constexpr const char* presetNames[] = { "Smoke", "Fire", "Explosion", "Distortion" };
            ImGui::Combo("Preset", &procedural.preset, presetNames, 4);
            ImGui::DragInt("Frame Size", &procedural.frameSize, 8.0f, 32, 512);
            ImGui::DragInt("Frames", &procedural.columns, 1.0f, 2, 32);
            ImGui::DragInt("Variants", &procedural.rows, 1.0f, 1, 16);
            ImGui::DragInt("Seed", &procedural.seed, 1.0f, 0, 1000000);
            ImGui::DragFloat("Noise Scale", &procedural.noiseScale, 0.05f, 0.25f, 32.0f);
            ImGui::DragFloat("Warp Strength", &procedural.warpStrength, 0.01f, 0.0f, 3.0f);
            const bool distortionPreset = procedural.preset
                == static_cast<int>(asset::ProceduralFlipbookPreset::Distortion);
            ImGui::BeginDisabled(distortionPreset);
            ImGui::Checkbox("Generate Motion Vectors", &procedural.generateMotionVectors);
            ImGui::EndDisabled();
            if (distortionPreset)
                ImGui::TextDisabled("DistortionはRG自体が変位なのでMotion Vectorを生成しません。");
            if (!pe.materialPath.empty()) {
                ImGui::Checkbox("Replace .mat Texture", &procedural.clearMaterialOverride);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(".matはTexture欄より優先されます。ONなら生成物を見える状態にするため"
                                      "Material参照を解除します。");
            }

            if (ImGui::Button("Generate & Assign", { -1.0f, 0.0f })) {
                asset::ProceduralFlipbookSettings settings;
                settings.preset = static_cast<asset::ProceduralFlipbookPreset>(procedural.preset);
                settings.frameSize = procedural.frameSize;
                settings.columns = procedural.columns;
                settings.rows = procedural.rows;
                settings.seed = static_cast<std::uint32_t>((std::max)(procedural.seed, 0));
                settings.noiseScale = procedural.noiseScale;
                settings.warpStrength = procedural.warpStrength;
                const auto result = asset::GenerateProceduralFlipbook(
                    ProceduralVFXOutputDirectory(ctx.projectRoot), settings);
                procedural.status = result.message;
                procedural.statusIsError = !result.success;
                if (result.success) {
                    if (procedural.clearMaterialOverride && !pe.materialPath.empty()) {
                        pe.materialPath.clear();
                        pe.loadedMaterialPath.clear();
                    }
                    // 生成物は .mat の albedo へ割り当てる。Emitter に直接テクスチャは保持しない。
                    pe.texture = {};
                    pe.loadedTexturePath.clear();
                    pe.spriteColumns = settings.columns;
                    pe.spriteRows = settings.rows;
                    pe.spriteStartFrame = 0;
                    pe.spriteEndFrame = settings.columns * settings.rows - 1;
                    pe.flipbookFrameBlending = true;
                    pe.spriteRandomStartFrame = false;
                    pe.spriteRandomRow = settings.rows > 1;
                    pe.motionVectorFlipbook = false;
                    pe.motionVectorTexturePath.clear();
                    pe.motionVectorTexture = {};
                    pe.loadedMotionVectorTexturePath.clear();

                    if (distortionPreset) {
                        pe.flipbookMode = scene::ParticleFlipbookMode::FramesPerSecond;
                        pe.flipbookFramesPerSecond = 24.0f;
                        pe.blendMode = scene::ParticleBlendMode::Alpha;
                        pe.distortion = true;
                    } else {
                        pe.flipbookMode = scene::ParticleFlipbookMode::Lifetime;
                        pe.distortion = false;
                        if (settings.preset == asset::ProceduralFlipbookPreset::Fire)
                            pe.blendMode = scene::ParticleBlendMode::Additive;
                        else if (settings.preset == asset::ProceduralFlipbookPreset::Explosion)
                            pe.blendMode = scene::ParticleBlendMode::Premultiplied;
                        else
                            pe.blendMode = scene::ParticleBlendMode::Alpha;

                        if (procedural.generateMotionVectors) {
                            asset::FlipbookMotionVectorSettings mvSettings;
                            mvSettings.columns = settings.columns;
                            mvSettings.rows = settings.rows;
                            mvSettings.loop = false;
                            mvSettings.rowSequences = settings.rows > 1;
                            const auto mvResult = asset::GenerateFlipbookMotionVectors(
                                result.albedoPath, mvSettings);
                            if (mvResult.success) {
                                pe.motionVectorFlipbook = true;
                                pe.motionVectorTexturePath = NormalizeAssetPath(mvResult.outputPath);
                                procedural.status += "\n" + mvResult.message;
                            } else {
                                procedural.status += "\nMV生成失敗: " + mvResult.message;
                                procedural.statusIsError = true;
                            }
                        }
                    }
                    changed = true;
                }
            }
            if (!procedural.status.empty()) {
                if (procedural.statusIsError)
                    ImGui::TextColored({ 1.0f, 0.4f, 0.3f, 1.0f }, "%s", procedural.status.c_str());
                else
                    ImGui::TextWrapped("%s", procedural.status.c_str());
            }
            if (!pe.materialPath.empty() && !procedural.clearMaterialOverride)
                ImGui::TextColored({ 1.0f, 0.72f, 0.35f, 1.0f },
                    ".matが設定中です。生成TextureよりMaterial側Albedoが優先されます。");
            ImGui::TreePop();
        }

        changed |= ImGui::Checkbox("Motion Vector Blending", &pe.motionVectorFlipbook);
        if (pe.motionVectorFlipbook) {
            if (widgets::AssetPathField("Motion Vector Atlas", pe.motionVectorTexturePath,
                                        widgets::kTextureAssetFilter, ctx.projectRoot)) {
                pe.motionVectorTexture = {};
                pe.loadedMotionVectorTexturePath.clear();
                changed = true;
            }
            changed |= ImGui::DragFloat("Motion Strength", &pe.motionVectorStrength, 0.01f, 0.0f, 8.0f);

            // MV アトラスは外部ツールでしか作れず「機能はあるのに使えない」状態だったため、
            // 現在のアトラスから生成してそのまま割り当てられるようにする。
            ImGui::Separator();
            const std::string& atlasPath = pe.materialPath;
            ImGui::BeginDisabled(pe.materialPath.empty());
            if (ImGui::Button("Generate From Texture", { -1.0f, 0.0f })) {
                asset::FlipbookMotionVectorSettings mvSettings;
                mvSettings.columns = pe.spriteColumns;
                mvSettings.rows = pe.spriteRows;
                mvSettings.loop = pe.flipbookMode == scene::ParticleFlipbookMode::FramesPerSecond
                    || pe.spriteRandomStartFrame;
                mvSettings.rowSequences = pe.spriteRandomRow;
                // Material の albedo はこの生成器の入力画像として直接は扱わない。
                // .mat の albedo を編集してから別途モーションベクトルを生成する。
                const std::string diskPath = ToProjectAssetDiskPath(ctx.projectRoot, pe.materialPath);
                const auto result = asset::GenerateFlipbookMotionVectors(diskPath, mvSettings);
                s_motionVectorStatus = result.message;
                s_motionVectorStatusIsError = !result.success;
                if (result.success) {
                    // 生成結果をそのまま割り当てる。手で貼り直す手間を残さない。
                    pe.motionVectorTexturePath = NormalizeAssetPath(result.outputPath);
                    pe.motionVectorTexture = {};
                    pe.loadedMotionVectorTexturePath.clear();
                    changed = true;
                }
            }
            ImGui::EndDisabled();
            if (pe.materialPath.empty())
                ImGui::TextDisabled("Material の albedo を設定すると生成できます。");
            if (!s_motionVectorStatus.empty()) {
                if (s_motionVectorStatusIsError)
                    ImGui::TextColored({ 1.0f, 0.4f, 0.3f, 1.0f }, "%s", s_motionVectorStatus.c_str());
                else
                    ImGui::TextWrapped("%s", s_motionVectorStatus.c_str());
            }
            (void)atlasPath;
        }
        EndModule();
    }

    // ── Renderer: 描画方式・マテリアル・ソフトパーティクル ─────────────────────
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
        // テクスチャの作りの違いを吸収する。素材を画像編集ソフトで加工させないための設定。
        // 並びは Rendering/Mask.hlsli の FBZZ_MASK_* と一致させること。
        const char* alphaItems[] = {
            "Texture Alpha", "Luminance (black = clear)",
            "Inverted Luminance (white = clear)", "Red Channel",
            "Green Channel", "Blue Channel", "Inverted Alpha" };
        int alphaSource = static_cast<int>(pe.alphaSource);
        if (ImGui::Combo("Alpha Source", &alphaSource, alphaItems, 7)) {
            pe.alphaSource = static_cast<scene::ParticleAlphaSource>(alphaSource);
            changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "テクスチャのどこを「不透明度」として読むかを選びます。\n\n"
                "Texture Alpha : アルファ付き素材 (通常の PNG/TGA)\n"
                "Luminance     : 黒背景でアルファが無い素材。明るいほど濃く出ます\n"
                "Inverted      : 白背景の素材。暗いほど濃く出ます\n"
                "R/G/B Channel : 1枚に複数のマスクを詰めたパック済み素材\n"
                "Inverted Alpha: アルファの意味が逆になっている素材\n\n"
                "素材を Alpha Blend にしたら黒い四角が出る場合は Luminance を選びます。\n"
                "この選択肢は全マテリアル共通の語彙です (Rendering/Mask.hlsli)。");
        // 黒背景素材 + アルファブレンドは典型的な事故なので、その組み合わせだけ助言を出す。
        if (pe.alphaSource == scene::ParticleAlphaSource::TextureAlpha
            && pe.blendMode != scene::ParticleBlendMode::Additive) {
            ImGui::TextDisabled("黒い矩形が出る場合はアルファ無し素材です。Luminance を試してください。");
        }

        const char* blendItems[] = { "Additive", "Alpha", "Premultiplied" };
        int blend = static_cast<int>(pe.blendMode);
        if (ImGui::Combo("Blend Mode", &blend, blendItems, 3)) {
            pe.blendMode = static_cast<scene::ParticleBlendMode>(blend);
            changed = true;
        }
        if (pe.blendMode == scene::ParticleBlendMode::Premultiplied && ImGui::IsItemHovered())
            ImGui::SetTooltip("RGB に alpha が乗ったテクスチャ用。alpha=0 で RGB>0 の画素は\n"
                              "加算として振る舞うため、発光する芯と背景を隠す煙を1枚で両立できます。");
        if (!pe.materialPath.empty())
            ImGui::TextDisabled("Blend Mode は .mat 側の設定で毎フレーム上書きされます。");
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
        if (widgets::AssetPathField("Material (.mat)", pe.materialPath, ".mat", ctx.projectRoot)) {
            pe.loadedMaterialPath.clear();
            pe.texture = {};
            pe.loadedTexturePath.clear();
            changed = true;
        }
        changed |= widgets::AssetPathField("Mesh Particle (optional)", pe.meshParticlePath,
                                           ".fbx,.obj,.mesh,.fzasset", ctx.projectRoot);
        if (!pe.meshParticlePath.empty())
            ImGui::TextDisabled("Mesh Particle uses deterministic CPU simulation.");
        changed |= ImGui::Checkbox("Soft Particles", &pe.softParticles);
        if (pe.softParticles)
            changed |= ImGui::DragFloat("Soft Fade Distance", &pe.softParticleFadeDistance, 0.01f, 0.001f, 100.0f);
        changed |= ImGui::DragFloat("HDR Emissive", &pe.emissiveScale, 0.01f, 0.0f, 100.0f);
        changed |= ImGui::Checkbox("Distortion / Heat Haze", &pe.distortion);
        if (pe.distortion)
            changed |= ImGui::DragFloat("Distortion Strength", &pe.distortionStrength, 0.001f, 0.0f, 0.25f, "%.4f");
        if (ImGui::Checkbox("Six-way Lit Smoke", &pe.sixWayLighting)) {
            // 役割が重複するため排他にする (両方掛けると二重に陰影が付いて濁る)。
            if (pe.sixWayLighting) pe.volumetric = false;
            changed = true;
        }
        if (pe.sixWayLighting) {
            // シェーダー側が saturate するため 1.0 が上限。それ以上は「元の色を捨てて
            // (ambient + N·L) で塗る」だけになり、暗い環境で煙が真っ黒に潰れる。
            changed |= ImGui::SliderFloat("Lighting Strength", &pe.lightingStrength, 0.0f, 1.0f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("0 = 元の色そのまま / 1 = 完全にライティングで置換");
        }
        if (ImGui::Checkbox("Volumetric Smoke", &pe.volumetric)) {
            if (pe.volumetric) pe.sixWayLighting = false;
            changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("ビルボード内で密度場をレイマーチして厚みを出します。\n"
                              "カメラが回り込んでも「紙が回った」ように見えません。\n"
                              "テクスチャは使わず密度場で色を作るため Six-way とは排他です。");
        if (pe.volumetric) {
            changed |= ImGui::DragInt("Volumetric Steps", &pe.volumetricSteps, 1, 1, 64);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("1 ピクセルあたりのループ回数です。増やすほど滑らかですが重くなります。");
            changed |= ImGui::DragFloat("Density", &pe.volumetricDensity, 0.01f, 0.0f, 20.0f);
            changed |= ImGui::SliderFloat("Anisotropy", &pe.volumetricAnisotropy, -0.95f, 0.95f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("正で前方散乱。逆光のとき煙の縁が光ります。");
            changed |= ImGui::DragFloat("Noise Scale", &pe.volumetricNoiseScale, 0.05f, 0.0f, 32.0f);
            if (pe.blendMode == scene::ParticleBlendMode::Additive)
                ImGui::TextDisabled("出力は事前乗算アルファです。Premultiplied ブレンドを推奨します。");
        }
        changed |= ImGui::Checkbox("Receive Shadows", &pe.receiveShadows);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("影の中で粒子を暗くします。\n"
                              "煙・埃が背景から浮いて見える最大の原因がこれです。\n"
                              "発光エフェクト(加算)では通常オフのままにします。");
        if (pe.receiveShadows) {
            changed |= ImGui::SliderFloat("Shadow Strength", &pe.shadowStrength, 0.0f, 1.0f);
            if (pe.blendMode == scene::ParticleBlendMode::Additive)
                ImGui::TextDisabled("加算ブレンドでは影が暗くしても見えにくくなります。"
                                    "煙は Alpha / Premultiplied を推奨。");
        }
        // 自己影。受け影とは別の現象なので、別のスライダーとして並べる。
        changed |= ImGui::DragFloat("Self Shadow", &pe.selfShadowStrength, 0.01f, 0.0f, 8.0f, "%.2f");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "粒子群が自分自身へ落とす影の濃さ (0 で無効)。\n"
                "受け影は「他の物体が落とす影」だけを扱うため、これが無いと\n"
                "粒子をいくら重ねても光の当たり方が一様で、厚みのある煙・雲が平坦に見えます。\n"
                "有効にすると光源から見た密度を積む追加パスが走り、GPU シミュレーションは使えません。");
        }
        if (pe.selfShadowStrength > 0.0f) {
            ImGui::TextDisabled("光源側の密度から Beer-Lambert 則で減衰させる近似です。"
                                "厚みの表現が目的で、物理的な正確さは狙っていません。");
            if (pe.simulationMode == scene::ParticleSimulationMode::Gpu)
                ImGui::TextColored({ 1.0f, 0.65f, 0.3f, 1.0f },
                                   "自己影は CPU 頂点バッファを要求するため、GPU 指定でも CPU で実行されます。");
        }
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
                    pe.boundsRadius, pe.visibleParticleCount, pe.isCulledThisFrame ? "Yes" : "No");
        EndModule();
    }

    // ── Presets: よく使う見た目のワンクリック設定 ─────────────────────────────
    if (BeginModule("Presets", nullptr, /*defaultOpen=*/false, changed)) {
        if (ImGui::Button("Fire")) {
            pe.colorStart = { 1.0f, 0.35f, 0.05f, 1.0f };
            pe.colorEnd   = { 1.0f, 0.02f, 0.0f, 0.0f };
            pe.gravity    = { 0.0f, 1.5f, 0.0f };
            pe.emitRate   = 80.0f;
            pe.lifetime   = 1.2f;
            pe.shape      = scene::ParticleEmitterShape::Cone;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Smoke")) {
            pe.colorStart = { 0.25f, 0.25f, 0.25f, 0.65f };
            pe.colorEnd   = { 0.05f, 0.05f, 0.05f, 0.0f };
            pe.gravity    = { 0.0f, 0.3f, 0.0f };
            pe.emitRate   = 25.0f;
            pe.lifetime   = 3.0f;
            pe.shape      = scene::ParticleEmitterShape::Sphere;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Sparks")) {
            pe.colorStart = { 1.0f, 0.8f, 0.2f, 1.0f };
            pe.colorEnd   = { 1.0f, 0.1f, 0.0f, 0.0f };
            pe.gravity    = { 0.0f, -9.8f, 0.0f };
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
            pe.gravity    = { 0.0f, -3.0f, 0.0f };
            pe.emitRate   = 0.0f;
            pe.lifetime   = 0.9f;
            pe.velocitySpread = 6.0f;
            pe.shape      = scene::ParticleEmitterShape::Sphere;
            pe.sphereRadius = 0.3f;
            pe.loop       = false;
            pe.duration   = 1.5f;
            pe.bursts.clear();
            pe.bursts.push_back({ 0.0f, 60, 1, 0.1f, 1.0f });
            pe.ResetPlayback();
            changed = true;
        }
        EndModule();
    }

    return changed;
}

} // namespace fbzz::editor
