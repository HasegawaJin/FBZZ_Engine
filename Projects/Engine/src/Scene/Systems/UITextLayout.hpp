/// @file    UITextLayout.hpp
/// @brief   GPU に依存しない文字列の行分割・リッチテキスト・省略規則。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#pragma once
#include <Engine/Scene/Systems/UIInteraction.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Components/UICanvas.hpp>
#include <Engine/Scene/Components/UICanvasGroup.hpp>
#include <Engine/Scene/Components/UIRect.hpp>
#include <Engine/Scene/Components/UIImage.hpp>
#include <Engine/Scene/Components/UIButton.hpp>
#include <Engine/Scene/Components/UIText.hpp>
#include <Engine/Scene/Components/UIControls.hpp>
#include <Engine/Scene/Components/UILayoutGroup.hpp>
#include <Engine/Scene/Components/UIElement.hpp>
#include <Engine/Scene/UITheme.hpp>
#include <Engine/Asset/UIThemeAsset.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/Utf8.hpp>
#include <algorithm>
#include <cmath>
#include <optional>
#include <string_view>
namespace fbzz::scene::ui_runtime_detail {
/// @note このテキストを実画面で何 px の em として焼けばよいか。
/// @note fontSize は Canvas 空間の行高さなので、Canvas → 実画面の倍率を掛けて実寸へ直す。
inline float ResolveTextRasterPixelHeight(float fontSize, const UIInteractionContext& ctx)
{
    return renderer::FontAtlas::ResolveRasterPixelHeight(fontSize * ctx.textPixelScale);
}

/// @note 実際に使う文字サイズ。autoSize が解いた値があればそれを、無ければ指定値。
/// @note 計測・アトラス解像度・描画の 3 箇所が同じ値を要る。1 つでも text.fontSize を
/// @note 直接読むと、縮んだときだけ字が滲む。
inline float EffectiveFontSize(const UIText& text)
{
    return text.resolvedFontSize > 0.0f ? text.resolvedFontSize : text.fontSize;
}

/// @name リッチテキスト
/// @note タグは「文字を出さずに以降の見た目を変える」指示なので、走査の途中で畳めば
/// @note 行分割も描画も「文字と属性の並び」だけを見ればよくなる。
/// @note 入れ子に上限を置くのは、</color> の閉じ忘れで状態が積み上がり続けないため。
constexpr int kRichTextStackDepth = 8;
/// @note 疑似ボールドのずらし量と、疑似イタリックの傾き (どちらも em 比)。
constexpr float kFauxBoldOffset = 0.035f;
constexpr float kFauxItalicShear = 0.21f;

struct UIRichTextState {
    math::Vector4 color{ 1.0f, 1.0f, 1.0f, 1.0f };
    float scale  = 1.0f;   ///< @note 基準 fontSize に対する倍率
    bool  bold   = false;
    bool  italic = false;

