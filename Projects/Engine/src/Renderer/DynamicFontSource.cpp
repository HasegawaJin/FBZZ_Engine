// FBZZ Engine
// DynamicFontSource.cpp | fbzz::renderer
// stb_truetype による SDF ラスタライズと stb_rect_pack によるアトラス配置
//
// 処理の流れ (1 グリフあたり):
//   1. stbtt_GetCodepointSDF で符号付き距離場ビットマップを得る (padding ぶん外側へ広がる)
//   2. stbrp_pack_rects でアトラス上の空き矩形へ配置する
//   3. CPU 側のページバッファへコピーし、ページのダーティ矩形を広げる
//   4. 全グリフを処理し終えたら、ページごとに 1 回だけ ITexture::UpdateRegion で転送する
//
// WHY (ダーティ矩形をまとめる): DX12 のリージョン転送は同期 Flush を伴うため、
//   グリフ 1 個ごとに転送すると日本語 1 行の初出で数十回 GPU を待つことになる。
//   ページ単位でまとめると、どれだけ文字が増えてもフレームあたり「ページ数」回で済む。
#include <Engine/Renderer/DynamicFontSource.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Renderer/ITexture.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>

#include <stb_rect_pack.h>
#include <stb_truetype.h>

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace fbzz::renderer {

namespace {

// アトラス 1 ページの一辺の上限 (px)。R8 なので 2048x2048 で 4MB。
// WHY: 日本語の常用漢字 (約 2,100 字) を 48px SDF + パディングで焼くと
//      1 ページにはやや足りない。足りなくなったらページを増やす設計にしてあるため、
//      初期確保を無駄に大きくせず、この値から始める。
constexpr int   ATLAS_SIZE_MAX = 2048;
constexpr int   ATLAS_SIZE_MIN = 512;

// 一辺に並べるグリフの目安数。ページの一辺はラスタライズ解像度からこれで決める。
// WHY 解像度に追従させるか: 同じフォントを解像度別に焼き分けるようになったため、
//      一辺を固定にすると小さく焼いたアトラスまで 4MB を占める。1 ページあたりの
//      収容字数を揃えたまま、実体だけを解像度なりの大きさにする。
constexpr int   ATLAS_GLYPHS_PER_SIDE = 32;

// SDF の広がり (px)。輪郭からこの距離までが 0〜255 にマップされる。
// WHY: 大きいほど太い縁取りやシャドウを距離場から作れるが、グリフ 1 個の占有面積が
//      (2 * padding)^2 ぶん増える。UI テキストの AA と軽い縁取りには 4px で足りる。
constexpr int   SDF_PADDING = 4;

// 輪郭を表す値。UNORM 化すると 128/255 ≒ 0.502 となり、シェーダーの 0.5 判定と一致する。
constexpr unsigned char SDF_ONEDGE_VALUE = 128;

// 距離 1px あたりの階調。padding px で 0 または 255 に飽和するよう設定する。
constexpr float SDF_PIXEL_DIST_SCALE =
    static_cast<float>(SDF_ONEDGE_VALUE) / static_cast<float>(SDF_PADDING);

// 1em あたりの行高さ。UISystem の scale は fontSize / lineHeight で決まるため、
// この値が「同じ fontSize を指定したときの字の大きさ」を決める。
//
// WHY (フォントの縦メトリクスを使わない): hhea の ascent - descent はフォントごとの
//   ばらつきが極端に大きい。実測で Roboto = 1.19em に対し Noto Sans JP = 1.45em、
//   Yu Gothic = 1.10em。これをそのまま基準にすると、同じ fontSize でも
//   和文フォントだけ 2 割小さく描画され、欧文と混植した UI で揃わない。
//   組版で一般的な 1.2 (CSS の normal 相当) に固定すると、
//   どのフォントでも 1em の描画サイズが一致し、既存の静的 Roboto アトラス
//   (48px 生成 / lineHeight 57 = 1.1875em) ともほぼ同じ大きさに揃う。
//
// NOTE: 行高さより ascent が大きいフォントでは字が行送りより上へはみ出すが、
//   lineHeight はクリップには使われず「拡大率と改行の送り量」の基準でしかないため、
//   描画が欠けることはない。
constexpr float LINE_HEIGHT_PER_EM = 1.2f;

// ラスタライズ解像度からページの一辺を決める。DX12 のリージョン転送が行頭を
// 256B 境界で扱うため、一辺も 256 の倍数へ丸める。
int ResolveAtlasSize(float pixelHeight)
{
    const int cell    = static_cast<int>(pixelHeight) + 2 * SDF_PADDING;
    const int desired = ((cell * ATLAS_GLYPHS_PER_SIDE + 255) / 256) * 256;
    return std::clamp(desired, ATLAS_SIZE_MIN, ATLAS_SIZE_MAX);
}

} // namespace

// ── 内部実装 ──────────────────────────────────────────────────────────────────
// WHY (pimpl): stb_truetype / stb_rect_pack の型をヘッダーへ露出させないため。
//      FontAtlas.hpp は UISystem からも include されるので、そこへ 20 万行の
//      シングルヘッダを持ち込みたくない。
struct DynamicFontSource::Impl {
    // アトラス 1 ページぶんの CPU 実体と GPU ハンドル。
    //
    // WHY (unique_ptr で持つ): stbrp_context は nodes 配列への生ポインタを内部に握る。
    //      Page を vector に値で入れると再確保のたびにポインタが宙を指すため、
    //      アドレスが動かない形で保持する必要がある。
    struct Page {
        std::vector<std::uint8_t>  pixels;       // R8 のアトラス実体
        ResourceHandle<TextureTag> texture;
        stbrp_context              packer{};
        std::vector<stbrp_node>    nodes;

        // このフレームで書き換えた領域 (半開区間 [x0, x1) × [y0, y1))
        std::uint32_t dirtyX0 = 0, dirtyY0 = 0, dirtyX1 = 0, dirtyY1 = 0;
        bool          dirty   = false;

        void MarkDirty(std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h)
        {
            if (w == 0 || h == 0) return;
            if (!dirty) {
                dirtyX0 = x; dirtyY0 = y; dirtyX1 = x + w; dirtyY1 = y + h;
                dirty   = true;
                return;
            }
            dirtyX0 = (std::min)(dirtyX0, x);
            dirtyY0 = (std::min)(dirtyY0, y);
            dirtyX1 = (std::max)(dirtyX1, x + w);
            dirtyY1 = (std::max)(dirtyY1, y + h);
        }
    };

    std::vector<std::uint8_t>           fontData;   // TTF/TTC の生バイト (font が参照し続ける)
    stbtt_fontinfo                      font{};
    bool                                valid = false;

    float scale      = 0.0f;   // フォント単位 → px の変換係数
    float lineHeight = 0.0f;
    float base       = 0.0f;   // 行の上端からベースラインまで
    float fallbackAdvance = 0.0f;
    int   atlasSize  = ATLAS_SIZE_MAX;   // ページ 1 枚の一辺 (px)。Load で解像度から決める

    std::vector<std::unique_ptr<Page>> pages;

    // 新しいページを確保して CPU バッファとパッカーを初期化する。
    // GPU テクスチャは呼び出し側 (AddGlyphs) が作って結び付ける。
    Page& AppendPage()
    {
        auto page = std::make_unique<Page>();
        page->pixels.assign(static_cast<std::size_t>(atlasSize) * atlasSize, 0u);
        page->nodes.resize(atlasSize);
        stbrp_init_target(&page->packer, atlasSize, atlasSize,
                          page->nodes.data(), static_cast<int>(page->nodes.size()));
        pages.push_back(std::move(page));
        return *pages.back();
    }
};

DynamicFontSource::DynamicFontSource() : m_impl(std::make_unique<Impl>()) {}
DynamicFontSource::~DynamicFontSource() = default;
DynamicFontSource::DynamicFontSource(DynamicFontSource&&) noexcept = default;
DynamicFontSource& DynamicFontSource::operator=(DynamicFontSource&&) noexcept = default;

bool DynamicFontSource::Load(const std::string& fontPath, float pixelHeight)
{
    if (pixelHeight <= 0.0f) return false;

    // WHY (PathFromUtf8 を通す): Windows の filesystem::path は std::string を
    //   ANSI コードページとして解釈するため、日本語を含むパスでは直接渡すと開けない。
    const std::filesystem::path fsPath = util::FileSystem::PathFromUtf8(fontPath);
    if (!util::FileSystem::ReadBinary(fsPath, m_impl->fontData) || m_impl->fontData.empty()) {
        FBZZ_LOG_ERROR("DynamicFontSource: フォントファイルを読めません: %s", fontPath.c_str());
        return false;
    }

    // .ttc (TrueType Collection) は複数フォントを内包するため、先頭フォントのオフセットを引く。
    // 単体 .ttf/.otf でも 0 が返るので分岐は不要。
    const int offset = stbtt_GetFontOffsetForIndex(m_impl->fontData.data(), 0);
    if (offset < 0 || !stbtt_InitFont(&m_impl->font, m_impl->fontData.data(), offset)) {
        FBZZ_LOG_ERROR("DynamicFontSource: フォントを解釈できません: %s", fontPath.c_str());
        m_impl->fontData.clear();
        return false;
    }

    // WHY (ScaleForPixelHeight ではなく ScaleForMappingEmToPixels):
    //   静的アトラスを焼いている gen_font_atlas.py は Pillow の
    //   ImageFont.truetype(path, size) を使っており、これは「em サイズ = size」という意味。
    //   一方 stbtt_ScaleForPixelHeight は「ascent - descent = 指定 px」を基準にするため、
    //   同じ数値を渡しても両者で字の大きさが 2 割ほどずれる。
    //   em 基準に揃えることで、静的フォントと動的フォントを同じ fontSize で並べても
    //   線の太さと字面の印象が一致する。
    m_impl->scale     = stbtt_ScaleForMappingEmToPixels(&m_impl->font, pixelHeight);
    m_impl->atlasSize = ResolveAtlasSize(pixelHeight);

    int ascent = 0, descent = 0, lineGap = 0;
    stbtt_GetFontVMetrics(&m_impl->font, &ascent, &descent, &lineGap);

    // 行高さは em に対する固定比で決める (理由は LINE_HEIGHT_PER_EM のコメント)。
    m_impl->lineHeight = pixelHeight * LINE_HEIGHT_PER_EM;
    // ベースラインの位置だけは実フォントの ascent を使う。
    // ここを固定比にすると、字が行の中で上下にずれて見える。
    m_impl->base       = static_cast<float>(ascent) * m_impl->scale;

    // 未登録グリフ用の送り幅は半角スペースを基準にする。
    int spaceAdvance = 0, spaceLsb = 0;
    stbtt_GetCodepointHMetrics(&m_impl->font, ' ', &spaceAdvance, &spaceLsb);
    m_impl->fallbackAdvance = (spaceAdvance > 0)
        ? static_cast<float>(spaceAdvance) * m_impl->scale
        : m_impl->lineHeight * 0.25f;

    m_impl->valid = (m_impl->lineHeight > 0.0f);
    if (!m_impl->valid)
        FBZZ_LOG_ERROR("DynamicFontSource: 行高さを決定できません: %s", fontPath.c_str());
    return m_impl->valid;
}

bool  DynamicFontSource::IsValid() const           { return m_impl->valid; }
float DynamicFontSource::GetLineHeight() const     { return m_impl->lineHeight; }
float DynamicFontSource::GetBase() const           { return m_impl->base; }
float DynamicFontSource::GetFallbackAdvance() const{ return m_impl->fallbackAdvance; }

float DynamicFontSource::GetKerning(char32_t previous, char32_t next) const
{
    if (!m_impl->valid) return 0.0f;
    const int kern = stbtt_GetCodepointKernAdvance(
        &m_impl->font, static_cast<int>(previous), static_cast<int>(next));
    return static_cast<float>(kern) * m_impl->scale;
}

bool DynamicFontSource::AddGlyphs(const std::vector<char32_t>&             codePoints,
                                  std::unordered_map<char32_t, FontGlyph>& glyphTable,
                                  std::vector<ResourceHandle<TextureTag>>& pages,
                                  ResourceManager&                         resources)
{
    if (!m_impl->valid || codePoints.empty()) return false;

    bool addedAny = false;

    for (const char32_t code : codePoints) {
        if (glyphTable.find(code) != glyphTable.end()) continue;

        // 送り幅はビットマップの有無に関わらず必要 (スペースなど図形を持たない文字がある)。
        int rawAdvance = 0, rawLsb = 0;
        stbtt_GetCodepointHMetrics(&m_impl->font, static_cast<int>(code), &rawAdvance, &rawLsb);

        FontGlyph glyph{};
        glyph.advance = static_cast<float>(rawAdvance) * m_impl->scale;

        // 符号付き距離場を生成する。図形を持たない文字 (スペース等) では nullptr が返る。
        int width = 0, height = 0, xoff = 0, yoff = 0;
        unsigned char* sdf = stbtt_GetCodepointSDF(
            &m_impl->font, m_impl->scale, static_cast<int>(code),
            SDF_PADDING, SDF_ONEDGE_VALUE, SDF_PIXEL_DIST_SCALE,
            &width, &height, &xoff, &yoff);

        if (!sdf || width <= 0 || height <= 0) {
            // 図形なし。送り幅だけ持つ空グリフとして登録し、次フレーム以降の再試行を防ぐ。
            if (sdf) stbtt_FreeSDF(sdf, nullptr);
            glyphTable[code] = glyph;
            addedAny = true;
            continue;
        }

        // 空きのあるページを探す。どこにも入らなければ新しいページを足す。
        stbrp_rect rect{};
        rect.w = static_cast<stbrp_coord>(width);
        rect.h = static_cast<stbrp_coord>(height);

        Impl::Page* target = nullptr;
        for (auto& page : m_impl->pages) {
            rect.was_packed = 0;
            stbrp_pack_rects(&page->packer, &rect, 1);
            if (rect.was_packed) { target = page.get(); break; }
        }
        if (!target) {
            if (width > m_impl->atlasSize || height > m_impl->atlasSize) {
                // 1 ページに収まらない巨大グリフ。ラスタライズ解像度の設定ミス。
                FBZZ_LOG_ERROR("DynamicFontSource: グリフ U+%04X (%dx%d) がアトラス %d を超えています",
                               static_cast<unsigned>(code), width, height, m_impl->atlasSize);
                stbtt_FreeSDF(sdf, nullptr);
                glyphTable[code] = glyph;   // 空グリフとして登録し、毎フレームの再試行を防ぐ
                addedAny = true;
                continue;
            }
            Impl::Page& fresh = m_impl->AppendPage();
            rect.was_packed = 0;
            stbrp_pack_rects(&fresh.packer, &rect, 1);
            if (!rect.was_packed) {
                FBZZ_LOG_ERROR("DynamicFontSource: 新規ページにもグリフ U+%04X を配置できません",
                               static_cast<unsigned>(code));
                stbtt_FreeSDF(sdf, nullptr);
                glyphTable[code] = glyph;
                addedAny = true;
                continue;
            }
            target = &fresh;
        }

        // CPU バッファへ 1 行ずつ転写する。
        const auto destX = static_cast<std::uint32_t>(rect.x);
        const auto destY = static_cast<std::uint32_t>(rect.y);
        for (int row = 0; row < height; ++row) {
            std::memcpy(target->pixels.data()
                            + (static_cast<std::size_t>(destY) + row) * m_impl->atlasSize + destX,
                        sdf + static_cast<std::size_t>(row) * width,
                        static_cast<std::size_t>(width));
        }
        target->MarkDirty(destX, destY,
                          static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
        stbtt_FreeSDF(sdf, nullptr);

        // ページ番号は m_impl->pages のインデックス。FontAtlas の pages 配列と一致させる。
        int pageIndex = 0;
        for (std::size_t i = 0; i < m_impl->pages.size(); ++i)
            if (m_impl->pages[i].get() == target) { pageIndex = static_cast<int>(i); break; }

        const float atlasSizeF = static_cast<float>(m_impl->atlasSize);
        glyph.u0 = static_cast<float>(destX) / atlasSizeF;
        glyph.v0 = static_cast<float>(destY) / atlasSizeF;
        glyph.u1 = static_cast<float>(destX + width)  / atlasSizeF;
        glyph.v1 = static_cast<float>(destY + height) / atlasSizeF;
        glyph.width   = static_cast<float>(width);
        glyph.height  = static_cast<float>(height);
        // stbtt の xoff/yoff はベースライン原点からの相対値 (yoff は上方向が負)。
        // レイアウトは行の上端を基準に組むため、yOffset へは base を足して変換する。
        glyph.xOffset = static_cast<float>(xoff);
        glyph.yOffset = m_impl->base + static_cast<float>(yoff);
        glyph.page    = pageIndex;

        glyphTable[code] = glyph;
        addedAny = true;
    }

    if (!addedAny) return false;

    // ページのテクスチャを必要なぶんだけ作り、FontAtlas 側の配列と長さを揃える。
    for (std::size_t i = 0; i < m_impl->pages.size(); ++i) {
        Impl::Page& page = *m_impl->pages[i];
        if (!page.texture.IsValid()) {
            const auto side = static_cast<std::uint32_t>(m_impl->atlasSize);
            page.texture = resources.CreateDynamicTexture(side, side, DynamicTextureFormat::R8);
            if (!page.texture.IsValid()) {
                FBZZ_LOG_ERROR("DynamicFontSource: 動的テクスチャを生成できません "
                               "(バックエンドが未対応の可能性)");
                return false;
            }
        }
        if (pages.size() <= i) pages.resize(i + 1);
        pages[i] = page.texture;
    }

    // ダーティ矩形をページごとに 1 回だけ転送する。
    for (auto& pagePtr : m_impl->pages) {
        Impl::Page& page = *pagePtr;
        if (!page.dirty) continue;

        ITexture* texture = resources.Get(page.texture);
        if (!texture) { page.dirty = false; continue; }

        const std::uint32_t w = page.dirtyX1 - page.dirtyX0;
        const std::uint32_t h = page.dirtyY1 - page.dirtyY0;
        // CPU バッファはアトラス全面なので、矩形の左上画素を先頭として
        // 行ピッチにアトラス幅をそのまま渡せば部分矩形を転送できる。
        const std::uint8_t* origin =
            page.pixels.data() + static_cast<std::size_t>(page.dirtyY0) * m_impl->atlasSize + page.dirtyX0;
        texture->UpdateRegion(page.dirtyX0, page.dirtyY0, w, h, origin,
                              static_cast<std::uint32_t>(m_impl->atlasSize));
        page.dirty = false;
    }

    return true;
}

} // namespace fbzz::renderer
