/// @file    FontPreview.cpp
/// @brief   TTF/TTC/OTF の字形を独立した画像へラスタライズする。
/// @author  Hasegawa Jin
/// @date    2026-09-25
#include <Editor/Panels/FontPreview.hpp>

#include <Editor/EditorContext.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Engine/Renderer/IImGuiRenderer.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Util/FileSystem.hpp>

#include <stb_truetype.h>
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <system_error>

namespace fbzz::editor {
namespace {

constexpr double FILE_CHECK_INTERVAL = 0.5;

bool HasGlyph(const stbtt_fontinfo& font, char32_t codepoint)
{
    return stbtt_FindGlyphIndex(&font, static_cast<int>(codepoint)) != 0;
}

/// @brief フォントに存在しない字形は代替フォントで埋めず、見本から除く。
/// @see https://github.com/nothings/stb/blob/master/stb_truetype.h stb_truetype glyph metrics and bitmap API
bool DrawLine(const stbtt_fontinfo& font, std::u32string_view sample,
              float emPixels, int baseline, FontPreviewImage& image)
{
    const float scale = stbtt_ScaleForMappingEmToPixels(&font, emPixels);
    float lineWidth = 0.0f;
    char32_t previous = 0;
    for (const char32_t codepoint : sample) {
        if (codepoint != U' ' && !HasGlyph(font, codepoint)) continue;
        if (previous != 0)
            lineWidth += static_cast<float>(stbtt_GetCodepointKernAdvance(
                &font, static_cast<int>(previous), static_cast<int>(codepoint))) * scale;
        int advance = 0;
        int bearing = 0;
        stbtt_GetCodepointHMetrics(&font, static_cast<int>(codepoint), &advance, &bearing);
        lineWidth += static_cast<float>(advance) * scale;
        previous = codepoint;
    }

    float penX = (static_cast<float>(image.width) - lineWidth) * 0.5f;
    previous = 0;
    bool painted = false;
    for (const char32_t codepoint : sample) {
        if (codepoint != U' ' && !HasGlyph(font, codepoint)) continue;
        if (previous != 0)
            penX += static_cast<float>(stbtt_GetCodepointKernAdvance(
                &font, static_cast<int>(previous), static_cast<int>(codepoint))) * scale;

        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetCodepointBitmapBox(&font, static_cast<int>(codepoint),
                                    scale, scale, &x0, &y0, &x1, &y1);
        const int glyphWidth = x1 - x0;
        const int glyphHeight = y1 - y0;
        if (glyphWidth > 0 && glyphHeight > 0) {
            std::vector<unsigned char> bitmap(
                static_cast<std::size_t>(glyphWidth) * glyphHeight);
            stbtt_MakeCodepointBitmap(&font, bitmap.data(), glyphWidth, glyphHeight,
                                      glyphWidth, scale, scale, static_cast<int>(codepoint));
            const int glyphX = static_cast<int>(std::floor(penX)) + x0;
            const int glyphY = baseline + y0;
            for (int y = 0; y < glyphHeight; ++y) {
                const int targetY = glyphY + y;
                if (targetY < 0 || targetY >= static_cast<int>(image.height)) continue;
                for (int x = 0; x < glyphWidth; ++x) {
                    const int targetX = glyphX + x;
                    if (targetX < 0 || targetX >= static_cast<int>(image.width)) continue;
                    const unsigned int coverage = bitmap[static_cast<std::size_t>(y) * glyphWidth + x];
                    if (coverage == 0) continue;
                    std::uint8_t* pixel = image.rgba.data() +
                        (static_cast<std::size_t>(targetY) * image.width + targetX) * 4u;
                    constexpr unsigned int foreground[] = { 235u, 240u, 248u };
                    for (int channel = 0; channel < 3; ++channel)
                        pixel[channel] = static_cast<std::uint8_t>(
                            (foreground[channel] * coverage + pixel[channel] * (255u - coverage) + 127u) / 255u);
                    painted = true;
                }
            }
        }

        int advance = 0;
        int bearing = 0;
        stbtt_GetCodepointHMetrics(&font, static_cast<int>(codepoint), &advance, &bearing);
        penX += static_cast<float>(advance) * scale;
        previous = codepoint;
    }
    return painted;
}

} /// @note namespace

bool RasterizeFontPreview(std::string_view path, bool large, FontPreviewImage& out)
{
    out = {};
    std::vector<std::uint8_t> fontBytes;
    if (!util::FileSystem::ReadBinary(util::FileSystem::PathFromUtf8(std::string(path)), fontBytes)
        || fontBytes.empty()) return false;

    stbtt_fontinfo font{};
    /// @note TTC は複数書体を含むため、実行時の DynamicFontSource と同じく先頭を見本にする。
    const int offset = stbtt_GetFontOffsetForIndex(fontBytes.data(), 0);
    if (offset < 0 || !stbtt_InitFont(&font, fontBytes.data(), offset)) return false;

    out.width = large ? 512u : 128u;
    out.height = large ? 192u : 128u;
    out.rgba.resize(static_cast<std::size_t>(out.width) * out.height * 4u);
    for (std::size_t pixel = 0; pixel < out.rgba.size(); pixel += 4) {
        out.rgba[pixel] = 30;
        out.rgba[pixel + 1] = 35;
        out.rgba[pixel + 2] = 44;
        out.rgba[pixel + 3] = 255;
    }

    const bool latin = HasGlyph(font, U'A');
    const bool japanese = HasGlyph(font, U'あ');
    bool painted = false;
    if (!large) {
        if (latin && japanese)
            painted = DrawLine(font, U"Aあ", 62.0f, 89, out);
        else if (latin)
            painted = DrawLine(font, U"Aa", 76.0f, 91, out);
        else if (japanese)
            painted = DrawLine(font, U"あ", 78.0f, 92, out);
    } else {
        if (latin) {
            painted = DrawLine(font, U"Aa Bb Cc 0123456789", 50.0f, 79, out);
            if (!japanese)
                painted |= DrawLine(font, U"The quick brown fox", 37.0f, 153, out);
        }
        if (japanese)
            painted |= DrawLine(font, U"あいうえお 漢字", 49.0f, 155, out);
    }
    if (!painted) out = {};
    return painted;
}

void FontPreviewView::ReleaseTexture()
{
    if (auto* resources = renderer::ResourceManager::Active();
        resources && resources == m_resourceOwner &&
        resources->GetResetVersion() == m_resetVersion && m_texture.IsValid())
        resources->Release(m_texture);
    m_texture = {};
}

void FontPreviewView::Release()
{
    ReleaseTexture();
    m_path.clear();
    m_resourceOwner = nullptr;
    m_writeTime = 0;
    m_resetVersion = 0;
    m_nextFileCheck = 0.0;
    m_checkedFile = false;
    m_fileExists = false;
    m_attempted = false;
}

void FontPreviewView::Draw(EditorContext& ctx, std::string_view path, float previewHeight)
{
    if (!ctx.resources || !ctx.imguiRenderer) {
        ImGui::TextDisabled("Font preview is unavailable.");
        return;
    }

    const std::uint64_t resetVersion = ctx.resources->GetResetVersion();
    if (path != m_path || ctx.resources != m_resourceOwner || resetVersion != m_resetVersion) {
        Release();
        m_path = path;
        m_resourceOwner = ctx.resources;
        m_resetVersion = resetVersion;
    }

    const double now = ImGui::GetTime();
    if (now >= m_nextFileCheck) {
        std::error_code error;
        const auto writeTime = std::filesystem::last_write_time(
            util::FileSystem::PathFromUtf8(m_path), error);
        const bool exists = !error;
        const std::int64_t revision = exists
            ? static_cast<std::int64_t>(writeTime.time_since_epoch().count()) : 0;
        if (!m_checkedFile || revision != m_writeTime || exists != m_fileExists) {
            ReleaseTexture();
            m_writeTime = revision;
            m_fileExists = exists;
            m_attempted = false;
        }
        m_checkedFile = true;
        m_nextFileCheck = now + FILE_CHECK_INTERVAL;
    }

    if (!m_fileExists) {
        ImGui::TextDisabled("Font file not found.");
        return;
    }
    if (!m_attempted) {
        m_attempted = true;
        FontPreviewImage image;
        if (RasterizeFontPreview(m_path, true, image))
            m_texture = ctx.resources->CreateTexture(image.rgba.data(), image.width, image.height);
    }
    if (!m_texture.IsValid() || !ctx.resources->Get(m_texture)) {
        ImGui::TextDisabled("Could not render this font.");
        return;
    }
    void* rawId = ctx.imguiRenderer->GetImTextureID(m_texture, *ctx.resources);
    if (!rawId) {
        ImGui::TextDisabled("Font GPU view is unavailable.");
        return;
    }

    const float width = (std::max)(1.0f,
        (std::min)(ImGui::GetContentRegionAvail().x, previewHeight * (512.0f / 192.0f)));
    ImGui::Image(widgets::ToImTextureID(rawId), { width, width * (192.0f / 512.0f) });
    ImGui::TextDisabled("Glyphs absent from this font are omitted.");
}

} /// @note namespace fbzz::editor