    math::Vector4 colorStack[kRichTextStackDepth]{};
    float         scaleStack[kRichTextStackDepth]{};
    int colorDepth = 0;
    int scaleDepth = 0;
};

/// @note "#RRGGBB" / "#RRGGBBAA" / "RRGGBB" を色へ。読めなければ false。
inline bool ParseRichColor(std::string_view body, math::Vector4& out)
{
    if (!body.empty() && body.front() == '#') body.remove_prefix(1);
    if (body.size() != 6 && body.size() != 8) return false;
    const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    float channels[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    for (std::size_t i = 0; i + 1 < body.size(); i += 2) {
        const int hi = hex(body[i]);
        const int lo = hex(body[i + 1]);
        if (hi < 0 || lo < 0) return false;
        channels[i / 2] = static_cast<float>(hi * 16 + lo) / 255.0f;
    }
    out = { channels[0], channels[1], channels[2], channels[3] };
    return true;
}

/// @note タグ 1 つを状態へ畳む。認識できないタグは false (呼び出し側が普通の文字として扱う)。
inline bool ApplyRichTag(std::string_view tag, float baseFontSize, UIRichTextState& state,
                  bool& outLineBreak)
{
    outLineBreak = false;
    if (tag.empty()) return false;

    if (tag == "br") { outLineBreak = true; return true; }
    if (tag == "b")  { state.bold = true;   return true; }
    if (tag == "/b") { state.bold = false;  return true; }
    if (tag == "i")  { state.italic = true;  return true; }
    if (tag == "/i") { state.italic = false; return true; }

    if (tag == "/color") {
        if (state.colorDepth > 0) state.color = state.colorStack[--state.colorDepth];
        else                      state.color = { 1.0f, 1.0f, 1.0f, 1.0f };
        return true;
    }
    if (tag == "/size") {
        if (state.scaleDepth > 0) state.scale = state.scaleStack[--state.scaleDepth];
        else                      state.scale = 1.0f;
        return true;
    }

    const std::size_t equals = tag.find('=');
    if (equals == std::string_view::npos) return false;
    const std::string_view name  = tag.substr(0, equals);
    const std::string_view value = tag.substr(equals + 1);

    if (name == "color") {
        math::Vector4 parsed{};
        if (!ParseRichColor(value, parsed)) return false;
        if (state.colorDepth < kRichTextStackDepth)
            state.colorStack[state.colorDepth++] = state.color;
        state.color = parsed;
        return true;
    }
    if (name == "alpha") {
        math::Vector4 parsed{};
        /// @note alpha は 1 バイトだけ。色と同じ読み取りへ寄せるため 6 桁へ水増しする。
        std::string_view digits = value;
        if (!digits.empty() && digits.front() == '#') digits.remove_prefix(1);
        if (digits.size() != 2) return false;
        const std::string expanded = std::string("0000") + std::string(digits);
        if (!ParseRichColor(expanded, parsed)) return false;
        /// @note 展開した 3 バイト目が alpha 値になる。
        state.color.w = parsed.z;
        return true;
    }
    if (name == "size") {
        if (baseFontSize <= 0.0f || value.empty()) return false;
        /// @note std::stof は読めない文字列で例外を投げる。タグは手書きで誤記が普通に混ざるので、
        /// @note 「読めなければ普通の文字として出す」で受け止める。
        std::size_t cursor = 0;
        const bool relative = value[0] == '+' || value[0] == '-';
        const float sign = value[0] == '-' ? -1.0f : 1.0f;
        if (relative) cursor = 1;
        float magnitude = 0.0f;
        bool  anyDigit  = false;
        for (; cursor < value.size() && value[cursor] >= '0' && value[cursor] <= '9'; ++cursor) {
            magnitude = magnitude * 10.0f + static_cast<float>(value[cursor] - '0');
            anyDigit  = true;
        }
        if (cursor < value.size() && value[cursor] == '.') {
            ++cursor;
            float place = 0.1f;
            for (; cursor < value.size() && value[cursor] >= '0' && value[cursor] <= '9'; ++cursor) {
                magnitude += static_cast<float>(value[cursor] - '0') * place;
                place *= 0.1f;
                anyDigit = true;
            }
        }
        if (!anyDigit || cursor != value.size()) return false;

        const float target   = sign * magnitude;
        const float absolute = relative ? baseFontSize + target : target;
        if (absolute <= 0.0f) return false;
        if (state.scaleDepth < kRichTextStackDepth)
            state.scaleStack[state.scaleDepth++] = state.scale;
        state.scale = absolute / baseFontSize;
        return true;
    }
    return false;
}

/// @brief 次に描く文字を 1 つ返す。タグはここで消化して state へ畳む。
/// @return 0 で終端。改行は U'\n' で返す (`<br>` も同じ値へ畳む)。
/// @note 計測と描画で同じ関数を使う。タグの解釈が 2 箇所にあると「測った幅と描いた幅が違う」。
inline char32_t NextRichGlyph(const std::string& source, std::size_t& offset, bool richText,
                       float baseFontSize, UIRichTextState& state, std::size_t& outBegin)
{
    while (offset < source.size()) {
        outBegin = offset;
        if (richText && source[offset] == '<') {
            const std::size_t close = source.find('>', offset + 1);
            /// @note 閉じない '<' は普通の文字。行末の不等号を消してしまわない。
            if (close != std::string::npos) {
                const std::string_view tag(source.data() + offset + 1, close - offset - 1);
                bool lineBreak = false;
                if (ApplyRichTag(tag, baseFontSize, state, lineBreak)) {
                    offset = close + 1;
                    if (lineBreak) return U'\n';
                    /// @note 見た目を変えただけ。次の文字を探しに戻る。
                    continue;
                }
            }
        }
        return util::Utf8::Decode(source, offset);
    }
    return 0;
}

/// @note 文字列を行へ分割し、各行のバイト範囲と表示幅 (スケール適用済み) を out へ書く
/// @note (空文字列でも 1 行)。レイアウト計算と描画が同じ送り幅の規則を要るので 1 本化する。
/// @note バイト範囲まで返すのは、折り返しが元の文字列に改行文字の無い位置で行を切るため。
/// @note 描く側が改行文字だけを見ると、測った行数と描く行数が食い違う。
/// @param fontSize 試したい文字サイズ。autoSize の探索が別の値で呼び直すため、
/// @note text.fontSize ではなく引数で受ける。
inline void LayoutTextLines(const UIText& text,
                     const renderer::FontAtlas& atlas,
                     float fontSize,
                     std::vector<UITextLine>& out)
{
    out.clear();
    if (atlas.GetLineHeight() <= 0.0f) return;

    const float baseScale = fontSize / atlas.GetLineHeight();
    const float cellH     = fontSize;
    const float limit     = text.maxWidth > 0.0f ? text.maxWidth : 0.0f;

    /// @note 今の行の先頭バイト
    std::size_t lineBegin = 0;
    float       lineW     = 0.0f;
    /// @note その行でいちばん大きい字の倍率。`<size>` を使うと行の高さが変わる。
    float       lineScale = 1.0f;
    char32_t    previous  = 0;

    /// @note 直近の折り返し候補 (空白の直後)。単語の途中で切らないために覚えておく。
    bool        hasBreak    = false;
    /// @note candidate の行終端 (空白は含めない)
    std::size_t breakEnd    = 0;
    /// @note 次の行の先頭
    std::size_t breakNext   = 0;
    float       breakWidth  = 0.0f;

    UIRichTextState style{};
    /// @note 折り返しで巻き戻すときのために、行頭時点の見た目も控える。
    UIRichTextState breakStyle{};

    auto pushLine = [&](std::size_t end, float width) {
        UITextLine line{};
        line.begin  = lineBegin;
        line.end    = end;
        line.width  = width;
        line.height = cellH * lineScale;
        out.push_back(line);
        lineScale = 1.0f;
    };

    std::size_t offset = 0;
    while (offset < text.text.size()) {
        std::size_t charBegin = offset;
        const char32_t code =
            NextRichGlyph(text.text, offset, text.richText, fontSize, style, charBegin);
        if (code == 0) break;

        if (code == U'\n') {
            pushLine(charBegin, lineW);
            lineBegin = offset;
            lineW     = 0.0f;
            previous  = 0;
            hasBreak  = false;
            continue;
        }

        const float glyphScale = baseScale * style.scale;

        /// @note カーニングは「前の文字との組」に対して定義されるため、行頭では適用しない。
        const float kerning = previous != 0 ? atlas.GetKerning(previous, code) * glyphScale : 0.0f;
        const renderer::FontGlyph* glyph = atlas.GetGlyph(code);
        const float advance = (glyph ? glyph->advance : atlas.GetFallbackAdvance()) * glyphScale
                            + text.letterSpacing;
        const float next = lineW + kerning + advance;

        /// @note 折り返し候補は空白「の直後」。行末の空白まで幅に数えると、
        /// @note 右揃え・中央揃えで見えない余白のぶんだけ行がずれる。
        if (code == U' ' || code == U'\t') {
            hasBreak   = true;
            breakEnd   = charBegin;
            breakNext  = offset;
            breakWidth = lineW;
            breakStyle = style;
        }

        /// @note 折り返し。行頭の 1 文字目は、はみ出しても切らない (切ると無限に進まない)。
        /// @note 行の高さより先に判定する。この文字は次の行へ送られるので、先に lineScale へ
        /// @note 畳むと大きい字が「まだ載っていない行」の高さを押し上げる。
        if (limit > 0.0f && next > limit && charBegin > lineBegin) {
            if (hasBreak && breakEnd > lineBegin) {
                pushLine(breakEnd, breakWidth);
                lineBegin = breakNext;
                /// @note 空白の次から測り直す
                offset    = breakNext;
                style     = breakStyle;
            } else {
                /// @note 空白の無い長い連なり (日本語・URL 等) は文字単位で折る。
                /// @note charBegin はタグ消化後の位置なので、style を戻すとタグが 1 回失われる。
                pushLine(charBegin, lineW);
                lineBegin = charBegin;
                offset    = charBegin;
            }
            lineW    = 0.0f;
            previous = 0;
            hasBreak = false;
            continue;
        }

        lineScale = (std::max)(lineScale, style.scale);
        lineW     = next;
        previous  = code;
    }
    pushLine(text.text.size(), lineW);
}

/// @note 行の束の高さ。行送りの倍率はここでだけ掛ける。
inline float TotalTextHeight(const std::vector<UITextLine>& lines, float lineSpacing)
{
    float total = 0.0f;
    for (const UITextLine& line : lines) total += line.height * lineSpacing;
    return total;
}

/// @note 箱に入らない行を落とす。Ellipsis なら最後に残った行へ印を付ける。
/// @note 戻り値は落とした行があったか。
inline bool ApplyTextOverflow(const UIText& text, std::vector<UITextLine>& lines)
{
    if (text.maxHeight <= 0.0f || text.overflow == TextOverflow::Overflow) return false;

    float used = 0.0f;
    std::size_t kept = 0;
    for (const UITextLine& line : lines) {
        const float next = used + line.height * text.lineSpacing;
        if (next > text.maxHeight) break;
        used = next;
        ++kept;
    }
    /// @note 1 行も入らないなら 1 行だけは残す。何も出ないより、切れていても読めるほうがよい。
    if (kept == 0) kept = 1;
    if (kept >= lines.size()) return false;

    lines.resize(kept);
    if (text.overflow == TextOverflow::Ellipsis && !lines.empty())
        lines.back().ellipsis = true;
    return true;
}

/// @note autoSize が使う「この大きさで箱に入るか」の判定。
inline bool TextFitsBox(const UIText& text, const std::vector<UITextLine>& lines)
{
    if (text.maxHeight > 0.0f
        && TotalTextHeight(lines, text.lineSpacing) > text.maxHeight) return false;
    if (text.maxWidth > 0.0f) {
        for (const UITextLine& line : lines)
            if (line.width > text.maxWidth) return false;
    }
    return true;
}

/// @note 箱に入る範囲でいちばん大きい文字サイズを二分探索で選ぶ。
/// @note 送り幅はサイズに比例するのでどの段のアトラスで測っても比率は同じ。
/// @note サイズごとに焼き直すと 1 要素の計測で解像度違いの実体が探索回数ぶん生まれる。
inline float SolveAutoFontSize(const UIText& text, const renderer::FontAtlas& atlas,
                        std::vector<UITextLine>& scratch)
{
    const float upper = text.autoSizeMax > 0.0f ? text.autoSizeMax : text.fontSize;
    const float lower = (std::min)((std::max)(text.autoSizeMin, 1.0f), upper);

    UIText probe = text;
    /// @note 探索の途中で省略が挟まると「入った」の判定が意味を失う。素の状態で測る。
    probe.overflow = TextOverflow::Overflow;

    LayoutTextLines(probe, atlas, upper, scratch);
    if (TextFitsBox(probe, scratch)) return upper;

    /// @note 1 px 未満の差は画面に出ない。文字数の多い段落で探索が長引くほうが害になるため、
    /// @note 回数を切る。
    float low = lower, high = upper;
    for (int i = 0; i < 8 && high - low > 0.5f; ++i) {
        const float mid = (low + high) * 0.5f;
        LayoutTextLines(probe, atlas, mid, scratch);
        if (TextFitsBox(probe, scratch)) low = mid;
        else                             high = mid;
    }
    return low;
}
}
