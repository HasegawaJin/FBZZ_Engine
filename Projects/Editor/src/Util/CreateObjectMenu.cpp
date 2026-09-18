/// @file    CreateObjectMenu.cpp
/// @brief   Create メニューの描画。
/// @author  Hasegawa Jin
/// @date    2026-09-16
#include <Editor/Util/CreateObjectMenu.hpp>

#include <Editor/EditorContext.hpp>
#include <Editor/Util/EditorTheme.hpp>
#include <Editor/Util/ObjectCreation.hpp>
#include <Editor/Util/ObjectPresets.hpp>
#include <Editor/Util/ScriptObjectFactory.hpp>
#include <Editor/Util/Selection.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>

#include <imgui.h>

#include <string_view>
#include <utility>
#include <vector>

namespace fbzz::editor {

namespace {

/// @brief メニュー経由の生成は Scene View の注視点へ置く。
OpArgs MakeCreateMenuArgs(const std::string& parentGuid)
{
    OpArgs args;
    if (!parentGuid.empty()) args.Set("parent", parentGuid);
    args.Set("placeInView", true);
    return args;
}

/// @brief 実行可否を Operator の poll から引いて 1 項目描き、押されたら out へ積む。
void DrawCreateMenuEntry(EditorContext& ctx, const char* label, const char* tooltip,
                         const char* operatorId, OpArgs args, PendingObjectCreate& out)
{
    const bool enabled = CanInvokeOperator(ctx, operatorId, args);
    if (ImGui::MenuItem(label, nullptr, false, enabled)) {
        out.operatorId  = operatorId;
        out.args        = std::move(args);
        out.beginRename = true;
    }
    if (tooltip != nullptr && tooltip[0] != '\0'
        && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tooltip);
}

scene::GameObject* DefaultCreateMenuParent(EditorContext& ctx)
{
    if (ctx.activeScene == nullptr || ctx.selectedEntities.empty()) return nullptr;
    /// @note 複数選択なら最後に選んだもの (Unity の active object 相当)。削除直後の古い選択はルート扱い。
    return ctx.activeScene->GetGameObject(ctx.selectedEntities.back());
}

void DrawCreateMenuPresets(EditorContext& ctx, const std::string& parentGuid, PendingObjectCreate& out)
{
    std::string_view openCategory;
    bool             categoryOpen = false;
    const auto closeCategory = [&]() {
        if (categoryOpen) ImGui::EndMenu();
        categoryOpen = false;
        openCategory = {};
    };

    for (const ObjectPreset& preset : ObjectPresetCatalog()) {
        if (preset.category != openCategory) {
            closeCategory();
            openCategory = preset.category;
            categoryOpen = !preset.category.empty()
                && ImGui::BeginMenu(std::string(preset.category).c_str());
        }
        if (!preset.category.empty() && !categoryOpen) continue;

        OpArgs args = MakeCreateMenuArgs(parentGuid);
        args.Set("preset", std::string(preset.id));
        DrawCreateMenuEntry(ctx, std::string(preset.label).c_str(), std::string(preset.description).c_str(),
                            "node.create_preset", std::move(args), out);
    }
    closeCategory();
}

void DrawCreateMenuPrefabs(EditorContext& ctx, const std::string& parentGuid, PendingObjectCreate& out)
{
    if (!ImGui::BeginMenu("Prefab")) return;

    const std::string assetRoot = ctx.projectRoot.empty() ? "Assets" : ctx.projectRoot + "/Assets";
    bool anyFound = false;
    for (const std::string& path : util::FileSystem::ListAll(assetRoot + "/Prefabs")) {
        const std::string ext = util::StringUtils::ToLower(util::FileSystem::GetExtension(path));
        if (ext != ".prefab") continue;
        anyFound = true;
        OpArgs args = MakeCreateMenuArgs(parentGuid);
        args.Set("path", path);
        DrawCreateMenuEntry(ctx, util::FileSystem::GetFilename(path).c_str(), nullptr,
                            "prefab.instantiate", std::move(args), out);
    }
    if (!anyFound) ImGui::TextDisabled("(No prefabs found in Assets/Prefabs)");
    ImGui::EndMenu();
}

void DrawCreateMenuScripts(EditorContext& ctx, const std::string& parentGuid, PendingObjectCreate& out)
{
    const bool open = ImGui::BeginMenu("Script Object");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Create a GameObject with the script and every component\n"
                          "it declares via FBZZ_REQUIRE_COMPONENT.");
    }
    if (!open) return;

