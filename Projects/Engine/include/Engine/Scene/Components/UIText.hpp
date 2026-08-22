/// @file UIText.hpp
/// @brief ランタイム UI のテキスト表示コンポーネント
/// @author Hasegawa Jin
/// @date 2026-05-23
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
    bool          enabled       = true;

    // ランタイム専用: UISystem が実測した文字の外接矩形サイズ。
    // WHY 持たせるか: アンカーとピボットは「自分のサイズ」が分からないと解けない。
    //     画像は scale.xy がそのままサイズだが、文字のサイズは字面とフォントで
    //     決まるので、描画時に測った結果を書き戻すしかない。
    //     Editor のギズモもこれを読んで、文字にも矩形を描く。
    math::Vector2 resolvedSize  = math::Vector2::ZERO;

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
    }
};

} // namespace fbzz::scene
