// FBZZ Engine
// VFXEditorPanel.cpp | fbzz::editor
// VFX (Particle) 専用エディター実装
// プレビューはシーンビューの実レンダリングに任せ、このパネルは
// 「再生制御・タイムライン・エフェクト構造・モジュール編集」に集中する。
#include <Editor/Panels/VFXEditorPanel.hpp>

#include <Editor/EditorContext.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/ParticleEmitterModules.hpp>
#include <Engine/Asset/VFXGraphAsset.hpp>
#include <Engine/Asset/VFXAuthoringSchema.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Renderer/IImGuiRenderer.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/Components/VFXGraphComponent.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <imgui.h>
#include <imnodes.h>
#include <algorithm>
#include <cfloat>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <filesystem>

namespace fbzz::editor {
namespace {

constexpr float kScrubStep     = 1.0f / 60.0f; // Step ボタン / スクラブの最小時間刻み
constexpr float kTimelineH     = 58.0f;        // タイムラインキャンバスの高さ [px]
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

bool IsVFXAssetPath(const std::string& path)
{
    constexpr const char* extension = ".vfx";
    return path.size() >= 4 && path.compare(path.size() - 4, 4, extension) == 0;
}

bool InputString(const char* label, std::string& value, std::size_t capacity = 512)
{
    std::vector<char> buffer((std::max)(capacity, value.size() + 2), '\0');
    std::memcpy(buffer.data(), value.data(), value.size());
    if (!ImGui::InputText(label, buffer.data(), buffer.size())) return false;
    value = buffer.data();
    return true;
}

bool ReadAssetPayload(const ImGuiPayload* payload, std::string& outPath)
{
    if (payload == nullptr || payload->Data == nullptr || payload->DataSize <= 1) return false;
    const auto* text = static_cast<const char*>(payload->Data);
    if (text[payload->DataSize - 1] != '\0') return false;
    outPath = text;
    return !outPath.empty();
}

bool EndsWithInsensitive(const std::string& path, const char* extension)
{
    const std::size_t extensionLength = std::strlen(extension);
    if (path.size() < extensionLength) return false;
    const std::size_t offset = path.size() - extensionLength;
    for (std::size_t index = 0; index < extensionLength; ++index) {
        const unsigned char left = static_cast<unsigned char>(path[offset + index]);
        const unsigned char right = static_cast<unsigned char>(extension[index]);
        if (std::tolower(left) != std::tolower(right)) return false;
    }
    return true;
}

bool AssetPathField(const char* label, std::string& value)
{
    bool changed = InputString(label, value);
    if (ImGui::BeginDragDropTarget()) {
        std::string path;
        if (ReadAssetPayload(ImGui::AcceptDragDropPayload("ASSET_PATH"), path)) {
            value = NormalizeAssetPath(path);
            changed = true;
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("Drop an asset here");
    return changed;
}

int InputPinId(int nodeId) { return 100000 + nodeId * 2; }
int OutputPinId(int nodeId) { return 100000 + nodeId * 2 + 1; }
int LinkId(int linkIndex) { return 200000 + linkIndex; }

asset::VFXGraphNode* FindGraphNode(asset::VFXGraphAsset& graph, int id)
{
    const auto iterator = std::find_if(graph.nodes.begin(), graph.nodes.end(),
        [id](const asset::VFXGraphNode& node) { return node.id == id; });
    return iterator == graph.nodes.end() ? nullptr : &*iterator;
}

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
    }
    return IM_COL32(60, 64, 74, 255);
}

const char* VFXParamTypeName(asset::VFXParamType type)
{
    constexpr const char* names[] = { "Float", "Int", "Bool", "Color", "Vector3", "Asset" };
    const int index = static_cast<int>(type);
    return index >= 0 && index < 6 ? names[index] : "Unknown";
}

asset::VFXParamValue DefaultVFXParamValue(asset::VFXParamType type)
{
    asset::VFXParamValue value;
    switch (type) {
    case asset::VFXParamType::Float: value.source = asset::VFXConstant{ 0.0f }; break;
    case asset::VFXParamType::Int: value.source = asset::VFXConstant{ 0 }; break;
    case asset::VFXParamType::Bool: value.source = asset::VFXConstant{ false }; break;
    case asset::VFXParamType::Color: value.source = asset::VFXConstant{ math::Vector4{ 1, 1, 1, 1 } }; break;
    case asset::VFXParamType::Vector3: value.source = asset::VFXConstant{ math::Vector3::ZERO }; break;
    case asset::VFXParamType::AssetRef: value.source = asset::VFXConstant{ std::string{} }; break;
    }
    return value;
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

void CollectExposablePaths(const reflection::ITypeSchema& schema, const std::string& prefix,
                           std::vector<std::string>& paths)
{
    for (const auto& property : schema.Properties()) {
        const std::string path = prefix.empty() ? std::string(property.key)
                                                : prefix + "." + std::string(property.key);
        if (property.type == reflection::PropertyType::Struct && property.childSchema != nullptr)
            CollectExposablePaths(*property.childSchema, path, paths);
        else if (property.exposable) paths.push_back(path);
    }
}

bool IsPathForVFXNode(asset::VFXNodeType type, std::string_view path)
{
    if (path.find('.') == std::string_view::npos) return true;
    switch (type) {
    case asset::VFXNodeType::Particle: return path.starts_with("particle.");
    case asset::VFXNodeType::Trail:
    case asset::VFXNodeType::MeshTrail: return path.starts_with("trail.");
    case asset::VFXNodeType::Light: return path.starts_with("light.");
    case asset::VFXNodeType::Audio: return path.starts_with("audio.");
    case asset::VFXNodeType::Decal: return path.starts_with("decal.");
    case asset::VFXNodeType::SubGraph: return path.starts_with("subGraph.");
    default: return false;
    }
}

bool IsVFXParamCompatible(asset::VFXParamType parameter, reflection::PropertyType property)
{
    switch (parameter) {
    case asset::VFXParamType::Float:
        return property == reflection::PropertyType::Float || property == reflection::PropertyType::Curve;
    case asset::VFXParamType::Int: return property == reflection::PropertyType::Int;
    case asset::VFXParamType::Bool: return property == reflection::PropertyType::Bool;
    case asset::VFXParamType::Color:
        return property == reflection::PropertyType::Color
            || property == reflection::PropertyType::Vector3
            || property == reflection::PropertyType::Gradient;
    case asset::VFXParamType::Vector3: return property == reflection::PropertyType::Vector3;
    case asset::VFXParamType::AssetRef:
        return property == reflection::PropertyType::AssetRef
            || property == reflection::PropertyType::String;
    }
    return false;
}

} // namespace

void VFXEditorPanel::OnBeforeBegin(EditorContext&)
{
    if (m_standaloneApplicationMode) {
        // 独立AppではルートWindowが唯一の制作領域。OSクライアント領域へ常時追従させて余白を作らない。
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(viewport->WorkSize, ImGuiCond_Always);
        return;
    }
    // WHY: NoAutoMerge を VFX Editor だけに付け、他の通常パネルは従来どおり DockSpace で管理する。
    //      OS ウィンドウのライフサイクル自体は ImGui backend に任せ、Win32/DX11 型を漏らさない。
    ImGuiWindowClass windowClass{};
    // 固定 ClassId は ImGui::Begin 前でも安全で、ini のレイアウト識別もフレーム間で安定する。
    windowClass.ClassId = static_cast<ImGuiID>(0x56465845u); // "VFXE"
    windowClass.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
    ImGui::SetNextWindowClass(&windowClass);

    // WHAT: メインEditorと完全に重ねず、右側の空き領域・別モニター・カスケード配置の順に選ぶ。
    // WHY: 同じ位置と大きさでは背面へ回ったVFX Editorの端すら見えず、閉じたように見えるため。
    const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    if (mainViewport != nullptr) {
        ImVec2 targetPos = mainViewport->Pos;
        ImVec2 targetSize = mainViewport->Size;
        const ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
        const ImVec2 mainCenter{ mainViewport->Pos.x + mainViewport->Size.x * 0.5f,
                                 mainViewport->Pos.y + mainViewport->Size.y * 0.5f };
        const ImGuiPlatformMonitor* mainMonitor = nullptr;
        for (const ImGuiPlatformMonitor& monitor : platform.Monitors) {
            if (mainCenter.x >= monitor.WorkPos.x && mainCenter.x < monitor.WorkPos.x + monitor.WorkSize.x
                && mainCenter.y >= monitor.WorkPos.y && mainCenter.y < monitor.WorkPos.y + monitor.WorkSize.y) {
                mainMonitor = &monitor;
                break;
            }
        }
        // 別モニターがあれば同じ大きさを保ち、右側のモニターを優先する。
        const ImGuiPlatformMonitor* secondary = nullptr;
        for (const ImGuiPlatformMonitor& monitor : platform.Monitors) {
            if (&monitor == mainMonitor) continue;
            if (secondary == nullptr
                || (monitor.WorkPos.x >= mainViewport->Pos.x + mainViewport->Size.x
                    && secondary->WorkPos.x < mainViewport->Pos.x + mainViewport->Size.x))
                secondary = &monitor;
        }
        if (secondary != nullptr) {
            targetPos = secondary->WorkPos;
            targetSize = { (std::min)(mainViewport->Size.x, secondary->WorkSize.x),
                           (std::min)(mainViewport->Size.y, secondary->WorkSize.y) };
        } else if (mainMonitor != nullptr) {
            constexpr float GAP = 10.0f;
            // 最小ウィンドウ制約と同じ幅を要求し、配置直後に右端が画面外へ押し出されるのを防ぐ。
            constexpr float MIN_USEFUL_WIDTH = 640.0f;
            const float rightX = mainViewport->Pos.x + mainViewport->Size.x + GAP;
            const float rightSpace = mainMonitor->WorkPos.x + mainMonitor->WorkSize.x - rightX;
            if (rightSpace >= MIN_USEFUL_WIDTH) {
                // Editor右側に実用幅がある場合は、その空きを余白のまま残さずVFX制作領域へ使う。
                targetPos = { rightX, mainViewport->Pos.y };
                targetSize = { rightSpace,
                    (std::min)(mainViewport->Size.y,
                               mainMonitor->WorkPos.y + mainMonitor->WorkSize.y - mainViewport->Pos.y) };
            } else {
                // 同一画面で横並びにできない場合も、右下へずらしてクリック可能な縁を必ず残す。
                constexpr float CASCADE = 36.0f;
                targetSize = { (std::min)(mainViewport->Size.x, mainMonitor->WorkSize.x - CASCADE),
                               (std::min)(mainViewport->Size.y, mainMonitor->WorkSize.y - CASCADE) };
                targetPos = {
                    std::clamp(mainViewport->Pos.x + CASCADE, mainMonitor->WorkPos.x,
                        mainMonitor->WorkPos.x + mainMonitor->WorkSize.x - targetSize.x),
                    std::clamp(mainViewport->Pos.y + CASCADE, mainMonitor->WorkPos.y,
                        mainMonitor->WorkPos.y + mainMonitor->WorkSize.y - targetSize.y)
                };
            }
        }
        const ImGuiCond placementCondition = m_resetWindowPlacementRequested
            ? ImGuiCond_Always : ImGuiCond_Appearing;
        ImGui::SetNextWindowPos(targetPos, placementCondition);
        ImGui::SetNextWindowSize(targetSize, placementCondition);
    }
    ImGui::SetNextWindowSizeConstraints({ 640.0f, 420.0f }, { FLT_MAX, FLT_MAX });
    if (m_focusWindowRequested) {
        ImGui::SetNextWindowCollapsed(false, ImGuiCond_Always);
        ImGui::SetNextWindowFocus();
    }
    m_focusWindowRequested = false;
    m_resetWindowPlacementRequested = false;
}

void VFXEditorPanel::RequestFocusAndReveal(bool resetPlacement)
{
    visible = true;
    m_focusWindowRequested = true;
    m_resetWindowPlacementRequested = m_resetWindowPlacementRequested || resetPlacement;
}

void VFXEditorPanel::OpenDroppedAsset(EditorContext& ctx, const std::string& path)
{
    if (IsVFXAssetPath(path)) {
        m_requestedAssetPath = path;
        return;
    }
    if (m_graphMode) AddGraphNodeFromAsset(path);
    else CreatePreviewEmitterFromAsset(ctx, path);
}

bool VFXEditorPanel::SaveCurrentGraph()
{
    const bool saved = SaveGraph();
    if (saved) m_restartPreviewRequested = true;
    return saved;
}

bool VFXEditorPanel::ReloadCurrentGraphFromDisk(const std::string& changedPath)
{
    (void)changedPath;
    if (m_graphPath.empty()) return true;
    if (m_graphDirty || !LoadGraph(m_graphPath)) return false;
    m_restartPreviewRequested = true;
    return true;
}

void VFXEditorPanel::OnInit(EditorContext&)
{
    ImNodes::SetImGuiContext(ImGui::GetCurrentContext());
    m_nodesContext = ImNodes::CreateContext();
    ImNodes::SetCurrentContext(m_nodesContext);
    ImNodes::StyleColorsDark();
    auto& style = ImNodes::GetStyle();
    style.Flags |= ImNodesStyleFlags_GridLinesPrimary;
    style.Colors[ImNodesCol_GridBackground] = IM_COL32(20, 22, 28, 255);
    style.Colors[ImNodesCol_GridLine] = IM_COL32(44, 48, 58, 110);
    style.Colors[ImNodesCol_GridLinePrimary] = IM_COL32(70, 76, 90, 150);
    m_graphEditorContext = ImNodes::EditorContextCreate();
}

void VFXEditorPanel::OnShutdown()
{
    if (m_nodesContext != nullptr) ImNodes::SetCurrentContext(m_nodesContext);
    if (m_graphEditorContext != nullptr) {
        ImNodes::EditorContextFree(m_graphEditorContext);
        m_graphEditorContext = nullptr;
    }
    if (m_nodesContext != nullptr) {
        ImNodes::DestroyContext(m_nodesContext);
        m_nodesContext = nullptr;
    }
}

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
    m_previewRequested = true;
    m_previewHovered = false;
    if (!m_requestedAssetPath.empty()) {
        ctx.selectedAssetPath = std::move(m_requestedAssetPath);
        m_requestedAssetPath.clear();
    }
    m_graphMode = IsVFXAssetPath(ctx.selectedAssetPath);
    if (m_standaloneApplicationMode && !m_graphMode && ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open Graph...", "Ctrl+O", false,
                                static_cast<bool>(ctx.requestOpenVFXAssetDialog)))
                ctx.requestOpenVFXAssetDialog();
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
    if (m_standaloneApplicationMode && !m_graphMode && !ImGui::GetIO().WantTextInput
        && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O) && ctx.requestOpenVFXAssetDialog)
        ctx.requestOpenVFXAssetDialog();
    if (m_graphMode != m_previousGraphMode && ctx.vfxPreviewScene != nullptr) {
        // Graph実行物と単体Emitterを同じPreview Worldへ残さず、モード切替時に所有物を明確に分ける。
        ctx.vfxPreviewScene->Clear();
        m_graphPreviewEntity = scene::EntityID::INVALID;
        m_previewSelectedEntity = scene::EntityID::INVALID;
        m_previousGraphMode = m_graphMode;
    }
    if (m_graphMode) {
        DrawGraphEditor(ctx);
        return;
    }

    if (ctx.vfxPreviewScene == nullptr) {
        ImGui::TextDisabled("VFX Preview World is unavailable.");
        return;
    }

    // WHY: EditorContextを複製してSceneと選択だけをPreview Worldへ差し替えることで、既存の
    //      Particle Inspectorを再利用しながらメインSceneのHierarchy・Undo・Dirtyを一切変更しない。
    EditorContext previewCtx = ctx;
    previewCtx.activeScene = ctx.vfxPreviewScene;
    previewCtx.selectedEntities.clear();
    if (previewCtx.activeScene->IsValid(m_previewSelectedEntity))
        previewCtx.selectedEntities.push_back(m_previewSelectedEntity);
    previewCtx.markSceneDirty = []() {};

    const std::vector<scene::EntityID> group = BuildEffectGroup(previewCtx);
    scene::ParticleEmitter* root = nullptr;
    if (!group.empty())
        root = previewCtx.activeScene->GetComponent<scene::ParticleEmitter>(group.front());

    // プレビュー速度の注入。毎フレーム書き込み、書き込みが止まると Engine 側が自動で 1.0 へ戻す。
    // Play Mode 中はゲーム本来の再生を邪魔しないよう注入しない。
    const bool inPlayMode = previewCtx.playMode && !previewCtx.playMode->IsInEditor();
    if (!inPlayMode) {
        for (const scene::EntityID id : group) {
            if (auto* emitter = previewCtx.activeScene->GetComponent<scene::ParticleEmitter>(id)) {
                emitter->editorTimeScale      = m_paused ? 0.0f : (std::max)(m_previewSpeed, 0.0f);
                emitter->editorTimeScaleFrame = Time::frameCount;
            }
        }
    }

    // Space で再生 / 一時停止 (パネルフォーカス中のみ。テキスト入力中は無視)
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
        && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Space, false) && root)
        m_paused = !m_paused;

    DrawTransport(previewCtx, group, root);
    ImGui::Separator();

    const float workspaceHeight = (std::max)(ImGui::GetContentRegionAvail().y, 180.0f);
    if (ImGui::BeginTable("##VFXEmitterWorkspace", 2,
            ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV
                | ImGuiTableFlags_SizingStretchProp,
            { 0.0f, workspaceHeight })) {
        ImGui::TableSetupColumn("Hierarchy", ImGuiTableColumnFlags_WidthFixed, m_emitterHierarchyWidth);
        ImGui::TableSetupColumn("Preview and Inspector", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::BeginChild("##VFXHierarchy", { 0.0f, 0.0f }, true);
        DrawHierarchy(previewCtx);
        ImGui::EndChild();

        ImGui::TableSetColumnIndex(1);
        const float rightHeight = ImGui::GetContentRegionAvail().y;
        const float maxPreviewHeight = (std::max)(180.0f, rightHeight - 170.0f);
        m_emitterPreviewHeight = std::clamp(m_emitterPreviewHeight, 140.0f, maxPreviewHeight);
        ImGui::BeginChild("##VFXEmitterPreview", { 0.0f, m_emitterPreviewHeight }, false);
        DrawPreviewViewport(previewCtx);
        ImGui::EndChild();

        ImGui::InvisibleButton("##VFXPreviewSplitter", { -1.0f, 7.0f });
        const bool splitterActive = ImGui::IsItemActive();
        if (ImGui::IsItemHovered() || splitterActive)
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        if (splitterActive)
            m_emitterPreviewHeight = std::clamp(
                m_emitterPreviewHeight + ImGui::GetIO().MouseDelta.y, 140.0f, maxPreviewHeight);
        const ImVec2 splitMin = ImGui::GetItemRectMin();
        const ImVec2 splitMax = ImGui::GetItemRectMax();
        const ImU32 splitColor = ImGui::GetColorU32(
            splitterActive || ImGui::IsItemHovered() ? ImGuiCol_SeparatorHovered : ImGuiCol_Separator);
        ImGui::GetWindowDrawList()->AddLine(
            { splitMin.x + 2.0f, (splitMin.y + splitMax.y) * 0.5f },
            { splitMax.x - 2.0f, (splitMin.y + splitMax.y) * 0.5f }, splitColor, 1.5f);

        ImGui::BeginChild("##VFXEmitterInspector", { 0.0f, 0.0f }, false);
        if (root) {
            DrawTimeline(previewCtx, group, root);
            DrawStats(previewCtx, group, root);
            ImGui::Separator();
            if (DrawParticleEmitterModules(*root, previewCtx)) {
                // Preview Worldは保存対象外。変更結果は即時プレビューだけへ反映する。
            }
        } else {
            ImGui::Spacing();
            ImGui::TextDisabled("No emitter selected in VFX Preview World");
            ImGui::TextWrapped("Create one in the VFX Hierarchy, or drop a texture, material or mesh asset into this window.");
        }
        ImGui::EndChild();
        ImGui::EndTable();
    }
    if (!previewCtx.selectedEntities.empty())
        m_previewSelectedEntity = previewCtx.PrimarySelected();
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
    ImGui::SameLine((std::max)(ImGui::GetCursorPosX() + 12.0f,
                               ImGui::GetContentRegionMax().x - 190.0f));
    ImGui::TextDisabled("World: VFX Preview (isolated)");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("Objects created here never enter the editing Scene");
}

