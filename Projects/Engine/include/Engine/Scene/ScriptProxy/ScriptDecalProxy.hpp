/// @file    ScriptDecalProxy.hpp
/// @brief   Script から DecalComponent を操作するショートハンド
/// @author  Hasegawa Jin
/// @date    2026-08-23
///
/// 弾痕・血痕・汚れなど動的デカールをランタイムで制御する。
#pragma once

#include <Engine/Scene/EntityRef.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <cstdint>
#include <string_view>

namespace fbzz::scene {

class Script;

struct ScriptDecalProxy {
    Script* script = nullptr;

    // ── 配置 ─────────────────────────────────────────────────────────────────
    /// レイキャストの当たり点と法線へ痕を 1 枚置く。
    ///
    /// 新しい GameObject を作って DecalComponent を付け、投影ボリュームを面へ合わせる。
    /// 自分の DecalComponent は見ないので、デカールを持たないスクリプトからも呼べる。
    ///
    /// WHY 専用の口を作るか: これまでは «GameObject を作って DecalComponent を付けて
    ///     投影軸の回転を組む» を痕の種類ごとに書き起こしていた。回転の組み方
    ///     (どのローカル軸が投影軸か) は投影の実装と対で決まる知識で、
    ///     呼び出し側に書き写させると実装を変えた瞬間に全部が静かにずれる。
    ///
    /// @param point        当たり点 (ワールド)。ボリュームの中心に置く
    /// @param normal       受け面の法線 (ワールド)。長さ 0 なら上向きとして扱う
    /// @param materialPath render_path = "decal" の .mat。空文字列で組み込み経路
    /// @param size         痕の一辺 [m] (投影ボリュームの面内サイズ)
    /// @param lifetime     [秒]。< 0 で永続 (既定)
    /// @param depth        投影ボリュームの厚み [m]。厚いほど凹凸をまたいで貼れるが、
    ///                     手前の別の面にも乗りやすくなる
    /// @param rollDegrees  法線まわりの回転 [度]。同じ痕を毎回違う向きで貼る用途
    /// @return 置いたデカールの参照。作れなかったときは無効な EntityRef
    ///
    /// @note 返った参照から GameObject を引けば、大きさや .mat の上書きを後から足せる。
    ///       置いた GameObject は runtimeGenerated なのでシーンへ保存されない。
    [[nodiscard]] EntityRef Spawn(const math::Vector3& point,
                                  const math::Vector3& normal,
                                  std::string_view materialPath,
                                  float size,
                                  float lifetime   = -1.0f,
                                  float depth      = 0.25f,
                                  float rollDegrees = 0.0f) const;

    void SetEnabled(bool enabled) const;

    // lifetime < 0 で永続、>= 0 で時間経過フェードアウト→削除に切り替える。
    void SetLifetime(float seconds)   const;
    void SetFadeTime(float fadeTime)  const;
    /// 出現時に濃くなっていく時間 [秒]。0 でいきなり全濃度。永続デカールでも効く。
    void SetFadeInTime(float seconds) const;
    /// コマ送り。frameCount <= 1 で無効。frameRate 0 は「寿命いっぱいで 1 周」。
    void SetFlipbook(int frameCount, int framesPerRow, float frameRate, bool loop) const;
    /// 重なった痕の前後。小さいほど先に描く = 後ろになる。
    void SetSortOrder(int order) const;
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
