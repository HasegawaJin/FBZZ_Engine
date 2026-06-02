// FBZZ Engine
// FontAtlas.cpp | fbzz::renderer
// TTF 由来のフォントアトラス PNG + FNT ファイルのロード実装
//
// FNT ファイル形式 (gen_font_atlas.py が出力する独自テキスト形式):
//   line_height <px>
//   base        <px>
//   cell_w      <px>
//   glyph <ascii_code> <u0> <v0> <u1> <v1> <advance>
#include <Engine/Renderer/FontAtlas.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <sstream>
#include <string>

namespace fbzz::renderer {

bool FontAtlas::Load(const std::string& basePath, ResourceManager& resources)
{
    const std::string pngPath = basePath + ".png";
    const std::string fntPath = basePath + ".fnt";

    // テクスチャをロード
    m_texture = resources.LoadTexture(pngPath);
    if (!m_texture.IsValid()) {
        FBZZ_LOG_ERROR("FontAtlas: テクスチャのロードに失敗しました: %s", pngPath.c_str());
        return false;
    }

    // FNT メタデータをテキスト読み込み
    std::string fntText;
    if (!util::FileSystem::ReadText(fntPath, fntText)) {
        FBZZ_LOG_ERROR("FontAtlas: FNT ファイルのロードに失敗しました: %s", fntPath.c_str());
        return false;
    }

    // 行ごとにパースする。
    // WHY: JSON/TOML パーサーへの依存を避けるため、gen_font_atlas.py と合意した
    //      軽量な独自テキスト形式を採用している。エンジン内部のみで使うフォーマット。
    std::istringstream ss(fntText);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.empty() || line[0] == '#') continue;

        std::istringstream ls(line);
        std::string token;
        ls >> token;

        if (token == "line_height") {
            ls >> m_lineHeight;
        } else if (token == "base") {
            ls >> m_base;
        } else if (token == "cell_w") {
            ls >> m_cellW;
        } else if (token == "glyph") {
            int code = 0;
            float u0, v0, u1, v1, advance;
            ls >> code >> u0 >> v0 >> u1 >> v1 >> advance;
            if (code >= 0 && code < kTableSize) {
                m_glyphs[code]   = { u0, v0, u1, v1, advance };
                m_hasGlyph[code] = true;
            }
        }
    }

    if (m_lineHeight <= 0.0f) {
        FBZZ_LOG_ERROR("FontAtlas: line_height が FNT ファイルに見つかりません: %s", fntPath.c_str());
        return false;
    }

    return true;
}

const FontGlyph* FontAtlas::GetGlyph(char c) const
{
    const int code = static_cast<unsigned char>(c);
    if (code < 0 || code >= kTableSize || !m_hasGlyph[code])
        return nullptr;
    return &m_glyphs[code];
}

} // namespace fbzz::renderer
