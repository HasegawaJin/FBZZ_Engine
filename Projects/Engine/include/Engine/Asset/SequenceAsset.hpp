/// @file    SequenceAsset.hpp
/// @brief   演出タイムライン (.sequence) のランタイム表現
/// @author  Hasegawa Jin
/// @date    2026-08-26
#pragma once

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
    Once = 0,   ///< 終端で停止し、restoreOnStop に従って復帰する
    Loop,       ///< duration で折り返す
    HoldEnd,    ///< 終端の姿勢を保ったまま再生状態を続ける
};

enum class SequenceTimeMode : uint8_t {
    Scaled = 0, ///< ヒットストップ・スローと一緒に伸び縮みする
    Unscaled,   ///< 止まっている画面の上で進める
};

enum class SequenceTransformSpace : uint8_t {
    Local = 0,
    World,
    /// 再生開始時の姿勢を原点とした相対。演出をどこで再生しても同じ形になる。
    RelativeToStart,
};

/// AnimationTrack の 1 区間。AnimatorComponent の Slot へ流し込む。
struct SequenceAnimationClip {
    double      start    = 0.0;   ///< シーケンス時刻 [s]
    double      duration = 0.0;   ///< 0 ならクリップ長
    std::string sourcePath;       ///< .fbx / .anim (ロード時は素のパス)
    std::string clipName;
    float       speed    = 1.0f;
    bool        loop     = false;
    double      clipIn   = 0.0;   ///< クリップ内の開始位置 [s]
    float       blendIn  = 0.0f;
    float       blendOut = 0.0f;
};

/// ActivationTrack が持つ「この区間だけ active」の 1 本。
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
    double      end    = 0.0;   ///< loop のときだけ意味を持つ (ここで止める)
};

struct SequenceVfxClip {
    double start   = 0.0;
    double end     = 0.0;    ///< 0 ならグラフ側の尺に任せる
    bool   restart = true;   ///< start で Restart() するか Resume() か
};

struct SequenceEventKey {
    double      time = 0.0;
    std::string name;         ///< "boss.entry.impact" のような点付き名
    int32_t     intParam   = 0;
    float       floatParam = 0.0f;
};

/// 1 つの binding キーに対する 1 系統の変化。
///
/// WHY 種別ごとの派生型にせず 1 つの struct へ畳むか:
///   トラックは 7 種しかなく、増える見込みも小さい。派生型にすると
///   vector<unique_ptr<Track>> になり、アセットのコピー・保存・Editor の
///   並べ替えがすべてポインタ経由になる。型ごとの分岐はどのみち評価側に
///   1 か所必要なので、データを平たく持ったほうが読む場所が減る。
///   type が選ばなかった種別のフィールドは、常に空のまま使わない。
struct SequenceTrack {
    SequenceTrackType type = SequenceTrackType::Event;
    std::string       name;          ///< 表示名 (エディタのみ)
    std::string       binding;       ///< 空 = ターゲット無し (Event のみ許す)
    bool              muted = false;
    /// 触った値を停止時に戻すか。Audio / VFX / Event は戻せないので無視される。
    bool              restoreOnStop = true;

    // --- Animation ---
    std::string                        layerName = "Base";
    std::vector<SequenceAnimationClip> animationClips;   ///< start 昇順・重なりなし

    // --- Transform ---
    SequenceTransformSpace      space  = SequenceTransformSpace::Local;
    AnimInterp                  interp = AnimInterp::Cubic;
    std::vector<VectorKey>      positions;
    std::vector<QuaternionKey>  rotations;
    std::vector<VectorKey>      scales;

    // --- Property ---
    /// targetPath は使わず binding で解決する。それ以外は .anim と同じ意味。
    PropertyAnimationTrack property;

    // --- Activation ---
    std::vector<SequenceRange> ranges;

    // --- Audio ---
    std::vector<SequenceAudioClip> audioClips;

    // --- VFX ---
    std::vector<SequenceVfxClip> vfxClips;

    // --- Event ---
    std::vector<SequenceEventKey> eventKeys;
};

/// 演出 1 本。シーンを知らないので、別のシーンでも同じものを再生できる。
struct SequenceAsset {
    std::string      name;
    double           duration = 0.0;
    SequenceWrapMode wrapMode = SequenceWrapMode::Once;
    SequenceTimeMode timeMode = SequenceTimeMode::Scaled;
    std::vector<SequenceTrack> tracks;

    /// duration が未設定 (0) なら、最も遅いキー・クリップ終端から導出する。
    [[nodiscard]] double GetDurationSeconds() const;
};

[[nodiscard]] bool LoadSequenceAsset(const std::string& path, SequenceAsset& outAsset);
[[nodiscard]] bool SaveSequenceAsset(const std::string& path, const SequenceAsset& asset);

[[nodiscard]] const char* SequenceTrackTypeName(SequenceTrackType type);
[[nodiscard]] SequenceTrackType SequenceTrackTypeFromName(std::string_view name);

} // namespace fbzz::asset
