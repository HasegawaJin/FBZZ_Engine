/// @file    UiTextFx.hpp
/// @brief   文字の色アニメーション。リッチテキストの `<color>` を 1 文字ずつ組み立てる
/// @author  Hasegawa Jin
/// @date    2026-09-07
///
/// @note UIText は 1 ドローに全グリフを詰めるため、シェーダーには «文字列の中のどこか»
///       が届かない (g_Rect は 0 で渡る ─ UITextGradient.hlsl の制約と同じ)。UISystem の
///       リッチテキストは `<color=#RRGGBBAA>` を頂点色として積めるので、文字ごとに色を
///       書いた文字列を毎フレーム組み直す。頂点色は UIText.color (ui.SetTextColor) に
///       掛かるので «絶対色» で書く ─ 呼ぶ側は color を白 (α だけ) にする約束。
///
/// 約束:
///   - 対象の UIText は richText = true にしておく (シーンかスクリプトで)
///   - 文字列の中の '<' はタグに読まれる。乱数グリフに '<' を使わない
///   - 毎フレーム SetText するのは変化があったときだけ (呼ぶ側が控えて比べる)
#pragma once

#include <Math/Vector4.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace sandbox::textfx {

/// UTF-8 を 1 文字 (コードポイント) ずつに切る。壊れた並びは 1 バイトずつ通す。
[[nodiscard]] inline std::vector<std::string> Split(std::string_view utf8)
{
    std::vector<std::string> out;
    out.reserve(utf8.size());
    for (std::size_t i = 0; i < utf8.size();) {
        const unsigned char c = static_cast<unsigned char>(utf8[i]);
        std::size_t n = 1;
        if      ((c & 0xE0) == 0xC0) n = 2;
        else if ((c & 0xF0) == 0xE0) n = 3;
        else if ((c & 0xF8) == 0xF0) n = 4;
        if (i + n > utf8.size()) n = 1;
        out.emplace_back(utf8.substr(i, n));
        i += n;
    }
    return out;
}

/// "#RRGGBBAA"。UISystem::ParseRichColor が読む形。
[[nodiscard]] inline std::string Hex(const ::fbzz::math::Vector4& c)
{
    const auto byte = [](float v) {
        return static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
    };
    char buf[12] = {};
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X%02X", byte(c.x), byte(c.y), byte(c.z), byte(c.w));
    return buf;
}

[[nodiscard]] inline ::fbzz::math::Vector4 Mix(const ::fbzz::math::Vector4& a,
                                               const ::fbzz::math::Vector4& b, float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t };
}

/// 1 文字を色で包む。
[[nodiscard]] inline std::string Wrap(std::string_view ch, const ::fbzz::math::Vector4& c)
{
    std::string s;
    s.reserve(ch.size() + 24);
    s += "<color=";
    s += Hex(c);
    s += '>';
    s += ch;
    s += "</color>";
    return s;
}

/// 走査。head (0 = 左端, 1 = 右端) を中心に幅 width の明るい帯が文字列を横切る。
/// 帯の外は base。head を [-width, 1+width] で動かせば、端から入って端へ抜ける。
///
/// @note 3 乗で落とす。線形だと帯の裾まで明るく «全体が白んだ» になる。
///       芯だけ立てると «光が舐めた» に見える (UITitleSheen と同じ判断)。
[[nodiscard]] inline std::string Sweep(std::string_view text, const ::fbzz::math::Vector4& base,
                                       const ::fbzz::math::Vector4& hi, float head, float width)
{
    const std::vector<std::string> chars = Split(text);
    const std::size_t n = chars.size();
    std::string out;
    out.reserve(text.size() * 26);
    const float w = std::max(width, 1.0e-3f);
    for (std::size_t i = 0; i < n; ++i) {
        const float x = n <= 1 ? 0.5f : static_cast<float>(i) / static_cast<float>(n - 1);
        const float d = std::clamp(1.0f - std::abs(x - head) / w, 0.0f, 1.0f);
        out += Wrap(chars[i], Mix(base, hi, d * d * d));
    }
    return out;
}

/// 2 色の染め分け。左端が left、右端が right、中央は base。strength で寄せる量。
///
/// @note 中央は残す。端から端まで塗ると «2 色のロゴ» になる。中央が素の色なら
///       «2 色のあいだに張られた文字» になる (UITitleSheen の左右染めと同じ理由)。
[[nodiscard]] inline std::string TwoTone(std::string_view text, const ::fbzz::math::Vector4& left,
                                          const ::fbzz::math::Vector4& base,
                                          const ::fbzz::math::Vector4& right, float strength)
{
    const std::vector<std::string> chars = Split(text);
    const std::size_t n = chars.size();
    std::string out;
    out.reserve(text.size() * 26);
    for (std::size_t i = 0; i < n; ++i) {
        const float x = n <= 1 ? 0.5f : static_cast<float>(i) / static_cast<float>(n - 1);
        const float toLeft  = std::clamp(1.0f - x * 2.0f, 0.0f, 1.0f);
        const float toRight = std::clamp(x * 2.0f - 1.0f, 0.0f, 1.0f);
        ::fbzz::math::Vector4 c = Mix(base, left, toLeft * strength);
        c = Mix(c, right, toRight * strength);
        out += Wrap(chars[i], c);
    }
    return out;
}

