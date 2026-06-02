// FBZZ Engine
// FontAtlas.hpp | fbzz::renderer
// TTF 由来のビットマップフォントアトラスのロードとグリフ UV 管理
//
// WHY: 既存の UISystem は 5x7 ピクセルビットマップを CPU で SDF 展開する手作り実装だった。
//      Kenney フォント等の TTF を使いたい場合、gen_font_atlas.py で生成した PNG + FNT を
//      このクラスで読み込むことで、クオリティと可読性を両立できる。
//
// 使い方:
//   FontAtlas atlas;
//   atlas.Load("Assets/Fonts/Kenney/Future", resources);  // .png / .fnt を自動付与
//   const FontGlyph* g = atlas.GetGlyph('A');
#pragma once
#include <Engine/Renderer/ResourceHandle.hpp>
#include <cstdint>
#include <string>

namespace fbzz::renderer {

class ResourceManager;

// 1 グリフの UV 座標と水平アドバンス。
// UV はアトラステクスチャの正規化座標 (0.0〜1.0)。
// advance はアトラスのレンダリングピクセル単位 (実画面への変換は scale を掛ける)。
struct FontGlyph {
    float u0 = 0.0f, v0 = 0.0f;   // アトラス上の UV 左上
    float u1 = 0.0f, v1 = 0.0f;   // アトラス上の UV 右下
    float advance = 0.0f;          // 次グリフ原点までの水平移動量 (アトラスピクセル単位)
};

// フォントアトラスのロードと参照。
// Load() で PNG テクスチャと FNT メタデータを読み込む。
// UISystem がこのクラスをパス文字列をキーにキャッシュし、重複ロードを防ぐ。
class FontAtlas {
public:
    // basePath に ".png" / ".fnt" を付与して両ファイルをロードする。
    // 失敗しても例外は投げない。IsValid() で成否を確認すること。
    bool Load(const std::string& basePath, ResourceManager& resources);

    // ロード成功かつテクスチャが有効なら true。
    [[nodiscard]] bool IsValid() const { return m_texture.IsValid(); }

    // ASCII コードに対応する FontGlyph ポインタを返す。
    // 範囲外・未登録の場合は nullptr。
    [[nodiscard]] const FontGlyph* GetGlyph(char c) const;

    // アトラス生成時の行高さ (px)。fontSize / line_height がスケール係数になる。
    [[nodiscard]] float GetLineHeight() const { return m_lineHeight; }

    // 上端からベースラインまでの距離 (px)。
    [[nodiscard]] float GetBase() const { return m_base; }

    // アトラス内の 1 グリフセル幅 (px)。
    [[nodiscard]] float GetCellW() const { return m_cellW; }

    [[nodiscard]] ResourceHandle<TextureTag> GetTexture() const { return m_texture; }

private:
    // ASCII 0x20〜0x7E (95 文字) + 番兵用に 128 エントリ確保
    static constexpr int kTableSize = 128;

    ResourceHandle<TextureTag> m_texture;
    FontGlyph m_glyphs[kTableSize] = {};
    bool      m_hasGlyph[kTableSize] = {};
    float     m_lineHeight = 0.0f;
    float     m_base       = 0.0f;
    float     m_cellW      = 0.0f;
};

} // namespace fbzz::renderer