void VFXEditorPanel::DrawHierarchy(EditorContext& ctx)
{
    ImGui::TextUnformatted("Effects");
    ImGui::Separator();
    ImGui::SameLine();
    ImGui::TextDisabled("VFX World");
    if (!ctx.activeScene) {
        ImGui::TextDisabled("No preview world");
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
    }
    if (ImGui::BeginDragDropTarget()) {
        std::string path;
        if (ReadAssetPayload(ImGui::AcceptDragDropPayload("ASSET_PATH"), path))
            CreatePreviewEmitterFromAsset(ctx, path);
        ImGui::EndDragDropTarget();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("Create an empty emitter, or drop an asset onto this button");
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
    if (!anyRoot) {
        ImGui::Spacing();
        ImGui::TextDisabled("No emitters in VFX Preview World");
        ImGui::TextWrapped("Drop a texture, material or mesh here to create one.");
    }
    if (!ImGui::GetIO().WantTextInput
        && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
        && ImGui::IsKeyPressed(ImGuiKey_Delete, false)
        && ctx.activeScene->IsValid(ctx.PrimarySelected())) {
        ctx.activeScene->DestroyGameObject(ctx.PrimarySelected());
        ctx.selectedEntities.clear();
        m_previewSelectedEntity = scene::EntityID::INVALID;
    }

    // Hierarchyの空白へ落とした場合はルートへ移動、AssetならルートEmitterとして生成する。
    ImGui::InvisibleButton("##VFXHierarchyDropZone",
                           { -1.0f, (std::max)(ImGui::GetContentRegionAvail().y, 36.0f) });
    if (ImGui::BeginDragDropTarget()) {
        const ImGuiPayload* entityPayload =
            ImGui::AcceptDragDropPayload("FBZZ_VFX_PREVIEW_ENTITY");
        if (entityPayload != nullptr && entityPayload->DataSize == sizeof(scene::EntityID)) {
            const scene::EntityID draggedId =
                *static_cast<const scene::EntityID*>(entityPayload->Data);
            if (auto* dragged = ctx.activeScene->GetGameObject(draggedId)) dragged->ClearParent();
        }
        std::string path;
        if (ReadAssetPayload(ImGui::AcceptDragDropPayload("ASSET_PATH"), path))
            CreatePreviewEmitterFromAsset(ctx, path);
        ImGui::EndDragDropTarget();
    }
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
    if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload("FBZZ_VFX_PREVIEW_ENTITY", &id, sizeof(id));
        ImGui::TextUnformatted(go->name.c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FBZZ_VFX_PREVIEW_ENTITY");
        if (payload != nullptr && payload->DataSize == sizeof(scene::EntityID)) {
            const scene::EntityID draggedId = *static_cast<const scene::EntityID*>(payload->Data);
            if (draggedId != id) {
                if (auto* dragged = ctx.activeScene->GetGameObject(draggedId))
                    dragged->SetParent(go);
            }
        }
        std::string path;
        if (ReadAssetPayload(ImGui::AcceptDragDropPayload("ASSET_PATH"), path)) {
            ctx.selectedEntities = { id };
            CreatePreviewEmitterFromAsset(ctx, path);
        }
        ImGui::EndDragDropTarget();
    }
    bool deleteRequested = false;
    if (ImGui::BeginPopupContextItem("##VFXEmitterContext")) {
        ImGui::TextDisabled("VFX Preview World");
        if (ImGui::MenuItem("Add Child Emitter")) {
            ctx.selectedEntities = { id };
            CreatePreviewEmitterFromAsset(ctx, "");
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Delete", "Del")) deleteRequested = true;
        ImGui::EndPopup();
    }
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
    if (deleteRequested) {
        ctx.activeScene->DestroyGameObject(id);
        if (ctx.PrimarySelected() == id) ctx.selectedEntities.clear();
        if (m_previewSelectedEntity == id) m_previewSelectedEntity = scene::EntityID::INVALID;
    }
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

bool VFXEditorPanel::LoadGraph(const std::string& path)
{
    m_graphPath = path;
    m_graphError.clear();
    asset::VFXGraphAsset loaded;
    if (!asset::ParseVFXGraphAsset(path, loaded, &m_graphError)) {
        m_graph = {};
        m_selectedGraphNodeId = -1;
        return false;
    }
    m_graph = std::move(loaded);
    // 構文が読めれば不正DAGも保持し、Inspector/Canvasから修復できるようにする。
    asset::ValidateVFXGraphAsset(m_graph, &m_graphError);
    m_graphDirty = false;
    m_graphUndo.clear();
    m_graphRedo.clear();
    m_graphPositionsPending = true;
    m_selectedGraphNodeId = -1;
    m_selectedGraphLinkIndex = -1;
    if (m_nodesContext != nullptr) ImNodes::SetCurrentContext(m_nodesContext);
    if (m_graphEditorContext != nullptr) {
        ImNodes::EditorContextSet(m_graphEditorContext);
        ImNodes::ClearNodeSelection();
        ImNodes::ClearLinkSelection();
        ImNodes::EditorContextResetPanning({ 0.0f, 0.0f });
    }
    return true;
}

bool VFXEditorPanel::SaveGraph()
{
    m_graphError.clear();
    if (!asset::SaveVFXGraphAsset(m_graphPath, m_graph, &m_graphError)) return false;
    m_graphDirty = false;
    AssetDirtyRegistry::MarkClean(m_graphPath);
    return true;
}

void VFXEditorPanel::AddGraphNode(asset::VFXNodeType type)
{
    PushGraphUndo();
    int nextId = 1;
    for (const auto& node : m_graph.nodes) nextId = (std::max)(nextId, node.id + 1);
    asset::VFXGraphNode node;
    node.id = nextId;
    node.type = type;
    node.name = asset::VFXNodeTypeName(type);
    if (m_pendingNodeSpawnValid) {
        // Canvas右クリックで開いたAddメニューはクリック位置へそのまま生成する(Unity Shader/VFX Graph相当のUX)。
        node.editorX = m_pendingNodeSpawnGridX;
        node.editorY = m_pendingNodeSpawnGridY;
        m_pendingNodeSpawnValid = false;
    } else {
        node.editorX = 260.0f + static_cast<float>((m_graph.nodes.size() % 3) * 220);
        node.editorY = 80.0f + static_cast<float>((m_graph.nodes.size() / 3) * 170);
    }
    node.duration = type == asset::VFXNodeType::Delay ? 0.25f
        : (type == asset::VFXNodeType::Particle ? node.particle.duration : 1.0f);
    m_graph.nodes.push_back(std::move(node));
    m_selectedGraphNodeId = nextId;
    m_graphDirty = true;
    m_graphPositionsPending = true;
}

void VFXEditorPanel::AddGraphNodeFromAsset(const std::string& sourcePath)
{
    const std::string path = NormalizeAssetPath(sourcePath);
    asset::VFXNodeType type = asset::VFXNodeType::Particle;
    if (EndsWithInsensitive(path, ".vfx")) type = asset::VFXNodeType::SubGraph;
    else if (EndsWithInsensitive(path, ".wav") || EndsWithInsensitive(path, ".ogg")
             || EndsWithInsensitive(path, ".mp3")) type = asset::VFXNodeType::Audio;
    else if (EndsWithInsensitive(path, ".mesh") || EndsWithInsensitive(path, ".fbx")
             || EndsWithInsensitive(path, ".obj")) type = asset::VFXNodeType::MeshTrail;
    else if (EndsWithInsensitive(path, ".png") || EndsWithInsensitive(path, ".jpg")
             || EndsWithInsensitive(path, ".jpeg") || EndsWithInsensitive(path, ".dds")
             || EndsWithInsensitive(path, ".tga") || EndsWithInsensitive(path, ".tex"))
        type = asset::VFXNodeType::Decal;
    else if (!EndsWithInsensitive(path, ".mat")) {
        m_graphError = "このアセット形式はVFXノードへ変換できません: " + path;
        return;
    }

    AddGraphNode(type);
    auto* node = FindGraphNode(m_graph, m_selectedGraphNodeId);
    if (node == nullptr) return;
    if (type == asset::VFXNodeType::SubGraph) node->subGraph.graphPath = path;
    else if (type == asset::VFXNodeType::Audio) node->audio.clipPath = path;
    else if (type == asset::VFXNodeType::MeshTrail) node->trail.meshPath = path;
    else if (type == asset::VFXNodeType::Decal) node->decal.albedoPath = path;
    else node->particle.materialPath = path;
    node->name = path.substr(path.find_last_of("/\\") + 1);
    m_restartPreviewRequested = true;
}

void VFXEditorPanel::CreatePreviewEmitterFromAsset(EditorContext& ctx, const std::string& sourcePath)
{
    if (ctx.activeScene == nullptr) return;
    const std::string path = NormalizeAssetPath(sourcePath);
    if (EndsWithInsensitive(path, ".vfx")) {
        // .vfxはEmitterのtextureとして扱わず、独立Graph Editorで開く。
        m_requestedAssetPath = path;
        return;
    }
    scene::EntityID parentId = scene::EntityID::INVALID;
    if (auto* selected = ctx.GetSelectedGO();
        selected != nullptr && selected->GetComponent<scene::ParticleEmitter>() != nullptr)
        parentId = selected->GetID();

    auto& gameObject = ctx.activeScene->CreateGameObject("VFX Emitter");
    if (ctx.activeScene->IsValid(parentId))
        if (auto* parent = ctx.activeScene->GetGameObject(parentId)) gameObject.SetParent(*parent);
    auto& emitter = gameObject.AddComponent<scene::ParticleEmitter>();
    if (EndsWithInsensitive(path, ".mat")) emitter.materialPath = path;
    else if (EndsWithInsensitive(path, ".mesh") || EndsWithInsensitive(path, ".fbx")
             || EndsWithInsensitive(path, ".obj")) emitter.meshShapePath = path;
    else emitter.texturePath = path;
    ctx.selectedEntities = { gameObject.GetID() };
    m_previewSelectedEntity = gameObject.GetID();
}

void VFXEditorPanel::DeleteSelectedGraphNode()
{
    const auto* node = FindGraphNode(m_graph, m_selectedGraphNodeId);
    if (node == nullptr || node->type == asset::VFXNodeType::Entry) return;
    PushGraphUndo();
    const int id = node->id;
    std::erase_if(m_graph.links, [id](const asset::VFXGraphLink& link) {
        return link.fromNode == id || link.toNode == id;
    });
    std::erase_if(m_graph.nodes, [id](const asset::VFXGraphNode& candidate) {
        return candidate.id == id;
    });
    m_selectedGraphNodeId = -1;
    m_graphDirty = true;
}

void VFXEditorPanel::DuplicateGraphNode(int nodeId)
{
    const auto* source = FindGraphNode(m_graph, nodeId);
    if (source == nullptr || source->type == asset::VFXNodeType::Entry) return;
    PushGraphUndo();
    int nextId = 1;
    for (const auto& node : m_graph.nodes) nextId = (std::max)(nextId, node.id + 1);
    asset::VFXGraphNode node = *source;
    node.id = nextId;
    node.name = source->name + " Copy";
    // 元ノードへ重ねず視認できるよう、少しずらした位置へ複製する。リンクは複製しない(接続の意図が
    // 不明瞭になるため、複製は「設定を引き継いだ新規ノード」として空の接続から始める)。
    node.editorX = source->editorX + 40.0f;
    node.editorY = source->editorY + 40.0f;
    m_graph.nodes.push_back(std::move(node));
    m_selectedGraphNodeId = nextId;
    m_graphDirty = true;
    m_graphPositionsPending = true;
}

void VFXEditorPanel::PushGraphUndo()
{
    PushGraphUndo(m_graph);
}

void VFXEditorPanel::PushGraphUndo(const asset::VFXGraphAsset& before)
{
    constexpr std::size_t MAX_HISTORY = 96;
    m_graphUndo.push_back(before);
    if (m_graphUndo.size() > MAX_HISTORY) m_graphUndo.erase(m_graphUndo.begin());
    m_graphRedo.clear();
}

void VFXEditorPanel::UndoGraphEdit()
{
    if (m_graphUndo.empty()) return;
    m_graphRedo.push_back(m_graph);
    m_graph = std::move(m_graphUndo.back());
    m_graphUndo.pop_back();
    m_graphDirty = true;
    m_graphPositionsPending = true;
    m_selectedGraphNodeId = -1;
    m_selectedGraphLinkIndex = -1;
    m_restartPreviewRequested = true;
}

void VFXEditorPanel::RedoGraphEdit()
{
    if (m_graphRedo.empty()) return;
    m_graphUndo.push_back(m_graph);
    m_graph = std::move(m_graphRedo.back());
    m_graphRedo.pop_back();
    m_graphDirty = true;
    m_graphPositionsPending = true;
    m_selectedGraphNodeId = -1;
    m_selectedGraphLinkIndex = -1;
    m_restartPreviewRequested = true;
}

void VFXEditorPanel::DrawGraphEditor(EditorContext& ctx)
{
    m_graphEditorUi.Render(*this, ctx);
    if (m_graphDirty) {
        const std::string capturedPath = m_graphPath;
        const asset::VFXGraphAsset capturedGraph = m_graph;
        AssetDirtyRegistry::Register(
            capturedPath, NormalizeAssetPath(capturedPath), "VFX",
            [capturedPath, capturedGraph]() {
                return asset::SaveVFXGraphAsset(capturedPath, capturedGraph);
            });
    }
}

void VFXEditorPanel::DrawGraphMenuBar(EditorContext& ctx)
{
    if (!ImGui::BeginMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open Graph...", "Ctrl+O", false,
                            static_cast<bool>(ctx.requestOpenVFXAssetDialog))) {
            ctx.requestOpenVFXAssetDialog();
        }
        if (ImGui::MenuItem("Save", "Ctrl+S", false, m_graphDirty)) {
            if (SaveGraph()) m_restartPreviewRequested = true;
        }
        if (ImGui::MenuItem("Reload from Disk", "Ctrl+R")) {
            LoadGraph(m_graphPath);
            m_restartPreviewRequested = true;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo VFX Edit", "Ctrl+Z", false, !m_graphUndo.empty())) UndoGraphEdit();
        if (ImGui::MenuItem("Redo VFX Edit", "Ctrl+Y", false, !m_graphRedo.empty())) RedoGraphEdit();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Add")) {
        DrawAddNodeMenu();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Templates")) {
        constexpr const char* templates[] = { "Explosion", "Fire", "Smoke", "Impact", "Magic" };
        for (const char* templateName : templates) {
            if (!ImGui::MenuItem(templateName)) continue;
            asset::VFXGraphAsset templateGraph;
            std::string error;
            std::string path = std::string("Assets/VFX/Templates/") + templateName + ".vfx";
            bool loaded = asset::LoadVFXGraphAsset(path, templateGraph, &error);
            if (!loaded && !ctx.engineRoot.empty()) {
                path = (std::filesystem::path(ctx.engineRoot) / "Assets/VFX/Templates"
                    / (std::string(templateName) + ".vfx")).generic_string();
                loaded = asset::LoadVFXGraphAsset(path, templateGraph, &error);
            }
            if (loaded) {
                PushGraphUndo();
                m_graph = std::move(templateGraph);
                m_graphDirty = true;
                m_graphPositionsPending = true;
                m_selectedGraphNodeId = -1;
                m_selectedGraphLinkIndex = -1;
                m_restartPreviewRequested = true;
            } else m_graphError = std::move(error);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Replaces this graph; Ctrl+Z restores it.");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Viewport (Always Visible)", nullptr, true, false);
        ImGui::MenuItem("Mini Map", nullptr, &m_showMiniMap);
        if (ImGui::MenuItem("Frame Selection", "F", false, m_selectedGraphNodeId > 0))
            m_focusSelectionRequested = true;
        ImGui::EndMenu();
    }
    if (!m_standaloneApplicationMode && ImGui::BeginMenu("Window")) {
        if (ImGui::MenuItem("Move to Available Screen Area"))
            RequestFocusAndReveal(true);
        ImGui::TextDisabled("Editor: View > Bring VFX Editor to Front");
        ImGui::EndMenu();
    }
    ImGui::EndMenuBar();

    if (!ImGui::GetIO().WantTextInput && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S)) {
        if (SaveGraph()) m_restartPreviewRequested = true;
    }
    if (!ImGui::GetIO().WantTextInput && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O)
        && ctx.requestOpenVFXAssetDialog) {
        ctx.requestOpenVFXAssetDialog();
    }
    if (!ImGui::GetIO().WantTextInput && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_R)) {
        LoadGraph(m_graphPath);
        m_restartPreviewRequested = true;
    }
    if (!ImGui::GetIO().WantTextInput && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z)) UndoGraphEdit();
    if (!ImGui::GetIO().WantTextInput && (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y)
        || ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z))) RedoGraphEdit();
}

