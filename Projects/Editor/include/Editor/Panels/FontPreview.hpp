/// @file    FontPreview.hpp
/// @brief   フォントファイルの字形プレビューを描画する。
/// @author  Hasegawa Jin
/// @date    2026-09-25
#pragma once

#include <Engine/Renderer/ResourceHandle.hpp>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::renderer { class ResourceManager; }

namespace fbzz::editor {

struct EditorContext;

struct FontPreviewImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
};

/// @brief フォント自身が持つ字形だけで見本画像を作る。
/// @return 読めないフォント、または見本に使える字形が無ければ false。
/// @note large は Inspector 用、false は AssetBrowser 用の解像度を選ぶ。
[[nodiscard]] bool RasterizeFontPreview(std::string_view path, bool large, FontPreviewImage& out);

class FontPreviewView {
public:
    /// @brief Inspector 内に見本画像を表示する。
    void Draw(EditorContext& ctx, std::string_view path, float previewHeight);
    /// @brief このプレビューが所有する GPU テクスチャを解放する。
    void Release();

private:
    void ReleaseTexture();

    std::string m_path;
    renderer::ResourceManager* m_resourceOwner = nullptr;
    renderer::ResourceHandle<renderer::TextureTag> m_texture;
    std::int64_t m_writeTime = 0;
    std::uint64_t m_resetVersion = 0;
    double m_nextFileCheck = 0.0;
    bool m_checkedFile = false;
    bool m_fileExists = false;
    bool m_attempted = false;
};

} /// @note namespace fbzz::editor
