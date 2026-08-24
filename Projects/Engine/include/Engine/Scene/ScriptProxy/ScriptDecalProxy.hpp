/// @file ScriptDecalProxy.hpp
/// @brief Script から DecalComponent を操作するショートハンド
/// @author Hasegawa Jin
/// @date 2026-08-23
///
/// 弾痕・血痕・汚れなど動的デカールをランタイムで制御する。
#pragma once

#include <Math/Vector4.hpp>
#include <cstdint>
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

    /// 不透明度の倍率 [0, 1]。ライフタイムフェードへ掛かるので .mat 経路でも効く。
    void SetOpacity(float opacity) const;

    /// 受け面が投影軸から傾くほど薄くする度合い。
    /// @param strength 0 でフェードなし、1 で最大
    /// @param limitDegrees これ以上寝た面では完全に消える [0, 89]
    void SetAngleFade(float strength, float limitDegrees) const;

    /// デカールを受け取るレイヤーのビットマスク。
    void SetReceiverLayerMask(uint32_t mask) const;

    // ── 組み込み経路 (materialPath が空のときだけ効く) ────────────────────────
    void SetAlbedoColor(float r, float g, float b, float a) const;
    void SetNormalStrength(float strength) const;
    void SetEmissiveColor(float r, float g, float b) const;
    void SetEmissiveScale(float scale) const;

    // テクスチャパスを変更する。変更後は DecalPass が再バインドする。
    void SetAlbedoTexture  (std::string_view path) const;
    void SetNormalTexture  (std::string_view path) const;
    void SetEmissiveTexture(std::string_view path) const;

    // ── .mat 経路 ─────────────────────────────────────────────────────────────
    /// 空文字列で組み込み経路へ戻す。render_path = "decal" の .mat だけが使える。
    void SetMaterial(std::string_view materialPath) const;

    /// このデカールだけのパラメータ上書き。共有 .mat の値は変えない。
    void SetMaterialFloat  (std::string_view param, float value) const;
    void SetMaterialVector2(std::string_view param, float x, float y) const;
    void SetMaterialVector4(std::string_view param, const math::Vector4& value) const;
    void SetMaterialColor  (std::string_view param, const math::Vector4& color) const;
    /// slot は .mat の [textures] と同じ名前 ("albedo" / "normal" / "emissive" ...)。
    void SetMaterialTexture(std::string_view slot, std::string_view texturePath) const;

    void ClearMaterialOverride (std::string_view param) const;
    void ClearMaterialOverrides() const;

    [[nodiscard]] bool  IsEnabled() const;
    [[nodiscard]] float GetOpacity() const;
    [[nodiscard]] float GetLifetime() const;
    [[nodiscard]] float GetFadeTime() const;
    /// DecalPass が加算している経過秒。lifetime < 0 (永続) でも増え続ける。
    [[nodiscard]] float GetAge() const;
    /// ライフタイムフェードと SetOpacity() を掛けた値 [0, 1]。
    /// アルベドのアルファと .mat の tint は含まない。
    /// 弾痕の消え際に合わせて音や光を落とす、といった用途で使う。
    [[nodiscard]] float GetEffectiveOpacity() const;
};

} // namespace fbzz::scene
