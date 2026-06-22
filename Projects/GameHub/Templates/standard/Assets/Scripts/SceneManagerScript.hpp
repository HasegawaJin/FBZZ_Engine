// FBZZ Engine
// SceneManagerScript.hpp | sandbox
// ボタンクリックによるシーン遷移ユーティリティスクリプト
//
// 使い方:
//   [ボタン GO に付ける場合]
//     targetScene: 遷移先シーン名
//     viaScene   : 中継シーン名 (例: "Load")。空の場合は直接遷移。
//
//   [Load.scene のコントローラー GO に付ける場合]
//     autoTransition = true
//     autoDelay      = 遷移までの秒数
//     targetScene    は OnStart で s_next から自動セットされる。
//
// WHY: 3 シーン分の個別コントローラーを作らず、1 スクリプトで全シーン遷移を賄う。
//      scene ファイルの 'Target Scene' / 'Via Scene' フィールドで遷移先を宣言的に設定できる。
#pragma once

#include <Engine/Scene/Components/UIButton.hpp>
#include <Engine/Scene/Script.hpp>
#include <string>

using namespace fbzz::scene;
using fbzz::Time;

namespace sandbox {

class SceneManagerScript : public Script {
    FBZZ_SCRIPT(SceneManagerScript)
public:
    // 遷移先シーン名。autoTransition=true の場合は OnStart で s_next から上書きされる。
    FBZZ_FIELD(std::string, targetScene, "", "Target Scene")
    // 中継シーン名。非空のとき s_next=targetScene を設定してからこのシーンをロードする。
    FBZZ_FIELD(std::string, viaScene,    "", "Via Scene")
    // true のとき autoDelay 秒後に targetScene へ自動遷移 (Load.scene 用)
    FBZZ_FIELD(bool,        autoTransition, false, "Auto Transition")
    FBZZ_FIELD(float,       autoDelay,      1.5f,  "Auto Delay")

    // Load.scene に「次のシーン」を伝える静的変数。
    // WHY: viaScene="Load" で遷移する際、ロード後の目的地をシーン間で引き渡す手段として使う。
    static inline std::string s_next;

    void OnStart()  override;
    void OnUpdate() override;

private:
    UIButton* m_btn    = nullptr;
    float     m_elapsed = 0.0f;
    bool      m_fired   = false; // 二重発火防止
};

} // namespace sandbox

#include "SceneManagerScript.generated.hpp"

#ifndef SceneManagerScript_IMPL
#define SceneManagerScript_IMPL

namespace sandbox {

void SceneManagerScript::OnStart()
{
    if (autoTransition) {
        // Load.scene モード: 静的変数から遷移先を受け取る
        if (!s_next.empty())
            targetScene = s_next;
    } else {
        // ボタンモード: 同 GO の UIButton をキャッシュ
        m_btn = scene.GetComponent<UIButton>();
    }
}

void SceneManagerScript::OnUpdate()
{
    if (m_fired) return;

    if (autoTransition) {
        m_elapsed += Time::deltaTime;
        if (m_elapsed >= autoDelay && !targetScene.empty()) {
            m_fired = true;
            scene.LoadScene(targetScene);
        }
        return;
    }

    if (!m_btn || !m_btn->onClick) return;
    m_fired = true;

    if (!viaScene.empty()) {
        // 中継シーンを経由する場合: 目的地を静的変数に保存してから中継シーンへ
        s_next = targetScene;
        scene.LoadScene(viaScene);
    } else {
        scene.LoadScene(targetScene);
    }
}

} // namespace sandbox
#endif
