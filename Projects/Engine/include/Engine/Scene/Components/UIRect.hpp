/// @file    UIRect.hpp
/// @brief   UI 要素の矩形・アンカー・ピボットの定義と、その解決規則
/// @author  Hasegawa Jin
/// @date    2026-08-22
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
/// WHY ストレッチを軸ごとの bool にするか (anchorMax の一致判定にしないか):
///   「anchorMax == anchor なら点アンカー」という規則にすると、anchorMax を
///   保存していない既存シーンが anchorMax = (0,0) で読み込まれ、anchor が
///   (1,1) の要素だけ突然ストレッチ扱いになる。既定値がそれ自体で意味を持つ
///   フィールドは、古いシーンを黙って壊す。意図は独立した bool で持つ。
struct UIAnchor {
    math::Vector2 anchor = math::Vector2::ZERO;  ///< 親のどこを基準にするか (ストレッチ時は下限)
    math::Vector2 pivot  = math::Vector2::ZERO;  ///< 自分のどこをその点へ合わせるか

    /// ストレッチする軸。true にすると、その軸のサイズは自分の scale ではなく
    /// 「anchor 〜 anchorMax が親の上で占める幅」から決まる。
    bool stretchX = false;
    bool stretchY = false;
    /// ストレッチ時の上限アンカー。stretch していない軸では読まない。
    math::Vector2 anchorMax = math::Vector2::ONE;
    /// ストレッチ時の上限側の余白 (Canvas ピクセル)。
    /// 下限側は transform.position がそのまま担うので、ここには上限側だけを置く。
    ///
    /// WHY 余白が要るか: 画面幅いっぱいの帯は作れても、「左右 40px を空けた帯」が
    ///     作れないと実際のレイアウトではほぼ使えない。
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
///
///   点アンカー   … 左上 = 親サイズ × anchor + ローカル位置 - 自分のサイズ × pivot
///   ストレッチ軸 … 下限 = 親サイズ × anchor    + ローカル位置
///                  上限 = 親サイズ × anchorMax - offsetMax
///
/// @param parentSize 親要素の矩形サイズ。直下が Canvas なら Canvas の寸法。
/// @param localPosition transform.position.xy (基準点からのずれ / 下限側の余白)
/// @param size この要素の矩形サイズ。ストレッチする軸では使わない。
[[nodiscard]] inline UIRect ResolveUIRect(const math::Vector2& parentSize,
                                          const math::Vector2& localPosition,
                                          const math::Vector2& size,
                                          const UIAnchor& anchoring)
{
    // 軸ごとに独立して解く。片方だけストレッチする構成 (画面幅の帯・縦の仕切り) が
    // 実用上いちばん多く、両軸を 1 つの式へ押し込めない。
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
        // 余白どうしが行き違っても矩形を裏返さない。裏返ると当たり判定が
        // 「どこにも当たらない」ではなく「常に当たる」側へ倒れることがある。
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
