/// @file    UICanvasGroup.hpp
/// @brief   UI の一群をまとめて薄くする・触れなくするコンポーネント
/// @author  Hasegawa Jin
/// @date    2026-08-27
///
/// WHY 要素ごとの色ではなく群で持つか:
///   画面の出し入れは「メニュー全体を 0.2 秒でフェードする」という単位で起きる。
///   これを要素の色でやると、フェードのたびに全要素の色を書き換えることになり、
///   スクリプトが UI の構成を知っている必要が出る (要素を 1 つ足すたびに
///   フェード処理の側も直す)。透明度を階層の属性にすれば、構成は関係なくなる。
///
/// WHY alpha と interactable を分けるか:
///   完全に透明でも押せてしまうと、隠したはずのメニューがクリックを吸う。
///   逆に「薄く見せたまま操作だけ止める」(処理中の待ち状態) も要る。
///   見た目と入力は別の軸なので、1 つの値にまとめない。
#pragma once

#include <Engine/Scene/Script.hpp>

namespace fbzz::scene {

struct UICanvasGroup {
    bool  enabled = true;
    /// この GameObject 以下の UIImage / UIText の不透明度へ掛かる係数 [0,1]。
    /// 入れ子になった場合は掛け合わさる。
    float alpha = 1.0f;
    /// false の間、配下の UIButton / UISlider などが一切入力を受け取らない。
    /// 見た目は変わらない。
    bool  interactable = true;
    /// false の間、配下はポインターを「素通し」する。
    ///
    /// WHY interactable と別に要るか: interactable = false は入力を止めるが、
    ///     その要素が最前面にいる限り背後の要素にも渡らない (吸って捨てる)。
    ///     背後を触らせたい全画面の暗幕やフェード板ではこちらを false にする。
    bool  blocksRaycasts = true;
    /// true にすると、上位の UICanvasGroup の alpha / interactable を無視して
    /// この群の値から始め直す。
    ///
    /// WHY 要るか: フェードアウト中の画面の上に、確認ダイアログのように
    ///     常に見えていてほしい層を重ねる場面がある。
    bool  ignoreParentGroups = false;

    const char* GetTypeName() const { return "UI Canvas Group"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.FloatRange("alpha", alpha, 0.0f, 1.0f);
        r.Field("interactable", interactable);
        r.Tooltip("false で配下の UI が入力を受け取らなくなります (見た目は変わりません)");
        r.Field("blocksRaycasts", blocksRaycasts);
        r.Tooltip("false で配下がポインターを素通しします。"
                  "背後を触らせたい暗幕・フェード板ではこちらを外します");
        r.Field("ignoreParentGroups", ignoreParentGroups);
        r.Tooltip("上位の Canvas Group の設定を引き継がず、この群の値から始めます");
    }
};

} // namespace fbzz::scene