    const std::vector<std::string> types = ScriptObjectTypeNames();
    /// @note DLL 未ロード・ビルド失敗中は空になる。理由を書かないとメニューが壊れて見える。
    if (types.empty()) ImGui::TextDisabled("(No scripts registered - is the script DLL built?)");
    for (const std::string& typeName : types) {
        OpArgs args = MakeCreateMenuArgs(parentGuid);
        args.Set("script", typeName);
        DrawCreateMenuEntry(ctx, typeName.c_str(), nullptr, "node.create_script_object", std::move(args), out);
    }
    ImGui::EndMenu();
}

} // namespace

void DrawCreateObjectMenuItems(EditorContext& ctx, const std::string& parentGuid, PendingObjectCreate& out)
{
    const scene::GameObject* parent = (ctx.activeScene != nullptr && !parentGuid.empty())
        ? ctx.activeScene->FindByGuid(parentGuid) : nullptr;
    if (IsInsidePrefabInstance(parent)) {
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(ThemeColor::Warning));
        ImGui::TextUnformatted("Adding under a prefab instance");
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", PrefabInstanceChildWarning());
        ImGui::Separator();
    }

    DrawCreateMenuPresets(ctx, parentGuid, out);
    ImGui::Separator();
    DrawCreateMenuPrefabs(ctx, parentGuid, out);
    DrawCreateMenuScripts(ctx, parentGuid, out);
}

void DrawCreateObjectMenu(EditorContext& ctx, PendingObjectCreate& out)
{
    const scene::GameObject* parent = DefaultCreateMenuParent(ctx);
    const std::string parentGuid = parent != nullptr ? parent->instanceId : std::string{};
    if (parent != nullptr) {
        ImGui::TextDisabled("Create under \"%s\"", parent->name.c_str());
        ImGui::Separator();
    }

    DrawCreateObjectMenuItems(ctx, parentGuid, out);

    if (parent != nullptr) {
        ImGui::Separator();
        if (ImGui::BeginMenu("Create at Root")) {
            DrawCreateObjectMenuItems(ctx, {}, out);
            ImGui::EndMenu();
        }
    }
}

OpResult InvokePendingObjectCreate(EditorContext& ctx, PendingObjectCreate& pending)
{
    if (!pending.IsSet()) {
        OpResult none;
        none.noChange = true;
        return none;
    }
    PendingObjectCreate taken = std::move(pending);
    pending = {};
    OpResult result = InvokeOperator(ctx, taken.operatorId, taken.args);
    if (!taken.moreArgs.empty()) {
        /// @note Operator は呼ぶたびに作ったものだけを選ぶので、全部作ってから選択を束ね直す。
        std::vector<scene::EntityID> created;
        const auto collect = [&ctx, &created](const OpResult& r) {
            const OpData* ids = r.ok ? r.data.Find("ids") : nullptr;
            if (ids == nullptr || !ctx.activeScene) return;
            for (const OpData& guid : ids->AsArray())
                if (const scene::GameObject* go = ctx.activeScene->FindByGuid(guid.AsString()))
                    created.push_back(go->GetID());
        };
        collect(result);
        for (const OpArgs& args : taken.moreArgs) {
            OpResult more = InvokeOperator(ctx, taken.operatorId, args);
            collect(more);
            if (result.ok || !more.ok) result = std::move(more);
        }
        if (!created.empty()) SelectEntities(ctx, std::move(created), SelectionReveal::Skip);
        return result;
    }
    /// @note Map Mode の絞り込み中は作った行が出ないことがあり、出ない行のリネームはキー操作を塞いだまま残る。
    const bool rowMayBeHidden = ctx.mapEditingMode && ctx.mapHierarchyFilter;
    if (result.ok && taken.beginRename && !rowMayBeHidden) {
        const OpData* ids = result.data.Find("ids");
        if (ids != nullptr && ids->AsArray().size() == 1) ctx.requestRenameSelected = true;
    }
    return result;
}

} // namespace fbzz::editor
