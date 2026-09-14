/// @file    InspectorEffects.cpp
/// @brief   パーティクル / 力場 / トレイル系コンポーネントの Inspector 描画。
/// @author  Hasegawa Jin
/// @date    2026-06-07

#include "InspectorEffects.hpp"

#include <Editor/Util/ParticleEditWidgets.hpp>
#include <Editor/Util/ParticleEmitterModules.hpp>
#include <Editor/Util/VFXLineInspector.hpp>
#include <Engine/Scene/Components/VFXLineComponent.hpp>
#include <algorithm>
#include <iterator>

namespace fbzz::editor {

void DrawEffectsInspectors(scene::GameObject* go, EditorContext& ctx, std::any& m_componentClipboard, const std::type_info*& m_componentClipboardType)
{
    // ParticleEmitter は Shuriken 式のモジュールスタックで描画する。
    // WHY: 約 80 項目あり、素直に並べると «どこを触ればよいか» が読めなくなる。
    //      Undo は DrawComponentSection の ActiveID 追跡がそのまま効く。
    DrawComponentSection<scene::ParticleEmitter>(go, ctx, m_componentClipboard, m_componentClipboardType, "Particle Emitter",
        [](scene::ParticleEmitter& pe, EditorContext& ctx) {
            // 戻り値は「どれか 1 つでも変わった」。捨てるとモジュール側の編集が
            // シーンの dirty マークへ伝わらず、保存し忘れて消える。
            if (DrawParticleEmitterModules(pe.settings, ctx, &pe) && ctx.markSceneDirty)
                ctx.markSceneDirty();
        });


    DrawComponentSection<scene::ForceField>(go, ctx, m_componentClipboard, m_componentClipboardType, "Particle Force Field",
        [](scene::ForceField& ff, EditorContext& ctx) {
            // 力のリストはエミッター内蔵の力と同じ UI。座標系は GameObject の Transform が
            // 決めるので選択は出さない。
            DrawForceFieldList(ff.forces, ctx, /*showSpace=*/false);
            // channels はシーンに置いた力場だけの概念 (内蔵の力は相手が決まっている)。
            // 力ごとに持てるが、束ねて置く用途では全部同じにしたいことが多いので
            // «先頭に合わせて全部へ» 配る。個別に分けたいなら GameObject を分ける。
            if (!ff.forces.empty()) {
                std::uint32_t mask = ff.forces.front().channels;
                if (widgets::ForceFieldChannelMask("Channels", mask))
                    for (auto& force : ff.forces) force.channels = mask;
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Only emitters sharing a bit with this mask receive these forces.");
            }
        });

    // 雷・ビーム。プリセットと «形が動くプレビュー» を上に置き、細かい値は Reflect の行で出す。
    // WHY 専用にするか: 40 近い値のうち «どれを触ると枝が増えるか» は数値の列からは読めない。
    //      触ったその場で形が変わるのを見せないと、雷は作れない。
    DrawComponentSection<scene::VFXLineComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "VFX Line",
        [](scene::VFXLineComponent& line, EditorContext& ctx) {
            if (DrawVFXLinePresetBar(line) && ctx.markSceneDirty) ctx.markSceneDirty();
            DrawVFXLinePreview(line);
            ComponentImGuiReflector reflector;
            reflector.m_projectRoot = ctx.projectRoot;
            line.Reflect(reflector);
        });

