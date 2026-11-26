/// @file    TexturePreview.hpp
/// @brief   Texture アセット用 Inspector プレビュー。
/// @author  Hasegawa Jin
/// @date    2026-09-24
#pragma once

#include <Graphics/Renderer/Resources/ResourceHandle.hpp>
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
    std::string m_sourcePath; ///< 現在プレビューしているテクスチャ原本のパス。
    renderer::ResourceManager* m_resourceOwner = nullptr; ///< 表示するテクスチャを解決した借用 ResourceManager。
    renderer::ResourceHandle<renderer::TextureTag> m_texture; ///< 現在の原本に対応する描画用テクスチャハンドル。
    std::int64_t m_writeTime = 0; ///< 前回確認したテクスチャ原本の更新時刻。
    std::uint64_t m_resetVersion = 0; ///< テクスチャ取得時の ResourceManager のリセット世代。
    double m_nextFileCheck = 0.0; ///< 原本ファイルを次に確認できる ImGui の時刻 [s]。
    bool m_fileExists = false; ///< 前回のファイル確認で原本が存在したか。
    bool m_checkedFile = false; ///< 選択中の原本パスについてファイル状態を確認済みか。
};

} // namespace fbzz::editor
