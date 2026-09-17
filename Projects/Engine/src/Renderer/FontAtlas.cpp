/// @file    FontAtlas.cpp
/// @brief   BMFont テキスト形式 / 旧独自形式の .fnt パースとページテクスチャのロード
/// @author  Hasegawa Jin
/// @date    2026-06-02
///
/// AngelCode BMFont テキスト形式と、gen_font_atlas.py が吐く旧独自形式 (互換維持) の両方を読み、
/// 旧形式は「均一セルの BMFont」に正規化して 1 本の描画パスに通す (焼き直せないフォントがあるため)。
/// @note 画素がカバレッジか距離場かを .fnt に書かせないのは、UIText.hlsl の 1px AA 式が両方を
///       そのまま扱え、描画側に分岐先が無いため (`FontAtlas.hpp` 参照)。
#include <Engine/Renderer/FontAtlas.hpp>
#include <Engine/Renderer/DynamicFontSource.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/Utf8.hpp>
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::renderer {

namespace {

/// 空白文字か。BMFont 行のトークン区切り判定に使う。
bool IsSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/// 行頭の空白を読み飛ばしつつ、次のトークン (空白まで) を返す。
std::string_view NextToken(std::string_view line, std::size_t& offset)
{
    while (offset < line.size() && IsSpace(line[offset])) ++offset;
    const std::size_t start = offset;
    while (offset < line.size() && !IsSpace(line[offset])) ++offset;
    return line.substr(start, offset - start);
}

/// BMFont の `key=value` を 1 組取り出す。
/// value がダブルクォートで囲まれている場合は、内部の空白を含めて 1 つの値として扱う
/// (例: face="Noto Sans JP")。取り出せなければ false。
bool NextAttribute(std::string_view line,
                   std::size_t&     offset,
                   std::string_view& key,
                   std::string_view& value)
{
    while (offset < line.size() && IsSpace(line[offset])) ++offset;
    if (offset >= line.size()) return false;

    const std::size_t keyStart = offset;
    while (offset < line.size() && line[offset] != '=' && !IsSpace(line[offset])) ++offset;
    key = line.substr(keyStart, offset - keyStart);

    /// @note '=' が無い単独トークンは属性ではない (値なしフラグ)。空の値で返す。
    if (offset >= line.size() || line[offset] != '=') {
        value = {};
        return !key.empty();
    }
    /// @note '=' を消費
    ++offset;

    if (offset < line.size() && line[offset] == '"') {
        /// @note 開きクォートを消費
        ++offset;
        const std::size_t valueStart = offset;
        while (offset < line.size() && line[offset] != '"') ++offset;
        value = line.substr(valueStart, offset - valueStart);
        /// @note 閉じクォートを消費
        if (offset < line.size()) ++offset;
    } else {
        const std::size_t valueStart = offset;
        while (offset < line.size() && !IsSpace(line[offset])) ++offset;
        value = line.substr(valueStart, offset - valueStart);
    }
    return !key.empty();
}

/// 例外を投げない数値変換。
/// @note std::stoi/std::stof は不正入力で例外を投げるが、AGENTS.md で throw は禁止。
///       strtol/strtof は失敗時に 0 を返すだけで済む。
long ToLong(std::string_view s)
{
    const std::string buffer(s);
    return std::strtol(buffer.c_str(), nullptr, 10);
}

float ToFloat(std::string_view s)
{
    const std::string buffer(s);
    return std::strtof(buffer.c_str(), nullptr);
}

/// パスからディレクトリ部分 (末尾の区切りを含まない) を取り出す。
/// 区切りが無ければ空文字列を返す。
std::string DirectoryOf(const std::string& path)
{
    const std::size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos) return {};
    return path.substr(0, slash);
}

/// .fnt の最初の意味のある行の先頭トークンで形式を判別する。
bool LooksLikeBMFont(const std::string& fntText)
{
    std::istringstream stream(fntText);
    std::string        line;
    while (std::getline(stream, line)) {
        if (line.empty() || line[0] == '#') continue;

        std::size_t            offset = 0;
        const std::string_view tag    = NextToken(line, offset);
        if (tag.empty()) continue;

        return tag == "info" || tag == "common" || tag == "page"
            || tag == "chars" || tag == "char";
    }
    return false;
}

/// 動的アトラスへ焼く解像度の段 (em px)。
/// @note 48 を含む理由: 既存の静的アトラスが 48px 生成であり、fontSize は行高さ基準で
///       正規化されるため、この段では静的/動的フォントで線の太さの印象が揃う。実質すべての
///       既定サイズ (fontSize 36〜48) が等倍でここへ落ちる。
/// @note 段の粗さ: 細かいほど要求解像度に近づくがアトラス実体も増える。1 段の差は最大 1.5 倍で、
///       SDF の縮小耐性 (padding 4px ぶん) に十分収まる。
constexpr float RASTER_PIXEL_HEIGHT_STEPS[] = {
    24.0f, 32.0f, 48.0f, 64.0f, 96.0f, 128.0f, 192.0f, 256.0f
};

} // namespace

