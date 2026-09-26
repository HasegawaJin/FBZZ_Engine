/// @file    InspectorPanel.hpp
/// @brief   選択 Entity のコンポーネントを表示・編集する。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#pragma once

#include <Editor/Panels/IPanel.hpp>
#include <Editor/Panels/FontPreview.hpp>
#include <Editor/Panels/MaterialPreview.hpp>
#include <Editor/Panels/TexturePreview.hpp>
#include <Engine/Asset/AssetHandle.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
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
    bool                  m_sectionStateRestored = false;

    /// @brief Hierarchy の選択変更から Inspector の対象を固定する。
    /// @note IK Solver の編集中も同じオブジェクトを表示し続ける。
    bool            m_locked         = false;
    scene::EntityID m_lockedEntityId = {};

    /// @brief アセット Inspector とロック対象の状態。
    /// @note Asset ロックは m_locked と無効な m_lockedEntityId と path の組み合わせで表す。
    std::string                                          m_inspectedAssetPath;
    asset::AssetHandle<asset::MaterialAsset>             m_inspectedMat;

    /// @brief .mat Inspector 下部のプレビュー。
    /// @note Preview パネルとは視点と照明を共有しない。
    MaterialPreviewView m_materialPreview;
    /// @brief 画像の Inspector 下部プレビュー。
    TexturePreviewView  m_texturePreview;
    /// @brief フォントの Inspector 下部プレビュー。
    FontPreviewView     m_fontPreview;

    void DrawAssetInspector(EditorContext& ctx, const std::string& assetPath);
};

} /// @note namespace fbzz::editor
