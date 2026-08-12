// FBZZ Engine
// VFXGraphInspector.cpp | fbzz::editor
// ノード / リンク / 公開パラメーター Inspector View の実装
#include <Editor/VFXEditor/Views/VFXGraphInspector.hpp>

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
#include <Engine/Scene/Components/VFXGraphComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <imnodes.h>
#include <toml++/toml.hpp>
#include <algorithm>
#include <any>
#include <cctype>
#include <charconv>
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
#include <Editor/VFXEditor/Views/VFXPreviewView.hpp>

namespace fbzz::editor {

// 共有ヘルパー (ノード色・アイコン・パラメーター編集など) は fbzz::editor::vfx に
// 隔離してある。Editor 全体の名前空間を汚さないための境界。
using namespace vfx;

namespace {

// 解析が返す推奨値 (JSON 表記の文字列) を ParticleEmitter のスキーマ leaf へ書き込む。
// WHY: 推奨値は AI と Editor で同じものを使うため、AI 側の表現 (JSON) をそのまま持っている。
//      Editor 側だけ別表現にすると、同じ推奨に 2 つの適用経路ができてドリフトする。
//      対応する型は解析が実際に返す bool / int / float だけで足りる。
bool ApplyParticleSchemaValue(scene::ParticleEmitter& particle,
                              const std::string& schemaPath, const std::string& value)
{
    // schemaPath は "particle." 始まりで返る (AI の vfx.node.setField と揃えてあるため)。
    // ParticleEmitter のスキーマはその接頭辞を持たないので落とす。
    constexpr std::string_view kPrefix = "particle.";
    const std::string_view leaf = schemaPath.starts_with(kPrefix)
        ? std::string_view(schemaPath).substr(kPrefix.size()) : std::string_view(schemaPath);

    reflection::ResolvedProperty resolved;
    if (!reflection::ResolveProperty(asset::GetParticleEmitterSchema(), &particle, leaf, resolved)
        || resolved.property == nullptr)
        return false;

    std::any typed;
    if (value == "true") {
        typed = true;
    } else if (value == "false") {
        typed = false;
    } else {
        // 数値。プロパティの宣言型に合わせないと set が拒否するため、型で分岐する。
        // 例外は規約で禁止されているため std::stof / stoi ではなく from_chars を使う。
        const char* first = value.data();
        const char* last = value.data() + value.size();
        if (resolved.property->type == reflection::PropertyType::Float) {
            float parsed = 0.0f;
            const auto result = std::from_chars(first, last, parsed);
            if (result.ec != std::errc{} || result.ptr != last) return false;
            typed = parsed;
        } else {
            int parsed = 0;
            const auto result = std::from_chars(first, last, parsed);
            if (result.ec != std::errc{} || result.ptr != last) return false;
            typed = parsed;
        }
    }
    return resolved.property->set(resolved.owner, typed);
}

} // namespace

bool VFXGraphInspector::DrawMaterialAnalysis(scene::ParticleEmitter& particle)
{
    const std::string& target = particle.materialPath;
    if (m_analyzedMaterialPath != target) {
        m_analyzedMaterialPath = target;
        m_materialAnalysis = asset::AnalyzeMaterial(asset::AssetManager::ResolveAssetPath(target));
    }
    if (!m_materialAnalysis.success) {
        ImGui::TextColored({ 1.0f, 0.55f, 0.48f, 1.0f }, "Material analysis failed: %s",
                           m_materialAnalysis.message.c_str());
        return false;
    }

    if (!ImGui::CollapsingHeader("Material Analysis")) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", m_materialAnalysis.message.c_str());
        return false;
    }

    ImGui::Indent();
    // 最初に出すのは「Emitter の blendMode が効かない」こと。
    // これを知らないまま下のスライダーを触ると、変えても絵が変わらず時間を溶かす。
    ImGui::TextColored({ 1.0f, 0.84f, 0.45f, 1.0f },
                       "Blend Mode comes from the .mat (%s) - the emitter setting is ignored",
                       m_materialAnalysis.blendMode.c_str());
    ImGui::TextDisabled("render_path %s   queue %d%s%s",
                        m_materialAnalysis.renderPath.c_str(), m_materialAnalysis.renderQueue,
                        m_materialAnalysis.doubleSided ? "   double-sided" : "",
                        m_materialAnalysis.depthWrite ? "   depth-write" : "");
    if (m_materialAnalysis.hasAlbedoTexture)
        ImGui::TextDisabled("albedo: %s", m_materialAnalysis.albedoAnalysis.success
                            ? m_materialAnalysis.albedoAnalysis.message.c_str()
                            : m_materialAnalysis.albedoTexturePath.c_str());

    for (const auto& finding : m_materialAnalysis.findings) {
        ImGui::TextColored({ 1.0f, 0.78f, 0.35f, 1.0f }, "!");
        ImGui::SameLine();
        ImGui::TextWrapped("%s", finding.c_str());
    }

