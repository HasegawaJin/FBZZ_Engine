/// @file    InspectorEffects.cpp
/// @brief   パーティクル / 力場 / トレイル系コンポーネントの Inspector 描画。
/// @author  Hasegawa Jin
/// @date    2026-06-07

#include <Editor/Inspector/Widgets/PropertyControls.hpp>
#include <Engine/Scene/Components/VFX/MeshTrailComponent.hpp>
#include <Engine/Scene/Components/Particles/ParticleEmitter.hpp>
#include <Engine/Scene/Components/VFX/TrailComponent.hpp>
#include <Engine/Scene/Fields/FlowField.hpp>
#include "InspectorEffects.hpp"

#include <Editor/VFX/ParticleEditWidgets.hpp>
#include <Editor/VFX/ParticleEmitterModules.hpp>
#include <Editor/VFX/VFXLineInspector.hpp>
#include <Engine/Scene/Components/VFX/VFXLineComponent.hpp>
#include <algorithm>
#include <iterator>

namespace fbzz::editor {

void DrawEffectsInspectors(scene::GameObject* go, EditorContext& ctx, std::any& m_componentClipboard, const std::type_info*& m_componentClipboardType)
{
    // ParticleEmitter は Shuriken 式のモジュールスタックで描画する。約 80 項目を素直に並べると
    // «どこを触ればよいか» が読めなくなるため。Undo は DrawComponentSection の ActiveID 追跡がそのまま効く。
    DrawComponentSection<scene::ParticleEmitter>(go, ctx, m_componentClipboard, m_componentClipboardType, "Particle Emitter",
        [](scene::ParticleEmitter& pe, EditorContext& ctx) {
            // 戻り値は「どれか 1 つでも変わった」。捨てるとモジュール側の編集が
            // シーンの dirty マークへ伝わらず、保存し忘れて消える。
            if (DrawParticleEmitterModules(pe.settings, ctx, &pe) && ctx.markSceneDirty) {
                ctx.markSceneDirty();
            }
        });


    DrawComponentSection<scene::FlowField>(go, ctx, m_componentClipboard, m_componentClipboardType, "Flow Field",
        [](scene::FlowField& ff, EditorContext& ctx) {
            // 流れのリストはエミッター内蔵の流れと同じ UI。座標系は GameObject の Transform が
            // 決めるので選択は出さない。
            // showSpace=
            DrawFlowFieldList(ff.forces, ctx, false);
            // channels はシーンに置いた場だけの概念 (内蔵の流れは相手が決まっている)。
            // 流れごとに持てるが、束ねて置く用途では全部同じにしたいことが多いので
            // «先頭に合わせて全部へ» 配る。個別に分けたいなら GameObject を分ける。
            if (!ff.forces.empty()) {
                std::uint32_t mask = ff.forces.front().channels;
                if (widgets::FlowFieldChannelMask("Channels", mask))
                    for (auto& force : ff.forces) {
                        force.channels = mask;
                    }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Only emitters sharing a bit with this mask receive these flows.");
                }
            }
        });

