/// @file    UIImage.hpp
/// @brief   UI スプライト描画コンポーネント。
/// @author  Hasegawa Jin
/// @date    2026-05-23
///
/// 位置とサイズは GameObject::transform の local 値から読む。
/// Texture は ResourceHandle で参照し、AssetManager / ResourceManager が所有する。
#pragma once
#include <Engine/Renderer/ResourceHandle.hpp>
#include <Math/Vector4.hpp>
#include <Math/Vector2.hpp>
#include <Engine/Scene/Components/UIRect.hpp>
#include <Engine/Scene/Script.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace fbzz::scene {

/// Canvas 1 単位あたりのピクセル数。Sprite の pixelsPerUnit を UI の寸法へ直す基準。
/// @note UI 座標は Canvas ピクセルだが pixelsPerUnit は「ワールド 1m あたりのピクセル数」で桁が違う。
///       pixelsPerUnit = 100 の素材を「1px = 1 Canvas px」と定義して橋渡しする。
inline constexpr float kUIReferencePixelsPerUnit = 100.0f;

/// UIImage の塗り潰し基点。fillAmount < 1 のとき、どこを起点に残すかを決める。
/// 体力ゲージのように「左から減る」「下から伸びる」といった方向制御に使う。
///
/// 辺 (0-3) は Edge / Radial 360 / Radial 180 が、角 (4-7) は Radial 90 が使う。
/// 方式に合わない値は NormalizeFillOrigin が近い側へ寄せるので、組み合わせで壊れない。
enum class UIImageFillOrigin {
    Left        = 0, ///< 左端を固定し右側を削る (一般的な体力バー)
    Right       = 1, ///< 右端を固定し左側を削る
    Bottom      = 2, ///< 下端を固定し上側を削る (縦ゲージ)
    Top         = 3, ///< 上端を固定し下側を削る
    BottomLeft  = 4,
    TopLeft     = 5,
    TopRight    = 6,
    BottomRight = 7,
};

/// 塗り潰しの削り方。
///   Edge       … fillOrigin の反対側から直線的に削る
///   Radial 360 … 要素の中心を軸に一周させる (クールダウン表示)
///   Radial 180 … fillOrigin の辺の中点を軸に半周させる (半円ゲージ)
///   Radial 90  … fillOrigin の角を軸に四半周させる (扇形ゲージ)
/// @note 90/180 は軸が辺の中点や角へ移り、その形で要素全体をちょうど覆う。360 を部分利用しても同じ絵にはならない。
enum class UIImageFillMethod {
    Edge      = 0,
    Radial360 = 1,
    Radial180 = 2,
    Radial90  = 3,
};

/// 方式が受け付けない fillOrigin を、意味の近い値へ寄せる。
/// Radial 90 は角を、それ以外は辺を要求する。
[[nodiscard]] inline UIImageFillOrigin NormalizeFillOrigin(
    UIImageFillMethod method, UIImageFillOrigin origin)
{
    using Origin = UIImageFillOrigin;
    const bool isCorner = origin >= Origin::BottomLeft;
    if (method == UIImageFillMethod::Radial90) {
        if (isCorner) return origin;
        switch (origin) {
        case Origin::Left:   return Origin::TopLeft;
        case Origin::Right:  return Origin::TopRight;
        case Origin::Bottom: return Origin::BottomLeft;
        case Origin::Top:    return Origin::TopLeft;
        default:             return Origin::BottomLeft;
        }
    }
    if (!isCorner) return origin;
    switch (origin) {
    case Origin::BottomLeft:  return Origin::Bottom;
    case Origin::BottomRight: return Origin::Bottom;
    case Origin::TopLeft:     return Origin::Top;
    case Origin::TopRight:    return Origin::Top;
    default:                  return Origin::Left;
    }
}

/// Simple は 1 枚を矩形いっぱいに伸ばす。
/// Sliced は Border を保ったまま中央だけを伸縮し、Tiled は原寸のまま敷き詰める。
enum class UIImageType {
    Simple = 0,
    Sliced = 1,
    Tiled  = 2,
};

