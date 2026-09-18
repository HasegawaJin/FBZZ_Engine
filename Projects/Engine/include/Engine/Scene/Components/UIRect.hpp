/// @file    UIRect.hpp
/// @brief   UI 要素の矩形・アンカー・ピボットの定義と、その解決規則
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// @note 1 ファイルに集約: position の意味が UIImage (左上基準) と UIText (align 依存) で
///       食い違い、合成式も UISystem.cpp と Editor 側で二重化していた。
/// @note アンカー (親のどこを基準にするか) とピボット (自分のどこを合わせるか) は別の問いで、
///       分けないと「右下基準で右下合わせ」以外の配置 (右下から 20px 内側等) ができない。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Math/Vector2.hpp>

namespace fbzz::scene {

/// UI 要素の矩形 (Canvas 空間、原点は左上・y は下向き)。
struct UIRect {
    math::Vector2 position = math::Vector2::ZERO;  ///< 左上
    math::Vector2 size     = math::Vector2::ZERO;

    [[nodiscard]] math::Vector2 Center() const
    {
        return { position.x + size.x * 0.5f, position.y + size.y * 0.5f };
    }
    /// 正規化座標 (0,0)=左上 〜 (1,1)=右下 の点を返す。
    [[nodiscard]] math::Vector2 PointAt(const math::Vector2& normalized) const
    {
        return { position.x + size.x * normalized.x, position.y + size.y * normalized.y };
    }
    [[nodiscard]] bool Contains(const math::Vector2& point) const
    {
        return point.x >= position.x && point.x <= position.x + size.x
            && point.y >= position.y && point.y <= position.y + size.y;
    }
};

/// 親の矩形内の基準点と、自分の矩形内の合わせ点。どちらも 0..1 の正規化座標。
/// @note 既定 (0,0)/(0,0) は旧 UIImage の挙動と一致し既存シーンは動かない。ストレッチは
///       axis ごとの bool で持つ (anchorMax==anchor 判定だと anchorMax 未保存の旧シーンが壊れる)。
struct UIAnchor {
    math::Vector2 anchor = math::Vector2::ZERO;  ///< 親のどこを基準にするか (ストレッチ時は下限)
    math::Vector2 pivot  = math::Vector2::ZERO;  ///< 自分のどこをその点へ合わせるか

    /// ストレッチする軸。true にすると、その軸のサイズは自分の scale ではなく
    /// 「anchor 〜 anchorMax が親の上で占める幅」から決まる。
    bool stretchX = false;
    bool stretchY = false;
    /// ストレッチ時の上限アンカー。stretch していない軸では読まない。
    math::Vector2 anchorMax = math::Vector2::ONE;
    /// ストレッチ時の上限側の余白 (Canvas ピクセル)。下限側は transform.position が担う。
    /// @note 無いと画面幅いっぱいの帯しか作れず、「左右 40px を空けた帯」等が表現できない。
    math::Vector2 offsetMax = math::Vector2::ZERO;

    void Reflect(IReflector& r)
    {
        r.Field("anchor", anchor);
        r.Tooltip("親のどこを基準にするか。(0,0)=左上 (0.5,0.5)=中央 (1,1)=右下。"
                  "Canvas の解像度が変わっても位置関係が保たれます。"
                  "ストレッチする軸では下限側のアンカーになります");
        r.Field("pivot", pivot);
        r.Tooltip("自分のどこを基準点へ合わせるか。(0,0)=左上 (0.5,0.5)=中央。"
                  "回転と拡縮の中心でもあります。ストレッチする軸では使いません");
        r.Field("stretchX", stretchX);
        r.Tooltip("横幅を親に追従させます。幅は scale.x ではなく "
                  "anchor.x 〜 anchorMax.x の範囲から決まります");
        r.Field("stretchY", stretchY);
        r.Tooltip("高さを親に追従させます");
        r.FieldIf("anchorMax", anchorMax, stretchX || stretchY,
                  "ストレッチの上限アンカー。(1,1) で親の右下まで");
        r.FieldIf("offsetMax", offsetMax, stretchX || stretchY,
                  "上限側の余白 (px)。下限側の余白は position が担います");
    }
};

/// 最終的な矩形を求める。UISystem と Editor はどちらもこれだけを使うこと。
///   点アンカー   … 左上 = 親サイズ × anchor + ローカル位置 - 自分のサイズ × pivot
///   ストレッチ軸 … 下限 = 親サイズ × anchor    + ローカル位置
///                  上限 = 親サイズ × anchorMax - offsetMax
/// @param parentSize 親要素の矩形サイズ。直下が Canvas なら Canvas の寸法。
/// @param localPosition transform.position.xy (基準点からのずれ / 下限側の余白)
/// @param size この要素の矩形サイズ。ストレッチする軸では使わない。
[[nodiscard]] inline UIRect ResolveUIRect(const math::Vector2& parentSize,
                                          const math::Vector2& localPosition,
                                          const math::Vector2& size,
                                          const UIAnchor& anchoring)
{
    /// @note 軸ごとに独立して解く。片方だけストレッチする構成 (画面幅の帯・縦の仕切り) が
    ///       実用上いちばん多く、両軸を 1 つの式へ押し込めない。
    const auto axis = [](float parent, float local, float extent, float anchorLow,
                         float anchorHigh, float offsetHigh, float pivot,
                         bool stretch, float& outPosition, float& outSize) {
        if (!stretch) {
            outSize     = extent;
            outPosition = parent * anchorLow + local - extent * pivot;
            return;
        }
        const float low  = parent * anchorLow + local;
        const float high = parent * anchorHigh - offsetHigh;
        outPosition = low;
        /// @note 余白どうしが行き違っても矩形を裏返さない。裏返ると当たり判定が
        ///       「どこにも当たらない」ではなく「常に当たる」側へ倒れることがある。
        outSize = high > low ? high - low : 0.0f;
    };

    UIRect rect{};
    axis(parentSize.x, localPosition.x, size.x, anchoring.anchor.x, anchoring.anchorMax.x,
         anchoring.offsetMax.x, anchoring.pivot.x, anchoring.stretchX,
         rect.position.x, rect.size.x);
    axis(parentSize.y, localPosition.y, size.y, anchoring.anchor.y, anchoring.anchorMax.y,
         anchoring.offsetMax.y, anchoring.pivot.y, anchoring.stretchY,
         rect.position.y, rect.size.y);
    return rect;
}

/// 基準点そのもの (親の中の位置)。ギズモがアンカーを描くのに使う。
[[nodiscard]] inline math::Vector2 ResolveUIAnchorPoint(const math::Vector2& parentSize,
                                                        const UIAnchor& anchoring)
{
    return { parentSize.x * anchoring.anchor.x, parentSize.y * anchoring.anchor.y };
}

} // namespace fbzz::scene
