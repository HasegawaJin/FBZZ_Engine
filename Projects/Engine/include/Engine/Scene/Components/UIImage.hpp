// FBZZ Engine
// UIImage.hpp | fbzz::scene
// UI スプライト描画コンポーネント
// 位置とサイズは GameObject::transform の local 値から読む。
// Texture は ResourceHandle で参照し、AssetManager / ResourceManager が所有する。
#pragma once
#include <Engine/Renderer/ResourceHandle.hpp>
#include <Math/Vector4.hpp>
#include <Math/Vector2.hpp>
#include <Engine/Scene/Script.hpp>
#include <string>

namespace fbzz::scene {

// UIImage の塗り潰し基点。fillAmount < 1 のとき、どの辺を起点に矩形を削るかを決める。
// 体力ゲージのように「左から減る」「下から伸びる」といった方向制御に使う。
enum class UIImageFillOrigin {
    Left   = 0, // 左端を固定し右側を削る (一般的な体力バー)
    Right  = 1, // 右端を固定し左側を削る
    Bottom = 2, // 下端を固定し上側を削る (縦ゲージ)
    Top    = 3, // 上端を固定し下側を削る
};

struct UIImage {
    std::string   texturePath = "";
    renderer::ResourceHandle<renderer::TextureTag> texture = {};
    math::Vector4 color  = { 1.0f, 1.0f, 1.0f, 1.0f };
    math::Vector2 uvMin  = { 0.0f, 0.0f };
    math::Vector2 uvMax  = { 1.0f, 1.0f };
    // 同一 Canvas 内の描画順。値が大きい要素ほど手前に描画する。
    int           sortOrder = 0;
    bool          enabled = true;
    // 塗り潰し量 [0,1]。1 で矩形全体、0 で非表示。体力・スタミナゲージ等の進捗表現に使う。
    // UISystem が描画時に矩形と UV を fillOrigin 方向へクリップする (transform.scale は変えない)。
    float             fillAmount = 1.0f;
    UIImageFillOrigin fillOrigin = UIImageFillOrigin::Left;
    // ランタイム専用: 最後にロードしたパスをキャッシュし、毎フレームの LoadTexture を回避する。
    std::string   loadedTexturePath = {};

    // ピクセル矩形 (x,y,w,h) をテクスチャサイズ (texW, texH) で正規化した UV ペアに変換する。
    // スクリプトからスプライトシートの切り抜き範囲を設定するときに使う。
    static std::pair<math::Vector2, math::Vector2> PixelRectToUV(
        float x, float y, float w, float h, float texW, float texH)
    {
        return { { x / texW, y / texH }, { (x + w) / texW, (y + h) / texH } };
    }

    const char* GetTypeName() const { return "UIImage"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled",     enabled);
        r.Field("texturePath", texturePath);
        r.Field("color",       color);
        r.Field("uvMin",       uvMin);
        r.Field("uvMax",       uvMax);
        r.Field("sortOrder",   sortOrder);
        r.FloatRange("fillAmount", fillAmount, 0.0f, 1.0f);
        static constexpr const char* kFillOriginLabels[] = { "Left", "Right", "Bottom", "Top" };
        int fill = static_cast<int>(fillOrigin);
        r.Enum("fillOrigin", fill, kFillOriginLabels);
        fill = (fill < 0 || fill > 3) ? 0 : fill;
        fillOrigin = static_cast<UIImageFillOrigin>(fill);
    }
};

} // namespace fbzz::scene