    bool changed = false;
    if (!m_materialAnalysis.recommendations.empty()) {
        ImGui::Spacing();
        // ここに出るのは .mat では表現できず Emitter 側にしか無い設定だけ。
        // blend_mode のように .mat を直すべきものは findings 側へ出している。
        ImGui::TextDisabled("Emitter-side settings");
        if (ImGui::Button("Apply All##MaterialAnalysis")) {
            for (const auto& recommendation : m_materialAnalysis.recommendations)
                changed |= ApplyParticleSchemaValue(particle, recommendation.schemaPath,
                                                    recommendation.value);
        }
        for (const auto& recommendation : m_materialAnalysis.recommendations) {
            ImGui::PushID(recommendation.schemaPath.c_str());
            if (ImGui::SmallButton("Apply"))
                changed |= ApplyParticleSchemaValue(particle, recommendation.schemaPath,
                                                    recommendation.value);
            ImGui::SameLine();
            ImGui::TextUnformatted(recommendation.schemaPath.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("= %s", recommendation.value.c_str());
            ImGui::TextWrapped("    %s", recommendation.reason.c_str());
            ImGui::PopID();
        }
    }
    ImGui::Unindent();
    return changed;
}

bool VFXGraphInspector::DrawTextureAnalysis(scene::ParticleEmitter& particle)
{
    // .mat が割り当てられていれば、描画に使われるのは .mat 側のテクスチャとブレンド設定。
    // そちらを解析しないと、実際には描かれない画像について推奨を出すことになる。
    if (!particle.materialPath.empty()) return DrawMaterialAnalysis(particle);

    const std::string& target = particle.texturePath;
    if (target.empty()) {
        m_analyzedTexturePath.clear();
        return false;
    }

    // パスが変わったときだけ解析し直す。毎フレーム走らせるとドラッグ操作が固まる。
    if (m_analyzedTexturePath != target) {
        m_analyzedTexturePath = target;
        m_textureAnalysis = asset::AnalyzeTexture(asset::AssetManager::ResolveAssetPath(target));
    }
    if (!m_textureAnalysis.success) {
        ImGui::TextColored({ 1.0f, 0.55f, 0.48f, 1.0f }, "Texture analysis failed: %s",
                           m_textureAnalysis.message.c_str());
        return false;
    }

    // 開閉は ImGui に持たせる (自前の状態と二重管理にすると必ずずれる)。
    if (!ImGui::CollapsingHeader("Texture Analysis")) {
        // 畳んでいる間も要約だけは出す。開かないと何も分からない状態にはしない。
        ImGui::SameLine();
        ImGui::TextDisabled("%s", m_textureAnalysis.message.c_str());
        return false;
    }

    ImGui::Indent();
    ImGui::Text("Type: %s   %dx%d%s", m_textureAnalysis.classification.c_str(),
                m_textureAnalysis.width, m_textureAnalysis.height,
                m_textureAnalysis.powerOfTwo ? "" : "  (not power of two)");

    // アルファは「チャンネルの有無」ではなく「実データを持つか」が判断材料。
    if (!m_textureAnalysis.alphaIsMeaningful)
        ImGui::TextColored({ 1.0f, 0.78f, 0.35f, 1.0f },
                           "Alpha carries no data (%.2f-%.2f) - needs Alpha Source = Luminance",
                           m_textureAnalysis.alphaMin, m_textureAnalysis.alphaMax);
    else
        ImGui::TextDisabled("Alpha %.2f-%.2f (mean %.2f)%s",
                            m_textureAnalysis.alphaMin, m_textureAnalysis.alphaMax,
                            m_textureAnalysis.alphaMean,
                            m_textureAnalysis.likelyPremultiplied ? "  premultiplied" : "");
    ImGui::TextDisabled("Coverage %.0f%%   Core hotspot x%.2f   Edge %.2f",
                        m_textureAnalysis.coverage * 100.0f,
                        m_textureAnalysis.coreHotspot, m_textureAnalysis.edgeHardness);

    if (!m_textureAnalysis.flipbookCandidates.empty()) {
        const auto& best = m_textureAnalysis.flipbookCandidates.front();
        ImGui::TextDisabled("Flipbook %dx%d (%d frames)", best.columns, best.rows,
                            best.columns * best.rows);
    }

    bool changed = false;
    if (!m_textureAnalysis.recommendations.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("Recommended settings");
        // 一括適用。1 件ずつ押させると、素材を差し替えるたびに 4〜5 回クリックすることになる。
        if (ImGui::Button("Apply All##TextureAnalysis")) {
            for (const auto& recommendation : m_textureAnalysis.recommendations)
                changed |= ApplyParticleSchemaValue(particle, recommendation.schemaPath,
                                                    recommendation.value);
        }
        for (const auto& recommendation : m_textureAnalysis.recommendations) {
            ImGui::PushID(recommendation.schemaPath.c_str());
            if (ImGui::SmallButton("Apply"))
                changed |= ApplyParticleSchemaValue(particle, recommendation.schemaPath,
                                                    recommendation.value);
            ImGui::SameLine();
            ImGui::TextUnformatted(recommendation.schemaPath.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("= %s", recommendation.value.c_str());
            // 根拠は畳まず出す。理由の見えない推奨は従う判断ができない。
            ImGui::TextWrapped("    %s", recommendation.reason.c_str());
            ImGui::PopID();
        }
    }
    ImGui::Unindent();
    return changed;
}

void VFXGraphInspector::DrawParameters()
{
    if (!ImGui::CollapsingHeader("Exposed Parameters", ImGuiTreeNodeFlags_DefaultOpen)) return;
    // パラメーター/バインドの変更はどのノードへ効くか分からないため、
    // ノード限定の再適用を解除して全ノードを対象へ戻す。
    const std::uint64_t revisionOnEntry = m_session.document.dirty.Revision();
    struct ClearNodeScope {
        VFXGraphDocument* document;
        std::uint64_t before;
        ~ClearNodeScope()
        {
            if (document->dirty.Revision() != before) document->liveDirtyNodeId = -1;
        }
    } clearNodeScope{ &m_session.document, revisionOnEntry };
    if (ImGui::Button("+ Parameter", { -1.0f, 0.0f })) {
        m_session.PushUndo();
        std::string name = "Parameter";
        int suffix = 1;
        const auto exists = [&](const std::string& candidate) {
            return std::any_of(m_session.document.graph.parameters.begin(), m_session.document.graph.parameters.end(),
                [&](const asset::VFXParamDefinition& item) { return item.name == candidate; });
        };
        while (exists(name)) name = "Parameter" + std::to_string(++suffix);
        m_session.document.graph.parameters.push_back({ name, asset::VFXParamType::Float,
                                       DefaultVFXParamValue(asset::VFXParamType::Float) });
        m_session.document.dirty = true;
    }

    std::vector<std::string> schemaPaths;
    CollectExposablePaths(asset::GetVFXNodeSchema(), {}, schemaPaths);
    int deleteParameter = -1;
    for (std::size_t index = 0; index < m_session.document.graph.parameters.size(); ++index) {
        auto& parameter = m_session.document.graph.parameters[index];
        ImGui::PushID(static_cast<int>(index));
        const std::string header = parameter.name + "  [" + VFXParamTypeName(parameter.type) + "]";
        if (ImGui::TreeNodeEx("##Parameter", ImGuiTreeNodeFlags_DefaultOpen, "%s", header.c_str())) {
            const std::string oldName = parameter.name;
            if (InputString("Name", parameter.name, 128)) {
                for (auto& binding : m_session.document.graph.bindings)
                    if (binding.paramName == oldName) binding.paramName = parameter.name;
                for (auto& variant : m_session.document.graph.variants)
                    for (auto& item : variant.overrides)
                        if (item.paramName == oldName) item.paramName = parameter.name;
                m_session.document.dirty = true;
            }
            int type = static_cast<int>(parameter.type);
            constexpr const char* types[] = { "Float", "Int", "Bool", "Color", "Vector3", "Asset" };
            if (ImGui::Combo("Type", &type, types, 6)) {
                parameter.type = static_cast<asset::VFXParamType>(type);
                parameter.defaultValue = DefaultVFXParamValue(parameter.type);
                m_session.document.dirty = true;
            }
            if (DrawVFXParamValue("Default", parameter.type, parameter.defaultValue,
                                  parameter.minimum, parameter.maximum, parameter.hasRange))
                m_session.document.dirty = true;
            if (parameter.type == asset::VFXParamType::Float
                || parameter.type == asset::VFXParamType::Int) {
                if (ImGui::Checkbox("Inspector Range", &parameter.hasRange)) m_session.document.dirty = true;
                if (parameter.hasRange) {
                    m_session.document.dirty |= ImGui::DragFloat("Minimum", &parameter.minimum, 0.01f);
                    m_session.document.dirty |= ImGui::DragFloat("Maximum", &parameter.maximum, 0.01f);
                }
            }
            ImGui::SeparatorText("Bindings");
            for (std::size_t bindingIndex = 0; bindingIndex < m_session.document.graph.bindings.size();) {
                auto& binding = m_session.document.graph.bindings[bindingIndex];
                if (binding.paramName != parameter.name) { ++bindingIndex; continue; }
                ImGui::PushID(static_cast<int>(bindingIndex));
                ImGui::TextWrapped("Node %d  %s", binding.nodeId, binding.schemaPath.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) {
                    m_session.document.graph.bindings.erase(m_session.document.graph.bindings.begin()
                        + static_cast<std::ptrdiff_t>(bindingIndex));
                    m_session.document.dirty = true;
                    ImGui::PopID();
                    continue;
                }
                ImGui::PopID();
                ++bindingIndex;
            }
            ImGui::BeginDisabled(m_session.selectedNodeId <= 0);
            if (ImGui::BeginCombo("Bind Selected Node", "Choose field...")) {
                const auto* selectedNode = FindGraphNode(m_session.document.graph, m_session.selectedNodeId);
                for (const std::string& path : schemaPaths) {
                    reflection::ResolvedProperty resolved;
                    if (selectedNode == nullptr || !IsPathForVFXNode(selectedNode->type, path)
                        || !reflection::ResolveProperty(asset::GetVFXNodeSchema(), selectedNode,
                                                        path, resolved)
                        || resolved.property == nullptr
                        || !IsVFXParamCompatible(parameter.type, resolved.property->type)) continue;
                    if (ImGui::Selectable(path.c_str())) {
                        m_session.document.graph.bindings.push_back({ parameter.name, m_session.selectedNodeId, path });
                        m_session.document.dirty = true;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
            if (ImGui::Button("Delete Parameter", { -1.0f, 0.0f })) deleteParameter = static_cast<int>(index);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (deleteParameter >= 0) {
        m_session.PushUndo();
        const std::string name = m_session.document.graph.parameters[static_cast<std::size_t>(deleteParameter)].name;
        m_session.document.graph.parameters.erase(m_session.document.graph.parameters.begin() + deleteParameter);
        std::erase_if(m_session.document.graph.bindings, [&](const asset::VFXParamBinding& binding) {
            return binding.paramName == name;
        });
        for (auto& variant : m_session.document.graph.variants)
            std::erase_if(variant.overrides, [&](const asset::VFXParamOverride& item) {
                return item.paramName == name;
            });
        m_session.document.dirty = true;
    }

    ImGui::SeparatorText("Variant Sets");
    if (ImGui::Button("+ Variant")) {
        m_session.PushUndo();
        m_session.document.graph.variants.push_back({ "Variant " + std::to_string(m_session.document.graph.variants.size() + 1), {} });
        m_session.document.dirty = true;
    }
    int deleteVariant = -1;
    for (std::size_t index = 0; index < m_session.document.graph.variants.size(); ++index) {
        auto& variant = m_session.document.graph.variants[index];
        ImGui::PushID(static_cast<int>(index));
        if (ImGui::TreeNodeEx("##Variant", ImGuiTreeNodeFlags_DefaultOpen, "%s", variant.name.c_str())) {
            m_session.document.dirty |= InputString("Name", variant.name, 128);
            if (ImGui::Button("Populate all defaults", { -1.0f, 0.0f })) {
                variant.overrides.clear();
                for (const auto& parameter : m_session.document.graph.parameters)
                    variant.overrides.push_back({ parameter.name, parameter.defaultValue });
                m_session.document.dirty = true;
            }
            for (auto& item : variant.overrides) {
                const auto definition = std::find_if(m_session.document.graph.parameters.begin(), m_session.document.graph.parameters.end(),
                    [&](const auto& parameter) { return parameter.name == item.paramName; });
                if (definition == m_session.document.graph.parameters.end()) continue;
                ImGui::PushID(item.paramName.c_str());
                ImGui::TextUnformatted(item.paramName.c_str());
                m_session.document.dirty |= DrawVFXParamValue("Value", definition->type, item.value,
                    definition->minimum, definition->maximum, definition->hasRange);
                ImGui::PopID();
            }
            if (ImGui::Button("Delete Variant", { -1.0f, 0.0f })) deleteVariant = static_cast<int>(index);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (deleteVariant >= 0) {
        m_session.document.graph.variants.erase(m_session.document.graph.variants.begin() + deleteVariant);
        m_session.document.dirty = true;
    }

    ImGui::SeparatorText("Sub-graph Parameter Forwarding");
    if (ImGui::Button("+ Forward selected Sub Graph", { -1.0f, 0.0f })) {
        const auto* selected = FindGraphNode(m_session.document.graph, m_session.selectedNodeId);
        if (selected != nullptr && selected->type == asset::VFXNodeType::SubGraph) {
            m_session.document.graph.subGraphForwards.push_back({ selected->id, {}, {} });
            m_session.document.dirty = true;
        } else m_session.document.error = "Sub Graphノードを選択してください";
    }
    for (std::size_t index = 0; index < m_session.document.graph.subGraphForwards.size();) {
        auto& forward = m_session.document.graph.subGraphForwards[index];
        ImGui::PushID(static_cast<int>(index));
        ImGui::Text("Node %d", forward.nodeId);
        m_session.document.dirty |= InputString("Parent Parameter", forward.parentParam, 128);
        m_session.document.dirty |= InputString("Child Parameter", forward.childParam, 128);
        if (ImGui::Button("Remove Forward", { -1.0f, 0.0f })) {
            m_session.document.graph.subGraphForwards.erase(m_session.document.graph.subGraphForwards.begin() + static_cast<std::ptrdiff_t>(index));
            m_session.document.dirty = true;
            ImGui::PopID();
            continue;
        }
        ImGui::PopID();
        ++index;
    }

    ImGui::SeparatorText("Signal Graph");
    if (ImGui::Button("+ Signal Node")) {
        int nextId = 1;
        for (const auto& signal : m_session.document.graph.signalNodes) nextId = (std::max)(nextId, signal.id + 1);
        m_session.document.graph.signalNodes.push_back({ .id = nextId });
        m_session.document.dirty = true;
    }
    constexpr const char* SIGNAL_OPERATIONS[] = {
        "Constant", "Time", "Sine", "Noise", "Add", "Subtract", "Multiply", "Divide", "Remap"
    };
    for (std::size_t signalIndex = 0; signalIndex < m_session.document.graph.signalNodes.size();) {
        auto& signal = m_session.document.graph.signalNodes[signalIndex];
        ImGui::PushID(signal.id);
        ImGui::Text("Signal Node %d", signal.id);
        int operation = static_cast<int>(signal.operation);
        if (ImGui::Combo("Operation", &operation, SIGNAL_OPERATIONS, 9)) {
            signal.operation = static_cast<asset::VFXSignalOperation>(operation);
            m_session.document.dirty = true;
        }
        m_session.document.dirty |= ImGui::DragInt("Input A", &signal.inputA, 1.0f, -1, 100000);
        m_session.document.dirty |= ImGui::DragInt("Input B", &signal.inputB, 1.0f, -1, 100000);
        m_session.document.dirty |= ImGui::DragFloat("Value A", &signal.valueA, 0.01f);
        m_session.document.dirty |= ImGui::DragFloat("Value B", &signal.valueB, 0.01f);
        if (ImGui::Button("Delete Signal Node", { -1.0f, 0.0f })) {
            const int removedId = signal.id;
            m_session.document.graph.signalNodes.erase(m_session.document.graph.signalNodes.begin() + static_cast<std::ptrdiff_t>(signalIndex));
            for (auto& dependent : m_session.document.graph.signalNodes) {
                if (dependent.inputA == removedId) dependent.inputA = -1;
                if (dependent.inputB == removedId) dependent.inputB = -1;
            }
            std::erase_if(m_session.document.graph.signalOutputs,
                [removedId](const auto& output) { return output.nodeId == removedId; });
            m_session.document.dirty = true;
            ImGui::PopID();
            continue;
        }
        ImGui::Separator();
        ImGui::PopID();
        ++signalIndex;
    }
    if (ImGui::Button("+ Signal Output")) {
        const int nodeId = m_session.document.graph.signalNodes.empty() ? 0 : m_session.document.graph.signalNodes.back().id;
        m_session.document.graph.signalOutputs.push_back({ "Signal " + std::to_string(m_session.document.graph.signalOutputs.size() + 1), nodeId });
        m_session.document.dirty = true;
    }
    for (std::size_t outputIndex = 0; outputIndex < m_session.document.graph.signalOutputs.size();) {
        auto& output = m_session.document.graph.signalOutputs[outputIndex];
        ImGui::PushID(static_cast<int>(outputIndex));
        m_session.document.dirty |= InputString("Output", output.name, 128);
        m_session.document.dirty |= ImGui::DragInt("Node", &output.nodeId, 1.0f, 0, 100000);
        if (ImGui::Button("Delete Output", { -1.0f, 0.0f })) {
            m_session.document.graph.signalOutputs.erase(m_session.document.graph.signalOutputs.begin() + static_cast<std::ptrdiff_t>(outputIndex));
            m_session.document.dirty = true;
            ImGui::PopID();
            continue;
        }
        ImGui::PopID();
        ++outputIndex;
    }
}


void VFXGraphInspector::Draw(EditorContext& ctx)
{
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) m_session.graphEditInProgress = false;
    if (!m_session.graphEditInProgress
        && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)
        && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        // クリック開始時のasset全体を保持すると、Drag・ColorPicker・文字入力を1操作として戻せる。
        m_session.PushUndo();
        m_session.graphEditInProgress = true;
    }
    ImGui::TextUnformatted("Graph Inspector");
    ImGui::Separator();
    // 参照画像は「今どのグラフを見ているか」とは独立した作業用の道具なので、
    // ノード選択に依存しない Graph Inspector の先頭に置く。
    m_previewView.DrawReferenceOverlayControls(ctx);
    ImGui::Separator();
    if (InputString("Name", m_session.document.graph.name, 128)) m_session.document.dirty = true;
    ImGui::TextDisabled("Nodes %d   Links %d", static_cast<int>(m_session.document.graph.nodes.size()),
                        static_cast<int>(m_session.document.graph.links.size()));
    const asset::VFXGraphBudgetStats budget = asset::CalculateVFXGraphBudget(m_session.document.graph);
    bool budgetChanged = false;
    budgetChanged |= ImGui::DragInt("Particle Budget", &m_session.document.graph.maxParticles, 100, 1, 10000000);
    budgetChanged |= ImGui::DragInt("Light Budget", &m_session.document.graph.maxLights, 1, 0, 1024);
    budgetChanged |= ImGui::DragInt("Audio Budget", &m_session.document.graph.maxAudioVoices, 1, 0, 1024);
    if (budgetChanged) m_session.document.dirty = true;
    const bool overBudget = budget.particles > m_session.document.graph.maxParticles
        || budget.lights > m_session.document.graph.maxLights || budget.audioVoices > m_session.document.graph.maxAudioVoices;
    ImGui::TextColored(overBudget ? ImVec4{ 1.0f, 0.35f, 0.2f, 1.0f }
                                  : ImVec4{ 0.45f, 0.85f, 0.55f, 1.0f },
                       "Cost  P %d/%d   L %d/%d   A %d/%d",
                       budget.particles, m_session.document.graph.maxParticles,
                       budget.lights, m_session.document.graph.maxLights,
                       budget.audioVoices, m_session.document.graph.maxAudioVoices);
    const auto drawBudget = [](const char* label, int used, int limit) {
        const float ratio = limit > 0 ? std::clamp(static_cast<float>(used) / limit, 0.0f, 1.0f)
                                      : (used > 0 ? 1.0f : 0.0f);
        char overlay[64];
        std::snprintf(overlay, sizeof(overlay), "%s  %d / %d", label, used, limit);
        ImGui::ProgressBar(ratio, { -1.0f, 0.0f }, overlay);
    };
    drawBudget("Particles", budget.particles, m_session.document.graph.maxParticles);
    drawBudget("Lights", budget.lights, m_session.document.graph.maxLights);
    drawBudget("Audio", budget.audioVoices, m_session.document.graph.maxAudioVoices);
    DrawParameters();
    ImGui::Separator();
    // グループ枠(注釈)が選択されていれば、そのタイトル・メモ・色を編集する。
    if (m_session.selectedGroupId > 0) {
        const auto groupIt = std::find_if(m_session.document.graph.groups.begin(), m_session.document.graph.groups.end(),
            [this](const asset::VFXGraphGroup& group) { return group.id == m_session.selectedGroupId; });
        if (groupIt != m_session.document.graph.groups.end()) {
            asset::VFXGraphGroup& group = *groupIt;
            ImGui::TextUnformatted("Group / Note");
            m_session.document.dirty |= InputString("Title", group.title, 128);
            // メモは複数行。付箋としてキャンバスへ表示される。
            std::vector<char> noteBuffer((std::max<std::size_t>)(1024, group.note.size() + 256), '\0');
            std::memcpy(noteBuffer.data(), group.note.data(), group.note.size());
            if (ImGui::InputTextMultiline("Note", noteBuffer.data(), noteBuffer.size(),
                                          { -1.0f, 90.0f })) {
                group.note = noteBuffer.data();
                m_session.document.dirty = true;
            }
            m_session.document.dirty |= ImGui::ColorEdit4("Color", &group.color.x,
                                              ImGuiColorEditFlags_AlphaBar);
            m_session.document.dirty |= ImGui::DragFloat2("Size", &group.width, 1.0f, 80.0f, 4000.0f);
            ImGui::Separator();
            if (ImGui::Button("Delete Group", { -1.0f, 0.0f })) {
                m_session.PushUndo();
                const int removedId = group.id;
                std::erase_if(m_session.document.graph.groups, [removedId](const asset::VFXGraphGroup& item) {
                    return item.id == removedId;
                });
                m_session.selectedGroupId = -1;
            }
            return;
        }
        m_session.selectedGroupId = -1; // 参照先が消えていたら選択解除
    }
    if (m_session.selectedLinkIndex >= 0
        && m_session.selectedLinkIndex < static_cast<int>(m_session.document.graph.links.size())) {
        auto& link = m_session.document.graph.links[static_cast<std::size_t>(m_session.selectedLinkIndex)];
        const auto* source = FindGraphNode(m_session.document.graph, link.fromNode);
        const auto* target = FindGraphNode(m_session.document.graph, link.toNode);
        ImGui::TextUnformatted("Event Link");
        ImGui::TextWrapped("%s  ->  %s", source != nullptr ? source->name.c_str() : "Missing source",
                          target != nullptr ? target->name.c_str() : "Missing target");
        const char* triggerItems[] = {
            "On Complete", "On Start", "On Collision", "On Death",
            "On Animation Event", "On Trigger"
        };
        int trigger = static_cast<int>(link.trigger);
        bool linkChanged = false;
        if (ImGui::Combo("Trigger", &trigger, triggerItems, 6)) {
            link.trigger = static_cast<asset::VFXLinkTrigger>(trigger);
            linkChanged = true;
        }
        linkChanged |= ImGui::DragFloat("Event Delay", &link.delay, 0.01f, 0.0f, 3600.0f, "%.2fs");
        if (link.trigger == asset::VFXLinkTrigger::OnAnimationEvent
            || link.trigger == asset::VFXLinkTrigger::OnTrigger) {
            char eventName[128]{};
            std::snprintf(eventName, sizeof(eventName), "%s", link.eventName.c_str());
            if (ImGui::InputText(link.trigger == asset::VFXLinkTrigger::OnTrigger
                    ? "Trigger Name" : "Event Name", eventName, sizeof(eventName))) {
                link.eventName = eventName;
                linkChanged = true;
            }
        }
        if (link.trigger == asset::VFXLinkTrigger::OnCollision)
            ImGui::TextWrapped("Source Particle collision starts the target once per graph cycle.");
        if (link.trigger == asset::VFXLinkTrigger::OnDeath)
            ImGui::TextWrapped("The first source Particle death starts the target once per graph cycle.");
        if (link.trigger == asset::VFXLinkTrigger::OnAnimationEvent)
            ImGui::TextWrapped("A matching Animation Event on the source Animated Mesh starts the target.");
        if (link.trigger == asset::VFXLinkTrigger::OnTrigger)
            ImGui::TextWrapped("ScriptVFXProxy::Trigger starts the target by name.");
        if (linkChanged) m_session.document.dirty = true;
        ImGui::Separator();
        if (ImGui::Button("Delete Link", { -1.0f, 0.0f })) {
            m_session.PushUndo();
            m_session.document.graph.links.erase(m_session.document.graph.links.begin() + m_session.selectedLinkIndex);
            m_session.selectedLinkIndex = -1;
            m_session.document.dirty = true;
        }
        return;
    }
    auto* node = FindGraphNode(m_session.document.graph, m_session.selectedNodeId);
    if (node == nullptr) {
        ImGui::Spacing();
        ImGui::TextDisabled("Nothing selected");
        ImGui::TextWrapped("Select a node or link to edit it. Right-click the canvas to add an effect.");
        ImGui::Spacing();
        ImGui::TextDisabled("F  Frame selection    A  Frame all");
        ImGui::TextDisabled("Ctrl+D  Duplicate    Ctrl+C/X/V  Copy / Cut / Paste");
        ImGui::TextDisabled("Del  Delete selection    Esc  Clear selection");
        ImGui::TextDisabled("Drag-box to multi-select nodes");
        ImGui::TextDisabled("F2 / double-click a node to rename");
        ImGui::Spacing();
        ImGui::TextDisabled("Right-click empty canvas  Add node menu");
        ImGui::TextDisabled("Right-click a link  Trigger / Break Link");
        ImGui::TextDisabled("Right-click a note  Rename / Fit / Delete");
        ImGui::TextDisabled("Alt + click a wire  Break it");
        ImGui::TextDisabled("Drag a pin to empty space  Add + auto-connect");
        ImGui::Spacing();
        ImGui::TextDisabled("New nodes auto-link from the selection (View menu to disable)");
        ImGui::TextDisabled("Templates > Merge appends without discarding this graph");
        return;
    }
    bool changed = false;
    ImGui::Text("%s #%d", asset::VFXNodeTypeName(node->type), node->id);
    if (node->type != asset::VFXNodeType::Entry)
        changed |= ImGui::Checkbox("Enabled", &node->enabled);
    changed |= InputString("Node Name", node->name, 128);
    // Reroute は時間を持たない。編集欄を出すと「設定したのに効かない」項目になるので出さない。
    if (node->type != asset::VFXNodeType::Entry
        && !asset::VFXNodeIsPassthrough(node->type)) {
        changed |= ImGui::DragFloat("Start Offset", &node->startOffset, 0.01f, 0.0f, 3600.0f, "%.2fs");
        if (node->type != asset::VFXNodeType::Particle)
            changed |= ImGui::DragFloat("Duration", &node->duration, 0.01f, 0.0f, 3600.0f, "%.2fs");
    } else if (asset::VFXNodeIsPassthrough(node->type)) {
        ImGui::TextDisabled("配線の中継点です。時間も空間上の位置も持ちません。");
    }
    if (!asset::VFXNodeHasNoInstance(node->type)) {
        changed |= ImGui::DragFloat3("Position", &node->localPosition.x, 0.01f);
        changed |= ImGui::DragFloat3("Rotation", &node->localRotationDegrees.x, 0.25f);
        changed |= ImGui::DragFloat3("Scale", &node->localScale.x, 0.01f, 0.001f, 1000.0f);
        changed |= InputString("Bone / Socket", node->attachBone, 128);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Optional BoneComponent name below the VFX owner.");
        if (!m_session.preview.actorBones.empty()) {
            ImGui::SameLine();
            if (ImGui::BeginCombo("##NodeBonePicker", "Pick")) {
                if (ImGui::Selectable("(None)", node->attachBone.empty())) {
                    node->attachBone.clear();
                    changed = true;
                }
                for (const auto& bone : m_session.preview.actorBones) {
                    if (ImGui::Selectable(bone.c_str(), node->attachBone == bone)) {
                        node->attachBone = bone;
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
        }

        // ── 空間の親ノード ──
        // WHY: link は「いつ動くか」を決める線で、位置には一切関与しない。衝撃波とそこから
        //      出る煙のように「まとめて傾けたい」まとまりは、発火順とは別の軸で決まる。
        //      ここで指定した親の Position/Rotation/Scale が、この Node へ合成される。
        // NOTE: 実行時は親ノード用のグループ GameObject が挟まるため、親ノードの再生時間や
        //       Mesh の膨張スケールは子へ波及しない (位置だけを継承する)。
        {
            const auto& nodes = m_session.document.graph.nodes;
            const auto parentIt = std::find_if(nodes.begin(), nodes.end(),
                [&](const asset::VFXGraphNode& candidate) { return candidate.id == node->parentNodeId; });
            const char* preview = (parentIt != nodes.end()) ? parentIt->name.c_str() : "(None)";
            if (ImGui::BeginCombo("Parent Node", preview)) {
                if (ImGui::Selectable("(None)", node->parentNodeId == -1)) {
                    node->parentNodeId = -1;
                    changed = true;
                }
                for (const auto& candidate : nodes) {
                    // 自分自身は親にできない。Entry / Delay は実体を持たないので選ばせない。
                    if (candidate.id == node->id) continue;
                    if (candidate.type == asset::VFXNodeType::Entry
                        || candidate.type == asset::VFXNodeType::Delay) continue;
                    // 自分の子孫を親に選ぶと循環する。選択肢の段階で外し、
                    // 保存時のエラーではなく「選べない」形で伝える。
                    bool isDescendant = false;
                    for (int cursor = candidate.parentNodeId, guard = 0;
                         cursor != -1 && guard <= static_cast<int>(nodes.size()); ++guard) {
                        if (cursor == node->id) { isDescendant = true; break; }
                        const auto up = std::find_if(nodes.begin(), nodes.end(),
                            [&](const asset::VFXGraphNode& n) { return n.id == cursor; });
                        cursor = (up != nodes.end()) ? up->parentNodeId : -1;
                    }
                    if (isDescendant) continue;
                    ImGui::PushID(candidate.id);
                    if (ImGui::Selectable(candidate.name.c_str(), candidate.id == node->parentNodeId)) {
                        node->parentNodeId = candidate.id;
                        changed = true;
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip("Inherit this node's Transform from another node.\n"
                                  "Independent from graph links (which control timing).");
            if (node->parentNodeId != -1 && !node->attachBone.empty())
                ImGui::TextColored({ 1.0f, 0.78f, 0.35f, 1.0f },
                                   "Bone / Socket takes priority - Parent Node is ignored");
        }
    }
    ImGui::Separator();
    if (node->type == asset::VFXNodeType::Particle) {
        // Scene Inspectorと同じUnity風モジュールスタックを使い、Graph側でも全機能を編集する。
        ImGui::TextDisabled("Quick Asset Drop");
        changed |= AssetPathField("Material##VFXQuickParticle", node->particle.materialPath, kMaterialExts);
        changed |= AssetPathField("Texture##VFXQuickParticle", node->particle.texturePath, kTextureExts);
        changed |= AssetPathField("Mesh Shape##VFXQuickParticle", node->particle.meshShapePath, kMeshExts);
        // 割り当てたテクスチャの解析。AI が読むのと同じ AnalyzeTexture を通すことで、
        // 「人が見る面」と「AI が読む面」が一致し続ける (別実装にすると必ずドリフトする)。
        changed |= DrawTextureAnalysis(node->particle);
        ImGui::Separator();
        EditorContext assetContext = ctx;
        assetContext.markSceneDirty = [this]() { m_session.document.dirty = true; };
        if (DrawParticleEmitterModules(node->particle, assetContext)) {
            node->duration = node->particle.duration;
            changed = true;
        }
    } else if (node->type == asset::VFXNodeType::Trail
               || node->type == asset::VFXNodeType::MeshTrail) {
        if (node->type == asset::VFXNodeType::MeshTrail)
            changed |= AssetPathField("Mesh", node->trail.meshPath, kMeshExts);
        changed |= AssetPathField("Material", node->trail.materialPath, kMaterialExts);
        changed |= AssetPathField("Texture", node->trail.texturePath, kTextureExts);
        changed |= ImGui::ColorEdit4("Start Color", &node->trail.colorStart.x);
        changed |= ImGui::ColorEdit4("End Color", &node->trail.colorEnd.x);
        changed |= ImGui::DragFloat("Trail Lifetime", &node->trail.lifetime, 0.01f, 0.01f, 3600.0f);
        if (node->type == asset::VFXNodeType::Trail) {
            changed |= ImGui::DragFloat("Width Start", &node->trail.widthStart, 0.01f, 0.0f, 1000.0f);
            changed |= ImGui::DragFloat("Width End", &node->trail.widthEnd, 0.01f, 0.0f, 1000.0f);
            changed |= ImGui::Checkbox("Beam Mode", &node->trail.beamMode);
            if (node->trail.beamMode) {
                changed |= ImGui::DragFloat3("Beam Start", &node->trail.beamStart.x, 0.01f);
                changed |= ImGui::DragFloat3("Beam End", &node->trail.beamEnd.x, 0.01f);
            }
        }
    } else if (node->type == asset::VFXNodeType::Light) {
        changed |= ImGui::ColorEdit3("Color", &node->light.color.x);
        changed |= ImGui::DragFloat("Intensity", &node->light.intensity, 0.05f, 0.0f, 100000.0f);
        changed |= ImGui::DragFloat("Range", &node->light.range, 0.05f, 0.0f, 100000.0f);
        ImGui::Separator();
        ImGui::TextDisabled("Envelope (node lifetime)");
        changed |= ImGui::Checkbox("Use Intensity Curve", &node->light.useIntensityCurve);
        if (node->light.useIntensityCurve) {
            changed |= widgets::CurveEditor("Intensity Multiplier", node->light.intensityCurve, 1.0f);
            ImGui::TextDisabled("Intensity への倍率。頭を 1・末尾を 0 にすると"
                                "爆発の閃光が一瞬光って消えます。");
        }
        changed |= ImGui::Checkbox("Use Color Gradient", &node->light.useColorGradient);
        if (node->light.useColorGradient) {
            changed |= widgets::GradientEditor("Light Color", node->light.colorGradient);
            ImGui::TextDisabled("Alpha は明るさ倍率として RGB に掛かります"
                                "(白熱 → 橙 → 暗赤 の色温度変化を1本で作れます)。");
        }
    } else if (node->type == asset::VFXNodeType::Audio) {
        changed |= AssetPathField("Clip", node->audio.clipPath, kAudioExts);
        changed |= ImGui::SliderFloat("Volume", &node->audio.volume, 0.0f, 1.0f);
        changed |= ImGui::DragFloat("Pitch", &node->audio.pitch, 0.01f, 0.01f, 4.0f);
        changed |= ImGui::SliderFloat("Spatial Blend", &node->audio.spatialBlend, 0.0f, 1.0f);
        changed |= ImGui::Checkbox("Loop Audio", &node->audio.loop);
    } else if (node->type == asset::VFXNodeType::Decal) {
        changed |= AssetPathField("Albedo", node->decal.albedoPath, kTextureExts);
        changed |= AssetPathField("Normal", node->decal.normalPath, kTextureExts);
        changed |= AssetPathField("Emissive", node->decal.emissivePath, kTextureExts);
        changed |= ImGui::ColorEdit4("Color", &node->decal.color.x);
        changed |= ImGui::DragFloat("Normal Strength", &node->decal.normalStrength, 0.01f, 0.0f, 8.0f);
        changed |= ImGui::DragFloat("Emissive Scale", &node->decal.emissiveScale, 0.01f, 0.0f, 10000.0f);
        changed |= ImGui::DragFloat("Fade Time", &node->decal.fadeTime, 0.01f, 0.0f, 3600.0f);
        changed |= ImGui::Checkbox("Use Fade Curve", &node->decal.useFadeCurve);
        if (node->decal.useFadeCurve) {
            changed |= widgets::CurveEditor("Opacity", node->decal.fadeCurve, 1.0f);
            ImGui::TextDisabled("Color の alpha と Emissive Scale の両方に掛かります。"
                                "終盤で急に落とすと焼け跡が「しばらく残って消える」動きになります。");
        }
        if (ImGui::TreeNode("Procedural Impact Decal Generator")) {
            // 関数ローカルstaticならグローバル状態を増やさず、ノード切替後も設定を保持できる。
            static ProceduralImpactDecalUiState procedural;
            ImGui::DragInt("Texture Size", &procedural.textureSize, 16.0f, 64, 2048);
            ImGui::DragInt("Seed", &procedural.seed, 1.0f, 0, 1000000);
            ImGui::SliderFloat("Impact Radius", &procedural.radius, 0.15f, 0.95f);
            ImGui::DragInt("Crack Count", &procedural.crackCount, 1.0f, 0, 64);
            ImGui::SliderFloat("Emissive Cracks", &procedural.emissiveStrength, 0.0f, 4.0f);
            if (ImGui::Button("Generate & Assign Decal Set", { -1.0f, 0.0f })) {
                asset::ProceduralImpactDecalSettings settings;
                settings.textureSize = procedural.textureSize;
                settings.seed = static_cast<std::uint32_t>((std::max)(procedural.seed, 0));
                settings.radius = procedural.radius;
                settings.crackCount = procedural.crackCount;
                settings.emissiveStrength = procedural.emissiveStrength;
                const auto result = asset::GenerateProceduralImpactDecal(
                    ProceduralDecalOutputDirectory(g_assetFieldProjectRoot), settings);
                procedural.status = result.message;
                procedural.statusIsError = !result.success;
                if (result.success) {
                    node->decal.albedoPath = NormalizeAssetPath(result.albedoPath);
                    node->decal.normalPath = NormalizeAssetPath(result.normalPath);
                    node->decal.emissivePath = NormalizeAssetPath(result.emissivePath);
                    node->decal.normalStrength = 1.0f;
                    node->decal.emissiveScale = procedural.emissiveStrength > 0.0f ? 1.0f : 0.0f;
                    changed = true;
                }
            }
            if (!procedural.status.empty()) {
                if (procedural.statusIsError)
                    ImGui::TextColored({ 1.0f, 0.4f, 0.3f, 1.0f }, "%s",
                                       procedural.status.c_str());
                else
                    ImGui::TextWrapped("%s", procedural.status.c_str());
            }
            ImGui::TextDisabled("Assets/Textures/Generated/VFX にAlbedo・Normal・Emissiveを保存します。");
            ImGui::TreePop();
        }
    } else if (node->type == asset::VFXNodeType::SubGraph) {
        changed |= AssetPathField("Graph (.vfx)", node->subGraph.graphPath, kVfxExts);
        ImGui::TextWrapped("Nested graph instances inherit play, pause, speed and editor preview state.");
    } else if (node->type == asset::VFXNodeType::ForceField) {
        constexpr const char* fieldItems[] = { "Wind", "Attract", "Repulse",
                                               "Vortex", "Turbulence", "Drag" };
        int fieldType = std::clamp(node->forceField.fieldType, 0, 5);
        if (ImGui::Combo("Field Type", &fieldType, fieldItems, 6)) {
            node->forceField.fieldType = fieldType;
            changed = true;
        }
        changed |= ImGui::DragFloat("Strength", &node->forceField.strength, 0.05f, -1000.0f, 1000.0f);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("加速度 [m/s^2]。Drag のときは減衰係数 [1/s]");
        changed |= ImGui::DragFloat("Radius", &node->forceField.radius, 0.05f, 0.0f, 10000.0f, "%.2fm");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("0 でシーン全体へ減衰なしに作用します");
        changed |= ImGui::DragFloat("Falloff Power", &node->forceField.falloffPower, 0.01f, 0.0f, 16.0f);
        if (fieldType == 0 || fieldType == 3) {
            changed |= ImGui::DragFloat3(fieldType == 0 ? "Wind Direction" : "Vortex Axis",
                                         &node->forceField.direction.x, 0.01f);
        }
        if (fieldType == 4) {
            changed |= ImGui::DragFloat("Noise Frequency", &node->forceField.noiseFrequency,
                                        0.01f, 0.0f, 32.0f);
            changed |= ImGui::DragFloat("Noise Speed", &node->forceField.noiseSpeed, 0.01f, 0.0f, 32.0f);
        }
        ImGui::TextWrapped("力場は範囲内の全パーティクルへ効きます (このグラフの外のエミッターにも作用)。");
    } else if (node->type == asset::VFXNodeType::Mesh) {
        changed |= AssetPathField("Mesh", node->mesh.meshPath, kMeshExts);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("primitive:sphere / primitive:cube / .fbx のいずれか");
        // よく使う組み込み形状はワンクリックで入れられるようにする。
        if (ImGui::SmallButton("Sphere")) { node->mesh.meshPath = "primitive:sphere"; changed = true; }
        ImGui::SameLine();
        if (ImGui::SmallButton("Cube")) { node->mesh.meshPath = "primitive:cube"; changed = true; }
        ImGui::SameLine();
        if (ImGui::SmallButton("Plane")) { node->mesh.meshPath = "primitive:plane"; changed = true; }
        changed |= AssetPathField("Material", node->mesh.materialPath, kMaterialExts);
        // 未設定でも描画は保証されている。何が使われるかを明示して不安にさせない。
        if (node->mesh.materialPath.empty()) {
            ImGui::TextDisabled("未設定: VFXMeshFallback.mat (加算/両面/Unlit) を使用");
            if (ImGui::SmallButton("Pin Fallback Material")) {
                node->mesh.materialPath = asset::VFX_MESH_FALLBACK_MATERIAL;
                changed = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("既定マテリアルを明示的に書き込みます (複製して差し替える起点に)");
        }
        ImGui::Separator();
        ImGui::TextDisabled("Envelope (node lifetime)");
        changed |= ImGui::DragFloat("Scale Start", &node->mesh.scaleStart, 0.01f, 0.0f, 1000.0f);
        changed |= ImGui::DragFloat("Scale End", &node->mesh.scaleEnd, 0.01f, 0.0f, 1000.0f);
        changed |= ImGui::SliderFloat("Ease Power", &node->mesh.scaleEasePower, 0.05f, 4.0f);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("1 で線形。1 未満だと最初に一気に広がって減速し、衝撃波らしくなります");
        changed |= ImGui::ColorEdit4("Color Start", &node->mesh.colorStart.x);
        changed |= ImGui::ColorEdit4("Color End", &node->mesh.colorEnd.x);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("マテリアルの albedo へ毎フレーム書き込みます。\n"
                              "加算ブレンドでは RGB を 0 に落とすと消えます。");
        ImGui::Separator();
        changed |= InputString("Extra Param", node->mesh.animatedParam, 64);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("任意。追加で動かすシェーダー変数名 (例: Dissolve の alphaCutoff)。\n"
                              "この GameObject 専用の上書きなので共有 .mat は変更されません。");
        if (!node->mesh.animatedParam.empty()) {
            changed |= ImGui::DragFloat("Param Start", &node->mesh.paramStart, 0.01f);
            changed |= ImGui::DragFloat("Param End", &node->mesh.paramEnd, 0.01f);
        }
    } else if (node->type == asset::VFXNodeType::AnimatedMesh) {
        changed |= AssetPathField("Model", node->animatedMesh.modelPath, kMeshExts);
        changed |= AssetPathField("Animator Controller", node->animatedMesh.controllerPath,
                                  kAnimatorControllerExts);
        changed |= AssetPathField("Material", node->animatedMesh.materialPath, kMaterialExts);
        changed |= InputString("Initial State", node->animatedMesh.initialState, 96);
        changed |= ImGui::DragFloat("Animation Speed", &node->animatedMesh.speed,
                                    0.01f, -8.0f, 8.0f);
        changed |= ImGui::SliderFloat("Start Normalized Time",
                                     &node->animatedMesh.startNormalizedTime, 0.0f, 1.0f);
        changed |= ImGui::DragInt("Mesh Index", &node->animatedMesh.meshIndex,
                                  0.1f, -1, 1024);
        changed |= ImGui::Checkbox("Loop Animation", &node->animatedMesh.loop);
        changed |= ImGui::Checkbox("Sync To Graph Time",
                                   &node->animatedMesh.syncToGraphTime);
        changed |= ImGui::Checkbox("Apply Root Motion",
                                   &node->animatedMesh.applyRootMotion);
        if (node->animatedMesh.controllerPath.empty())
            ImGui::TextColored({ 1.0f, 0.65f, 0.2f, 1.0f },
                               "Saved Animated Mesh nodes require an Animator Controller.");
    } else if (node->type == asset::VFXNodeType::ScreenEffect) {
        changed |= ImGui::ColorEdit3("Flash Color", &node->screenEffect.flashColor.x);
        changed |= ImGui::SliderFloat("Flash", &node->screenEffect.flashIntensity, 0.0f, 1.0f);
        changed |= ImGui::DragFloat("Bloom Boost", &node->screenEffect.bloomBoost, 0.01f, 0.0f, 8.0f);
        changed |= ImGui::DragFloat("Chromatic Aberration", &node->screenEffect.chromaticAberration,
                                    0.001f, 0.0f, 0.2f, "%.4f");
        changed |= ImGui::DragFloat("Lens Distortion", &node->screenEffect.lensDistortion,
                                    0.01f, -1.0f, 1.0f);
        changed |= ImGui::DragFloat("Vignette", &node->screenEffect.vignette, 0.01f, 0.0f, 1.0f);
        ImGui::Separator();
        changed |= ImGui::DragFloat("Fade In", &node->screenEffect.fadeInTime, 0.005f, 0.0f, 10.0f, "%.3fs");
        changed |= ImGui::DragFloat("Fade Out", &node->screenEffect.fadeOutTime, 0.005f, 0.0f, 10.0f, "%.3fs");
        ImGui::TextWrapped("シーンのカラーグレーディングを置き換えず、上から加算します。"
                           "複数の ScreenEffect が同時に走っても合成されます"
                           "(フラッシュだけは最も強いものを採用)。");
    } else if (node->type == asset::VFXNodeType::CameraShake) {
        changed |= ImGui::DragFloat("Amplitude", &node->cameraShake.amplitude, 0.005f, 0.0f, 5.0f, "%.3fm");
        changed |= ImGui::DragFloat("Rotation Amplitude", &node->cameraShake.rotationAmplitude,
                                    0.05f, 0.0f, 45.0f, "%.2f deg");
        changed |= ImGui::DragFloat("Frequency", &node->cameraShake.frequency, 0.25f, 0.0f, 60.0f, "%.1fHz");
        changed |= ImGui::SliderFloat("Falloff Power", &node->cameraShake.falloffPower, 0.05f, 8.0f);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("大きいほど頭で強く出て素早く収まります (打撃感が硬くなる)");
        changed |= ImGui::DragFloat("Reach Radius", &node->cameraShake.radius, 0.5f, 0.0f, 1000.0f, "%.1fm");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("この距離までカメラが揺れます。0 で距離減衰なし");
        ImGui::TextWrapped("カメラ本体は書き換えず、描画時のビューだけをずらします。"
                           "操作中のカメラの向きは壊れません。");
    } else if (node->type == asset::VFXNodeType::TimeScale) {
        changed |= ImGui::SliderFloat("Time Scale", &node->timeScale.timeScale, 0.0f, 2.0f);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("0 で完全停止。ヒットストップは 0.05〜0.2 が目安");
        changed |= ImGui::DragFloat("Blend In", &node->timeScale.blendInTime, 0.005f, 0.0f, 5.0f, "%.3fs");
        changed |= ImGui::DragFloat("Blend Out", &node->timeScale.blendOutTime, 0.005f, 0.0f, 5.0f, "%.3fs");
        ImGui::TextWrapped("ゲーム実行時のみ効きます (プレビューではエディタ全体が"
                           "遅くなるため無効)。複数同時なら最も遅い要求を採用します。");
    } else if (node->type == asset::VFXNodeType::Wind) {
        changed |= ImGui::DragFloat3("Direction", &node->wind.direction.x, 0.01f);
        changed |= ImGui::DragFloat("Strength", &node->wind.strength, 0.05f, 0.0f, 100.0f);
        changed |= ImGui::DragFloat("Turbulence", &node->wind.turbulence, 0.01f, 0.0f, 50.0f);
        changed |= ImGui::DragFloat("Pulse Frequency", &node->wind.pulseFrequency, 0.01f, 0.0f, 32.0f);
        ImGui::TextWrapped("WindZone として働き、フォリッジ・水面・パーティクルなど"
                           "風を読む全システムへ効きます。");
    }
    // このノードの、公開パラメーターに bind 済みの leaf 一覧。
    // WHY: bind した leaf は生成時に必ずパラメーター値で上書きされる (ApplyVFXBindings)。
    //      上の手書き UI や Advanced で直接編集しても保存はされるが実行時には無視されるため、
    //      「どのフィールドがパラメーター管理下か」をノード単位で見えるようにしておく。
    //      値の食い違い自体は CollectVFXGraphWarnings が警告 banner に出す。
    {
        bool headerDrawn = false;
        for (const auto& binding : m_session.document.graph.bindings) {
            if (binding.nodeId != node->id) continue;
            if (!headerDrawn) {
                ImGui::Separator();
                ImGui::TextDisabled("Driven by exposed parameters");
                headerDrawn = true;
            }
            ImGui::BulletText("%s", binding.schemaPath.c_str());
            ImGui::SameLine();
            ImGui::TextColored({ 0.55f, 0.82f, 1.0f, 1.0f }, "<- %s", binding.paramName.c_str());
        }
    }

    // authoring スキーマ由来の全フィールド。上の手書き UI が主役で、ここは常に最新の完全な一覧。
    // WHY: 手書き UI が触っていない leaf だけを出す「除外リスト」は、それ自体が
    //      フィールド追加のたびに更新を要する 2 つ目の手書き面になり、防ごうとしている
    //      ドリフトを再生産する。よって除外はせず、全 leaf を折り畳みで重複表示する。
    //      新しいフィールドはスキーマへ宣言した瞬間からここに現れ、編集できる
    //      (binding / AI の vfx.schema と見える範囲が一致していることの確認にもなる)。
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Advanced (schema)")) {
        ImGui::TextDisabled("Every authoring field, generated from the type schema.");
        if (widgets::DrawSchemaProperties(asset::GetVFXNodeSchema(), node, ctx.projectRoot))
            changed = true;
    }

    if (changed) {
        m_session.document.dirty = true;
        // 触ったのはこのノードだけ。ランタイム側の再適用を1ノードに絞る。
        m_session.document.liveDirtyNodeId = node->id;
    }
    if (node->type != asset::VFXNodeType::Entry) {
        ImGui::Separator();
        if (ImGui::Button("Delete Node", { -1.0f, 0.0f })) m_canvas.DeleteSelectedNode();
    }
}

} // namespace fbzz::editor
