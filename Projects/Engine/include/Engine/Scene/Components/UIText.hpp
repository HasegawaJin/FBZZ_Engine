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

// 複数行の行揃え。
//
// WHY 位置ではなく行揃えなのか:
//   以前の align は「position がテキストのどこを指すか」まで兼ねていて、
//   Center にした瞬間に文字が左へ半分ずれた。位置の基準は UIAnchor が持ち、
//   align は確保した矩形の中で各行をどちらへ寄せるかだけを決める。
//   おかげで「中央揃えの文字」と「中央に置いた文字」を別々に指定できる。
enum class TextAlign { Left, Center, Right };

// 箱の中で行の束をどこへ寄せるか。
//
// WHY 箱がないと意味を持たないか: 行の束の高さがそのまま矩形の高さなら、
//     上下に寄せる余地が無い。maxHeight を指定して初めて余りが生まれる。
enum class TextVerticalAlign { Top, Middle, Bottom };

// 箱に収まりきらない行をどう扱うか。
//   Overflow … そのまま溢れさせる (矩形の高さは maxHeight のまま)
//   Truncate … 入らない行を捨てる
//   Ellipsis … 入らない行を捨て、最後の行の末尾を "…" に置き換える
enum class TextOverflow { Overflow, Truncate, Ellipsis };

// 位置は GameObject::transform.localPosition.xy と anchoring で決まる。
struct UIText {
    // 親のどこを基準に、自分のどこを合わせるか (UIRect.hpp 参照)。
    // UIImage と完全に同じ規則で、同じ値を入れれば同じ場所に出る。
    UIAnchor      anchoring{};
    std::string   text          = "Text";
    float         fontSize      = 42.0f;
    float         letterSpacing = 4.0f;
    math::Vector4 color         = { 1.0f, 1.0f, 1.0f, 1.0f };
    // 同一 Canvas 内の描画順。値が大きい要素ほど手前に描画する。
    int           sortOrder     = 0;
    TextAlign     align         = TextAlign::Left;
    // フォントの指定。2 通りの書き方を受け付ける。
    //   ".ttf" / ".ttc" / ".otf" … そのファイルを直接指定。使った字だけ実行時に焼く
    //   拡張子なしのベースパス     … 静的アトラス。"<base>.fnt" (+ PNG) を読む
    // 例: "Assets/Fonts/MPLUS1p/MPLUS1p-Regular.ttf"
    //     "Assets/Fonts/Default/Roboto/Roboto-VariableFont_wdth,wght"
    // 空文字列なら ProjectSettings の既定フォントを使う。
    std::string   fontPath      = "";
    // UI マテリアル (.mat)。空なら組み込みの UIText で描く。
    //
    // WHY 文字にもマテリアルを許すか:
    //   見出しのグラデーション・縁取り・発光は、文字だけ組み込み固定にすると
    //   「画像で文字を作る」しか手が無くなる。そうすると文言の変更が
    //   画像の描き直しになり、多言語化も破綻する。
    //
    // 制約: t0 にはフォントアトラスが入り、`.r` は 0.5 を輪郭とする場
    //       (静的ならカバレッジ、動的なら SDF。UIText.hlsl 参照)。
    //       g_Rect はグリフ単位ではなく 0 が渡る (1 ドローに複数文字を詰めるため)、
    //       ので矩形基準の図形は描けない。UI/Material/UITextGradient.hlsl を参照。
    std::string   materialPath  = "";
    // 折り返し幅 (Canvas ピクセル)。0 で折り返さない。
    //
    // WHY 要るか: これが無いと文字の矩形は「字面の実測」でしかなく、長い文言ほど
    //   横へ伸び続ける。文言はローカライズや調整で必ず伸び縮みするので、
    //   「ここまでで折り返す」を指定できないと、レイアウトが文言の長さに支配される。
    //   幅を決めると align も本来の意味 (その幅の中での行揃え) で効くようになる。
    float         maxWidth      = 0.0f;
    // 箱の高さ (Canvas ピクセル)。0 で「行の束の高さがそのまま矩形の高さ」。
    //
    // WHY 要るか: 縦揃えも省略も自動縮小も「入れたい箱」が決まって初めて解ける。
    //     幅だけ決めても、文言が伸びた分は下へ流れ続けて隣の要素を踏む。
    float         maxHeight     = 0.0f;
    bool          enabled       = true;

