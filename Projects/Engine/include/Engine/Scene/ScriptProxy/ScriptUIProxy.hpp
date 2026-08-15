// FBZZ Engine
// ScriptUIProxy.hpp | fbzz::scene
// Script から UI コンポーネントを操作するショートハンド
#pragma once

#include <Math/Vector4.hpp>
#include <string_view>

namespace fbzz::scene {

class Script;
class GameObject;

// UIButton の当たり状態。IsHovered() / IsPressed() の戻り値と 1 対 1 で対応する。
enum class UIButtonPhase {
    Normal,
    Hovered,
    Pressed,
    Disabled,   // enabled == false または isInteractable == false
};

struct ScriptUIProxy {
    Script* script = nullptr;

    // ── ボタン入力の取得 ──────────────────────────────────────────────────────
    // WHY ポーリングにするか: コールバック登録にすると、スクリプト DLL がリロードされた
    //     瞬間に UIButton 側へ登録済みの std::function が解放済みコードを指す。
    //     UISystem が立てる 1 フレーム限定フラグを読むだけなら寿命問題が起きない。
    //
    // WHY 1 フレーム遅れるか: UISystem はレンダーパスから呼ばれるため、フレーム N の描画で
    //     立ったフラグをフレーム N+1 の OnUpdate() が読む。フラグは次の UISystem 呼び出しまで
    //     保持されるので、取りこぼしも二重発火も起きない。
    [[nodiscard]] bool WasClicked() const;
    [[nodiscard]] bool WasHoverEnter() const;
    [[nodiscard]] bool WasHoverExit() const;
    [[nodiscard]] bool IsHovered() const;
    [[nodiscard]] bool IsPressed() const;
    [[nodiscard]] bool IsInteractable() const;
    [[nodiscard]] UIButtonPhase GetButtonPhase() const;

    // GameObject ターゲット版。ボタンが子要素や別 Canvas にある構成 (メニュー画面を
    // 1 つのスクリプトで捌く場合) はこちらを使う。
    [[nodiscard]] bool WasClicked(GameObject* go) const;
    [[nodiscard]] bool WasHoverEnter(GameObject* go) const;
    [[nodiscard]] bool WasHoverExit(GameObject* go) const;
    [[nodiscard]] bool IsHovered(GameObject* go) const;
    [[nodiscard]] bool IsPressed(GameObject* go) const;
    [[nodiscard]] bool IsInteractable(GameObject* go) const;
    [[nodiscard]] UIButtonPhase GetButtonPhase(GameObject* go) const;

    void SetButtonInteractable(bool v) const;
    void SetButtonInteractable(GameObject* go, bool v) const;
    void SetImageColor(const math::Vector4& color) const;
    void SetText(std::string_view text) const;
    void SetCanvasSortOrder(int order) const;
    // スプライトシートのピクセル矩形 (x,y,w,h) をテクスチャサイズ (texW,texH) で正規化し
    // UIImage の uvMin / uvMax へ書き込む。
    void SetImageSpriteRect(float x, float y, float w, float h, float texW, float texH) const;
    // 自 GO の UIImage 塗り潰し量 [0,1] を設定する (体力ゲージ等)。
    void SetImageFillAmount(float amount) const;

    // ── GameObject ターゲット版 ───────────────────────────────────────────────
    // 体力バーのように UI が別 GO (HUD キャンバスや子要素) にある場合、
    // スクリプトが保持する GameObject* を直接指定して UI を更新する。
    void SetImageColor(GameObject* go, const math::Vector4& color) const;
    void SetImageFillAmount(GameObject* go, float amount) const;
    void SetText(GameObject* go, std::string_view text) const;
    void SetImageEnabled(GameObject* go, bool enabled) const;
};

} // namespace fbzz::scene
