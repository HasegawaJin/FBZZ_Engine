// FBZZ Engine
// VFXEditorUiCommon.cpp | fbzz::editor
// View 群で共有する ImGui ヘルパーの実装
#include <Editor/VFXEditor/Views/VFXEditorUiCommon.hpp>

#include <Editor/VFXEditor/Application/VFXEditorSession.hpp>
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
#include <Engine/Scene/Components/ParticleGpuSimulation.hpp>
#include <Engine/Scene/Components/VFXGraphComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <imnodes.h>
#include <toml++/toml.hpp>
#include <algorithm>
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

namespace fbzz::editor::vfx {

// 実体はウィジェット層へ移した。VFX 以外のパネルからも使うため。
// ここは呼び出し側を壊さないための転送だけ。
bool InputString(const char* label, std::string& value, std::size_t capacity)
{
    return widgets::InputString(label, value, capacity);
}

bool ReadAssetPayload(const ImGuiPayload* payload, std::string& outPath)
{
    if (payload == nullptr || payload->Data == nullptr || payload->DataSize <= 1) return false;
    const auto* text = static_cast<const char*>(payload->Data);
    if (text[payload->DataSize - 1] != '\0') return false;
    outPath = text;
    return !outPath.empty();
}

// Inspector のアセット欄で使う projectRoot。widgets::AssetPathField が必要とするが
// この匿名ヘルパーは EditorContext を受け取らないため、描画開始時に一度だけ写す。
std::string g_assetFieldProjectRoot;
renderer::ResourceManager* g_thumbnailResources = nullptr;
renderer::IImGuiRenderer*  g_thumbnailImGuiRenderer = nullptr;

std::string ProceduralDecalOutputDirectory(const std::string& projectRoot)
{
    if (projectRoot.empty()) return "Assets/Textures/Generated/VFX";
    return projectRoot + "/Assets/Textures/Generated/VFX";
}

// Scene Inspector と同じアセット欄。拡張子バッジ + ファイル名だけを表示し、
// クリックでフルパス編集、"..." でピッカー、ドラッグ&ドロップで割り当てできる。
// WHY: VFX Editor だけ自前の InputText でフルパスを出していたため、長いパスが欄で切れて
//      何が刺さっているのか読めなかった。表示も操作もメインエディタへ揃える。
bool AssetPathField(const char* label, std::string& value, const char* filterExts)
{
    return widgets::AssetPathField(label, value, filterExts, g_assetFieldProjectRoot);
}

int InputPinId(int nodeId) { return 100000 + nodeId * 2; }
int OutputPinId(int nodeId) { return 100000 + nodeId * 2 + 1; }
int LinkId(int linkIndex) { return 200000 + linkIndex; }

const char* VFXNodeIcon(asset::VFXNodeType type)
{
    switch (type) {
    case asset::VFXNodeType::Entry: return "IN";
    case asset::VFXNodeType::Delay: return "T";
    case asset::VFXNodeType::Particle: return "P";
    case asset::VFXNodeType::Trail: return "TR";
    case asset::VFXNodeType::MeshTrail: return "MT";
    case asset::VFXNodeType::Light: return "L";
    case asset::VFXNodeType::Audio: return "A";
    case asset::VFXNodeType::Decal: return "D";
    case asset::VFXNodeType::SubGraph: return "SG";
    case asset::VFXNodeType::ForceField: return "FF";
    case asset::VFXNodeType::Mesh: return "M";
    case asset::VFXNodeType::ScreenEffect: return "FX";
    case asset::VFXNodeType::CameraShake: return "CAM";
    case asset::VFXNodeType::TimeScale: return "TIME";
    case asset::VFXNodeType::Wind: return "W";
    case asset::VFXNodeType::Reroute: return "\xe2\x80\xa2"; // 中黒。配線の折れ点であることを最小の記号で示す
    case asset::VFXNodeType::AnimatedMesh: return "AM";
    }
    return "?";
}

ImU32 VFXNodeColor(asset::VFXNodeType type)
{
    switch (type) {
    case asset::VFXNodeType::Entry: return IM_COL32(43, 112, 82, 255);
    case asset::VFXNodeType::Delay: return IM_COL32(91, 83, 132, 255);
    case asset::VFXNodeType::Particle: return IM_COL32(129, 76, 35, 255);
    case asset::VFXNodeType::Trail:
    case asset::VFXNodeType::MeshTrail: return IM_COL32(42, 100, 122, 255);
    case asset::VFXNodeType::Light: return IM_COL32(139, 112, 29, 255);
    case asset::VFXNodeType::Audio: return IM_COL32(92, 56, 123, 255);
    case asset::VFXNodeType::Decal: return IM_COL32(107, 58, 67, 255);
    case asset::VFXNodeType::SubGraph: return IM_COL32(48, 83, 133, 255);
    case asset::VFXNodeType::ForceField: return IM_COL32(38, 108, 96, 255);
    case asset::VFXNodeType::Mesh: return IM_COL32(88, 96, 118, 255);
    case asset::VFXNodeType::ScreenEffect: return IM_COL32(146, 74, 108, 255);
    case asset::VFXNodeType::CameraShake: return IM_COL32(122, 62, 46, 255);
    case asset::VFXNodeType::TimeScale: return IM_COL32(70, 70, 96, 255);
    case asset::VFXNodeType::Wind: return IM_COL32(52, 96, 88, 255);
    // 配線の一部として読ませたいので、他のノードより彩度を落として背景に寄せる。
    case asset::VFXNodeType::Reroute: return IM_COL32(78, 82, 90, 255);
    case asset::VFXNodeType::AnimatedMesh: return IM_COL32(76, 112, 154, 255);
    }
    return IM_COL32(60, 64, 74, 255);
}

// ノードが代表する「絵」のパスを返す。無ければ空。
// WHY: 種別ごとに素材の在り処が違う (Particle は material/texture、Decal は albedo)。
//      サムネイルを出す側がノード型の知識を持たなくて済むよう、ここで 1 か所に寄せる。
std::string VFXNodeThumbnailAsset(const asset::VFXGraphNode& node)
{
    switch (node.type) {
    case asset::VFXNodeType::Particle:
        // 実行時は materialPath が blendMode ごと上書きするので、あるならそちらを代表画にする。
        return node.particle.materialPath;
    case asset::VFXNodeType::Trail:
    case asset::VFXNodeType::MeshTrail:
        return node.trail.materialPath;
    case asset::VFXNodeType::Decal:
        return node.decal.albedoPath;
    case asset::VFXNodeType::Mesh:
        return node.mesh.materialPath;
    case asset::VFXNodeType::AnimatedMesh:
        return node.animatedMesh.materialPath.empty()
            ? node.animatedMesh.modelPath : node.animatedMesh.materialPath;
    default:
        return {};
    }
}

void DrawVFXNodeThumbnail(const asset::VFXGraphNode& node)
{
    if (g_thumbnailResources == nullptr || g_thumbnailImGuiRenderer == nullptr) return;
    const std::string path = VFXNodeThumbnailAsset(node);
    if (path.empty()) return;
    void* textureId = widgets::ResolveAssetThumbnail(path, g_thumbnailResources, g_thumbnailImGuiRenderer);
    if (textureId == nullptr) return;

    // 小さすぎると何の絵か判らず、大きいとノードが縦に伸びて配線が見えなくなる。
    // Solo フィルタのバッジと並べても 1 行に収まる高さに合わせる。
    constexpr float kThumbnailSize = 48.0f;
    ImGui::Image(widgets::ToImTextureID(textureId), ImVec2{ kThumbnailSize, kThumbnailSize });
    if (ImGui::IsItemHovered()) {
        // 拡大表示。粒子素材はアルファの抜き方が要で、48px では縁の勾配まで見えない。
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(path.c_str());
        ImGui::Image(widgets::ToImTextureID(textureId), ImVec2{ 192.0f, 192.0f });
        ImGui::EndTooltip();
    }
}

// ノード本体へ種別ごとの要点を出す。
// WHY: 以前は offset/duration しか出ておらず、どのマテリアルを使う Particle なのか、
//      どの .vfx を呼ぶ SubGraph なのかを知るには毎回 Inspector で1つずつ選ぶ必要があった。
void DrawVFXNodeSummary(const asset::VFXGraphNode& node)
{
    const auto assetLine = [](const char* label, const std::string& path) {
        if (path.empty()) return;
        ImGui::TextDisabled("%s %s", label, PathBasename(path).c_str());
    };
    DrawVFXNodeThumbnail(node);
    switch (node.type) {
    case asset::VFXNodeType::Particle:
        assetLine("M", node.particle.materialPath);
        assetLine("Mesh", node.particle.meshShapePath);
        ImGui::TextDisabled("rate %.0f/s  max %d%s", node.particle.emitRate,
                            node.particle.maxParticles, node.particle.loop ? "  loop" : "");
        if (!node.particle.bursts.empty())
            ImGui::TextDisabled("bursts %d", static_cast<int>(node.particle.bursts.size()));
        // 有効な高度表現をバッジで出す。
        // WHY: 歪み・six-way lighting・モーションベクターは実装済みなのに Inspector の
        //      モジュールスタック深部にあり、存在に気付かれないまま使われていなかった。
        {
            // GPU バッジは「要求」ではなく「実際に GPU で回るか」で出す。
            // WHY: 縮退したノードに GPU と書いてあると、グラフを見ただけでは
            //      どこで性能を取り逃しているのか永久に判らない。
            const auto gpuFallback = scene::GetParticleGpuFallbackReason(node.particle);
            const std::pair<bool, const char*> badges[] = {
                { node.particle.distortion, "DISTORT" },
                { node.particle.sixWayLighting, "6WAY" },
                { node.particle.motionVectorFlipbook, "MV" },
                { node.particle.softParticles, "SOFT" },
                { gpuFallback == scene::ParticleGpuFallbackReason::None, "GPU" },
            };
            bool first = true;
            for (const auto& [active, label] : badges) {
                if (!active) continue;
                if (!first) ImGui::SameLine();
                first = false;
                ImGui::TextColored({ 0.55f, 0.82f, 0.95f, 1.0f }, "%s", label);
            }
            if (node.particle.simulationMode == scene::ParticleSimulationMode::Gpu
                && gpuFallback != scene::ParticleGpuFallbackReason::None) {
                if (!first) ImGui::SameLine();
                ImGui::TextColored({ 1.0f, 0.65f, 0.3f, 1.0f }, "GPU\xe2\x86\x92""CPU (%s)",
                                   scene::ParticleGpuFallbackFieldName(gpuFallback));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", scene::ParticleGpuFallbackDescription(gpuFallback));
            }
        }
        break;
    case asset::VFXNodeType::Trail:
    case asset::VFXNodeType::MeshTrail:
        assetLine("Mesh", node.trail.meshPath);
        assetLine("M", node.trail.materialPath);
        ImGui::TextDisabled("life %.2fs  width %.2f->%.2f", node.trail.lifetime,
                            node.trail.widthStart, node.trail.widthEnd);
        break;
    case asset::VFXNodeType::Light: {
        // 色は文字より四角で見せた方が一目で分かる。
        const ImVec4 color{ node.light.color.x, node.light.color.y, node.light.color.z, 1.0f };
        ImGui::ColorButton("##VFXNodeLightColor", color,
                           ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                           { 14.0f, 14.0f });
        ImGui::SameLine();
        ImGui::TextDisabled("%.1f int  %.1fm", node.light.intensity, node.light.range);
        break;
    }
    case asset::VFXNodeType::Audio:
        assetLine("Clip", node.audio.clipPath);
        ImGui::TextDisabled("vol %.2f  pitch %.2f%s", node.audio.volume, node.audio.pitch,
                            node.audio.loop ? "  loop" : "");
        break;
    case asset::VFXNodeType::Decal:
        assetLine("Albedo", node.decal.albedoPath);
        ImGui::TextDisabled("fade %.2fs", node.decal.fadeTime);
        break;
    case asset::VFXNodeType::SubGraph:
        if (node.subGraph.graphPath.empty())
            ImGui::TextColored({ 1.0f, 0.72f, 0.35f, 1.0f }, "no graph assigned");
        else
            assetLine("Graph", node.subGraph.graphPath);
        break;
    case asset::VFXNodeType::ForceField: {
        constexpr const char* fieldNames[] = { "Wind", "Attract", "Repulse",
                                               "Vortex", "Turbulence", "Drag" };
        const int fieldType = std::clamp(node.forceField.fieldType, 0, 5);
        ImGui::TextDisabled("%s  %.1f", fieldNames[fieldType], node.forceField.strength);
        if (node.forceField.radius > 0.0f) ImGui::TextDisabled("radius %.1fm", node.forceField.radius);
        else ImGui::TextDisabled("global (no falloff)");
        break;
    }
    case asset::VFXNodeType::Mesh: {
        assetLine("Mesh", node.mesh.meshPath);
        if (node.mesh.materialPath.empty()) ImGui::TextDisabled("M (fallback additive)");
        else assetLine("M", node.mesh.materialPath);
        const ImVec4 start{ node.mesh.colorStart.x, node.mesh.colorStart.y,
                            node.mesh.colorStart.z, 1.0f };
        ImGui::ColorButton("##VFXNodeMeshColor", start,
                           ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                           { 14.0f, 14.0f });
        ImGui::SameLine();
        ImGui::TextDisabled("scale %.2f -> %.2f", node.mesh.scaleStart, node.mesh.scaleEnd);
        break;
    }
    case asset::VFXNodeType::AnimatedMesh:
        assetLine("Model", node.animatedMesh.modelPath);
        assetLine("Ctrl", node.animatedMesh.controllerPath);
        if (!node.animatedMesh.initialState.empty())
            ImGui::TextDisabled("state %s  x%.2f", node.animatedMesh.initialState.c_str(),
                                node.animatedMesh.speed);
        break;
    case asset::VFXNodeType::ScreenEffect: {
        if (node.screenEffect.flashIntensity > 0.0f) {
            const ImVec4 flash{ node.screenEffect.flashColor.x, node.screenEffect.flashColor.y,
                                node.screenEffect.flashColor.z, 1.0f };
            ImGui::ColorButton("##VFXNodeFlash", flash,
                               ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                               { 14.0f, 14.0f });
            ImGui::SameLine();
            ImGui::TextDisabled("flash %.2f", node.screenEffect.flashIntensity);
        }
        if (node.screenEffect.bloomBoost > 0.0f)
            ImGui::TextDisabled("bloom +%.2f", node.screenEffect.bloomBoost);
        if (node.screenEffect.chromaticAberration > 0.0f)
            ImGui::TextDisabled("chroma +%.3f", node.screenEffect.chromaticAberration);
        if (node.screenEffect.lensDistortion != 0.0f)
            ImGui::TextDisabled("distort %+.2f", node.screenEffect.lensDistortion);
        break;
    }
    case asset::VFXNodeType::CameraShake:
        ImGui::TextDisabled("amp %.2fm  rot %.1f deg", node.cameraShake.amplitude,
                            node.cameraShake.rotationAmplitude);
        ImGui::TextDisabled("%.0fHz  reach %.0fm", node.cameraShake.frequency,
                            node.cameraShake.radius);
        break;
    case asset::VFXNodeType::TimeScale:
        ImGui::TextDisabled("x%.2f", node.timeScale.timeScale);
        ImGui::TextDisabled("in %.2fs  out %.2fs", node.timeScale.blendInTime,
                            node.timeScale.blendOutTime);
        break;
    case asset::VFXNodeType::Wind:
        ImGui::TextDisabled("strength %.2f  turb %.2f", node.wind.strength, node.wind.turbulence);
        break;
    case asset::VFXNodeType::Delay:
    case asset::VFXNodeType::Entry:
        break;
    }
}

bool DrawVFXParamValue(const char* label, asset::VFXParamType type, asset::VFXParamValue& value,
                       float minimum, float maximum, bool hasRange)
{
    constexpr const char* SOURCE_NAMES[] = { "Constant", "Curve", "Gradient", "Random Range", "Attribute", "Signal" };
    int sourceType = static_cast<int>(value.source.index());
    bool sourceChanged = false;
    ImGui::PushID(label);
    if (ImGui::BeginCombo("Source", SOURCE_NAMES[sourceType])) {
        for (int candidate = 0; candidate < 6; ++candidate) {
            const bool supported = candidate == 0 || candidate >= 4
                || (candidate == 1 && type == asset::VFXParamType::Float)
                || (candidate == 2 && type == asset::VFXParamType::Color)
                || (candidate == 3 && (type == asset::VFXParamType::Float || type == asset::VFXParamType::Int));
            if (!supported) continue;
            if (ImGui::Selectable(SOURCE_NAMES[candidate], sourceType == candidate)) {
                sourceType = candidate;
                sourceChanged = true;
                if (candidate == 0) value = DefaultVFXParamValue(type);
                else if (candidate == 1) value.source = asset::VFXCurveSource{};
                else if (candidate == 2) value.source = asset::VFXGradientSource{};
                else if (candidate == 3) value.source = asset::VFXRandomRange{ minimum, maximum };
                else if (candidate == 4) value.source = asset::VFXAttributeRef{};
                else value.source = asset::VFXSignalRef{};
            }
        }
        ImGui::EndCombo();
    }
    bool changed = sourceChanged;
    if (auto* curve = std::get_if<asset::VFXCurveSource>(&value.source)) {
        int count = static_cast<int>(curve->curve.keyCount);
        changed |= ImGui::SliderInt("Keys", &count, 1, 4);
        curve->curve.keyCount = static_cast<std::uint32_t>(count);
        for (int index = 0; index < count; ++index) {
            ImGui::PushID(index);
            changed |= ImGui::DragFloat2("Time / Value", &curve->curve.keys[static_cast<std::size_t>(index)].time, 0.01f);
            ImGui::PopID();
        }
        ImGui::PopID();
        return changed;
    }
    if (auto* gradient = std::get_if<asset::VFXGradientSource>(&value.source)) {
        int count = static_cast<int>(gradient->gradient.keyCount);
        changed |= ImGui::SliderInt("Keys", &count, 1, 4);
        gradient->gradient.keyCount = static_cast<std::uint32_t>(count);
        for (int index = 0; index < count; ++index) {
            ImGui::PushID(index);
            changed |= ImGui::SliderFloat("Time", &gradient->gradient.keys[static_cast<std::size_t>(index)].time, 0.0f, 1.0f);
            changed |= ImGui::ColorEdit4("Color", &gradient->gradient.keys[static_cast<std::size_t>(index)].color.x);
            ImGui::PopID();
        }
        ImGui::PopID();
        return changed;
    }
    if (auto* random = std::get_if<asset::VFXRandomRange>(&value.source)) {
        changed |= ImGui::DragFloatRange2("Range", &random->minimum, &random->maximum, 0.01f);
        ImGui::PopID();
        return changed;
    }
    if (auto* attribute = std::get_if<asset::VFXAttributeRef>(&value.source)) {
        changed |= InputString("Attribute Path", attribute->path, 256);
        ImGui::TextDisabled("self/parent/guid:<id>.transform|physics|animator|script.*");
        ImGui::PopID();
        return changed;
    }
    if (auto* signal = std::get_if<asset::VFXSignalRef>(&value.source)) {
        changed |= InputString("Signal Output", signal->signalName, 128);
        ImGui::PopID();
        return changed;
    }
    auto* constant = std::get_if<asset::VFXConstant>(&value.source);
    if (constant == nullptr) {
        ImGui::TextDisabled("%s uses a dynamic value source", label);
        ImGui::PopID();
        return false;
    }
    if (type == asset::VFXParamType::Float) {
        auto* item = std::get_if<float>(constant);
        if (item == nullptr) { value = DefaultVFXParamValue(type); item = std::get_if<float>(std::get_if<asset::VFXConstant>(&value.source)); }
        changed |= hasRange ? ImGui::SliderFloat(label, item, minimum, maximum) : ImGui::DragFloat(label, item, 0.01f);
    }
    else if (type == asset::VFXParamType::Int) {
        auto* item = std::get_if<int>(constant);
        if (item == nullptr) { value = DefaultVFXParamValue(type); item = std::get_if<int>(std::get_if<asset::VFXConstant>(&value.source)); }
        changed |= hasRange ? ImGui::SliderInt(label, item, static_cast<int>(minimum), static_cast<int>(maximum))
                            : ImGui::DragInt(label, item);
    }
    else if (type == asset::VFXParamType::Bool) {
        auto* item = std::get_if<bool>(constant);
        if (item == nullptr) { value = DefaultVFXParamValue(type); item = std::get_if<bool>(std::get_if<asset::VFXConstant>(&value.source)); }
        changed |= ImGui::Checkbox(label, item);
    }
    else if (type == asset::VFXParamType::Color) {
        auto* item = std::get_if<math::Vector4>(constant);
        if (item == nullptr) { value = DefaultVFXParamValue(type); item = std::get_if<math::Vector4>(std::get_if<asset::VFXConstant>(&value.source)); }
        changed |= ImGui::ColorEdit4(label, &item->x);
    }
    else if (type == asset::VFXParamType::Vector3) {
        auto* item = std::get_if<math::Vector3>(constant);
        if (item == nullptr) { value = DefaultVFXParamValue(type); item = std::get_if<math::Vector3>(std::get_if<asset::VFXConstant>(&value.source)); }
        changed |= ImGui::DragFloat3(label, &item->x, 0.01f);
    }
    else {
        auto* item = std::get_if<std::string>(constant);
        if (item == nullptr) { value = DefaultVFXParamValue(type); item = std::get_if<std::string>(std::get_if<asset::VFXConstant>(&value.source)); }
        changed |= AssetPathField(label, *item);
    }
    ImGui::PopID();
    return changed;
}

} // namespace fbzz::editor::vfx