    // 行送りの倍率。1 で fontSize ぶん、1.5 で 1.5 倍の間隔になる。
    //
    // WHY 加算 px ではなく倍率か: 行間は字の大きさに比例して見えるべき値で、
    //     px で持つと fontSize を変えるたびに入れ直すことになる。
    float             lineSpacing   = 1.0f;
    TextVerticalAlign verticalAlign = TextVerticalAlign::Top;
    TextOverflow      overflow      = TextOverflow::Overflow;

    // 先頭から何文字まで描くか。-1 で全部。単位は UTF-8 のバイトではなく文字。
    //
    // WHY コンポーネントに持たせるか (文字列を切って渡させないか):
    //   会話やチュートリアルの 1 文字ずつ表示は、文字列を毎フレーム切って
    //   SetText するのが素朴な実装になる。だが切る位置は UTF-8 の境界で
    //   なければならず、その判定を書く場所がスクリプトの数だけ増える。
    //   さらに文字列の再構築が毎フレーム走る。描く量だけを指定すれば、
    //   文字列は 1 つのまま、境界の判定も 1 箇所で済む。
    // NOTE: リッチテキストのタグは文字数に数えない (見えない指示のため)。
    int           visibleCharacters = -1;

    // 箱に収まるよう fontSize を自動で縮める。maxWidth / maxHeight が基準になる。
    //
    // WHY 要るか: 文言はローカライズと調整で必ず伸びる。伸びた側に合わせて
    //     全部を小さくすると短い文言が貧相になり、放置すると溢れる。
    //     「入る範囲でいちばん大きく」だけが両方を満たす。
    bool          autoSize      = false;
    float         autoSizeMin   = 8.0f;
    // 0 で fontSize を上限に使う (縮むだけで、指定より大きくはならない)。
    float         autoSizeMax   = 0.0f;

    // タグ付きの文字列として解釈する。
    //   <color=#RRGGBB> <color=#RRGGBBAA> </color>
    //   <alpha=#AA>
    //   <size=32> <size=+8> <size=-4> </size>   ※ 行の高さはその行の最も大きい字で決まる
    //   <b> </b>   疑似ボールド (同じ字をわずかにずらして 2 度描く)
    //   <i> </i>   疑似イタリック (行の中で字を傾ける)
    //   <br>       改行
    //
    // WHY 疑似ボールドか: 太字は本来 Bold ウェイトのフォントファイルが要る。
    //     フォント指定を 2 つに増やすと、指定漏れが「太字だけ出ない」という
    //     分かりにくい欠け方になる。1 フォントで完結する近似を既定にする。
    bool          richText      = false;

    // ランタイム専用: UISystem が実測した文字の外接矩形サイズ。
    // WHY 持たせるか: アンカーとピボットは「自分のサイズ」が分からないと解けない。
    //     画像は scale.xy がそのままサイズだが、文字のサイズは字面とフォントで
    //     決まるので、描画時に測った結果を書き戻すしかない。
    //     Editor のギズモもこれを読んで、文字にも矩形を描く。
    math::Vector2 resolvedSize  = math::Vector2::ZERO;
    // ランタイム専用: autoSize が実際に採用した文字サイズ。autoSize が false なら fontSize。
    //
    // WHY 持ち回すか: 計測と描画が別々に解き直すと、縮んだ結果が 1 フレームずれて
    //     文字が震える。解くのは計測の 1 回だけにして、描画はこの値を読む。
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
