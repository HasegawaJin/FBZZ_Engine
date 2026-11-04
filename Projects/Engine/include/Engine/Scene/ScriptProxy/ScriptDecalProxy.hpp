/// @file    ScriptDecalProxy.hpp
/// @brief   Script から DecalComponent を操作するショートハンド
/// @author  Hasegawa Jin
/// @date    2026-08-23

/// @note 弾痕・血痕・汚れなど動的デカールをランタイムで制御する。
#pragma once
#include <Engine/Api.hpp>

#include <Engine/Scene/EntityRef.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <cstdint>
#include <string_view>

namespace fbzz::scene {

class Script;

struct ScriptDecalProxy {
    Script* script = nullptr;

    /// @name 配置
    /// @{
    /// @brief レイキャストの当たり点と法線へ痕を 1 枚置く。DecalComponent が無くても呼べる。
    /// @param point,normal 当たり点・受け面の法線 (ともにワールド)。normal は長さ 0 なら上向き
    /// @param materialPath render_path = "decal" の .mat。空文字列で組み込み経路
    /// @param size,lifetime,depth,rollDegrees 一辺[m]・寿命[秒](<0で永続)・投影厚み[m]・
    /// @note 法線まわりの回転[度]
    /// @return 置いたデカールの参照。作れなければ無効な EntityRef (runtimeGenerated で非保存)
    [[nodiscard]] FBZZ_ENGINE_API EntityRef Spawn(const math::Vector3& point,
                                  const math::Vector3& normal,
                                  std::string_view materialPath,
                                  float size,
                                  float lifetime   = -1.0f,
                                  float depth      = 0.25f,
                                  float rollDegrees = 0.0f) const;

    FBZZ_ENGINE_API void SetEnabled(bool enabled) const;
    /// @}

    /// @brief lifetime < 0 で永続、>= 0 で時間経過フェードアウト→削除に切り替える。
    FBZZ_ENGINE_API void SetLifetime(float seconds)   const;
    FBZZ_ENGINE_API void SetFadeTime(float fadeTime)  const;
    /// @note 出現時に濃くなっていく時間 [秒]。0 でいきなり全濃度。永続デカールでも効く。
    FBZZ_ENGINE_API void SetFadeInTime(float seconds) const;
    /// @note コマ送り。frameCount <= 1 で無効。frameRate 0 は「寿命いっぱいで 1 周」。
    FBZZ_ENGINE_API void SetFlipbook(int frameCount, int framesPerRow, float frameRate, bool loop) const;
    /// @note 重なった痕の前後。小さいほど先に描く = 後ろになる。
    FBZZ_ENGINE_API void SetSortOrder(int order) const;
    /// @brief age をリセットする (タイムラインを巻き戻してデカールを再表示したい場合)。
    FBZZ_ENGINE_API void ResetAge() const;

    /// @note 不透明度の倍率 [0, 1]。ライフタイムフェードへ掛かるので .mat 経路でも効く。
    FBZZ_ENGINE_API void SetOpacity(float opacity) const;

    /// @note 受け面が投影軸から傾くほど薄くする度合い。
    /// @param strength 0 でフェードなし、1 で最大
    /// @param limitDegrees これ以上寝た面では完全に消える [0, 89]
    FBZZ_ENGINE_API void SetAngleFade(float strength, float limitDegrees) const;

    /// @note デカールを受け取るレイヤーのビットマスク。
    FBZZ_ENGINE_API void SetReceiverLayerMask(uint32_t mask) const;

    /// @name 組み込み経路 (materialPath が空のときだけ効く)
    /// @{
    FBZZ_ENGINE_API void SetAlbedoColor(float r, float g, float b, float a) const;
    FBZZ_ENGINE_API void SetNormalStrength(float strength) const;
    FBZZ_ENGINE_API void SetEmissiveColor(float r, float g, float b) const;
    FBZZ_ENGINE_API void SetEmissiveScale(float scale) const;

    /// @brief テクスチャパスを変更する。変更後は DecalPass が再バインドする。
    FBZZ_ENGINE_API void SetAlbedoTexture  (std::string_view path) const;
    FBZZ_ENGINE_API void SetNormalTexture  (std::string_view path) const;
    FBZZ_ENGINE_API void SetEmissiveTexture(std::string_view path) const;
    /// @}

    /// @name .mat 経路
    /// @{
    /// @brief 空文字列で組み込み経路へ戻す。render_path = "decal" の .mat だけが使える。
    FBZZ_ENGINE_API void SetMaterial(std::string_view materialPath) const;

    /// @note このデカールだけのパラメータ上書き。共有 .mat の値は変えない。
    FBZZ_ENGINE_API void SetMaterialFloat  (std::string_view param, float value) const;
    FBZZ_ENGINE_API void SetMaterialVector2(std::string_view param, float x, float y) const;
    FBZZ_ENGINE_API void SetMaterialVector4(std::string_view param, const math::Vector4& value) const;
    FBZZ_ENGINE_API void SetMaterialColor  (std::string_view param, const math::Vector4& color) const;
    /// @note slot は .mat の [textures] と同じ名前 ("albedo" / "normal" / "emissive" ...)。
    FBZZ_ENGINE_API void SetMaterialTexture(std::string_view slot, std::string_view texturePath) const;

    FBZZ_ENGINE_API void ClearMaterialOverride (std::string_view param) const;
    FBZZ_ENGINE_API void ClearMaterialOverrides() const;
    /// @}

    [[nodiscard]] FBZZ_ENGINE_API bool  IsEnabled() const;
    [[nodiscard]] FBZZ_ENGINE_API float GetOpacity() const;
    [[nodiscard]] FBZZ_ENGINE_API float GetLifetime() const;
    [[nodiscard]] FBZZ_ENGINE_API float GetFadeTime() const;
    /// @note DecalPass が加算している経過秒。lifetime < 0 (永続) でも増え続ける。
    [[nodiscard]] FBZZ_ENGINE_API float GetAge() const;
    /// @note ライフタイムフェードと SetOpacity() を掛けた値 [0, 1]。
    /// @note アルベドのアルファと .mat の tint は含まない。
    /// @note 弾痕の消え際に合わせて音や光を落とす、といった用途で使う。
    [[nodiscard]] FBZZ_ENGINE_API float GetEffectiveOpacity() const;
};

} /// @note namespace fbzz::scene
