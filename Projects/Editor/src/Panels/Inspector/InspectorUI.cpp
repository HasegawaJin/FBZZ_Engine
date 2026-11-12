/// @file    InspectorUI.cpp
/// @brief   RegistryとReflect定義から標準コンポーネントInspectorを自動生成する。
/// @author  Hasegawa Jin
/// @date    2026-06-07
#include <Engine/Scene/ComponentRegistry.hpp>
#include <Engine/Scene/Components/WaterfallComponent.hpp>
#include <Engine/Scene/Components/WaterZoneComponent.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Util/WaterfallFluidBake.hpp>
#include "InspectorUI.hpp"

namespace fbzz::editor {

void BindWaterObjectReferenceFields(ComponentImGuiReflector& reflector, scene::Scene* world)
{
    if (world) {
        reflector.m_goNameResolver = [world](scene::EntityID id) -> std::string {
            const auto* object = world->GetGameObject(id);
            return object ? object->name : "(Missing)";
        };
        reflector.m_goGuidResolver = [world](scene::EntityID id) -> std::string {
            const auto* object = world->GetGameObject(id);
            return object ? object->instanceId : std::string{};
        };
        reflector.m_goListProvider = [world] {
            std::vector<std::pair<scene::EntityID, std::string>> list;
            for (auto& object : world->GameObjects()) list.emplace_back(object.GetID(), object.name);
            return list;
        };
        reflector.m_refTypeValidator = [world](scene::EntityID id, const char* type) {
            auto* object = world->GetGameObject(id);
            return object && (!type || !type[0] || HasRegisteredComponentByName(*object, type));
        };
    }
}

namespace {
void DrawWaterfallSettings(scene::WaterfallComponent& waterfall, scene::GameObject& owner, EditorContext& ctx)
{
    ComponentImGuiReflector reflector;
    reflector.m_projectRoot = ctx.projectRoot;
    BindWaterObjectReferenceFields(reflector, ctx.activeScene);
    waterfall.Reflect(reflector);
    OpArgs args;
    args.Set("node", owner.instanceId);
    ImGui::BeginDisabled(!CanInvokeOperator(ctx, "waterfall.fluid.bake", args));
    if (ImGui::Button("Bake 3D Fluid")) InvokeOperator(ctx, "waterfall.fluid.bake", args);
    ImGui::EndDisabled();
    if (CanInvokeOperator(ctx, "waterfall.fluid.cancel", args)) {
        ImGui::SameLine();
        if (ImGui::Button("Cancel Bake")) InvokeOperator(ctx, "waterfall.fluid.cancel", args);
    }
    const auto status = WaterfallFluidBakeStatus(owner.instanceId);
    if (!status.empty()) ImGui::TextWrapped("%s", status.c_str());
}
} /// @note namespace

/// @brief RegistryでAutomatic指定された全コンポーネントを共通Reflectorへ接続する。
/// @note 新しい単純コンポーネントは専用Inspector関数を追加せず、自動的にここへ入る。
void DrawAutomaticInspectors(scene::ComponentCategory category,
                             scene::GameObject* go,
                             EditorContext& ctx,
                             std::any& componentClipboard,
                             const std::type_info*& componentClipboardType)
{
    scene::ForEachRegisteredComponent([&]<typename T, typename Registration>() {
        if constexpr (Registration::inspectorMode == scene::ComponentInspectorMode::Automatic) {
            if (Registration::category == category) {
                if constexpr (std::is_same_v<T, scene::WaterfallComponent>) {
                    DrawComponentSection<T>(go, ctx, componentClipboard, componentClipboardType, Registration::displayName,
                        [go](T& waterfall, EditorContext& context) { DrawWaterfallSettings(waterfall, *go, context); });
                } else if constexpr (std::is_same_v<T, scene::WaterZoneComponent>) {
                    DrawComponentSection<T>(go, ctx, componentClipboard, componentClipboardType, Registration::displayName,
                        [](T& zone, EditorContext& context) {
                            ComponentImGuiReflector reflector;
                            BindWaterObjectReferenceFields(reflector, context.activeScene);
                            zone.Reflect(reflector);
                        });
                } else {
                    DrawReflectedComponentSection<T>(
                        go, ctx, componentClipboard, componentClipboardType, Registration::displayName);
                }
            }
        }
    });
}

} /// @note namespace fbzz::editor