struct UIImage {
    /// 親のどこを基準に、自分のどこを合わせるか。既定は左上/左上で、
    /// アンカー導入前の挙動と同じ (UIRect.hpp 参照)。
    UIAnchor      anchoring{};
    /// .mat への参照。空なら組み込みの UISprite で描く。
    /// @note texturePath だけで済む用途が大半なので、素材を増やさずに済む既定。マテリアルは
    ///       メッシュ材質と共有し、render_path = "ui" で UI 用途を区別する。
    std::string   materialPath = "";
    std::string   texturePath = "";
    renderer::ResourceHandle<renderer::TextureTag> texture = {};
    math::Vector4 color  = { 1.0f, 1.0f, 1.0f, 1.0f };
    math::Vector2 uvMin  = { 0.0f, 0.0f };
    math::Vector2 uvMax  = { 1.0f, 1.0f };
    /// 同一 Canvas 内の描画順。値が大きい要素ほど手前に描画する。
    int           sortOrder = 0;
    bool          enabled = true;
    /// 塗り潰し量 [0,1]。1 で矩形全体、0 で非表示。体力・スタミナゲージ等の進捗表現に使う。
    /// UISystem が描画時に矩形と UV を fillOrigin 方向へクリップする (transform.scale は変えない)。
    float             fillAmount = 1.0f;
    UIImageFillOrigin fillOrigin = UIImageFillOrigin::Left;
    UIImageFillMethod fillMethod = UIImageFillMethod::Edge;
    /// Radial のときの回転方向。Edge では使わない。
    /// 90 / 180 では「軸から見てどちらの端から満ちるか」を決める (覆う範囲は同じ)。
    bool              fillClockwise = true;
    UIImageType       imageType = UIImageType::Simple;
    /// 9-slice の余白 (L,T,R,B ピクセル)。Sliced のときだけ効く。
    /// @note Sprite の Border は Sprite 型の .meta 専用のため、素の .png でも使えるようここに持つ。
    ///       全成分 0 (既定) なら Sprite Editor の Border へ譲る (EffectiveBorder 参照)。
    math::Vector4     border = { 0.0f, 0.0f, 0.0f, 0.0f };
    /// Border とタイルの描画寸法をこの値で割る。1 なら元画像のピクセル寸法どおり。
    /// 低解像度の素材を大きな画面で使うとき、素材を作り直さずに角とタイルだけ拡大できる。
    float             pixelsPerUnitMultiplier = 1.0f;
    /// @name マテリアルのこの要素だけの上書き (ランタイム専用)
    /// @{
    /// @note .mat は参照する全要素の共有実体のため、要素ごとの差分はここに持つ。値は毎フレーム
    ///       動く量 (HP・充填率等) なのでシーンへは保存せず、ui.SetMaterialFloat() 等で書く。
    std::unordered_map<std::string, std::vector<float>> materialParamOverrides;
    std::unordered_map<std::string, std::string>        materialTextureOverrides;

    /// ランタイム専用: この要素だけ一時的に別の絵で描く (UIButton の状態別スプライト)。
    /// @note texturePath は人が指定しシーンに保存される値なので直接書き換えない。差し替えは
    ///       描画側の一時的な上書きとして別フィールドに持つ。
    std::string   overrideTexturePath = {};
    bool          hasTextureOverride = false;

    /// ランタイム専用: 最後にロードしたパスをキャッシュし、毎フレームの LoadTexture を回避する。
    std::string   loadedTexturePath = {};
    /// Spriteサブアセット参照時に.metaから解決したUV。通常TextureではuvMin/uvMaxを使う。
    math::Vector2 resolvedSpriteUvMin = { 0.0f, 0.0f };
    math::Vector2 resolvedSpriteUvMax = { 1.0f, 1.0f };
    math::Vector4 resolvedSpriteBorder = { 0.0f, 0.0f, 0.0f, 0.0f }; ///< L,T,R,B pixels
    math::Vector2 resolvedTextureSize = { 0.0f, 0.0f };
    /// 切り抜き (Sprite なら矩形、通常テクスチャなら画像全体) のピクセル寸法。
    /// Tiled の 1 タイル分の大きさと、Inspector の Set Native Size がこれを読む。
    math::Vector2 resolvedSizePixels = { 0.0f, 0.0f };
    bool          hasResolvedSprite = false;