void VFXEditorPanel::DrawGraphStatusBar()
{
    std::string validationError;
    const bool valid = asset::ValidateVFXGraphAsset(m_graph, &validationError);
    const asset::VFXGraphBudgetStats budget = asset::CalculateVFXGraphBudget(m_graph);
    const bool overBudget = budget.particles > m_graph.maxParticles
        || budget.lights > m_graph.maxLights || budget.audioVoices > m_graph.maxAudioVoices;

    ImGui::Separator();
    ImGui::TextColored(valid && !overBudget ? ImVec4{ 0.38f, 0.82f, 0.52f, 1.0f }
                                             : ImVec4{ 1.0f, 0.42f, 0.30f, 1.0f },
                       valid && !overBudget ? "READY" : "NEEDS ATTENTION");
    ImGui::SameLine();
    ImGui::TextDisabled("%d nodes  |  %d links  |  P %d/%d  L %d/%d  A %d/%d",
                        static_cast<int>(m_graph.nodes.size()), static_cast<int>(m_graph.links.size()),
                        budget.particles, m_graph.maxParticles, budget.lights, m_graph.maxLights,
                        budget.audioVoices, m_graph.maxAudioVoices);
    if (!valid && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", validationError.c_str());
    const float shortcutX = (std::max)(ImGui::GetCursorPosX() + 16.0f,
                                       ImGui::GetContentRegionMax().x - 165.0f);
    ImGui::SameLine(shortcutX);
    ImGui::TextDisabled("Ctrl+S Save   F Focus");
}

void VFXEditorPanel::DrawAddNodeMenu()
{
    struct Item { asset::VFXNodeType type; const char* hint; };
    constexpr Item items[] = {
        { asset::VFXNodeType::Particle, "Particles and flipbooks" },
        { asset::VFXNodeType::Trail, "Motion trail" },
        { asset::VFXNodeType::MeshTrail, "Mesh after-image" },
        { asset::VFXNodeType::Light, "Point light" },
        { asset::VFXNodeType::Audio, "Spatial audio" },
        { asset::VFXNodeType::Decal, "Projected decal" },
        { asset::VFXNodeType::Delay, "Timing only" },
        { asset::VFXNodeType::SubGraph, "Nested .vfx" },
    };
    for (const Item& item : items) {
        const std::string label = std::string("[") + VFXNodeIcon(item.type) + "] "
            + asset::VFXNodeTypeName(item.type);
        if (ImGui::MenuItem(label.c_str())) AddGraphNode(item.type);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", item.hint);
    }
}

void VFXEditorPanel::DrawPreviewViewport(EditorContext& ctx, bool fillAvailable)
{
    ImGui::TextUnformatted("Preview");
    ImGui::SameLine();
    ImGui::TextDisabled("Isolated World");

    const ImVec2 available = ImGui::GetContentRegionAvail();
    // 狭いウィンドウでも横スクロールを発生させず、Inspector のリサイズ操作へ追従する。
    const float width = (std::max)(available.x, 1.0f);
    const float maxHeight = (std::max)(available.y, 90.0f);
    // Graph専用Viewport列では縦方向も全て使い、Canvasと同じ高さで結果を見続けられるようにする。
    const float height = fillAvailable
        ? maxHeight
        : std::clamp(width * (9.0f / 16.0f), 90.0f, maxHeight);
    previewWidth = std::floor(width);
    previewHeight = std::floor(height);

    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 end{ start.x + width, start.y + height };
    ImGui::InvisibleButton("##VFXPreview", { width, height });
    const bool viewportHovered = ImGui::IsItemHovered();
    m_previewHovered = m_previewHovered || viewportHovered;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && ImGui::BeginTooltip()) {
        ImGui::TextUnformatted("RMB + WASD: Fly   MMB: Pan   Wheel: Dolly");
        ImGui::TextUnformatted("Alt + LMB: Orbit   Drop asset: Create effect");
        ImGui::EndTooltip();
    }
    if (ImGui::BeginDragDropTarget()) {
        std::string path;
        if (ReadAssetPayload(ImGui::AcceptDragDropPayload("ASSET_PATH"), path)) {
            if (m_graphMode) AddGraphNodeFromAsset(path);
            else CreatePreviewEmitterFromAsset(ctx, path);
        }
        ImGui::EndDragDropTarget();
    }
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(start, end, IM_COL32(5, 6, 9, 255));
    if (previewRT.IsValid() && ctx.imguiRenderer && ctx.resources) {
        void* nativeId = ctx.imguiRenderer->GetImTextureID(previewRT, *ctx.resources, 0);
        if (nativeId != nullptr) {
            const ImTextureID textureId = static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(nativeId));
            drawList->AddImage(textureId, start, end);
        }
    } else {
        drawList->AddText({ start.x + 12.0f, start.y + 12.0f },
                          IM_COL32(150, 155, 165, 255), "Preview RenderTarget unavailable");
    }
    drawList->AddRect(start, end, IM_COL32(70, 76, 90, 255));
    drawList->AddRectFilled({ start.x + 10.0f, start.y + 10.0f },
                            { start.x + 128.0f, start.y + 31.0f }, IM_COL32(10, 14, 20, 190), 4.0f);
    drawList->AddText({ start.x + 17.0f, start.y + 13.0f }, IM_COL32(112, 210, 168, 255),
                      "PREVIEW WORLD");
}

