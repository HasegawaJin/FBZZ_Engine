/// @file    UITextRendering.cpp
/// @brief   テキストの表示設定、ページ別のグリフ頂点と描画バッチ。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#include "UIRendering.hpp"

#include "../../Scene/Systems/UI/UIRuntimeGeometry.hpp"
#include "../../Scene/Systems/UI/UITextLayout.hpp"

#include <Graphics/Renderer/Commands/DrawCall.hpp>
#include <Graphics/Renderer/IRenderer.hpp>
#include <Graphics/Renderer/Resources/ResourceManager.hpp>
#include <algorithm>

namespace fbzz::scene::ui_render_detail {

namespace {
using namespace ui_runtime_detail;

/// @note バッファ容量を超えるページは三角形境界で分割する。
constexpr uint32_t TEXT_VERTEX_CAPACITY = 4096;

void SubmitTextPages(const CanvasDrawContext& draw, const UIText& text, const renderer::FontAtlas& atlas);

void SubmitTextWithAtlas(const CanvasDrawContext& draw,
                         const UIText& text,
                         math::Vector2 position,
                         const math::Vector2& parentSize)
{
    const float fontSize = EffectiveFontSize(text);
    renderer::FontAtlas& atlas =
        GetOrLoadFontAtlas(text.fontPath, ResolveTextRasterPixelHeight(fontSize, draw.context),
                           draw.context, draw.resources);
    if (!atlas.IsValid() || atlas.GetLineHeight() <= 0.0f) return;

    /// @note 描画経路からも焼いておく。UITextSizeSystem を通らないテキストでもグリフが要る。
    /// @note 登録済みなら UTF-8 走査だけで戻る。
    atlas.PrepareText(text.text, draw.resources);

    const float baseScale = fontSize / atlas.GetLineHeight();

    /// @note 整列と改行位置の決定に行分割が要るので常に事前計算する。
    /// @note Left だけ別扱いにすると UTF-8 走査が 2 通りになり、そちらの方がズレの温床になる。
    std::vector<UITextLine>& lines = draw.context.textLineScratch;
    LayoutTextLines(text, atlas, fontSize, lines);
    ApplyTextOverflow(text, lines);
    if (lines.empty()) return;

    /// @note 実測サイズは UITextSizeSystem が同じフレームの描画前に確定させている。
    /// @note 測るのは ComputeTextLogicalSize だけ。2 箇所で出すと描画位置と当たり判定がずれる。
    const math::Vector2 textSize = (text.resolvedSize.x > 0.0f || text.resolvedSize.y > 0.0f)
        ? text.resolvedSize
        : math::Vector2{ 0.0f, lines.front().height };

    /// @note 矩形の左上。ここから先は画像とまったく同じ規則で置かれる。
    const UIRect textRect = ResolveUIRect(parentSize, position, textSize, text.anchoring);

    /// @note 縦揃え。箱に余りがあるときだけ意味を持つ (余りが無ければ 0 になる)。
    const float contentHeight = TotalTextHeight(lines, text.lineSpacing);
    const float verticalSpace = (std::max)(0.0f, textSize.y - contentHeight);
    const float originY = textRect.position.y
        + (text.verticalAlign == TextVerticalAlign::Middle ? verticalSpace * 0.5f
         : text.verticalAlign == TextVerticalAlign::Bottom ? verticalSpace : 0.0f);

    /// @note align は「確保した矩形の中で行をどちらへ寄せるか」だけを決める。
    /// @note position の意味まで変えると、中央揃えにした瞬間に文字が左へ半分ずれる。
    auto lineStartX = [&](float lineWidth) -> float {
        if (text.align == TextAlign::Center) return textRect.position.x + (textSize.x - lineWidth) * 0.5f;
        if (text.align == TextAlign::Right)  return textRect.position.x + (textSize.x - lineWidth);
        return textRect.position.x;
    };

    /// @note ページごとに頂点を分ける。BMFont のマルチページアトラスではグリフごとに参照
    /// @note テクスチャが変わるので、まとめてから分ければ切り替えが最小回数で済む。
    std::vector<std::vector<UIVertex2D>>& pageVertices = draw.context.textPageScratch;
    if (pageVertices.size() < atlas.GetPageCount())
        pageVertices.resize(atlas.GetPageCount());
    if (pageVertices.empty()) return;
    const std::size_t pageCount = atlas.GetPageCount();
    for (std::size_t pageIndex = 0; pageIndex < pageCount; ++pageIndex) pageVertices[pageIndex].clear();
    pageVertices[0].reserve(text.text.size() * 6);

    /// @note グリフ 1 つを四角形として積む。マスクのクリップもここで通す。
    /// @note ドロー単位で切ると「はみ出した 1 文字のために行ごと消える」になる。
    /// @note 枠のところで文字が半分だけ見えるのが正しい。
    const auto emitGlyph = [&](const renderer::FontGlyph& glyph, float left, float top,
                               float right, float bottom, float shear, const math::Vector4& color) {
        const std::size_t pageIndex =
            (glyph.page >= 0 && static_cast<std::size_t>(glyph.page) < pageVertices.size())
                ? static_cast<std::size_t>(glyph.page) : 0;
        std::vector<UIVertex2D>& vertices = pageVertices[pageIndex];

        /// @note 疑似イタリックは下端を固定して上端をずらす。字が浮かないよう軸は足元。
        const ClipVertex quad[4] = {
            { { left + shear,  top    }, { glyph.u0, glyph.v0 }, color },
            { { left,          bottom }, { glyph.u0, glyph.v1 }, color },
            { { right,         bottom }, { glyph.u1, glyph.v1 }, color },
            { { right + shear, top    }, { glyph.u1, glyph.v0 }, color },
        };
        ClipVertex clippedGlyph[MAX_CLIP_POLYGON_VERTICES];
        const int clippedVertexCount = ClipPolygonToPlanes(draw.context.clipPlanes, quad, 4,
            clippedGlyph, MAX_CLIP_POLYGON_VERTICES);
        if (clippedVertexCount < 3) return;
        for (int triangleIndex = 1; triangleIndex + 1 < clippedVertexCount; ++triangleIndex) {
            const auto& first = clippedGlyph[0];
            const auto& second = clippedGlyph[triangleIndex];
            const auto& third = clippedGlyph[triangleIndex + 1];
            vertices.push_back({ first.position, first.uv, first.color });
            vertices.push_back({ second.position, second.uv, second.color });
            vertices.push_back({ third.position, third.uv, third.color });
        }
    };

    /// @note 省略記号の送り幅。行末をどこで打ち切るかの判定に要る。
    const renderer::FontGlyph* ellipsisGlyph = atlas.GetGlyph(U'…');
    const float ellipsisAdvance = ellipsisGlyph
        ? ellipsisGlyph->advance * baseScale + text.letterSpacing : 0.0f;

    /// @note 描く文字数の上限。-1 は無制限。計測は全文で行う ─ 1 文字ずつ出す間に箱が
    /// @note 伸び縮みすると周りのレイアウトが毎フレーム動く。
    const int visibleLimit = text.visibleCharacters;
    int drawnCharacters = 0;

    /// @note 見た目の状態は行をまたいで続く。行ごとに作り直すと </color> が行頭で切れる。
    UIRichTextState style{};
    std::size_t cursor = 0;
    float penY = originY;

    for (std::size_t lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
        const UITextLine& line = lines[lineIndex];
        float penX = lineStartX(line.width);
        /// @note 省略する行は、記号ぶんの幅を残したところで打ち切る。
        /// @note 基準は箱の右端であって、行の開始位置ではない。
        const float lineLimitX = textRect.position.x + textSize.x - ellipsisAdvance;

        char32_t previousCodePoint = 0;
        cursor = line.begin;
        bool truncatedHere = false;

        while (cursor < line.end) {
            std::size_t characterStart = cursor;
            const char32_t codePoint =
                NextRichGlyph(text.text, cursor, text.richText, fontSize, style, characterStart);
            if (codePoint == 0 || codePoint == U'\n') break;
            /// @note 行末がタグで終わっていると、走査はタグを食べたあと次の行の 1 文字目まで
            /// @note 読み進んでしまう。見た目の変化は残したまま、字だけをここで止める。
            if (characterStart >= line.end) { cursor = characterStart; break; }
            if (visibleLimit >= 0 && drawnCharacters >= visibleLimit) { truncatedHere = true; break; }

            const float glyphScale = baseScale * style.scale;
            if (previousCodePoint != 0)
                penX += atlas.GetKerning(previousCodePoint, codePoint) * glyphScale;
            previousCodePoint = codePoint;

            const renderer::FontGlyph* glyph = atlas.GetGlyph(codePoint);
            if (!glyph) {
                penX += atlas.GetFallbackAdvance() * glyphScale + text.letterSpacing;
                ++drawnCharacters;
                continue;
            }
            if (line.ellipsis && penX + glyph->advance * glyphScale > lineLimitX) {
                truncatedHere = true;
                break;
            }

            /// @note 空白文字のように画像を持たないグリフは、送りだけ進めて四角形を出さない。
            if (glyph->width > 0.0f && glyph->height > 0.0f) {
                /// @note 行の下端を揃える。`<size>` で大きい字が混ざっても足元が動かない。
                const float baselineShift = (line.height - fontSize * style.scale);
                const float glyphLeft = penX + glyph->xOffset * glyphScale;
                const float glyphTop = penY + baselineShift + glyph->yOffset * glyphScale;
                const float glyphRight = glyphLeft + glyph->width * glyphScale;
                const float glyphBottom = glyphTop + glyph->height * glyphScale;
                const float shear = style.italic ? (glyphBottom - glyphTop) * kFauxItalicShear : 0.0f;

                emitGlyph(*glyph, glyphLeft, glyphTop, glyphRight, glyphBottom, shear, style.color);
                if (style.bold) {
                    const float boldOffset = fontSize * style.scale * kFauxBoldOffset;
                    emitGlyph(*glyph, glyphLeft + boldOffset, glyphTop, glyphRight + boldOffset,
                        glyphBottom, shear, style.color);
                }
            }

            penX += glyph->advance * glyphScale + text.letterSpacing;
            ++drawnCharacters;
        }

        if (line.ellipsis && truncatedHere && ellipsisGlyph
            && ellipsisGlyph->width > 0.0f && ellipsisGlyph->height > 0.0f) {
            const float baselineShift = line.height - fontSize;
            const float glyphLeft = penX + ellipsisGlyph->xOffset * baseScale;
            const float glyphTop = penY + baselineShift + ellipsisGlyph->yOffset * baseScale;
            emitGlyph(*ellipsisGlyph, glyphLeft, glyphTop,
                glyphLeft + ellipsisGlyph->width * baseScale,
                glyphTop + ellipsisGlyph->height * baseScale, 0.0f, style.color);
        }

        penY += line.height * text.lineSpacing;
        if (visibleLimit >= 0 && drawnCharacters >= visibleLimit) break;
    }
    SubmitTextPages(draw, text, atlas);
}

void SubmitTextPages(const CanvasDrawContext& draw, const UIText& text, const renderer::FontAtlas& atlas)
{
    /// @note 定数バッファは全ページ・全チャンク共通なので 1 回だけ更新する。
    UIConstantsCB constants{};
    constants.ortho  = draw.canvasToClip;
    constants.color  = text.color;
    constants.uvRect = { 0.0f, 0.0f, 1.0f, 1.0f };
    /// @note テキストは 1 ドローに複数グリフを詰めるため、矩形が 1 つに定まらない。
    /// @note 図形を描くための寸法は無い、という意味でゼロを渡す。
    constants.rect   = { 0.0f, 0.0f, 0.0f, 0.0f };
    draw.resources.Update(draw.context.constants, &constants, sizeof(constants));

    renderer::DrawCall drawCall;
    /// @note UIText.hlsl の .r チャンネルを coverage として使い、alpha チャンネル依存を排除する。
    drawCall.shader             = draw.context.textShader;
    drawCall.pipelineState      = draw.pipelineState;
    drawCall.constantBuffers[0] = draw.context.constants;
    drawCall.layer              = draw.layer;
    drawCall.topology           = renderer::PrimitiveTopology::TRIANGLE_LIST;

    /// @note 未設定 (大多数) なら nullptr が返り、以降は組み込みのテキスト描画になる。
    if (const UIMaterialBinding* material =
            ResolveUIMaterial(draw.context, draw.resources, text.materialPath);
        material && material->valid) {
        BindUIMaterial(draw, *material, drawCall, 1);
    }

    const auto& pageVertices = draw.context.textPageScratch;
    for (std::size_t pageIndex = 0; pageIndex < pageVertices.size(); ++pageIndex) {
        const std::vector<UIVertex2D>& vertices = pageVertices[pageIndex];
        if (vertices.empty()) continue;

        drawCall.textures[0] = atlas.GetTexture(static_cast<int>(pageIndex));
        if (!drawCall.textures[0].IsValid()) continue;

        /// @note kTextVBVertices を超えるテキストはチャンク分割して複数ドローで描く。
        /// @note 3 の倍数へ丸めるのは、4096 が 3 で割り切れず、そのまま切ると境目に
        /// @note 三角形 1 枚に足りない頂点が残って画面を横切る破片になるため。
        /// @note クリップで 1 文字あたりの頂点数が一定でなくなった今は短い文章でも当たりうる。
        constexpr uint32_t TEXT_CHUNK_VERTEX_COUNT = (TEXT_VERTEX_CAPACITY / 3) * 3;
        uint32_t vertexOffset = 0;
        const uint32_t totalVertices = static_cast<uint32_t>(vertices.size());
        while (vertexOffset < totalVertices) {
            const uint32_t chunkVertexCount = (std::min)(totalVertices - vertexOffset, TEXT_CHUNK_VERTEX_COUNT);
            /// @note チャンクごとに別実体を借りる。1 本を上書きすると、記録型バックエンドでは
            /// @note 全チャンクが最後のグリフ群になる。
            const auto vertexBuffer = draw.context.vertexPool.Acquire(
                draw.resources, TEXT_VERTEX_CAPACITY, static_cast<uint32_t>(sizeof(UIVertex2D)));
            if (!vertexBuffer.IsValid()) break;
            draw.resources.Update(vertexBuffer, vertices.data() + vertexOffset, chunkVertexCount * sizeof(UIVertex2D));
            drawCall.vertexBuffer = vertexBuffer;
            drawCall.vertexCount  = chunkVertexCount;
            draw.device.Submit(drawCall, draw.resources);
            vertexOffset += chunkVertexCount;
        }
    }
}

void SubmitText(const CanvasDrawContext& draw,
                const UIText& text,
                math::Vector2 position,
                const math::Vector2& parentSize)
{
    if (!text.enabled || text.text.empty() || text.fontSize <= 0.0f) return;

    if (text.fontPath.empty()) {
        UIText textWithDefaultFont = text;
        textWithDefaultFont.fontPath = draw.context.defaultFontPath;
        /// @note resolvedSize は UITextSizeSystem が本体へ書き済み。複製は fontPath を
        /// @note 補うだけなので、書き戻すものは何も無い。
        SubmitTextWithAtlas(draw,
                            textWithDefaultFont, position, parentSize);
        return;
    }
    SubmitTextWithAtlas(draw,
                        text, position, parentSize);
}

} /// @note namespace

void DrawTextElement(const CanvasDrawContext& draw, const UIText& text, const UITransform2D& transform,
    float groupOpacity, const asset::UIThemeStyle* theme)
{
    if (!theme && groupOpacity >= 1.0f) {
        SubmitText(draw, text, transform.position, transform.parentSize);
        return;
    }

    /// @note 実行時のテーマと透明度を保存対象のコンポーネントへ書き戻さない。
    UIText styledText = text;
    if (theme) ApplyUIThemeToText(*theme, styledText);
    styledText.color.w *= groupOpacity;
    SubmitText(draw, styledText, transform.position, transform.parentSize);
}

} /// @note namespace fbzz::scene::ui_render_detail