FontAtlas::FontAtlas()                                  = default;
FontAtlas::FontAtlas(FontAtlas&&) noexcept              = default;

FontAtlas::~FontAtlas()
{
    /// @note Active() を使う理由: アトラスはキャッシュ (UISystemContext) の要素として畳まれ、
    ///       破棄地点へ ResourceManager& を渡す経路が無い (Material と同じ扱い)。ResourceManager
    ///       より後に消えるアトラスでは Active() が空で、そのときは返す先が無いので何もしない。
    if (ResourceManager* resources = ResourceManager::Active())
        ReleaseGpuResources(*resources);
}

/// @note メンバーを足したらここにも足すこと (既定のムーブに任せられないのは、
///       上書きされる側のページを先に返す必要があるため)。
FontAtlas& FontAtlas::operator=(FontAtlas&& other) noexcept
{
    if (this == &other) return *this;
    /// @note 上書きされる側のページを先に返す。ムーブ代入で «元のページを捨てる» のは
    ///       フォントを焼き直したときに通る (同じキーへ新しいアトラスを入れ直す)。
    if (ResourceManager* resources = ResourceManager::Active())
        ReleaseGpuResources(*resources);

    m_pages           = std::move(other.m_pages);
    m_glyphs          = std::move(other.m_glyphs);
    m_kernings        = std::move(other.m_kernings);
    m_dynamic         = std::move(other.m_dynamic);
    m_lineHeight      = other.m_lineHeight;
    m_base            = other.m_base;
    m_fallbackAdvance = other.m_fallbackAdvance;
    other.m_pages.clear();
    return *this;
}

void FontAtlas::ReleaseGpuResources(ResourceManager& resources)
{
    for (ResourceHandle<TextureTag>& page : m_pages)
        if (page.IsValid()) resources.Release(page);
    m_pages.clear();
}