    /// ピクセル矩形 (x,y,w,h) をテクスチャサイズ (texW, texH) で正規化した UV ペアに変換する。
    /// スクリプトからスプライトシートの切り抜き範囲を設定するときに使う。
    static std::pair<math::Vector2, math::Vector2> PixelRectToUV(
        float x, float y, float w, float h, float texW, float texH)
    {
        return { { x / texW, y / texH }, { (x + w) / texW, (y + h) / texH } };
    }

    /// 実際に描く絵のパス。上書き中はそちら、それ以外は指定された texturePath。
    [[nodiscard]] const std::string& EffectiveTexturePath() const
    {
        return hasTextureOverride ? overrideTexturePath : texturePath;
    }

    /// 実際に 9-slice へ使う余白 (元画像のピクセル単位)。border が既定値のままなら
    /// Sprite Editor で切った Border を使う。
    [[nodiscard]] math::Vector4 EffectiveBorder() const
    {
        const bool authored = border.x > 0.0f || border.y > 0.0f
                           || border.z > 0.0f || border.w > 0.0f;
        return authored ? border : resolvedSpriteBorder;
    }

    /// 元画像のピクセル寸法を描画ピクセルへ直す係数。
    [[nodiscard]] float UnitScale() const
    {
        return pixelsPerUnitMultiplier > 0.0f ? 1.0f / pixelsPerUnitMultiplier : 1.0f;
    }

    const char* GetTypeName() const { return "UIImage"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled",     enabled);
        anchoring.Reflect(r);
        r.Field("materialPath", materialPath);
        r.Tooltip("UI マテリアル (.mat)。空欄で組み込みのスプライト描画。"
                  "render_path = \"ui\" のものだけが使えます");
        r.Field("texturePath", texturePath);
        r.ColorField("color",       color);
        r.Field("uvMin",       uvMin);
        r.Field("uvMax",       uvMax);
        r.Field("sortOrder",   sortOrder);
        static constexpr const char* kImageTypeLabels[] = { "Simple", "Sliced", "Tiled" };
        int type = static_cast<int>(imageType);
        r.Enum("imageType", type, kImageTypeLabels);
        type = (type < 0 || type > 2) ? 0 : type;
        imageType = static_cast<UIImageType>(type);

        r.FieldIf("border", border, imageType != UIImageType::Simple,
                  "9-slice の余白 (左, 上, 右, 下 のピクセル数)。"
                  "0 のままなら Sprite Editor で切った Border を使います。"
                  "Tiled では余白の内側だけがタイルになります");
        r.FieldIf("pixelsPerUnitMultiplier", pixelsPerUnitMultiplier,
                  imageType != UIImageType::Simple,
                  "Border とタイルの描画寸法をこの値で割ります。"
                  "1 で元画像のピクセル寸法どおり、2 で半分の大きさ");
        if (pixelsPerUnitMultiplier <= 0.0f) pixelsPerUnitMultiplier = 1.0f;

        r.FloatRange("fillAmount", fillAmount, 0.0f, 1.0f);
        static constexpr const char* kFillMethodLabels[] = {
            "Edge", "Radial 360", "Radial 180", "Radial 90" };
        int method = static_cast<int>(fillMethod);
        r.Enum("fillMethod", method, kFillMethodLabels);
        method = (method < 0 || method > 3) ? 0 : method;
        fillMethod = static_cast<UIImageFillMethod>(method);
        /// @note 角は Radial 90 専用。他の方式で選ばれたら下の Normalize が辺へ戻す。
        static constexpr const char* kFillOriginLabels[] = {
            "Left", "Right", "Bottom", "Top",
            "Bottom Left", "Top Left", "Top Right", "Bottom Right" };
        int fill = static_cast<int>(fillOrigin);
        r.Enum("fillOrigin", fill, kFillOriginLabels);
        fill = (fill < 0 || fill > 7) ? 0 : fill;
        fillOrigin = NormalizeFillOrigin(fillMethod, static_cast<UIImageFillOrigin>(fill));
        r.FieldIf("fillClockwise", fillClockwise,
                  fillMethod != UIImageFillMethod::Edge,
                  "fillOrigin を起点にどちら回りで満たすか");
    }
    /// @}
};

} // namespace fbzz::scene