void VFXEditorPanel::DrawGraphToolbar(EditorContext& ctx)
{
    ImGui::Text("%s%s", m_graph.name.c_str(), m_graphDirty ? "  *" : "");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", m_graphPath.c_str());
    ImGui::SameLine();
    const bool graphSaved = ImGui::Button("Save") && SaveGraph();
    if (graphSaved) m_restartPreviewRequested = true;
    ImGui::SameLine();
    const bool graphReloaded = ImGui::Button("Reload");
    if (graphReloaded) {
        LoadGraph(m_graphPath);
        m_restartPreviewRequested = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("+ Add Effect")) ImGui::OpenPopup("##VFXAddNode");
    if (ImGui::BeginPopup("##VFXAddNode")) {
        ImGui::TextDisabled("Effect Nodes");
        ImGui::Separator();
        DrawAddNodeMenu();
        ImGui::EndPopup();
    }

    const std::string assetPath = NormalizeAssetPath(m_graphPath);
    scene::GameObject* previewObject = ctx.vfxPreviewScene != nullptr
        ? ctx.vfxPreviewScene->GetGameObject(m_graphPreviewEntity) : nullptr;
    scene::VFXGraphComponent* instance = previewObject != nullptr
        ? previewObject->GetComponent<scene::VFXGraphComponent>() : nullptr;

    // Preview Instance は専用 Scene に自動生成する。Hierarchy選択やScene Dirtyを一切変更しない。
    if (instance == nullptr && ctx.vfxPreviewScene != nullptr && !assetPath.empty()) {
        auto& gameObject = ctx.vfxPreviewScene->CreateGameObject("__VFX_PREVIEW_ROOT");
        scene::VFXGraphComponent component;
        component.graphPath = assetPath;
        component.playOnAwake = false;
        component.playing = true;
        component.editorPreviewFrame = Time::frameCount;
        gameObject.AddComponent<scene::VFXGraphComponent>(std::move(component));
        m_graphPreviewEntity = gameObject.GetID();
        instance = gameObject.GetComponent<scene::VFXGraphComponent>();
    }
    if ((graphSaved || graphReloaded || m_restartPreviewRequested) && instance != nullptr) {
        instance->reloadRequested = true;
        instance->Restart();
        m_restartPreviewRequested = false;
    }
    ImGui::SameLine();
    if (instance != nullptr) {
        instance->editorPreviewFrame = Time::frameCount;
        ImGui::TextDisabled("  %.2fs / %.2fs", instance->playTime, instance->graphDuration);
        ImGui::SameLine();
        if (ImGui::Button(instance->playing ? "Pause Preview" : "Play Preview")) {
            if (instance->playing) instance->Pause();
            else instance->Resume();
        }
        ImGui::SameLine();
        if (ImGui::Button("Restart Preview")) {
            instance->reloadRequested = true;
            instance->Restart();
        }
        ImGui::SameLine();
        if (ImGui::Button("Stop Preview")) instance->Stop();
        ImGui::SameLine();
        ImGui::Checkbox("Loop", &instance->loop);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        ImGui::DragFloat("Speed", &instance->speed, 0.01f, 0.0f, 8.0f);
        if (!ImGui::GetIO().WantTextInput && ImGui::Shortcut(ImGuiKey_Space)) {
            if (instance->playing) instance->Pause();
            else instance->Resume();
        }
        if (instance->graphDuration > 0.0f) {
            float scrubTime = instance->playTime;
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::SliderFloat("Timeline", &scrubTime, 0.0f, instance->graphDuration, "%.3fs")) {
                instance->Pause();
                instance->editorScrubTime = scrubTime;
                instance->editorPreviewFrame = Time::frameCount;
            }
            if (ImGui::Button("Step -")) {
                instance->Pause();
                instance->editorScrubTime = (std::max)(instance->playTime - kScrubStep, 0.0f);
            }
            ImGui::SameLine();
            if (ImGui::Button("Step +")) {
                instance->Pause();
                instance->editorScrubTime = (std::min)(instance->playTime + kScrubStep,
                                                       instance->graphDuration);
            }
        }
        if (ctx.vfxPreviewScene != nullptr) {
            int activeNodes = 0;
            int liveParticles = 0;
            int visibleParticles = 0;
            for (const auto& runtimeNode : instance->runtimeNodes) {
                if (runtimeNode.active) ++activeNodes;
                if (!runtimeNode.entity.IsValid()) continue;
                const auto* emitter = ctx.vfxPreviewScene->GetComponent<scene::ParticleEmitter>(runtimeNode.entity);
                if (emitter == nullptr) continue;
                liveParticles += emitter->simulationMode == scene::ParticleSimulationMode::Gpu
                    ? emitter->visibleParticleCount : static_cast<int>(emitter->particles.size());
                visibleParticles += emitter->visibleParticleCount;
            }
            ImGui::TextDisabled("Runtime  active %d/%d   particles %d   visible %d",
                                activeNodes, static_cast<int>(instance->runtimeNodes.size()),
                                liveParticles, visibleParticles);
        }
        if (m_graphDirty && ImGui::IsItemHovered())
            ImGui::SetTooltip("Save the graph before reloading the preview instance");
    }
}