/// 解読。progress (0..1) で左から文字が確定していく。まだ確定していない文字は
/// 乱数グリフを scramble の色で出し、確定の直前の 1〜2 文字は settled と hot の間で光る。
///
/// @note 乱数器を持ち回ると呼ぶ側の状態になるため、seed (文字列ごと) と
///       tick (呼ぶ側が進める整数) から作る。同じ入力で同じ絵になり、tick を数フレーム
///       ごとに進めれば «ちらつき» の速さも決められる。乱数グリフに `<` は使わない
///       ─ リッチテキストのタグと読まれる。
[[nodiscard]] inline std::string Decode(std::string_view text, float progress, std::uint32_t seed,
                                        std::uint32_t tick, const ::fbzz::math::Vector4& settled,
                                        const ::fbzz::math::Vector4& hot,
                                        const ::fbzz::math::Vector4& scramble)
{
    static constexpr std::string_view kPool = "0123456789ABCDEF+-/#%";
    const std::vector<std::string> chars = Split(text);
    const std::size_t n = chars.size();
    if (n == 0) return {};
    std::string out;
    out.reserve(text.size() * 26 + 64);
    const float p = std::clamp(progress, 0.0f, 1.0f);
    /// @note 確定は文字 1 つぶん幅の斜面で進める (0/1 で切ると «カタカタ» とだけ見える)。
    const float edge = p * static_cast<float>(n + 1);
    for (std::size_t i = 0; i < n; ++i) {
        /// @note > 1 で確定、< 0 で未着手
        const float local = edge - static_cast<float>(i);
        if (chars[i] == " " || chars[i] == "\n") { out += chars[i]; continue; }
        if (local >= 1.0f) {
            /// @note 確定直後の 1.5 文字ぶんだけ熱を残す。
            const float heat = std::clamp(1.0f - (local - 1.0f) / 1.5f, 0.0f, 1.0f);
            out += Wrap(chars[i], Mix(settled, hot, heat * heat));
        } else if (local > -0.6f) {
            /// @note 確定の直前 1.6 文字ぶんだけ乱数グリフ。それより先を乱すと、幅の違う
            ///       グリフが並んで文字列全体が伸び縮みし、«解読» ではなく «崩れ» に見える。
            std::uint32_t h = seed * 2654435761u ^ (static_cast<std::uint32_t>(i) * 40503u)
                            ^ (tick * 2246822519u);
            h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
            const char g = kPool[h % kPool.size()];
            out += Wrap(std::string_view(&g, 1), scramble);
        } else {
            /// @note 未着手は本来の文字を透明で置く。幅が変わらないので確定済みの部分が動かない。
            out += Wrap(chars[i], { 0.0f, 0.0f, 0.0f, 0.0f });
        }
    }
    return out;
}

/// 頂点色を «制御値» として書く (UITextHero.hlsl 用)。
///   r = 出現 0..1 / g = 1 − 走査 / b = 極 (0 = ＋, 1 = −) / a = 不透明度
///
/// @note UIText には要素ごとのマテリアル上書きが無く時間も届かないため、色ではなく
///       制御値を積む。グリフごとに 4 つの数を渡せる経路は頂点色だけで、
///       UITextHero.hlsl はこの 4 つから溶け込み・走査・染め・色ずれを作る。
struct HeroGlyph {
    float reveal = 1.0f;
    float sheen  = 0.0f;
    float pol    = 0.5f;
    float alpha  = 1.0f;
};

/// 各文字の制御値を fn(i, n) で決めて文字列を組む。
template <class Fn>
[[nodiscard]] inline std::string Encode(std::string_view text, Fn&& fn)
{
    const std::vector<std::string> chars = Split(text);
    const std::size_t n = chars.size();
    std::string out;
    out.reserve(text.size() * 26);
    for (std::size_t i = 0; i < n; ++i) {
        const HeroGlyph g = fn(i, n);
        out += Wrap(chars[i], { g.reveal, 1.0f - std::clamp(g.sheen, 0.0f, 1.0f), g.pol, g.alpha });
    }
    return out;
}

/// 見出しの定番: 左から順に溶け込み (reveal)、周期で走査 (sweep)、左右の極 (pol)。
///   reveal   … 全体の出現 0..1 (文字ごとに 0.6 文字ぶん遅らせる)
///   sweepHead… 走査の中心 (-0.4..1.4 で端から端へ)。走らせないときは 9 など範囲外
[[nodiscard]] inline std::string Hero(std::string_view text, float reveal, float sweepHead,
                                      float sweepWidth = 0.35f, float alpha = 1.0f)
{
    return Encode(text, [&](std::size_t i, std::size_t n) {
        const float x = n <= 1 ? 0.5f : static_cast<float>(i) / static_cast<float>(n - 1);
        HeroGlyph g;
        /// @note 出現は左から。全体が 1 になる頃に右端も 1 になるよう、遅れは (1 - x) 側へ寄せる。
        g.reveal = std::clamp((reveal - x * 0.45f) / 0.55f, 0.0f, 1.0f);
        const float d = std::clamp(1.0f - std::abs(x - sweepHead) / std::max(sweepWidth, 1.0e-3f), 0.0f, 1.0f);
        g.sheen = d;
        g.pol   = x;
        g.alpha = alpha;
        return g;
    });
}

} // namespace sandbox::textfx
