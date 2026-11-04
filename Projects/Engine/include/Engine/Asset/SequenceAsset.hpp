/// @file    SequenceAsset.hpp
/// @brief   演出タイムライン (.sequence) のランタイム表現
/// @author  Hasegawa Jin
/// @date    2026-08-26
#pragma once
#include <Engine/Api.hpp>

#include <Engine/Asset/AnimationClip.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::asset {

enum class SequenceTrackType : uint8_t {
    Animation = 0,
    Transform,
    Property,
    Activation,
    Audio,
    VFX,
    Event,
};

enum class SequenceWrapMode : uint8_t {
    Once = 0,   ///< @note 終端で停止し、restoreOnStop に従って復帰する
    Loop,       ///< @note duration で折り返す
    HoldEnd,    ///< @note 終端の姿勢を保ったまま再生状態を続ける
};

enum class SequenceTimeMode : uint8_t {
    Scaled = 0, ///< @note ヒットストップ・スローと一緒に伸び縮みする
    Unscaled,   ///< @note 止まっている画面の上で進める
};

enum class SequenceTransformSpace : uint8_t {
    Local = 0,
    World,
    /// @note 再生開始時の姿勢を原点とした相対。演出をどこで再生しても同じ形になる。
    RelativeToStart,
};

/// @note AnimationTrack の 1 区間。AnimatorComponent の Slot へ流し込む。
struct SequenceAnimationClip {
    double      start    = 0.0;   ///< @note シーケンス時刻 [s]
    double      duration = 0.0;   ///< @note 0 ならクリップ長
    std::string sourcePath;       ///< @note .fbx / .anim (ロード時は素のパス)
    std::string clipName;
    float       speed    = 1.0f;
    bool        loop     = false;
    double      clipIn   = 0.0;   ///< @note クリップ内の開始位置 [s]
    float       blendIn  = 0.0f;
    float       blendOut = 0.0f;
};

/// @note ActivationTrack が持つ「この区間だけ active」の 1 本。
struct SequenceRange {
    double start = 0.0;
    double end   = 0.0;
};

struct SequenceAudioClip {
    double      start  = 0.0;
    std::string clipPath;
    std::string bus    = "SE";
    float       volume = 1.0f;
    bool        loop   = false;
    double      end    = 0.0;   ///< @note loop のときだけ意味を持つ (ここで止める)
};

struct SequenceVfxClip {
    double start   = 0.0;
    double end     = 0.0;    ///< @note 0 ならグラフ側の尺に任せる
    bool   restart = true;   ///< @note start で Restart() するか Resume() か
};

struct SequenceEventKey {
    double      time = 0.0;
    std::string name;         ///< @note "boss.entry.impact" のような点付き名
    int32_t     intParam   = 0;
    float       floatParam = 0.0f;
};

/// @note 1 つの binding キーに対する 1 系統の変化。
/// @note トラックは 7 種しかなく増える見込みも小さいため、種別ごとの派生型にせず 1 つの struct
/// @note へ畳んだ。派生型にすると `vector<unique_ptr<Track>>` になりコピー・保存・Editor の
/// @note 並べ替えがすべてポインタ経由になる。型ごとの分岐はどのみち評価側に 1 か所必要なので、
/// @note データを平たく持つほうが読む場所が減る。type が選ばなかった種別のフィールドは常に空。
struct SequenceTrack {
    SequenceTrackType type = SequenceTrackType::Event;
    std::string       name;          ///< @note 表示名 (エディタのみ)
    std::string       binding;       ///< @note 空 = ターゲット無し (Event のみ許す)
    bool              muted = false;
    /// @note 触った値を停止時に戻すか。Audio / VFX / Event は戻せないので無視される。
    bool              restoreOnStop = true;

    /// @name Animation
    /// @{
    std::string                        layerName = "Base";
    std::vector<SequenceAnimationClip> animationClips;   ///< @note start 昇順・重なりなし
    /// @}

    /// @name Transform
    /// @{
    SequenceTransformSpace      space  = SequenceTransformSpace::Local;
    AnimInterp                  interp = AnimInterp::Cubic;
    std::vector<VectorKey>      positions;
    std::vector<QuaternionKey>  rotations;
    std::vector<VectorKey>      scales;
    /// @}

    /// @name Property
    /// @{
    /// @note targetPath は使わず binding で解決する。それ以外は .anim と同じ意味。
    PropertyAnimationTrack property;
    /// @}

    /// @name Activation
    /// @{
    std::vector<SequenceRange> ranges;
    /// @}

    /// @name Audio
    /// @{
    std::vector<SequenceAudioClip> audioClips;
    /// @}

    /// @name VFX
    /// @{
    std::vector<SequenceVfxClip> vfxClips;
    /// @}

    /// @name Event
    /// @{
    std::vector<SequenceEventKey> eventKeys;
    /// @}
};

/// @note 演出 1 本。シーンを知らないので、別のシーンでも同じものを再生できる。
struct SequenceAsset {
    std::string      name;
    double           duration = 0.0;
    SequenceWrapMode wrapMode = SequenceWrapMode::Once;
    SequenceTimeMode timeMode = SequenceTimeMode::Scaled;
    std::vector<SequenceTrack> tracks;

    /// @note duration が未設定 (0) なら、最も遅いキー・クリップ終端から導出する。
    [[nodiscard]] FBZZ_ENGINE_API double GetDurationSeconds() const;
};

[[nodiscard]] FBZZ_ENGINE_API bool LoadSequenceAsset(const std::string& path, SequenceAsset& outAsset);
[[nodiscard]] FBZZ_ENGINE_API bool SaveSequenceAsset(const std::string& path, const SequenceAsset& asset);
/// @note 通常のファイル入出力と直接APIのステージングが同じ形式・参照変換を使う。失敗時は出力を保持する。
[[nodiscard]] FBZZ_ENGINE_API bool LoadSequenceAssetFromToml(std::string_view text, SequenceAsset& outAsset);
[[nodiscard]] FBZZ_ENGINE_API bool SerializeSequenceAsset(const SequenceAsset& asset, std::string& text);

[[nodiscard]] FBZZ_ENGINE_API const char* SequenceTrackTypeName(SequenceTrackType type);
[[nodiscard]] FBZZ_ENGINE_API SequenceTrackType SequenceTrackTypeFromName(std::string_view name);

} /// @note namespace fbzz::asset
