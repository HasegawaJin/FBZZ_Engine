/// @file    InspectorPanel.hpp
/// @brief   選択 Entity のコンポーネントを表示・編集する。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#pragma once

#include <Editor/Panels/IPanel.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>
#include <Engine/Scene/Entity.hpp>
#include <any>
#include <string>
#include <typeinfo>

namespace fbzz::editor {

class InspectorPanel : public IPanel {
public:
    const char* GetWindowName() const override { return "Inspector"; }
    void OnShutdown() override;

protected:
    void OnRenderContent(EditorContext& ctx) override;

private:
    std::any              m_componentClipboard;
    const std::type_info* m_componentClipboardType = nullptr;
    char                  m_addComponentFilter[64] = {};
    bool                  m_sectionStateRestored = false; // 起動時に ImGui StateStorage を一度だけ復元した後 true

    // ロック機能: true のとき m_lockedEntityId のオブジェクトを固定表示する。
    // WHY: IK Solver の設定中など、Hierarchy で別オブジェクトをクリックしても
    //      Inspector の表示を切り替えずに編集を続けられるようにする。
    bool            m_locked         = false;
    scene::EntityID m_lockedEntityId = {};

    // アセットインスペクター状態。
    // WHY: InspectorPanel は EditorApp.cpp の make_unique で確保されるため、頻繁にメンバを増やすと
    //      増分ビルドで古い sizeof(InspectorPanel) が残った際に破損しやすい。
    //      Asset ロックは m_locked && !m_lockedEntityId.IsValid() と、この path の組み合わせで表現する。
    std::string                                          m_inspectedAssetPath;
    renderer::ResourceHandle<renderer::MaterialAssetTag> m_inspectedMat;

    void DrawAssetInspector(EditorContext& ctx, const std::string& assetPath);
};

} // namespace fbzz::editor
