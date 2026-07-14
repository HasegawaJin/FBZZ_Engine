// FBZZ Engine
// UIText.hpp | fbzz::scene
// ランタイム UI のテキスト表示コンポーネント
// デバッグ表示や簡易 UI の文字列・サイズ・色を保持する。
// fontPath が空の場合は内蔵 5x7 SDF アトラスを使用する。
// fontPath を指定した場合は gen_font_atlas.py で生成した PNG + FNT アトラスを使用する。
#pragma once
#include <Engine/Scene/Script.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector4.hpp>
#include <string>

namespace fbzz::scene {

enum class TextAlign { Left, Center, Right };

// 位置は GameObject::transform.localPosition.xy で管理する
struct UIText {
    std::string   text          = "Text";
    float         fontSize      = 42.0f;
    float         letterSpacing = 4.0f;
    math::Vector4 color         = { 1.0f, 1.0f, 1.0f, 1.0f };
    // 同一 Canvas 内の描画順。値が大きい要素ほど手前に描画する。
    int           sortOrder     = 0;
    TextAlign     align         = TextAlign::Left;
    // フォントアトラスのベースパス (拡張子なし)。
    // 例: "Assets/Fonts/Kenney/Future"
    //   → "Assets/Fonts/Kenney/Future.png" + ".fnt" を UISystem がロードする。
    // 空文字列のままにすると内蔵 SDF アトラス (後方互換) を使用する。
    std::string   fontPath      = "";
    bool          enabled       = true;

    const char* GetTypeName() const { return "UIText"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled",       enabled);
        r.Field("text",          text);
        r.FloatRange("fontSize", fontSize, 1.0f, 512.0f);
        r.FloatRange("letterSpacing", letterSpacing, 0.0f, 128.0f);
        r.Field("color",         color);
        r.Field("sortOrder",     sortOrder);
        static constexpr const char* kAlignLabels[] = { "Left", "Center", "Right" };
        int alignInt = static_cast<int>(align);
        r.Enum("align", alignInt, kAlignLabels);
        alignInt = (alignInt < 0 || alignInt > 2) ? 0 : alignInt;
        align = static_cast<TextAlign>(alignInt);
        r.Field("fontPath",      fontPath);
        r.Tooltip("生成済みフォントアトラスのベースパス。空欄では内蔵SDFフォントを使用します。");
    }
};

} // namespace fbzz::scene
