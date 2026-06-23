// FBZZ Engine
// ScriptDecalProxy.hpp | fbzz::scene
// Script から DecalComponent を操作するショートハンド。
// 弾痕・血痕・汚れなど動的デカールをランタイムで制御する。
#pragma once

#include <string_view>

namespace fbzz::scene {

class Script;

struct ScriptDecalProxy {
    Script* script = nullptr;

    void SetEnabled(bool enabled) const;

    // lifetime < 0 で永続、>= 0 で時間経過フェードアウト→削除に切り替える。
    void SetLifetime(float seconds)   const;
    void SetFadeTime(float fadeTime)  const;
    // age をリセットする（タイムラインを巻き戻してデカールを再表示したい場合）。
    void ResetAge() const;

    void SetAlbedoColor(float r, float g, float b, float a) const;
    void SetNormalStrength(float strength) const;
    void SetEmissiveColor(float r, float g, float b) const;
    void SetEmissiveScale(float scale) const;

    // テクスチャパスを変更する。変更後は RenderSystem が再バインドする。
    void SetAlbedoTexture  (std::string_view path) const;
    void SetNormalTexture  (std::string_view path) const;
    void SetEmissiveTexture(std::string_view path) const;
};

} // namespace fbzz::scene
