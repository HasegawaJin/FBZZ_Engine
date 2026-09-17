/// @file    UIText.hpp
/// @brief   ランタイム UI のテキスト表示コンポーネント
/// @author  Hasegawa Jin
/// @date    2026-05-23
///
/// 文字列・サイズ・色と、フォントの指定を持つ。フォントの読み込み規則は
/// FontAtlas.hpp を参照 (fontPath が空なら ProjectSettings の既定フォント)。
#pragma once
#include <Engine/Scene/Components/UIRect.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector4.hpp>
#include <string>

namespace fbzz::scene {

/// 複数行の行揃え。
/// @note 以前は position がテキストのどこを指すかも兼ね、Center で文字が左へ半分ずれた。
///       位置の基準は UIAnchor が持ち、align は矩形内で行をどちらへ寄せるかだけを決める。
enum class TextAlign { Left, Center, Right };

/// 箱の中で行の束をどこへ寄せるか。
/// @note 行の束の高さがそのまま矩形の高さなら寄せる余地が無い。maxHeight 指定で余りが生まれる。
enum class TextVerticalAlign { Top, Middle, Bottom };

/// 箱に収まりきらない行をどう扱うか。
///   Overflow … そのまま溢れさせる (矩形の高さは maxHeight のまま)
///   Truncate … 入らない行を捨てる
///   Ellipsis … 入らない行を捨て、最後の行の末尾を "…" に置き換える
enum class TextOverflow { Overflow, Truncate, Ellipsis };

/// 位置は GameObject::transform.localPosition.xy と anchoring で決まる。
struct UIText {
    /// 親のどこを基準に、自分のどこを合わせるか (UIRect.hpp 参照)。
    /// UIImage と完全に同じ規則で、同じ値を入れれば同じ場所に出る。
    UIAnchor      anchoring{};
    std::string   text          = "Text";
    float         fontSize      = 42.0f;
    float         letterSpacing = 4.0f;
    math::Vector4 color         = { 1.0f, 1.0f, 1.0f, 1.0f };
    /// 同一 Canvas 内の描画順。値が大きい要素ほど手前に描画する。
    int           sortOrder     = 0;
    TextAlign     align         = TextAlign::Left;
    /// フォントの指定。2 通りの書き方を受け付ける。
    ///   ".ttf" / ".ttc" / ".otf" … そのファイルを直接指定。使った字だけ実行時に焼く
    ///   拡張子なしのベースパス     … 静的アトラス。`<base>.fnt` (+ PNG) を読む
    /// 例: "Assets/Fonts/MPLUS1p/MPLUS1p-Regular.ttf"
    ///     "Assets/Fonts/Default/Roboto/Roboto-VariableFont_wdth,wght"
    /// 空文字列なら ProjectSettings の既定フォントを使う。
    std::string   fontPath      = "";
    /// UI マテリアル (.mat)。空なら組み込みの UIText で描く。グラデーション・縁取り・発光を
    /// 文字ごと画像化せず表現する (多言語化を壊さないため)。
    /// @note 契約: t0 はフォントアトラス、 `.r` は 0.5 を輪郭とする値 (静的はカバレッジ、動的は SDF)。
    ///       g_Rect はグリフ単位でなく 0 (1 ドローに複数文字を詰めるため) — 矩形基準の図形は不可。
    ///       詳細は `UI/Material/UITextGradient.hlsl` を参照。
    std::string   materialPath  = "";
    /// 折り返し幅 (Canvas ピクセル)。0 で折り返さない。
    /// @note 無いと文言の長さがそのままレイアウトを支配する。幅を決めると align も
    ///       「その幅の中での行揃え」として意味を持つ。
    float         maxWidth      = 0.0f;
    /// 箱の高さ (Canvas ピクセル)。0 で「行の束の高さがそのまま矩形の高さ」。
    /// @note 縦揃え・省略・自動縮小はこの高さが無いと解けない。無いと文言が伸びた分だけ
    ///       下へ流れ続け隣の要素を踏む。
    float         maxHeight     = 0.0f;
    bool          enabled       = true;

    /// 行送りの倍率。1 で fontSize ぶん、1.5 で 1.5 倍の間隔になる。
    /// @note px でなく倍率にする理由: 行間は字の大きさに比例すべき値で、px だと
    ///       fontSize を変えるたびに入れ直しになる。
    float             lineSpacing   = 1.0f;
    TextVerticalAlign verticalAlign = TextVerticalAlign::Top;
    TextOverflow      overflow      = TextOverflow::Overflow;

    /// 先頭から何文字まで描くか。-1 で全部。単位は UTF-8 のバイトではなく文字。
    /// @note 文字列を毎フレーム切って SetText する実装は UTF-8 境界判定がスクリプトの数だけ
    ///       増えるため、描く量の指定に一本化する。リッチテキストのタグは数えない。
    int           visibleCharacters = -1;

    /// 箱に収まるよう fontSize を自動で縮める。maxWidth / maxHeight が基準になる。
    /// @note 文言は必ず伸び縮みする。全体を縮めると短い文言が貧相になり、放置すると溢れるので、
    ///       「入る範囲でいちばん大きく」に一本化する。
    bool          autoSize      = false;
    float         autoSizeMin   = 8.0f;
    /// 0 で fontSize を上限に使う (縮むだけで、指定より大きくはならない)。
    float         autoSizeMax   = 0.0f;

