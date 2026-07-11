// FBZZ Engine
// ScriptUIProxy.hpp | fbzz::scene
// Script から UI コンポーネントを操作するショートハンド
#pragma once

#include <Math/Vector4.hpp>
#include <string_view>

namespace fbzz::scene {

class Script;
class GameObject;

struct ScriptUIProxy {
    Script* script = nullptr;

    void SetButtonInteractable(bool v) const;
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
