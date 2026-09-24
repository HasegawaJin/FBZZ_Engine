/// @file    TexturePreview.hpp
/// @brief   Texture アセット用 Inspector プレビュー。
/// @author  Hasegawa Jin
/// @date    2026-09-24
#pragma once

#include <Engine/Renderer/ResourceHandle.hpp>
#include <cstdint>
#include <string>
#include <string_view>

namespace fbzz::renderer { class ResourceManager; }

namespace fbzz::editor {

struct EditorContext;

/// @brief 選択中の画像を Inspector の末尾に表示する。
class TexturePreviewView {
public:
    /// @brief 画像の縦横比を保ち、市松背景に収まるように描画する。
    void Draw(EditorContext& ctx, std::string_view sourcePath, float previewHeight);

private:
    std::string m_sourcePath;
    renderer::ResourceManager* m_resourceOwner = nullptr;
    renderer::ResourceHandle<renderer::TextureTag> m_texture;
    std::int64_t m_writeTime = 0;
    std::uint64_t m_resetVersion = 0;
    double m_nextFileCheck = 0.0;
    bool m_fileExists = false;
    bool m_checkedFile = false;
};

} /// @note namespace fbzz::editor
