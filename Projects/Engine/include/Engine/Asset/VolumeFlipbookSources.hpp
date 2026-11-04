/// @file    VolumeFlipbookSources.hpp
/// @brief   Volume Flipbook Baker が焼く «puff の並べ方» (ソース) の登録口と、同梱ソースの設定。
/// @author  Hasegawa Jin
/// @date    2026-09-11

/// @note ソースは «設定と焼く時間範囲から VolumePuff の列を返す関数» に名前を付けたもの。
/// @note 同梱ソース (Puff / RisingPlume / Fireball / Torch / Fountain / WaterSplash / BloodSpray /
/// @note BloodBurst / Emitter) も RegisterVolumeSource で登録されており、足したソースと区別しない。

/// @note 登録ソースは設定と時間範囲から VolumePuff 列を返し、中心位置と FlowMap は共通の解析式が評価する。
/// @pre 出力する puff は正規化された箱 [-1,1]^3 に収める。
/// @see Docs/design/volume-flipbook-baker.md
#pragma once
#include <Engine/Api.hpp>

#include <Engine/Asset/VolumeFlipbookAnalytic.hpp>

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::asset {

struct VolumeFlipbookBakeSettings;

/// @note VolumeSourceSettings::emitter を読む同梱ソースの名前。
inline constexpr const char* kVolumeEmitterSourceName = "Emitter";

/// @note 汎用ソース "Emitter" の設定。C++ を書かずに puff の出方を決める。単位は秒と bake 空間。
struct VolumeEmitterSettings {
    /// @note true = 0 コマ目で全部出す一発もの / false = 一定間隔で湧き続ける (Loop できる)。
    bool burst = true;
    /// @note burst なら総数、連続なら 1 秒あたりの数。
    int count = 24;
    math::Vector3 origin{ 0.0f, -0.5f, 0.0f };
    /// @note origin から散らす球の半径。
    float originRadius = 0.05f;
    math::Vector3 direction{ 0.0f, 1.0f, 0.0f };
    float coneAngleDegrees = 20.0f;
    float speed = 1.0f;
    /// @note speed に掛かる乱数の幅 [0,1]。
    float speedRandom = 0.3f;
    math::Vector3 gravity{ 0.0f, -1.2f, 0.0f };
    /// @note 速度に比例する減速 [1/s]。
    float drag = 0.0f;
    float lifetime = 1.0f;
    float lifetimeRandom = 0.2f;
    float fadeIn = 0.05f;
    float fadeOut = 0.3f;
    float radius = 0.1f;
    float radiusRandom = 0.3f;
    /// @note 寿命の間に半径が何倍になるか。
    float growth = 1.5f;
    /// @note 回転の速さ [rad/s]。軸は puff ごとに乱数。
    float spin = 2.0f;
    float density = 1.5f;
    float temperature = 0.0f;
    float coolingTime = 0.5f;
    float liquid = 0.0f;
    float noiseScale = 1.0f;
    float stretch = 1.0f;
    float stretchPerSpeed = 0.0f;
    float colorKeyMin = 0.0f;
    float colorKeyMax = 1.0f;
};

struct VolumeSourceSettings {
    /// @note 登録名 (RegisterVolumeSource)。見つからなければ puff は 0 個。
    std::string preset = "RisingPlume";
    std::uint32_t seed = 1;
    /// @note ループできるソース (VolumeSourceCanLoop) だけが見る。frameCount·frameDt 後に場が完全に元へ戻る。
    bool loop = false;
    /// @note 負ならソースの既定 (DefaultVolumeStartTime)。
    float startTime = -1.0f;
    float frameDt = 1.0f / 24.0f;
    int frameCount = 64;
    /// @note ソース "Emitter" だけが見る。
    VolumeEmitterSettings emitter;
};

/// @note ソースへ渡す、焼く時間範囲。
struct VolumeSourceRange {
    /// @note 0 コマ目の時刻。
    float start = 0.0f;
    /// @note frameCount·frameDt。
    float duration = 1.0f;
};

struct VolumeSourceDesc {
    std::string name;
    /// @note パネルに出す 1 行の説明。
    std::string description;
    std::function<std::vector<VolumePuff>(const VolumeSourceSettings&, const VolumeSourceRange&)> build;
    /// @note 省略時は 0。湧き続けるソースは寿命ぶん先を返し、定常状態を 0 コマ目にする。
    std::function<float(const VolumeSourceSettings&)> defaultStartTime;
    /// @note Loop を立てれば duration 後に場が元へ戻るか (loop フラグ自体は見ない)。省略時はループしない。
    std::function<bool(const VolumeSourceSettings&)> canLoop;
    /// @note ソースを選んだときに Look を推奨値へ寄せる。既定の Look へ戻した後に呼ばれる。省略時は既定のまま。
    std::function<void(VolumeFlipbookBakeSettings&)> applyLook;
};

/// @note 同じ名前が既にあれば置き換える。メインスレッドから呼ぶこと。
/// @note FindVolumeSource が返したポインタは、同名の置き換え後も同じ要素を指し続ける。
FBZZ_ENGINE_API void RegisterVolumeSource(VolumeSourceDesc desc);
[[nodiscard]] FBZZ_ENGINE_API const VolumeSourceDesc* FindVolumeSource(std::string_view name);
/// @note 登録順 (同梱ソースが先頭)。
[[nodiscard]] FBZZ_ENGINE_API const std::deque<VolumeSourceDesc>& VolumeSources();

[[nodiscard]] FBZZ_ENGINE_API float DefaultVolumeStartTime(const VolumeSourceSettings& settings);
[[nodiscard]] FBZZ_ENGINE_API float ResolveVolumeStartTime(const VolumeSourceSettings& settings);
[[nodiscard]] FBZZ_ENGINE_API bool VolumeSourceCanLoop(const VolumeSourceSettings& settings);
/// @note 焼いた Atlas を FPS モードでループ再生してよいか (canLoop かつ loop)。
[[nodiscard]] FBZZ_ENGINE_API bool VolumeSourceLoops(const VolumeSourceSettings& settings);

/// @note ベイクする時間範囲 [start, start + frameCount·frameDt] に関わる puff を列挙する。
[[nodiscard]] FBZZ_ENGINE_API std::vector<VolumePuff> BuildVolumePuffs(const VolumeSourceSettings& settings);

/// @note Look (光・色・液体) を既定値へ戻し、選ばれているソースの applyLook を掛ける。
/// @note カメラ・解像度・出力先は触らない。
FBZZ_ENGINE_API void ApplyVolumeSourceLook(VolumeFlipbookBakeSettings& settings);

/// @name ソースを書くための道具
/// @{

/// @note 決定的な乱数 [0,1)。seed・index・channel が同じなら常に同じ値。
[[nodiscard]] FBZZ_ENGINE_API float VolumeHash01(std::uint32_t seed, std::uint32_t index, std::uint32_t channel);
/// @note 決定的な乱数 [-1,1)。
[[nodiscard]] FBZZ_ENGINE_API float VolumeHashSigned(std::uint32_t seed, std::uint32_t index, std::uint32_t channel);
/// @note 単位球面上の一様な向き。channel から 2 つ消費する。
[[nodiscard]] FBZZ_ENGINE_API math::Vector3 VolumeRandomDirection(std::uint32_t seed, std::uint32_t index, std::uint32_t channel);
/// @note axis を中心とする半頂角 halfAngleRadians の円錐内 (立体角一様) の単位ベクトル。channel から 2 つ消費する。
[[nodiscard]] FBZZ_ENGINE_API math::Vector3 VolumeRandomInCone(const math::Vector3& axis, float halfAngleRadians,
                                               std::uint32_t seed, std::uint32_t index, std::uint32_t channel);

/// @note 湧き続けるソース用。[start - maxLifetime, start + duration] に生まれる puff を period 間隔で make に作らせる。
/// @note settings.loop なら period を duration の約数へ寄せ、j 番目と j+M 番目に同じ variant を渡す
/// @note (M = duration / period)。make が variant だけで見た目を決めれば、場は duration で厳密に元へ戻る。
FBZZ_ENGINE_API void AppendPeriodicVolumePuffs(const VolumeSourceSettings& settings, const VolumeSourceRange& range,
                               float period, float maxLifetime,
                               const std::function<VolumePuff(std::uint32_t variant, float birthTime)>& make,
                               std::vector<VolumePuff>& out);
/// @}

} /// @note namespace fbzz::asset
