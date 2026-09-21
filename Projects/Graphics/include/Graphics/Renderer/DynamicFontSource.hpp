/// @file    DynamicFontSource.hpp
/// @brief   TTF/TTC/OTF から実行時にグリフをラスタライズして育てる動的フォントアトラス。
/// @author  Hasegawa Jin
/// @date    2026-08-19
/// @note 日本語は JIS 第 1 水準だけで約 3,000 字あり、静的アトラスへ全部載せると PNG が数 MB に
/// @note       なる。TextMeshPro の Dynamic Font Asset 同様「使われた文字だけをその場で焼く」ことで、
/// @note       アトラスは実使用ぶんで済む。
/// @note 48px 固定のカバレッジは拡大で角張るため、stbtt_GetCodepointSDF で符号付き距離場を焼く。
/// @note       UIText.hlsl の fwidth 正規化がそのまま距離場の AA として機能し、カバレッジと SDF で
/// @note       分岐する必要がない。
/// @note FontAtlas 経由でのみ使う。fontPath の拡張子が .ttf/.ttc/.otf のとき FontAtlas が生成し、
/// @note       UISystem がレイアウト前に FontAtlas::PrepareText() を呼ぶと未登録グリフが焼かれる。
#pragma once
#include <Graphics/Renderer/FontAtlas.hpp>
#include <Graphics/Renderer/ResourceHandle.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace fbzz::renderer {

class ResourceManager;

class DynamicFontSource {
public:
    DynamicFontSource();
    ~DynamicFontSource();
    DynamicFontSource(DynamicFontSource&&) noexcept;
    DynamicFontSource& operator=(DynamicFontSource&&) noexcept;
    DynamicFontSource(const DynamicFontSource&)            = delete;
    DynamicFontSource& operator=(const DynamicFontSource&) = delete;

    /// @brief フォントファイルを読み込み、メトリクスを確定する。
    /// @param pixelHeight アトラスへ焼く際のラスタライズ解像度 (行高さではなく em 相当の px)。
    /// @note テクスチャは最初のグリフ追加時に遅延生成されるため、ここでは GPU に触らない。
    bool Load(const std::string& fontPath, float pixelHeight);

    [[nodiscard]] bool IsValid() const;

    /// @name フォントのメトリクス (アトラスピクセル単位)
    /// @{
    [[nodiscard]] float GetLineHeight() const;
    [[nodiscard]] float GetBase() const;
    [[nodiscard]] float GetFallbackAdvance() const;
    /// @}

    /// @brief 未登録のコードポイント群をラスタライズしてアトラスへ配置し、glyphTable へ登録する。
    /// @note 新しいページが必要になった場合は pages へテクスチャハンドルを追加する。追加した
    /// @note       内容は関数の最後に 1 度だけ GPU へ転送される (ページごとにダーティ矩形 1 回)。
    /// @return 1 つでもグリフを追加できたら true。
    bool AddGlyphs(const std::vector<char32_t>&             codePoints,
                   std::unordered_map<char32_t, FontGlyph>& glyphTable,
                   std::vector<ResourceHandle<TextureTag>>& pages,
                   ResourceManager&                         resources);

    /// @brief 2 文字間のカーニング量 (アトラスピクセル単位)。
    /// @note stb_truetype が読むのは旧来の `kern` テーブルのみで、GPOS ベースのカーニングしか
    /// @note       持たない現代的なフォントでは 0 が返る。
    [[nodiscard]] float GetKerning(char32_t previous, char32_t next) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} /// @note namespace fbzz::renderer