void VFXEditorPanel::DrawGraphCanvas(EditorContext&)
{
    if (m_nodesContext == nullptr || m_graphEditorContext == nullptr) return;
    const asset::VFXGraphAsset graphBeforeCanvasEdit = m_graph;
    ImNodes::SetCurrentContext(m_nodesContext);
    ImNodes::EditorContextSet(m_graphEditorContext);
    // ImNodesはBeginNodeEditor直後のカーソル位置をCanvas原点とするため、右クリックAddの
    // スポーン座標算出(Screen space -> Grid space)にはこのタイミングで取得した値を使う。
    const ImVec2 canvasOriginScreenPos = ImGui::GetCursorScreenPos();
    ImNodes::BeginNodeEditor();
    if (m_graphPositionsPending) {
        for (const auto& node : m_graph.nodes)
            ImNodes::SetNodeGridSpacePos(node.id, { node.editorX, node.editorY });
        m_graphPositionsPending = false;
    }
    for (const auto& node : m_graph.nodes) {
        const ImU32 color = VFXNodeColor(node.type);
        ImNodes::PushColorStyle(ImNodesCol_NodeBackground, IM_COL32(31, 34, 42, 255));
        ImNodes::PushColorStyle(ImNodesCol_NodeBackgroundHovered, IM_COL32(40, 44, 54, 255));
        ImNodes::PushColorStyle(ImNodesCol_NodeBackgroundSelected, IM_COL32(48, 52, 64, 255));
        ImNodes::PushColorStyle(ImNodesCol_TitleBar, color);
        ImNodes::PushColorStyle(ImNodesCol_TitleBarHovered, color);
        ImNodes::PushColorStyle(ImNodesCol_TitleBarSelected, color);
        ImNodes::BeginNode(node.id);
        ImNodes::BeginNodeTitleBar();
        ImGui::Text("[%s]  %s", VFXNodeIcon(node.type), node.name.c_str());
        ImNodes::EndNodeTitleBar();
        if (node.type != asset::VFXNodeType::Entry) {
            ImNodes::BeginInputAttribute(InputPinId(node.id));
            ImGui::TextUnformatted("In");
            ImNodes::EndInputAttribute();
        }
        ImNodes::BeginStaticAttribute(300000 + node.id);
        ImGui::TextDisabled("%s", asset::VFXNodeTypeName(node.type));
        if (node.type != asset::VFXNodeType::Entry)
            ImGui::TextDisabled("offset %.2fs  duration %.2fs", node.startOffset, node.duration);
        ImNodes::EndStaticAttribute();
        ImNodes::BeginOutputAttribute(OutputPinId(node.id));
        ImGui::Indent(90.0f);
        ImGui::TextUnformatted("Out");
        ImNodes::EndOutputAttribute();
        ImNodes::EndNode();
        for (int i = 0; i < 6; ++i) ImNodes::PopColorStyle();
    }
    for (std::size_t i = 0; i < m_graph.links.size(); ++i) {
        const auto& link = m_graph.links[i];
        const ImU32 linkColor = link.trigger == asset::VFXLinkTrigger::OnCollision
            ? IM_COL32(235, 116, 82, 255)
            : (link.trigger == asset::VFXLinkTrigger::OnDeath
                ? IM_COL32(196, 108, 224, 255)
            : (link.trigger == asset::VFXLinkTrigger::OnStart
                ? IM_COL32(92, 184, 224, 255) : IM_COL32(170, 177, 192, 255)));
        ImNodes::PushColorStyle(ImNodesCol_Link, linkColor);
        ImNodes::PushColorStyle(ImNodesCol_LinkSelected, IM_COL32(255, 218, 108, 255));
        ImNodes::Link(LinkId(static_cast<int>(i)), OutputPinId(link.fromNode), InputPinId(link.toNode));
        ImNodes::PopColorStyle();
        ImNodes::PopColorStyle();
    }
    if (m_showMiniMap) ImNodes::MiniMap(0.16f, ImNodesMiniMapLocation_BottomRight);
    ImNodes::EndNodeEditor();

    // Asset BrowserからCanvasへ直接落とし、拡張子から適切なEffectノードへ変換する。
    if (ImGui::BeginDragDropTarget()) {
        std::string path;
        if (ReadAssetPayload(ImGui::AcceptDragDropPayload("ASSET_PATH"), path))
            AddGraphNodeFromAsset(path);
        ImGui::EndDragDropTarget();
    }

    // ノード上の右クリックはノードコンテキストメニュー、空きスペースはAdd Effectメニューへ振り分ける
    // (UnityのShader/VFX Graphと同じ使い分け)。
    int hoveredNodeId = -1;
    const bool nodeHovered = ImNodes::IsNodeHovered(&hoveredNodeId);
    if (nodeHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        m_graphContextMenuNodeId = hoveredNodeId;
        m_selectedGraphNodeId = hoveredNodeId;
        ImGui::OpenPopup("##VFXNodeContext");
    } else if (!nodeHovered && ImNodes::IsEditorHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        const ImVec2 mouseScreenPos = ImGui::GetMousePos();
        const ImVec2 panning = ImNodes::EditorContextGetPanning();
        m_pendingNodeSpawnGridX = mouseScreenPos.x - canvasOriginScreenPos.x - panning.x;
        m_pendingNodeSpawnGridY = mouseScreenPos.y - canvasOriginScreenPos.y - panning.y;
        m_pendingNodeSpawnValid = true;
        ImGui::OpenPopup("##VFXCanvasAddNode");
    }
    if (ImGui::BeginPopup("##VFXCanvasAddNode")) {
        ImGui::TextDisabled("Add Effect Node");
        ImGui::Separator();
        DrawAddNodeMenu();
        ImGui::EndPopup();
    } else {
        m_pendingNodeSpawnValid = false;
    }
    if (ImGui::BeginPopup("##VFXNodeContext")) {
        const auto* contextNode = FindGraphNode(m_graph, m_graphContextMenuNodeId);
        if (contextNode != nullptr) {
            ImGui::TextDisabled("%s", contextNode->name.c_str());
            ImGui::Separator();
            if (contextNode->type != asset::VFXNodeType::Entry) {
                if (ImGui::MenuItem("Duplicate", "Ctrl+D")) DuplicateGraphNode(m_graphContextMenuNodeId);
                if (ImGui::MenuItem("Delete", "Del")) {
                    m_selectedGraphNodeId = m_graphContextMenuNodeId;
                    DeleteSelectedGraphNode();
                }
            } else {
                ImGui::TextDisabled("Entry node can't be duplicated or deleted");
            }
        }
        ImGui::EndPopup();
    }

    if (!ImGui::GetIO().WantTextInput && ImGui::Shortcut(ImGuiKey_F))
        m_focusSelectionRequested = true;
    if (!ImGui::GetIO().WantTextInput
        && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_D) && m_selectedGraphNodeId > 0)
        DuplicateGraphNode(m_selectedGraphNodeId);
    if (m_focusSelectionRequested && m_selectedGraphNodeId > 0) {
        ImNodes::EditorContextMoveToNode(m_selectedGraphNodeId);
        m_focusSelectionRequested = false;
    }

    if (!ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        ImNodes::ClearNodeSelection();
        ImNodes::ClearLinkSelection();
        m_selectedGraphNodeId = -1;
        m_selectedGraphLinkIndex = -1;
    }

    for (auto& node : m_graph.nodes) {
        if (ImNodes::IsNodeSelected(node.id)) {
            m_selectedGraphNodeId = node.id;
            m_selectedGraphLinkIndex = -1;
        }
        const ImVec2 position = ImNodes::GetNodeGridSpacePos(node.id);
        if (std::fabs(position.x - node.editorX) > 0.1f || std::fabs(position.y - node.editorY) > 0.1f) {
            if (!m_graphEditInProgress) {
                PushGraphUndo(graphBeforeCanvasEdit);
                m_graphEditInProgress = true;
            }
            node.editorX = position.x;
            node.editorY = position.y;
            m_graphDirty = true;
        }
    }

    int startAttribute = 0;
    int endAttribute = 0;
    if (ImNodes::IsLinkCreated(&startAttribute, &endAttribute)) {
        int fromNode = -1;
        int toNode = -1;
        for (const auto& node : m_graph.nodes) {
            if (OutputPinId(node.id) == startAttribute) fromNode = node.id;
            if (InputPinId(node.id) == endAttribute) toNode = node.id;
            if (OutputPinId(node.id) == endAttribute) fromNode = node.id;
            if (InputPinId(node.id) == startAttribute) toNode = node.id;
        }
        const auto* target = FindGraphNode(m_graph, toNode);
        if (fromNode > 0 && toNode > 0 && target != nullptr
            && target->type != asset::VFXNodeType::Entry) {
            m_graph.links.push_back({ fromNode, toNode });
            std::string error;
            std::vector<float> starts;
            float duration = 0.0f;
            const bool duplicate = std::count_if(m_graph.links.begin(), m_graph.links.end(),
                [fromNode, toNode](const asset::VFXGraphLink& link) {
                    return link.fromNode == fromNode && link.toNode == toNode;
                }) > 1;
            if (!duplicate && asset::BuildVFXGraphSchedule(m_graph, starts, duration, &error)) {
                // push_back後に検証するため、追加前の状態を明示的に履歴へ積む。
                asset::VFXGraphAsset before = m_graph;
                before.links.pop_back();
                PushGraphUndo(before);
                m_graphDirty = true;
                m_graphError.clear();
                m_selectedGraphLinkIndex = static_cast<int>(m_graph.links.size()) - 1;
                m_selectedGraphNodeId = -1;
            } else {
                m_graph.links.pop_back();
                m_graphError = duplicate ? "VFXリンクが重複しています" : error;
            }
        }
    }
    int destroyedLinkId = 0;
    if (ImNodes::IsLinkDestroyed(&destroyedLinkId)) {
        const int destroyedIndex = destroyedLinkId - LinkId(0);
        if (destroyedIndex >= 0 && destroyedIndex < static_cast<int>(m_graph.links.size())) {
            PushGraphUndo();
            m_graph.links.erase(m_graph.links.begin() + destroyedIndex);
            m_selectedGraphLinkIndex = -1;
            m_graphDirty = true;
        }
    }
    const bool deletePressed = !ImGui::GetIO().WantTextInput
        && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
        && ImGui::IsKeyPressed(ImGuiKey_Delete, false);
    for (std::size_t i = 0; i < m_graph.links.size(); ++i) {
        if (ImNodes::IsLinkSelected(LinkId(static_cast<int>(i)))) {
            m_selectedGraphLinkIndex = static_cast<int>(i);
            m_selectedGraphNodeId = -1;
        }
        if (ImNodes::IsLinkSelected(LinkId(static_cast<int>(i))) && deletePressed) {
            PushGraphUndo();
            m_graph.links.erase(m_graph.links.begin() + static_cast<std::ptrdiff_t>(i));
            m_selectedGraphLinkIndex = -1;
            m_graphDirty = true;
            break;
        }
    }
    if (m_selectedGraphNodeId > 0 && deletePressed)
        DeleteSelectedGraphNode();
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) m_graphEditInProgress = false;
}