    /// タグ付きの文字列として解釈する。
    ///   `<color=#RRGGBB> <color=#RRGGBBAA> </color> <alpha=#AA>`
    ///   `<size=32> <size=+8> <size=-4> </size>` (行高はその行の最大サイズで決まる)
    ///   `<b> </b>` 疑似ボールド (字を僅かにずらし 2 度描く)  `<i> </i>` 疑似イタリック  `<br>` 改行
    /// @note 疑似ボールドは Bold フォント追加による「太字だけ出ない」欠けを避けるための近似。
    bool          richText      = false;

    /// ランタイム専用: UISystem が実測した文字の外接矩形サイズ。
    /// @note アンカー/ピボットの解決に自分のサイズが要るが、文字は字面とフォントでサイズが
    ///       決まるため描画時の実測を書き戻す。Editor のギズモもこれを読んで矩形を描く。
    math::Vector2 resolvedSize  = math::Vector2::ZERO;
    /// ランタイム専用: autoSize が実際に採用した文字サイズ。autoSize が false なら fontSize。
    /// @note 計測と描画で別々に解き直すと縮んだ結果が 1 フレームずれて字が震えるため、
    ///       計測の 1 回だけで解き描画はこの値を読む。
    float         resolvedFontSize = 0.0f;

    const char* GetTypeName() const { return "UIText"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled",       enabled);
        anchoring.Reflect(r);
        r.Field("text",          text);
        r.FloatRange("fontSize", fontSize, 1.0f, 512.0f);
        r.FloatRange("letterSpacing", letterSpacing, 0.0f, 128.0f);
        r.ColorField("color",         color);
        r.Field("sortOrder",     sortOrder);
        static constexpr const char* kAlignLabels[] = { "Left", "Center", "Right" };
        int alignInt = static_cast<int>(align);
        r.Enum("align", alignInt, kAlignLabels);
        alignInt = (alignInt < 0 || alignInt > 2) ? 0 : alignInt;
        align = static_cast<TextAlign>(alignInt);
        r.Field("fontPath",      fontPath);
        r.Tooltip(".ttf / .ttc / .otf を直接指定するか、静的アトラスのベースパス "
                  "(拡張子なし) を指定します。空欄ではプロジェクトの既定フォントを使用します");
        r.Field("materialPath",  materialPath);
        r.Tooltip("UI マテリアル (.mat)。空欄で組み込みのテキスト描画。"
                  "render_path = \"ui\" のものだけが使えます");
        r.FloatRange("maxWidth", maxWidth, 0.0f, 4096.0f);
        r.Tooltip("この幅 (Canvas px) で折り返します。0 で折り返さない。"
                  "指定するとこの幅が矩形の幅になり、align はその中での行揃えになります");
        r.FloatRange("maxHeight", maxHeight, 0.0f, 4096.0f);
        r.Tooltip("箱の高さ (Canvas px)。0 で行の束の高さがそのまま矩形になります。"
                  "縦揃え・省略・自動縮小はこの高さを基準にします");

        r.Group("Layout");
        r.FloatRange("lineSpacing", lineSpacing, 0.1f, 4.0f);
        r.Tooltip("行送りの倍率。1 で fontSize ぶん");
        static constexpr const char* kVAlignLabels[] = { "Top", "Middle", "Bottom" };
        int vAlignInt = static_cast<int>(verticalAlign);
        r.Enum("verticalAlign", vAlignInt, kVAlignLabels);
        vAlignInt = (vAlignInt < 0 || vAlignInt > 2) ? 0 : vAlignInt;
        verticalAlign = static_cast<TextVerticalAlign>(vAlignInt);
        r.Tooltip("maxHeight を指定したときだけ効きます");
        static constexpr const char* kOverflowLabels[] = { "Overflow", "Truncate", "Ellipsis" };
        int overflowInt = static_cast<int>(overflow);
        r.Enum("overflow", overflowInt, kOverflowLabels);
        overflowInt = (overflowInt < 0 || overflowInt > 2) ? 0 : overflowInt;
        overflow = static_cast<TextOverflow>(overflowInt);

        r.Group("Fitting");
        r.Field("autoSize", autoSize);
        r.Tooltip("maxWidth / maxHeight に収まる範囲でいちばん大きい文字サイズを選びます");
        r.FieldIf("autoSizeMin", autoSizeMin, autoSize, "これ以上は縮めません");
        r.FieldIf("autoSizeMax", autoSizeMax, autoSize,
                  "上限。0 で fontSize を上限にします (縮むだけ)");
        r.Readonly("resolvedFontSize", resolvedFontSize);

        r.Group("Rich Text");
        r.Field("richText", richText);
        r.Tooltip("<color=#RRGGBB> </color> / <alpha=#AA> / <size=32> </size> / "
                  "<b> </b> / <i> </i> / <br> を解釈します");
        r.Field("visibleCharacters", visibleCharacters);
        r.Tooltip("先頭から何文字まで描くか。-1 で全部。"
                  "1 文字ずつ出す演出はこの値を進めます (タグは数えません)");
    }
};

} // namespace fbzz::scene
