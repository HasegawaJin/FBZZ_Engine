// FBZZ Engine
// ParticleEmitterModules.cpp | fbzz::editor
// ParticleEmitter の Shuriken 式モジュールスタック UI 実装
#include <Editor/Util/ParticleEmitterModules.hpp>

#include <Editor/EditorContext.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Editor/Util/ParticleEditWidgets.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <imgui.h>
#include <algorithm>
#include <cstdio>

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
    if (ImGui::Button(pe.playing ? "Pause" : "Play")) pe.playing = !pe.playing;
    ImGui::SameLine();
    if (ImGui::Button("Restart")) pe.ResetPlayback();
    ImGui::SameLine();
    if (ImGui::Button("Stop")) pe.playing = false;
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        pe.particles.clear();
        pe.emitAccum = 0.0f;
        pe.burstPending = 0;
        pe.prewarmSpawnPending = 0;
        pe.gpuClearPending = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Burst")) pe.burstPending += 10;
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
        changed |= ImGui::Checkbox("Motion Vector Blending", &pe.motionVectorFlipbook);
        if (pe.motionVectorFlipbook) {
            if (widgets::AssetPathField("Motion Vector Atlas", pe.motionVectorTexturePath, ".fztex,.png,.dds", ctx.projectRoot)) {
                pe.motionVectorTexture = {};
                pe.loadedMotionVectorTexturePath.clear();
                changed = true;
            }
            changed |= ImGui::DragFloat("Motion Strength", &pe.motionVectorStrength, 0.01f, 0.0f, 8.0f);
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
        const char* blendItems[] = { "Additive", "Alpha" };
        int blend = static_cast<int>(pe.blendMode);
        if (ImGui::Combo("Blend Mode", &blend, blendItems, 2)) {
            pe.blendMode = static_cast<scene::ParticleBlendMode>(blend);
            changed = true;
        }
        const char* sortItems[] = { "None", "Back To Front" };
        int sort = static_cast<int>(pe.sortMode);
        if (ImGui::Combo("Sort Mode", &sort, sortItems, 2)) {
            pe.sortMode = static_cast<scene::ParticleSortMode>(sort);
            changed = true;
        }

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
        // texturePath — deprecated フォールバック。materialPath が空のときだけ使われる。
        if (widgets::AssetPathField("Texture (fallback)", pe.texturePath, ".fztex,.png,.dds", ctx.projectRoot)) {
            pe.texture = {};
            pe.loadedTexturePath.clear();
            changed = true;
        }

        changed |= ImGui::Checkbox("Soft Particles", &pe.softParticles);
        if (pe.softParticles)
            changed |= ImGui::DragFloat("Soft Fade Distance", &pe.softParticleFadeDistance, 0.01f, 0.001f, 100.0f);
        changed |= ImGui::DragFloat("HDR Emissive", &pe.emissiveScale, 0.01f, 0.0f, 100.0f);
        changed |= ImGui::Checkbox("Distortion / Heat Haze", &pe.distortion);
        if (pe.distortion)
            changed |= ImGui::DragFloat("Distortion Strength", &pe.distortionStrength, 0.001f, 0.0f, 0.25f, "%.4f");
        changed |= ImGui::Checkbox("Six-way Lit Smoke", &pe.sixWayLighting);
        if (pe.sixWayLighting)
            changed |= ImGui::DragFloat("Lighting Strength", &pe.lightingStrength, 0.01f, 0.0f, 8.0f);
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
