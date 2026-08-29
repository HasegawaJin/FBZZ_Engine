/// @file    UIElement.hpp
/// @brief   Canvas 配下で矩形を持つ「UI 要素」の判定
/// @author  Hasegawa Jin
/// @date    2026-08-26
/// WHY 1 か所に置くか:
///   「これは UI 要素か」を Editor は選択・ギズモ・Inspector の 3 か所で問う。
///   絵を持つもの (UIImage / UIText) だけを条件に書くと、Mask や Scroll View の
///   ように自分では何も描かない要素が Viewport から一切触れなくなる。条件が
///   散っていると UI コンポーネントを 1 つ足すたびに拾い漏れる箇所が出る。
#pragma once

#include <Engine/Scene/Components/UIButton.hpp>
#include <Engine/Scene/Components/UIControls.hpp>
#include <Engine/Scene/Components/UIImage.hpp>
#include <Engine/Scene/Components/UILayoutGroup.hpp>
#include <Engine/Scene/Components/UIText.hpp>
#include <Engine/Scene/GameObject.hpp>

namespace fbzz::scene {

/// Canvas 空間の矩形を持つ要素なら true。Canvas 自身は含まない。
[[nodiscard]] inline bool IsUIElement(GameObject& go)
{
    return go.GetComponent<UIImage>()
        || go.GetComponent<UIText>()
        || go.GetComponent<UIButton>()
        || go.GetComponent<UIMask>()
        || go.GetComponent<UISlider>()
        || go.GetComponent<UIToggle>()
        || go.GetComponent<UIScrollView>()
        || go.GetComponent<UIInputField>()
        || go.GetComponent<UIEventTrigger>()
        || go.GetComponent<UILayoutGroup>()
        || go.GetComponent<UIContentSizeFitter>()
        || go.GetComponent<UINavigation>()
        || go.GetComponent<UIDragSource>()
        || go.GetComponent<UIDropTarget>();
    // NOTE: UICanvasGroup は入れない。あれは配下の見え方と入力可否を変えるだけで
    //       自分の矩形を持たない (掴んでも動かす対象が無い)。
}

/// サイズが実測でしか決まらない要素。掴んで広げることはできない。
[[nodiscard]] inline bool HasMeasuredUISize(GameObject& go)
{
    return go.GetComponent<UIText>() && !go.GetComponent<UIImage>();
}

} // namespace fbzz::scene
