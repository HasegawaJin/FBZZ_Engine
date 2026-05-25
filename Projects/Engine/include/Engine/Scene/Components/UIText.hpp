// FBZZ Engine
// UIText.hpp | fbzz::scene
// ランタイム UI のテキスト表示コンポーネント
// デバッグ表示や簡易 UI の文字列・サイズ・色を保持する。
// 本格的なフォント管理は Renderer / UI 側の責務に分ける。
#pragma once
#include <Engine/Scene/Script.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector4.hpp>
#include <string>

namespace fbzz::scene {

// 位置は GameObject::transform.localPosition.xy で管理する
struct UIText {
    std::string   text          = "Text";
    float         fontSize      = 42.0f;
    float         letterSpacing = 4.0f;
    math::Vector4 color         = { 1.0f, 1.0f, 1.0f, 1.0f };
    bool          enabled       = true;

    const char* GetTypeName() const { return "UIText"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled",       enabled);
        r.Field("text",          text);
        r.Field("fontSize",      fontSize);
        r.Field("letterSpacing", letterSpacing);
        r.Field("color",         color);
    }
};

} // namespace fbzz::scene
