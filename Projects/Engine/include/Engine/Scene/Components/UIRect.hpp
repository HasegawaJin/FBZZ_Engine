/// @file UIRect.hpp
/// @brief UI 要素の矩形・アンカー・ピボットの定義と、その解決規則
/// @author Hasegawa Jin
/// @date 2026-08-22
///
/// WHY 1 ファイルに集めるか:
///   これまで「position が矩形のどこを指すか」がコンポーネントごとに違っていた。
///     UIImage … position = 矩形の左上
///     UIText  … position = align によって左端 / 中央 / 右端 に変わる基準点
///   同じ座標を入れた画像と文字が別の場所に出るうえ、align を変えただけで文字が
///   動く。並べる作業のたびに「これはどっちの流儀だったか」を思い出す必要があり、
///   それはコンポーネントを増やすほど悪化する。
///
///   さらに合成の式そのものが UISystem.cpp と Editor の ViewportUI.cpp に
///   別々に書かれており、Play 中と編集中で計算が分かれていた。定義を 1 つにする。
///
/// WHY アンカーとピボットを分けるか:
///   「親のどこを基準にするか」と「自分のどこをその点に合わせるか」は別の問い。
///   1 つにまとめると、画面右下へ寄せた要素を「右下基準で右下合わせ」にしか
///   できず、右下から左へ 20px といった指定ができない。
///   Canvas の解像度が変わっても崩れない配置には、この 2 つが要る。
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
///
/// 既定値 (0,0) / (0,0) は「親の左上を基準に、自分の左上を合わせる」で、
/// アンカーを導入する前の UIImage の挙動と完全に一致する。
/// 既存シーンは 1 つも動かない。
struct UIAnchor {
    math::Vector2 anchor = math::Vector2::ZERO;  ///< 親のどこを基準にするか
    math::Vector2 pivot  = math::Vector2::ZERO;  ///< 自分のどこをその点へ合わせるか

    void Reflect(IReflector& r)
    {
        r.Field("anchor", anchor);
        r.Tooltip("親のどこを基準にするか。(0,0)=左上 (0.5,0.5)=中央 (1,1)=右下。"
                  "Canvas の解像度が変わっても位置関係が保たれます");
        r.Field("pivot", pivot);
        r.Tooltip("自分のどこを基準点へ合わせるか。(0,0)=左上 (0.5,0.5)=中央。"
                  "回転と拡縮の中心でもあります");
    }
};

/// 最終的な矩形を求める。UISystem と Editor はどちらもこれだけを使うこと。
///
///   左上 = 親サイズ × anchor + ローカル位置 - 自分のサイズ × pivot
///
/// @param parentSize 親要素の矩形サイズ。直下が Canvas なら Canvas の寸法。
/// @param localPosition transform.position.xy (基準点からのずれ)
/// @param size この要素の矩形サイズ
[[nodiscard]] inline UIRect ResolveUIRect(const math::Vector2& parentSize,
                                          const math::Vector2& localPosition,
                                          const math::Vector2& size,
                                          const UIAnchor& anchoring)
{
    UIRect rect{};
    rect.size = size;
    rect.position = {
        parentSize.x * anchoring.anchor.x + localPosition.x - size.x * anchoring.pivot.x,
        parentSize.y * anchoring.anchor.y + localPosition.y - size.y * anchoring.pivot.y,
    };
    return rect;
}

/// 基準点そのもの (親の中の位置)。ギズモがアンカーを描くのに使う。
[[nodiscard]] inline math::Vector2 ResolveUIAnchorPoint(const math::Vector2& parentSize,
                                                        const UIAnchor& anchoring)
{
    return { parentSize.x * anchoring.anchor.x, parentSize.y * anchoring.anchor.y };
}

} // namespace fbzz::scene
