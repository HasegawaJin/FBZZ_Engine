/// @file    FontPreview.hpp
/// @brief   フォントファイルの字形プレビューを描画する。
/// @author  Hasegawa Jin
/// @date    2026-09-25
#pragma once

#include <Graphics/Renderer/Resources/ResourceHandle.hpp>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::renderer { class ResourceManager; }

namespace fbzz::editor {

struct EditorContext;

struct FontPreviewImage {
    std::uint32_t width = 0; ///< プレビュー画像の幅 [px]。
    std::uint32_t height = 0; ///< プレビュー画像の高さ [px]。
    std::vector<std::uint8_t> rgba; ///< プレビュー画像の RGBA 8 ビット画素を行優先で保持する。
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

    std::string m_path; ///< 現在ラスタライズしているフォントのパス。
    renderer::ResourceManager* m_resourceOwner = nullptr; ///< プレビューの GPU 画像を作った ResourceManager を識別する借用ポインター。
    renderer::ResourceHandle<renderer::TextureTag> m_texture; ///< ラスタライズした見本を表示する GPU テクスチャのハンドル。
    std::int64_t m_writeTime = 0; ///< 見本を更新したときのフォントファイルの更新時刻。
    std::uint64_t m_resetVersion = 0; ///< プレビュー画像を確保した ResourceManager のリセット世代。
    double m_nextFileCheck = 0.0; ///< フォントの更新時刻を次に確かめる ImGui の時刻 [s]。
    bool m_checkedFile = false; ///< 現在のパスのファイル情報を確認したか。
    bool m_fileExists = false; ///< 前回の確認でフォントファイルを見つけたか。
    bool m_attempted = false; ///< 現在のファイル版に対して見本の作成を試したか。
};

} // namespace fbzz::editor