    DrawComponentSection<scene::TrailComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Trail",
        [](scene::TrailComponent& trail, EditorContext& ctx) {
            ImGui::DragFloat("Duration", &trail.duration, 0.01f, 0.01f, 30.0f);
            ImGui::DragInt("Max Points", &trail.maxPoints, 1, 2, 512);
            ImGui::DragFloat("Sample Interval", &trail.sampleInterval, 0.001f, 0.0f, 1.0f);
            ImGui::DragFloat("Min Vertex Dist", &trail.minVertexDist, 0.001f, 0.0f, 10.0f);
            // 瞬間移動の切断。0 は «切らない» で、既存シーンの見た目を保つ既定。
            ImGui::DragFloat("Break Distance", &trail.breakDistance, 0.01f, 0.0f, 500.0f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Cut the trail when it jumps farther than this in one frame. 0 = never cut.");

            // 幅は «2 点 + イージング» と «多キーカーブ» の二択。両方出すと
            // どちらが効いているのか読めないので、有効な側だけを見せる。
            //
            // WHY キャンバスの戻り値を拾うか: カーブ / グラデーションはキーを
            //     ドラッグして編集するため、ImGui の ActiveID 追跡では «変わった» を
            //     取り切れない。捨てるとシーンの dirty が立たず、保存し忘れで消える。
            bool curveChanged = false;
            ImGui::Checkbox("Use Width Curve", &trail.widthCurveEnabled);
            if (trail.widthCurveEnabled) {
                ImGui::DragFloat("Width Scale", &trail.widthStart, 0.001f, 0.0f, 10.0f);
                curveChanged |= widgets::CurveEditor("Width Curve", trail.widthCurve, 1.0f, 96.0f, &ctx.projectRoot);
            } else {
                ImGui::DragFloat("Width Start", &trail.widthStart, 0.001f, 0.0f, 10.0f);
                ImGui::DragFloat("Width End", &trail.widthEnd, 0.001f, 0.0f, 10.0f);
                const char* widthEasingItems[] = { "Linear", "Ease In", "Ease Out", "Ease In Out" };
                int widthEasing = static_cast<int>(trail.widthEasing);
                if (ImGui::Combo("Width Easing", &widthEasing, widthEasingItems, 4))
                    trail.widthEasing = static_cast<scene::TrailWidthEasing>(widthEasing);
            }

            ImGui::Checkbox("Use Color Gradient", &trail.colorGradientEnabled);
            if (trail.colorGradientEnabled) {
                curveChanged |= widgets::GradientEditor("Color Gradient", trail.colorGradient, &ctx.projectRoot);
            } else {
                float cs[4] = { trail.colorStart.x, trail.colorStart.y, trail.colorStart.z, trail.colorStart.w };
                if (ImGui::ColorEdit4("Color Start", cs))
                    trail.colorStart = { cs[0], cs[1], cs[2], cs[3] };
                float ce[4] = { trail.colorEnd.x, trail.colorEnd.y, trail.colorEnd.z, trail.colorEnd.w };
                if (ImGui::ColorEdit4("Color End", ce))
                    trail.colorEnd = { ce[0], ce[1], ce[2], ce[3] };
            }
            if (curveChanged && ctx.markSceneDirty) ctx.markSceneDirty();

            const char* alignmentItems[] = { "Camera Facing", "World Up" };
            int alignment = static_cast<int>(trail.alignment);
            if (ImGui::Combo("Alignment", &alignment, alignmentItems, 2))
                trail.alignment = static_cast<scene::TrailAlignment>(alignment);

            ImGui::DragInt("Smooth Subdivisions", &trail.smoothSubdivisions, 1, 0, 8);
            char boneBuf[256];
            std::snprintf(boneBuf, sizeof(boneBuf), "%s", trail.attachBone.c_str());
            if (ImGui::InputText("Attach Bone", boneBuf, sizeof(boneBuf)))
                trail.attachBone = boneBuf;
            widgets::DragVec3("Attach Offset", trail.attachOffset, 0.001f);
            ImGui::Checkbox("Clear On Disable", &trail.clearOnDisable);
            // .mat アセット参照。albedo テクスチャを .mat から解決する。
            // 変更時はキャッシュを無効化してレンダーパスに再ロードさせる。
            if (widgets::AssetPathField("Material (.mat)", trail.materialPath, ".mat", ctx.projectRoot)) {
                trail.loadedMaterialPath.clear();
                trail.texture = {};
                trail.loadedTexturePath.clear();
            }
            const char* uvModeItems[] = { "Stretch", "Tile" };
            int uvMode = static_cast<int>(trail.uvMode);
            if (ImGui::Combo("UV Mode", &uvMode, uvModeItems, 2))
                trail.uvMode = static_cast<scene::TrailUVMode>(uvMode);
            ImGui::DragFloat("UV Scroll Speed", &trail.uvScrollSpeed, 0.001f, -20.0f, 20.0f);
            ImGui::DragFloat("UV Tiling", &trail.uvTiling, 0.001f, 0.001f, 100.0f);
        });

    DrawComponentSection<scene::MeshTrailComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Mesh Trail",
        [](scene::MeshTrailComponent& trail, EditorContext& ctx) {
            ImGui::DragFloat("Duration", &trail.duration, 0.01f, 0.01f, 30.0f);
            ImGui::DragFloat("Sample Interval", &trail.sampleInterval, 0.001f, 0.0f, 1.0f);
            ImGui::DragFloat("Min Vertex Dist", &trail.minVertexDist, 0.001f, 0.0f, 10.0f);
            ImGui::DragInt("Max Samples", &trail.maxSamples, 1, 1, 128);

            float cs[4] = { trail.colorStart.x, trail.colorStart.y, trail.colorStart.z, trail.colorStart.w };
            if (ImGui::ColorEdit4("Color Start", cs))
                trail.colorStart = { cs[0], cs[1], cs[2], cs[3] };
            float ce[4] = { trail.colorEnd.x, trail.colorEnd.y, trail.colorEnd.z, trail.colorEnd.w };
            if (ImGui::ColorEdit4("Color End", ce))
                trail.colorEnd = { ce[0], ce[1], ce[2], ce[3] };

            ImGui::Checkbox("Double Sided", &trail.doubleSided);
            ImGui::Checkbox("Clear On Disable", &trail.clearOnDisable);
            // .mat アセット参照。albedo テクスチャと doubleSided を .mat から解決する。
            // 変更時はキャッシュを無効化してレンダーパスに再ロードさせる。
            if (widgets::AssetPathField("Material (.mat)", trail.materialPath, ".mat", ctx.projectRoot)) {
                trail.loadedMaterialPath.clear();
                trail.texture = {};
                trail.loadedTexturePath.clear();
            }
        });

}


} // namespace fbzz::editor
