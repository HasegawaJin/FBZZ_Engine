// FBZZ Engine
// InspectorUI.cpp | fbzz::editor
// RegistryとReflect定義から標準コンポーネントInspectorを自動生成する
#include "InspectorUI.hpp"

namespace fbzz::editor {

// RegistryでAutomatic指定された全コンポーネントを共通Reflectorへ接続する。
// WHY: 新しい単純コンポーネントは専用Inspector関数を追加せず、自動的にここへ入る。
void DrawAutomaticInspectors(scene::ComponentCategory category,
                             scene::GameObject* go,
                             EditorContext& ctx,
                             std::any& componentClipboard,
                             const std::type_info*& componentClipboardType)
{
    scene::ForEachRegisteredComponent([&]<typename T, typename Registration>() {
        if constexpr (Registration::inspectorMode == scene::ComponentInspectorMode::Automatic) {
            if (Registration::category == category) {
                DrawReflectedComponentSection<T>(
                    go, ctx, componentClipboard, componentClipboardType, Registration::displayName);
            }
        }
    });
}

} // namespace fbzz::editor