void VFXEditorPanel::DrawGraphParameters()
{
    if (!ImGui::CollapsingHeader("Exposed Parameters", ImGuiTreeNodeFlags_DefaultOpen)) return;
    if (ImGui::Button("+ Parameter", { -1.0f, 0.0f })) {
        PushGraphUndo();
        std::string name = "Parameter";
        int suffix = 1;
        const auto exists = [&](const std::string& candidate) {
            return std::any_of(m_graph.parameters.begin(), m_graph.parameters.end(),
                [&](const asset::VFXParamDefinition& item) { return item.name == candidate; });
        };
        while (exists(name)) name = "Parameter" + std::to_string(++suffix);
        m_graph.parameters.push_back({ name, asset::VFXParamType::Float,
                                       DefaultVFXParamValue(asset::VFXParamType::Float) });
        m_graphDirty = true;
    }

    std::vector<std::string> schemaPaths;
    CollectExposablePaths(asset::GetVFXNodeSchema(), {}, schemaPaths);
    int deleteParameter = -1;
    for (std::size_t index = 0; index < m_graph.parameters.size(); ++index) {
        auto& parameter = m_graph.parameters[index];
        ImGui::PushID(static_cast<int>(index));
        const std::string header = parameter.name + "  [" + VFXParamTypeName(parameter.type) + "]";
        if (ImGui::TreeNodeEx("##Parameter", ImGuiTreeNodeFlags_DefaultOpen, "%s", header.c_str())) {
            const std::string oldName = parameter.name;
            if (InputString("Name", parameter.name, 128)) {
                for (auto& binding : m_graph.bindings)
                    if (binding.paramName == oldName) binding.paramName = parameter.name;
                for (auto& variant : m_graph.variants)
                    for (auto& item : variant.overrides)
                        if (item.paramName == oldName) item.paramName = parameter.name;
                m_graphDirty = true;
            }
            int type = static_cast<int>(parameter.type);
            constexpr const char* types[] = { "Float", "Int", "Bool", "Color", "Vector3", "Asset" };
            if (ImGui::Combo("Type", &type, types, 6)) {
                parameter.type = static_cast<asset::VFXParamType>(type);
                parameter.defaultValue = DefaultVFXParamValue(parameter.type);
                m_graphDirty = true;
            }
            if (DrawVFXParamValue("Default", parameter.type, parameter.defaultValue,
                                  parameter.minimum, parameter.maximum, parameter.hasRange))
                m_graphDirty = true;
            if (parameter.type == asset::VFXParamType::Float
                || parameter.type == asset::VFXParamType::Int) {
                if (ImGui::Checkbox("Inspector Range", &parameter.hasRange)) m_graphDirty = true;
                if (parameter.hasRange) {
                    m_graphDirty |= ImGui::DragFloat("Minimum", &parameter.minimum, 0.01f);
                    m_graphDirty |= ImGui::DragFloat("Maximum", &parameter.maximum, 0.01f);
                }
            }
            ImGui::SeparatorText("Bindings");
            for (std::size_t bindingIndex = 0; bindingIndex < m_graph.bindings.size();) {
                auto& binding = m_graph.bindings[bindingIndex];
                if (binding.paramName != parameter.name) { ++bindingIndex; continue; }
                ImGui::PushID(static_cast<int>(bindingIndex));
                ImGui::TextWrapped("Node %d  %s", binding.nodeId, binding.schemaPath.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) {
                    m_graph.bindings.erase(m_graph.bindings.begin()
                        + static_cast<std::ptrdiff_t>(bindingIndex));
                    m_graphDirty = true;
                    ImGui::PopID();
                    continue;
                }
                ImGui::PopID();
                ++bindingIndex;
            }
            ImGui::BeginDisabled(m_selectedGraphNodeId <= 0);
            if (ImGui::BeginCombo("Bind Selected Node", "Choose field...")) {
                const auto* selectedNode = FindGraphNode(m_graph, m_selectedGraphNodeId);
                for (const std::string& path : schemaPaths) {
                    reflection::ResolvedProperty resolved;
                    if (selectedNode == nullptr || !IsPathForVFXNode(selectedNode->type, path)
                        || !reflection::ResolveProperty(asset::GetVFXNodeSchema(), selectedNode,
                                                        path, resolved)
                        || resolved.property == nullptr
                        || !IsVFXParamCompatible(parameter.type, resolved.property->type)) continue;
                    if (ImGui::Selectable(path.c_str())) {
                        m_graph.bindings.push_back({ parameter.name, m_selectedGraphNodeId, path });
                        m_graphDirty = true;
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
        PushGraphUndo();
        const std::string name = m_graph.parameters[static_cast<std::size_t>(deleteParameter)].name;
        m_graph.parameters.erase(m_graph.parameters.begin() + deleteParameter);
        std::erase_if(m_graph.bindings, [&](const asset::VFXParamBinding& binding) {
            return binding.paramName == name;
        });
        for (auto& variant : m_graph.variants)
            std::erase_if(variant.overrides, [&](const asset::VFXParamOverride& item) {
                return item.paramName == name;
            });
        m_graphDirty = true;
    }

    ImGui::SeparatorText("Variant Sets");
    if (ImGui::Button("+ Variant")) {
        PushGraphUndo();
        m_graph.variants.push_back({ "Variant " + std::to_string(m_graph.variants.size() + 1), {} });
        m_graphDirty = true;
    }
    int deleteVariant = -1;
    for (std::size_t index = 0; index < m_graph.variants.size(); ++index) {
        auto& variant = m_graph.variants[index];
        ImGui::PushID(static_cast<int>(index));
        if (ImGui::TreeNodeEx("##Variant", ImGuiTreeNodeFlags_DefaultOpen, "%s", variant.name.c_str())) {
            m_graphDirty |= InputString("Name", variant.name, 128);
            if (ImGui::Button("Populate all defaults", { -1.0f, 0.0f })) {
                variant.overrides.clear();
                for (const auto& parameter : m_graph.parameters)
                    variant.overrides.push_back({ parameter.name, parameter.defaultValue });
                m_graphDirty = true;
            }
            for (auto& item : variant.overrides) {
                const auto definition = std::find_if(m_graph.parameters.begin(), m_graph.parameters.end(),
                    [&](const auto& parameter) { return parameter.name == item.paramName; });
                if (definition == m_graph.parameters.end()) continue;
                ImGui::PushID(item.paramName.c_str());
                ImGui::TextUnformatted(item.paramName.c_str());
                m_graphDirty |= DrawVFXParamValue("Value", definition->type, item.value,
                    definition->minimum, definition->maximum, definition->hasRange);
                ImGui::PopID();
            }
            if (ImGui::Button("Delete Variant", { -1.0f, 0.0f })) deleteVariant = static_cast<int>(index);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (deleteVariant >= 0) {
        m_graph.variants.erase(m_graph.variants.begin() + deleteVariant);
        m_graphDirty = true;
    }

    ImGui::SeparatorText("Sub-graph Parameter Forwarding");
    if (ImGui::Button("+ Forward selected Sub Graph", { -1.0f, 0.0f })) {
        const auto* selected = FindGraphNode(m_graph, m_selectedGraphNodeId);
        if (selected != nullptr && selected->type == asset::VFXNodeType::SubGraph) {
            m_graph.subGraphForwards.push_back({ selected->id, {}, {} });
            m_graphDirty = true;
        } else m_graphError = "Sub Graphノードを選択してください";
    }
    for (std::size_t index = 0; index < m_graph.subGraphForwards.size();) {
        auto& forward = m_graph.subGraphForwards[index];
        ImGui::PushID(static_cast<int>(index));
        ImGui::Text("Node %d", forward.nodeId);
        m_graphDirty |= InputString("Parent Parameter", forward.parentParam, 128);
        m_graphDirty |= InputString("Child Parameter", forward.childParam, 128);
        if (ImGui::Button("Remove Forward", { -1.0f, 0.0f })) {
            m_graph.subGraphForwards.erase(m_graph.subGraphForwards.begin() + static_cast<std::ptrdiff_t>(index));
            m_graphDirty = true;
            ImGui::PopID();
            continue;
        }
        ImGui::PopID();
        ++index;
    }

    ImGui::SeparatorText("Signal Graph");
    if (ImGui::Button("+ Signal Node")) {
        int nextId = 1;
        for (const auto& signal : m_graph.signalNodes) nextId = (std::max)(nextId, signal.id + 1);
        m_graph.signalNodes.push_back({ .id = nextId });
        m_graphDirty = true;
    }
    constexpr const char* SIGNAL_OPERATIONS[] = {
        "Constant", "Time", "Sine", "Noise", "Add", "Subtract", "Multiply", "Divide", "Remap"
    };
    for (std::size_t signalIndex = 0; signalIndex < m_graph.signalNodes.size();) {
        auto& signal = m_graph.signalNodes[signalIndex];
        ImGui::PushID(signal.id);
        ImGui::Text("Signal Node %d", signal.id);
        int operation = static_cast<int>(signal.operation);
        if (ImGui::Combo("Operation", &operation, SIGNAL_OPERATIONS, 9)) {
            signal.operation = static_cast<asset::VFXSignalOperation>(operation);
            m_graphDirty = true;
        }
        m_graphDirty |= ImGui::DragInt("Input A", &signal.inputA, 1.0f, -1, 100000);
        m_graphDirty |= ImGui::DragInt("Input B", &signal.inputB, 1.0f, -1, 100000);
        m_graphDirty |= ImGui::DragFloat("Value A", &signal.valueA, 0.01f);
        m_graphDirty |= ImGui::DragFloat("Value B", &signal.valueB, 0.01f);
        if (ImGui::Button("Delete Signal Node", { -1.0f, 0.0f })) {
            const int removedId = signal.id;
            m_graph.signalNodes.erase(m_graph.signalNodes.begin() + static_cast<std::ptrdiff_t>(signalIndex));
            for (auto& dependent : m_graph.signalNodes) {
                if (dependent.inputA == removedId) dependent.inputA = -1;
                if (dependent.inputB == removedId) dependent.inputB = -1;
            }
            std::erase_if(m_graph.signalOutputs,
                [removedId](const auto& output) { return output.nodeId == removedId; });
            m_graphDirty = true;
            ImGui::PopID();
            continue;
        }
        ImGui::Separator();
        ImGui::PopID();
        ++signalIndex;
    }
    if (ImGui::Button("+ Signal Output")) {
        const int nodeId = m_graph.signalNodes.empty() ? 0 : m_graph.signalNodes.back().id;
        m_graph.signalOutputs.push_back({ "Signal " + std::to_string(m_graph.signalOutputs.size() + 1), nodeId });
        m_graphDirty = true;
    }
    for (std::size_t outputIndex = 0; outputIndex < m_graph.signalOutputs.size();) {
        auto& output = m_graph.signalOutputs[outputIndex];
        ImGui::PushID(static_cast<int>(outputIndex));
        m_graphDirty |= InputString("Output", output.name, 128);
        m_graphDirty |= ImGui::DragInt("Node", &output.nodeId, 1.0f, 0, 100000);
        if (ImGui::Button("Delete Output", { -1.0f, 0.0f })) {
            m_graph.signalOutputs.erase(m_graph.signalOutputs.begin() + static_cast<std::ptrdiff_t>(outputIndex));
            m_graphDirty = true;
            ImGui::PopID();
            continue;
        }
        ImGui::PopID();
        ++outputIndex;
    }
}

void VFXEditorPanel::DrawGraphInspector(EditorContext& ctx)
{
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) m_graphEditInProgress = false;
    if (!m_graphEditInProgress
        && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)
        && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        // クリック開始時のasset全体を保持すると、Drag・ColorPicker・文字入力を1操作として戻せる。
        PushGraphUndo();
        m_graphEditInProgress = true;
    }
    ImGui::TextUnformatted("Graph Inspector");
    ImGui::Separator();
    if (InputString("Name", m_graph.name, 128)) m_graphDirty = true;
    ImGui::TextDisabled("Nodes %d   Links %d", static_cast<int>(m_graph.nodes.size()),
                        static_cast<int>(m_graph.links.size()));
    const asset::VFXGraphBudgetStats budget = asset::CalculateVFXGraphBudget(m_graph);
    bool budgetChanged = false;
    budgetChanged |= ImGui::DragInt("Particle Budget", &m_graph.maxParticles, 100, 1, 10000000);
    budgetChanged |= ImGui::DragInt("Light Budget", &m_graph.maxLights, 1, 0, 1024);
    budgetChanged |= ImGui::DragInt("Audio Budget", &m_graph.maxAudioVoices, 1, 0, 1024);
    if (budgetChanged) m_graphDirty = true;
    const bool overBudget = budget.particles > m_graph.maxParticles
        || budget.lights > m_graph.maxLights || budget.audioVoices > m_graph.maxAudioVoices;
    ImGui::TextColored(overBudget ? ImVec4{ 1.0f, 0.35f, 0.2f, 1.0f }
                                  : ImVec4{ 0.45f, 0.85f, 0.55f, 1.0f },
                       "Cost  P %d/%d   L %d/%d   A %d/%d",
                       budget.particles, m_graph.maxParticles,
                       budget.lights, m_graph.maxLights,
                       budget.audioVoices, m_graph.maxAudioVoices);
    const auto drawBudget = [](const char* label, int used, int limit) {
        const float ratio = limit > 0 ? std::clamp(static_cast<float>(used) / limit, 0.0f, 1.0f)
                                      : (used > 0 ? 1.0f : 0.0f);
        char overlay[64];
        std::snprintf(overlay, sizeof(overlay), "%s  %d / %d", label, used, limit);
        ImGui::ProgressBar(ratio, { -1.0f, 0.0f }, overlay);
    };
    drawBudget("Particles", budget.particles, m_graph.maxParticles);
    drawBudget("Lights", budget.lights, m_graph.maxLights);
    drawBudget("Audio", budget.audioVoices, m_graph.maxAudioVoices);
    DrawGraphParameters();
    ImGui::Separator();
    if (m_selectedGraphLinkIndex >= 0
        && m_selectedGraphLinkIndex < static_cast<int>(m_graph.links.size())) {
        auto& link = m_graph.links[static_cast<std::size_t>(m_selectedGraphLinkIndex)];
        const auto* source = FindGraphNode(m_graph, link.fromNode);
        const auto* target = FindGraphNode(m_graph, link.toNode);
        ImGui::TextUnformatted("Event Link");
        ImGui::TextWrapped("%s  ->  %s", source != nullptr ? source->name.c_str() : "Missing source",
                          target != nullptr ? target->name.c_str() : "Missing target");
        const char* triggerItems[] = { "On Complete", "On Start", "On Collision", "On Death" };
        int trigger = static_cast<int>(link.trigger);
        bool linkChanged = false;
        if (ImGui::Combo("Trigger", &trigger, triggerItems, 4)) {
            link.trigger = static_cast<asset::VFXLinkTrigger>(trigger);
            linkChanged = true;
        }
        linkChanged |= ImGui::DragFloat("Event Delay", &link.delay, 0.01f, 0.0f, 3600.0f, "%.2fs");
        if (link.trigger == asset::VFXLinkTrigger::OnCollision)
            ImGui::TextWrapped("Source Particle collision starts the target once per graph cycle.");
        if (link.trigger == asset::VFXLinkTrigger::OnDeath)
            ImGui::TextWrapped("The first source Particle death starts the target once per graph cycle.");
        if (linkChanged) m_graphDirty = true;
        ImGui::Separator();
        if (ImGui::Button("Delete Link", { -1.0f, 0.0f })) {
            PushGraphUndo();
            m_graph.links.erase(m_graph.links.begin() + m_selectedGraphLinkIndex);
            m_selectedGraphLinkIndex = -1;
            m_graphDirty = true;
        }
        return;
    }
    auto* node = FindGraphNode(m_graph, m_selectedGraphNodeId);
    if (node == nullptr) {
        ImGui::Spacing();
        ImGui::TextDisabled("Nothing selected");
        ImGui::TextWrapped("Select a node or link to edit it. Right-click the canvas to add an effect.");
        ImGui::Spacing();
        ImGui::TextDisabled("F  Frame selection");
        ImGui::TextDisabled("Del  Delete selection");
        ImGui::TextDisabled("Esc  Clear selection");
        return;
    }
    bool changed = false;
    ImGui::Text("%s #%d", asset::VFXNodeTypeName(node->type), node->id);
    changed |= InputString("Node Name", node->name, 128);
    if (node->type != asset::VFXNodeType::Entry) {
        changed |= ImGui::DragFloat("Start Offset", &node->startOffset, 0.01f, 0.0f, 3600.0f, "%.2fs");
        if (node->type != asset::VFXNodeType::Particle)
            changed |= ImGui::DragFloat("Duration", &node->duration, 0.01f, 0.0f, 3600.0f, "%.2fs");
    }
    if (node->type != asset::VFXNodeType::Entry && node->type != asset::VFXNodeType::Delay) {
        changed |= ImGui::DragFloat3("Position", &node->localPosition.x, 0.01f);
        changed |= ImGui::DragFloat3("Rotation", &node->localRotationDegrees.x, 0.25f);
        changed |= ImGui::DragFloat3("Scale", &node->localScale.x, 0.01f, 0.001f, 1000.0f);
        changed |= InputString("Bone / Socket", node->attachBone, 128);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Optional BoneComponent name below the VFX owner.");
    }
    ImGui::Separator();
    if (node->type == asset::VFXNodeType::Particle) {
        // Scene Inspectorと同じUnity風モジュールスタックを使い、Graph側でも全機能を編集する。
        ImGui::TextDisabled("Quick Asset Drop");
        changed |= AssetPathField("Material##VFXQuickParticle", node->particle.materialPath);
        changed |= AssetPathField("Texture##VFXQuickParticle", node->particle.texturePath);
        changed |= AssetPathField("Mesh Shape##VFXQuickParticle", node->particle.meshShapePath);
        ImGui::Separator();
        EditorContext assetContext = ctx;
        assetContext.markSceneDirty = [this]() { m_graphDirty = true; };
        if (DrawParticleEmitterModules(node->particle, assetContext)) {
            node->duration = node->particle.duration;
            changed = true;
        }
    } else if (node->type == asset::VFXNodeType::Trail
               || node->type == asset::VFXNodeType::MeshTrail) {
        if (node->type == asset::VFXNodeType::MeshTrail)
            changed |= AssetPathField("Mesh", node->trail.meshPath);
        changed |= AssetPathField("Material", node->trail.materialPath);
        changed |= AssetPathField("Texture", node->trail.texturePath);
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
    } else if (node->type == asset::VFXNodeType::Audio) {
        changed |= AssetPathField("Clip", node->audio.clipPath);
        changed |= ImGui::SliderFloat("Volume", &node->audio.volume, 0.0f, 1.0f);
        changed |= ImGui::DragFloat("Pitch", &node->audio.pitch, 0.01f, 0.01f, 4.0f);
        changed |= ImGui::SliderFloat("Spatial Blend", &node->audio.spatialBlend, 0.0f, 1.0f);
        changed |= ImGui::Checkbox("Loop Audio", &node->audio.loop);
    } else if (node->type == asset::VFXNodeType::Decal) {
        changed |= AssetPathField("Albedo", node->decal.albedoPath);
        changed |= AssetPathField("Normal", node->decal.normalPath);
        changed |= AssetPathField("Emissive", node->decal.emissivePath);
        changed |= ImGui::ColorEdit4("Color", &node->decal.color.x);
        changed |= ImGui::DragFloat("Normal Strength", &node->decal.normalStrength, 0.01f, 0.0f, 8.0f);
        changed |= ImGui::DragFloat("Emissive Scale", &node->decal.emissiveScale, 0.01f, 0.0f, 10000.0f);
        changed |= ImGui::DragFloat("Fade Time", &node->decal.fadeTime, 0.01f, 0.0f, 3600.0f);
    } else if (node->type == asset::VFXNodeType::SubGraph) {
        changed |= AssetPathField("Graph (.vfx)", node->subGraph.graphPath);
        ImGui::TextWrapped("Nested graph instances inherit play, pause, speed and editor preview state.");
    }
    if (changed) m_graphDirty = true;
    if (node->type != asset::VFXNodeType::Entry) {
        ImGui::Separator();
        if (ImGui::Button("Delete Node", { -1.0f, 0.0f })) DeleteSelectedGraphNode();
    }
}

} // namespace fbzz::editor
