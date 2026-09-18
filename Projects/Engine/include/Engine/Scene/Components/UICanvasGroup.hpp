/// @file    UICanvasGroup.hpp
/// @brief   UI の一群をまとめて薄くする・触れなくするコンポーネント
/// @author  Hasegawa Jin
/// @date    2026-08-27
///
/// @note 要素ごとの色ではなく群で持つのは、画面の出し入れが「メニュー全体を 0.2 秒で
///       フェードする」という単位で起きるため。要素の色でやるとスクリプトが UI の構成を
///       知る必要が出る。透明度を階層の属性にすれば構成は関係なくなる。
/// @note alpha と interactable を分けるのは、完全に透明でも押せると隠したメニューがクリックを
///       吸うため。「薄く見せたまま操作だけ止める」も要り、見た目と入力は別の軸で持つ。
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
    /// @note interactable = false は入力を止めるが、最前面にいる限り背後にも渡らない
    ///       (吸って捨てる)。背後を触らせたい全画面の暗幕やフェード板ではこちらを false にする。
    bool  blocksRaycasts = true;
    /// true にすると、上位の UICanvasGroup の alpha / interactable を無視して
    /// この群の値から始め直す。
    /// @note フェードアウト中の画面の上に、確認ダイアログのように常に見えていてほしい層を
    ///       重ねる場面があるため。
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
