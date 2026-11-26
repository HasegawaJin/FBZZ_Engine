/// @file    SequenceAsset.hpp
/// @brief   演出タイムライン (.sequence) のランタイム表現
/// @author  Hasegawa Jin
/// @date    2026-08-26
#pragma once
#include <Engine/Api.hpp>

#include <Engine/Asset/Animation/Clips/AnimationClip.hpp>

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
    double      start    = 0.0; ///< シーケンス時刻 [s]
    double      duration = 0.0; ///< 0 ならクリップ長
    std::string sourcePath; ///< .fbx / .anim (ロード時は素のパス)
    std::string clipName; ///< 再生するアニメーションクリップをモデル内で照合する名前。
    float       speed    = 1.0f; ///< クリップ内の経過時間に掛ける再生倍率。
    bool        loop     = false; ///< シーケンス上の指定区間内でクリップを繰り返すか。
    double      clipIn   = 0.0; ///< クリップ内の開始位置 [s]
    float       blendIn  = 0.0f; ///< 区間の冒頭でアニメーションの重みを上げる時間 [s]。
    float       blendOut = 0.0f; ///< 区間の末尾でアニメーションの重みを下げる時間 [s]。
};

/// @note ActivationTrack が持つ「この区間だけ active」の 1 本。
struct SequenceRange {
    double start = 0.0; ///< シーケンスの先頭を基準にした開始時刻 [s]。
    double end   = 0.0; ///< シーケンスの先頭を基準にした終了時刻 [s]。
};

struct SequenceAudioClip {
    double      start  = 0.0; ///< シーケンスの先頭を基準にした開始時刻 [s]。
    std::string clipPath; ///< 再生するオーディオクリップのアセット参照。
    std::string bus    = "SE"; ///< 音声を送るミキサーバス名。
    float       volume = 1.0f; ///< オーディオクリップの再生音量倍率。
    bool        loop   = false; ///< シーケンス上の指定区間内でクリップを繰り返すか。
    double      end    = 0.0; ///< loop のときだけ意味を持つ (ここで止める)
};

struct SequenceVfxClip {
    double start   = 0.0; ///< シーケンスの先頭を基準にした開始時刻 [s]。
    double end     = 0.0; ///< 0 ならグラフ側の尺に任せる
    bool   restart = true; ///< start で Restart() するか Resume() か
};

struct SequenceEventKey {
    double      time = 0.0; ///< イベントを発行するシーケンス上の時刻 [s]。
    std::string name; ///< "boss.entry.impact" のような点付き名
    int32_t     intParam   = 0; ///< イベントのリスナーへ渡す整数引数。
    float       floatParam = 0.0f; ///< イベントのリスナーへ渡す浮動小数点引数。
};

/// @note 1 つの binding キーに対する 1 系統の変化。
/// @note トラックは 7 種しかなく増える見込みも小さいため、種別ごとの派生型にせず 1 つの struct
/// @note へ畳んだ。派生型にすると `vector<unique_ptr<Track>>` になりコピー・保存・Editor の
/// @note 並べ替えがすべてポインタ経由になる。型ごとの分岐はどのみち評価側に 1 か所必要なので、
/// @note データを平たく持つほうが読む場所が減る。type が選ばなかった種別のフィールドは常に空。
struct SequenceTrack {
    SequenceTrackType type = SequenceTrackType::Event; ///< このトラックが評価するアニメーション・音声・演出などの種別。
    std::string       name; ///< 表示名 (エディタのみ)
    std::string       binding; ///< 空 = ターゲット無し (Event のみ許す)
    bool              muted = false; ///< このトラックの評価と副作用の発行を抑えるか。
    bool              restoreOnStop = true; ///< 触った値を停止時に戻すか。Audio / VFX / Event は戻せないので無視される。

    /// @name Animation
    /// @{
    std::string                        layerName = "Base"; ///< トラックのアニメーションを適用する Animator レイヤー名。
    std::vector<SequenceAnimationClip> animationClips; ///< start 昇順・重なりなし
    /// @}

    /// @name Transform
    /// @{
    SequenceTransformSpace      space  = SequenceTransformSpace::Local; ///< 姿勢キーの値をローカルまたはワールドとして解釈する座標系。
    AnimInterp                  interp = AnimInterp::Cubic; ///< 姿勢キーの間を補う補間方式。
    std::vector<VectorKey>      positions; ///< 対象の位置を駆動するシーケンス時刻 [s] 順のキー列。
    std::vector<QuaternionKey>  rotations; ///< 対象の回転を駆動するシーケンス時刻 [s] 順のキー列。
    std::vector<VectorKey>      scales; ///< 対象の倍率を駆動するシーケンス時刻 [s] 順のキー列。
    /// @}

    /// @name Property
    /// @{
    /// @note targetPath は使わず binding で解決する。それ以外は .anim と同じ意味。
    PropertyAnimationTrack property; ///< 対象コンポーネントまたは材質のプロパティを駆動するキーと型。
    /// @}

    /// @name Activation
    /// @{
    std::vector<SequenceRange> ranges; ///< トラックの効果が有効になるシーケンス上の区間列。
    /// @}

    /// @name Audio
    /// @{
    std::vector<SequenceAudioClip> audioClips; ///< 音声の開始時刻と再生設定の列。
    /// @}

    /// @name VFX
    /// @{
    std::vector<SequenceVfxClip> vfxClips; ///< VFX を発火する時刻と再生設定の列。
    /// @}

    /// @name Event
    /// @{
    std::vector<SequenceEventKey> eventKeys; ///< 再生位置が通過したときに発行するイベント列。
    /// @}
};

/// @note 演出 1 本。シーンを知らないので、別のシーンでも同じものを再生できる。
struct SequenceAsset {
    std::string      name; ///< 制作・再生側で参照するシーケンス名。
    double           duration = 0.0; ///< シーケンスの先頭から末尾までの再生時間 [s]。
    SequenceWrapMode wrapMode = SequenceWrapMode::Once; ///< 再生位置が区間の端に達したときの停止・反復方式。
    SequenceTimeMode timeMode = SequenceTimeMode::Scaled; ///< 再生時刻の進行に使うスケール済み時間と実時間の選択。
    std::vector<SequenceTrack> tracks; ///< シーケンスが所有する対象別のトラック列。

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

} // namespace fbzz::asset