bool FontAtlas::IsDynamicFontPath(const std::string& path)
{
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;

    std::string ext = path.substr(dot);
    for (char& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".ttf" || ext == ".ttc" || ext == ".otf";
}

float FontAtlas::ResolveRasterPixelHeight(float desiredPixelHeight)
{
    float largest = RASTER_PIXEL_HEIGHT_STEPS[0];
    for (const float step : RASTER_PIXEL_HEIGHT_STEPS) {
        largest = step;
        if (desiredPixelHeight <= step) return step;
    }
    return largest;
}

bool FontAtlas::Load(const std::string& path, ResourceManager& resources, float rasterPixelHeight)
{
    /// @note TTF/TTC/OTF を直接指定された場合は動的モード。日本語フォントは字種が多く
    ///       静的アトラスに載せきれないため、`.ttf` を fontPath に書けばそのまま出る導線を用意する。
    if (IsDynamicFontPath(path))
        return LoadDynamic(path, rasterPixelHeight);

    const std::string basePath = path;
    const std::string fntPath  = basePath + ".fnt";

    /// @note 旧実装から順序を変更: BMFont はページ PNG のファイル名を .fnt 内の
    ///       `page ... file="..."` で宣言するため、テクスチャより先にメタデータを読む必要がある。
    std::string fntText;
    if (!util::FileSystem::ReadText(fntPath, fntText)) {
        FBZZ_LOG_ERROR("FontAtlas: failed to load FNT file: %s", fntPath.c_str());
        return false;
    }

    const bool parsed = LooksLikeBMFont(fntText)
        ? ParseBMFont(fntText, fntPath, resources)
        : ParseLegacy(fntText, basePath, resources);

    if (!parsed) return false;

    if (m_lineHeight <= 0.0f) {
        FBZZ_LOG_ERROR("FontAtlas: lineHeight is missing or zero: %s", fntPath.c_str());
        return false;
    }
    if (m_glyphs.empty()) {
        FBZZ_LOG_ERROR("FontAtlas: no glyphs were parsed: %s", fntPath.c_str());
        return false;
    }
    return true;
}

bool FontAtlas::ParseBMFont(const std::string& fntText,
                            const std::string& fntPath,
                            ResourceManager&   resources)
{
    const std::string directory = DirectoryOf(fntPath);

    /// @note scaleW / scaleH はアトラス寸法。char の x/y/width/height を UV へ正規化するのに使う。
    float scaleW = 0.0f;
    float scaleH = 0.0f;

    /// @note ページ番号は宣言順とは限らないため、id をインデックスとして疎に埋める。
    std::vector<std::string> pageFiles;

    std::istringstream stream(fntText);
    std::string        line;
    while (std::getline(stream, line)) {
        if (line.empty() || line[0] == '#') continue;

        std::size_t            offset = 0;
        const std::string_view tag    = NextToken(line, offset);
        if (tag.empty()) continue;

        if (tag == "common") {
            std::string_view key, value;
            while (NextAttribute(line, offset, key, value)) {
                if      (key == "lineHeight") m_lineHeight = ToFloat(value);
                else if (key == "base")       m_base       = ToFloat(value);
                else if (key == "scaleW")     scaleW       = ToFloat(value);
                else if (key == "scaleH")     scaleH       = ToFloat(value);
            }
        } else if (tag == "page") {
            int         id   = 0;
            std::string file;
            std::string_view key, value;
            while (NextAttribute(line, offset, key, value)) {
                if      (key == "id")   id   = static_cast<int>(ToLong(value));
                else if (key == "file") file = std::string(value);
            }
            if (id >= 0 && !file.empty()) {
                if (static_cast<std::size_t>(id) >= pageFiles.size())
                    pageFiles.resize(static_cast<std::size_t>(id) + 1);
                pageFiles[static_cast<std::size_t>(id)] = file;
            }
        } else if (tag == "char") {
            char32_t  code = 0;
            FontGlyph glyph{};
            float     x = 0.0f, y = 0.0f;

            std::string_view key, value;
            while (NextAttribute(line, offset, key, value)) {
                if      (key == "id")       code          = static_cast<char32_t>(ToLong(value));
                else if (key == "x")        x             = ToFloat(value);
                else if (key == "y")        y             = ToFloat(value);
                else if (key == "width")    glyph.width   = ToFloat(value);
                else if (key == "height")   glyph.height  = ToFloat(value);
                else if (key == "xoffset")  glyph.xOffset = ToFloat(value);
                else if (key == "yoffset")  glyph.yOffset = ToFloat(value);
                else if (key == "xadvance") glyph.advance = ToFloat(value);
                else if (key == "page")     glyph.page    = static_cast<int>(ToLong(value));
            }

            /// @note UV は char の画素矩形をアトラス寸法で割って求める。
            if (scaleW > 0.0f && scaleH > 0.0f) {
                glyph.u0 = x / scaleW;
                glyph.v0 = y / scaleH;
                glyph.u1 = (x + glyph.width)  / scaleW;
                glyph.v1 = (y + glyph.height) / scaleH;
            }
            m_glyphs[code] = glyph;
        } else if (tag == "kerning") {
            char32_t first = 0, second = 0;
            float    amount = 0.0f;
            std::string_view key, value;
            while (NextAttribute(line, offset, key, value)) {
                if      (key == "first")  first  = static_cast<char32_t>(ToLong(value));
                else if (key == "second") second = static_cast<char32_t>(ToLong(value));
                else if (key == "amount") amount = ToFloat(value);
            }
            if (amount != 0.0f)
                m_kernings[MakeKerningKey(first, second)] = amount;
        }
    }

    if (scaleW <= 0.0f || scaleH <= 0.0f) {
        FBZZ_LOG_ERROR("FontAtlas: common scaleW/scaleH missing: %s", fntPath.c_str());
        return false;
    }
    if (pageFiles.empty()) {
        FBZZ_LOG_ERROR("FontAtlas: no page declaration found: %s", fntPath.c_str());
        return false;
    }

    /// @note ページ PNG は .fnt からの相対パスで書かれているので、.fnt のディレクトリを前置する。
    m_pages.reserve(pageFiles.size());
    for (const std::string& file : pageFiles) {
        if (file.empty()) {
            m_pages.push_back(ResourceHandle<TextureTag>::Null());
            continue;
        }
        const std::string pngPath = directory.empty() ? file : directory + "/" + file;
        ResourceHandle<TextureTag> texture = resources.LoadTexture(pngPath);
        if (!texture.IsValid())
            FBZZ_LOG_ERROR("FontAtlas: failed to load page texture: %s", pngPath.c_str());
        m_pages.push_back(texture);
    }

    /// @note 未登録グリフの送り幅は半角スペースを基準にする。無ければ行高さの 1/4。
    if (const FontGlyph* space = GetGlyph(U' '); space && space->advance > 0.0f)
        m_fallbackAdvance = space->advance;
    else
        m_fallbackAdvance = m_lineHeight * 0.25f;

    return true;
}

bool FontAtlas::ParseLegacy(const std::string& fntText,
                            const std::string& basePath,
                            ResourceManager&   resources)
{
    /// @note 旧形式は単一ページ固定で、テクスチャ名は basePath + ".png"。
    const std::string pngPath = basePath + ".png";
    ResourceHandle<TextureTag> texture = resources.LoadTexture(pngPath);
    if (!texture.IsValid()) {
        FBZZ_LOG_ERROR("FontAtlas: failed to load texture: %s", pngPath.c_str());
        return false;
    }
    m_pages.push_back(texture);

    /// @note 旧形式は全グリフが同じセルを占めるため、cell_w / line_height を
    ///       そのまま各グリフの width / height として展開する。
    float cellW = 0.0f;

    struct LegacyGlyph { char32_t code; FontGlyph glyph; };
    std::vector<LegacyGlyph> pending;

    std::istringstream stream(fntText);
    std::string        line;
    while (std::getline(stream, line)) {
        if (line.empty() || line[0] == '#') continue;

        std::istringstream lineStream(line);
        std::string        token;
        lineStream >> token;

        if (token == "line_height") {
            lineStream >> m_lineHeight;
        } else if (token == "base") {
            lineStream >> m_base;
        } else if (token == "cell_w") {
            lineStream >> cellW;
        } else if (token == "glyph") {
            int   code = 0;
            float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f, advance = 0.0f;
            lineStream >> code >> u0 >> v0 >> u1 >> v1 >> advance;

            FontGlyph glyph{};
            glyph.u0      = u0;
            glyph.v0      = v0;
            glyph.u1      = u1;
            glyph.v1      = v1;
            glyph.advance = advance;
            glyph.page    = 0;
            /// @note width / height は cell_w / line_height を読み終えてから埋める
            pending.push_back({ static_cast<char32_t>(code), glyph });
        }
    }

    /// @note cell_w/line_height が glyph 行より後ろに現れても正しく展開できるよう、
    ///       グリフ矩形の確定は全行読み終えた後に回している。
    for (LegacyGlyph& entry : pending) {
        entry.glyph.width   = cellW;
        entry.glyph.height  = m_lineHeight;
        entry.glyph.xOffset = 0.0f;
        entry.glyph.yOffset = 0.0f;
        m_glyphs[entry.code] = entry.glyph;
    }

    /// @note 旧 UISystem は未登録グリフに cellW * 0.5 を送っていた。その挙動を保つ。
    m_fallbackAdvance = cellW * 0.5f;
    return true;
}

bool FontAtlas::LoadDynamic(const std::string& fontPath, float rasterPixelHeight)
{
    auto source = std::make_unique<DynamicFontSource>();
    if (!source->Load(fontPath, rasterPixelHeight)) {
        FBZZ_LOG_ERROR("FontAtlas: 動的フォントを開けません: %s", fontPath.c_str());
        return false;
    }

    m_lineHeight      = source->GetLineHeight();
    m_base            = source->GetBase();
    m_fallbackAdvance = source->GetFallbackAdvance();
    m_dynamic         = std::move(source);

    /// @note この時点ではまだグリフもテクスチャも無い。最初の PrepareText で作られる。
    return true;
}

void FontAtlas::PrepareText(std::string_view utf8Text, ResourceManager& resources)
{
    if (!m_dynamic || utf8Text.empty()) return;

    /// @note 未登録のコードポイントだけを集める。
    std::vector<char32_t> missing;
    std::size_t offset = 0;
    while (offset < utf8Text.size()) {
        const char32_t code = util::Utf8::Decode(utf8Text, offset);
        if (code == 0 || code == U'\n') continue;
        if (m_glyphs.find(code) == m_glyphs.end())
            missing.push_back(code);
    }
    if (missing.empty()) return;

    /// @note 同じ文字が何度も出るテキストで重複ラスタライズしないよう畳む。
    std::sort(missing.begin(), missing.end());
    missing.erase(std::unique(missing.begin(), missing.end()), missing.end());

    m_dynamic->AddGlyphs(missing, m_glyphs, m_pages, resources);
}

bool FontAtlas::IsValid() const
{
    /// @note 動的モードは最初の PrepareText までページを持たないため、
    ///       ソースが開けている時点で有効とみなす (そうしないと初回の描画がまるごと落ちる)。
    if (m_dynamic) return m_dynamic->IsValid();

    for (const ResourceHandle<TextureTag>& page : m_pages)
        if (page.IsValid()) return true;
    return false;
}

const FontGlyph* FontAtlas::GetGlyph(char32_t codePoint) const
{
    const auto it = m_glyphs.find(codePoint);
    return it == m_glyphs.end() ? nullptr : &it->second;
}

float FontAtlas::GetKerning(char32_t previous, char32_t next) const
{
    /// @note 動的モードはフォントの kern テーブルを直接引く (.fnt のカーニング表を持たない)。
    if (m_dynamic) return m_dynamic->GetKerning(previous, next);

    if (m_kernings.empty()) return 0.0f;
    const auto it = m_kernings.find(MakeKerningKey(previous, next));
    return it == m_kernings.end() ? 0.0f : it->second;
}

ResourceHandle<TextureTag> FontAtlas::GetTexture(int page) const
{
    if (page < 0 || static_cast<std::size_t>(page) >= m_pages.size())
        return ResourceHandle<TextureTag>::Null();
    return m_pages[static_cast<std::size_t>(page)];
}

} // namespace fbzz::renderer
