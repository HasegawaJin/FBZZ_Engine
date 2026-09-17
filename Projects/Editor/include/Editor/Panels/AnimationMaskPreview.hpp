/// @file    AnimationMaskPreview.hpp
/// @brief   Avatar Mask を IPreviewPanel 契約で描画する Inspector 埋め込みプレビュー。
/// @author  Hasegawa Jin
/// @date    2026-08-19
#pragma once

#include <Editor/Panels/IPreviewPanel.hpp>
#include <Engine/Asset/AvatarMaskAsset.hpp>

#include <string_view>

namespace fbzz::editor {

/// @brief .mask の保存内容と、編集中の一時状態を同じ描画経路へ渡すプレビュー実装。
/// @note Inspector と独立 Preview を別実装にすると、ウェイトの解決規則や色付けが分岐し表示だけ古くなるため、
///       IPreviewPanel の小さな実装へ集約する。
class AnimationMaskPreview final : public IPreviewPanel {
public:
    [[nodiscard]] const char* GetPreviewName() const override { return "Animation Mask Preview"; }
    [[nodiscard]] bool Supports(std::string_view extension) const override;
    [[nodiscard]] bool DrawPreview(EditorContext& ctx,
                                   std::string_view assetPath,
                                   float previewHeight) override;

    /// Inspector の未保存編集をプレビューへ反映するためのオーバーロード。
    [[nodiscard]] bool DrawPreview(EditorContext& ctx,
                                   std::string_view assetPath,
                                   const asset::AvatarMaskAsset& mask,
                                   float previewHeight);
};

} // namespace fbzz::editor