    // 雷・ビーム。プリセットと «形が動くプレビュー» を上に置き、細かい値は Reflect の行で出す。
    // 40 近い値のうち «どれを触ると枝が増えるか» は数値の列からは読めず、触ったその場で形が
    // 変わるのを見せないと雷は作れないため専用にした。
    DrawComponentSection<scene::VFXLineComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "VFX Line",
        [](scene::VFXLineComponent& line, EditorContext& ctx) {
            if (DrawVFXLinePresetBar(line) && ctx.markSceneDirty) {
                ctx.markSceneDirty();
            }
            DrawVFXLinePreview(line);
            ComponentImGuiReflector reflector;
            reflector.m_projectRoot = ctx.projectRoot;
            line.Reflect(reflector);
        });

    DrawComponentSection<scene::TrailComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Trail",
        [](scene::TrailComponent& trail, EditorContext& ctx) {
            property::DragFloat("Duration", &trail.duration, 0.01f, 0.01f, 30.0f);
            property::DragInt("Max Points", &trail.maxPoints, 1, 2, 512);
            property::DragFloat("Sample Interval", &trail.sampleInterval, 0.001f, 0.0f, 1.0f);
            property::DragFloat("Min Vertex Dist", &trail.minVertexDist, 0.001f, 0.0f, 10.0f);
            // 瞬間移動の切断。0 は «切らない» で、既存シーンの見た目を保つ既定。
            property::DragFloat("Break Distance", &trail.breakDistance, 0.01f, 0.0f, 500.0f);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Cut the trail when it jumps farther than this in one frame. 0 = never cut.");
            }

            // 幅は «2 点 + イージング» と «多キーカーブ» の二択で、有効な側だけを見せる。
            // curveChanged を拾うのは、カーブ / グラデーションのキードラッグ編集は ImGui の
            // ActiveID 追跡が拾えず、捨てると dirty マークが立たず保存し忘れで消えるため。
            bool curveChanged = false;
            property::Checkbox("Use Width Curve", &trail.widthCurveEnabled);
            if (trail.widthCurveEnabled) {
                property::DragFloat("Width Scale", &trail.widthStart, 0.001f, 0.0f, 10.0f);
                curveChanged |= widgets::CurveEditor("Width Curve", trail.widthCurve, 1.0f, 96.0f, &ctx.projectRoot);
            } else {
                property::DragFloat("Width Start", &trail.widthStart, 0.001f, 0.0f, 10.0f);
                property::DragFloat("Width End", &trail.widthEnd, 0.001f, 0.0f, 10.0f);
                const char* widthEasingItems[] = { "Linear", "Ease In", "Ease Out", "Ease In Out" };
                int widthEasing = static_cast<int>(trail.widthEasing);
                if (property::Combo("Width Easing", &widthEasing, widthEasingItems, 4)) {
                    trail.widthEasing = static_cast<scene::TrailWidthEasing>(widthEasing);
                }
            }

            property::Checkbox("Use Color Gradient", &trail.colorGradientEnabled);
            if (trail.colorGradientEnabled) {
                curveChanged |= widgets::GradientEditor("Color Gradient", trail.colorGradient, &ctx.projectRoot);
            } else {
                float cs[4] = { trail.colorStart.x, trail.colorStart.y, trail.colorStart.z, trail.colorStart.w };
                if (property::ColorEdit4("Color Start", cs)) {
                    trail.colorStart = { cs[0], cs[1], cs[2], cs[3] };
                }
                float ce[4] = { trail.colorEnd.x, trail.colorEnd.y, trail.colorEnd.z, trail.colorEnd.w };
                if (property::ColorEdit4("Color End", ce)) {
                    trail.colorEnd = { ce[0], ce[1], ce[2], ce[3] };
                }
            }
            if (curveChanged && ctx.markSceneDirty) {
                ctx.markSceneDirty();
            }

            const char* alignmentItems[] = { "Camera Facing", "World Up" };
            int alignment = static_cast<int>(trail.alignment);
            if (property::Combo("Alignment", &alignment, alignmentItems, 2)) {
                trail.alignment = static_cast<scene::TrailAlignment>(alignment);
            }

            property::DragInt("Smooth Subdivisions", &trail.smoothSubdivisions, 1, 0, 8);
            char boneBuf[256];
            std::snprintf(boneBuf, sizeof(boneBuf), "%s", trail.attachBone.c_str());
            if (property::InputText("Attach Bone", boneBuf, sizeof(boneBuf))) {
                trail.attachBone = boneBuf;
            }
            widgets::DragVec3("Attach Offset", trail.attachOffset, 0.001f);
            property::Checkbox("Clear On Disable", &trail.clearOnDisable);
            // .mat アセット参照。albedo テクスチャを .mat から解決する。
            // 変更時はキャッシュを無効化してレンダーパスに再ロードさせる。
            if (property::AssetPathField("Material (.mat)", trail.materialPath, ".mat", ctx.projectRoot)) {
                trail.loadedMaterialPath.clear();
                trail.texture = {};
                trail.loadedTexturePath.clear();
            }
            const char* uvModeItems[] = { "Stretch", "Tile" };
            int uvMode = static_cast<int>(trail.uvMode);
            if (property::Combo("UV Mode", &uvMode, uvModeItems, 2)) {
                trail.uvMode = static_cast<scene::TrailUVMode>(uvMode);
            }
            property::DragFloat("UV Scroll Speed", &trail.uvScrollSpeed, 0.001f, -20.0f, 20.0f);
            property::DragFloat("UV Tiling", &trail.uvTiling, 0.001f, 0.001f, 100.0f);
        });

    DrawComponentSection<scene::MeshTrailComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Mesh Trail",
        [](scene::MeshTrailComponent& trail, EditorContext& ctx) {
            property::DragFloat("Duration", &trail.duration, 0.01f, 0.01f, 30.0f);
            property::DragFloat("Sample Interval", &trail.sampleInterval, 0.001f, 0.0f, 1.0f);
            property::DragFloat("Min Vertex Dist", &trail.minVertexDist, 0.001f, 0.0f, 10.0f);
            property::DragInt("Max Samples", &trail.maxSamples, 1, 1, 128);

            float cs[4] = { trail.colorStart.x, trail.colorStart.y, trail.colorStart.z, trail.colorStart.w };
            if (property::ColorEdit4("Color Start", cs)) {
                trail.colorStart = { cs[0], cs[1], cs[2], cs[3] };
            }
            float ce[4] = { trail.colorEnd.x, trail.colorEnd.y, trail.colorEnd.z, trail.colorEnd.w };
            if (property::ColorEdit4("Color End", ce)) {
                trail.colorEnd = { ce[0], ce[1], ce[2], ce[3] };
            }

            property::Checkbox("Double Sided", &trail.doubleSided);
            property::Checkbox("Clear On Disable", &trail.clearOnDisable);
            // .mat アセット参照。albedo テクスチャと doubleSided を .mat から解決する。
            // 変更時はキャッシュを無効化してレンダーパスに再ロードさせる。
            if (property::AssetPathField("Material (.mat)", trail.materialPath, ".mat", ctx.projectRoot)) {
                trail.loadedMaterialPath.clear();
                trail.texture = {};
                trail.loadedTexturePath.clear();
            }
        });

}


} // namespace fbzz::editor
