/// @file    SequencePlayerComponent.hpp
/// @brief   .sequence を 1 本再生し、binding キーをシーンの GameObject へ結ぶ
/// @author  Hasegawa Jin
/// @date    2026-08-26
#pragma once

#include <Engine/Asset/SequenceAsset.hpp>
#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/Script.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fbzz::scene {

/// .sequence 側の binding 名と、このシーンでの実体の対応。
///
/// WHY アセットへ焼かないか: EntityRef は GameObject の instanceId で解決される。
///     アセットへ入れると「その .scene 専用の演出」になり、同じ登場演出を別の
///     シーンやテスト用シーンで再生できなくなる。
struct SequenceBinding {
    std::string key;
    EntityRef   target;
};

/// 停止時に元へ戻すための、トラック 1 本ぶんの開始時スナップショット。
///
/// WHY 呼び出し側の後始末ではなくトラックの性質として持つか:
///     演出は必ず途中で死ぬ (DLL ホットリロード / Play・Stop 往復 / シーン遷移)。
///     「入力を止めたまま」「ボスが空中に浮いたまま」が残らないことを、
///     演出を書いた人の注意力ではなく仕組みで保証する。
struct SequenceTrackRuntime {
    EntityID target;             ///< 解決済み binding。無効なら未解決
    bool     warnedUnresolved = false;
    bool     warnedContract   = false;   ///< 契約違反 (ボーンへの Transform 書き込み等)
    bool     hasSnapshot      = false;

    /// 復帰に必要なトラック情報。スナップショットと同じ瞬間にここへ写す。
    ///
    /// WHY アセットを引き直さないか: 復帰しなければならない場面には
    ///     「アセットが差し替わった」が含まれる (ディスクの再読込 / エディタの編集)。
    ///     戻し方をアセット側から読むと、差し替わった後は戻す先が分からなくなる。
    ///     元の値と一緒に「どこへ戻すか」も撮っておけば、復帰は常に成立する。
    asset::SequenceTrackType type = asset::SequenceTrackType::Event;
    bool                     restoreOnStop = true;
    std::string              layerName;        ///< Animation
    /// Property の当たり先 (キーは持たない。復帰には識別だけあればよい)。
    asset::PropertyAnimationTrack propertyBinding;

    // Transform
    math::Vector3    position = math::Vector3::ZERO;
    math::Quaternion rotation = math::Quaternion::Identity();
    math::Vector3    scale    = math::Vector3::ONE;
    /// RelativeToStart の基準となる、再生開始時のローカル姿勢。
    math::Vector3    basePosition = math::Vector3::ZERO;
    math::Quaternion baseRotation = math::Quaternion::Identity();

    // Activation
    bool active = true;

    // Property
    asset::AnimValueType propertyType  = asset::AnimValueType::Float;
    float                propertyValue[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    int32_t              propertyInt   = 0;
    bool                 propertyBool  = false;
    bool                 propertyFound = false;
    bool                 hadMaterialOverride = false;
    std::vector<float>   materialValues;

    // Animation — この Track が握っている Slot の状態
    int  activeClip = -1;        ///< animationClips の添字。-1 なら何も差し込んでいない
    bool slotActive = false;

    // Audio — 止める責任があるループ再生
    int  loopingClip = -1;
};

/// 演出 1 本の再生窓口。1 GameObject につき 1 本。
struct SequencePlayerComponent {
    bool        enabled = true;
    std::string sequencePath;
    std::vector<SequenceBinding> bindings;
    bool  playOnAwake = false;
    float speed       = 1.0f;

    /// アセットが持つ wrapMode / timeMode をこのコンポーネントの値で上書きするか。
    ///
    /// WHY 既定を false にするか: 尺の終わり方 (HoldEnd) と時間軸 (Unscaled) は
    ///     演出そのものの性質で、.sequence 側が正本。既定で上書きすると
    ///     「アセットには HoldEnd と書いてあるのに Once で戻る」が黙って起きる。
    bool overrideAssetSettings = false;
    asset::SequenceWrapMode wrapMode = asset::SequenceWrapMode::Once;
    asset::SequenceTimeMode timeMode = asset::SequenceTimeMode::Scaled;

    // ── ランタイム状態 (Reflect しない) ────────────────────────────────────
    // WHY 保存しないか: 再生位置は Play/Stop 往復で既定値へ戻るのが正しい。
    //     シーンに焼くと「保存した瞬間の演出の途中」から始まる。
    bool   playing = false;
    bool   paused  = false;
    double time    = 0.0;
    /// 離散トラックを発火した半開区間の下端。「前フレームの時刻」とほぼ同じだが、
    /// 再生開始直後だけ負になる (時刻 0 に置いたキーを取りこぼさないため)。
    double timePrev = 0.0;

    /// エディタのスクラブ位置 [s]。負なら未指定。離散トラックは発火しない。
    float editorScrubTime = -1.0f;
    /// スクラブに入る直前の姿勢を撮ったか。スクラブを抜けるときに 1 回だけ戻す。
    bool  editorSnapshot  = false;

    // Script / Editor が積み、SequenceSystem が消費する要求。
    // WHY 直接状態を書かないか: 再生開始はスナップショット取得を伴い、停止は復帰を伴う。
    //     これらは binding の解決が済んでいる System 側でしか正しく行えない。
    bool   pendingPlay = false;
    bool   pendingStop = false;
    double pendingSeek = -1.0;

    /// エディタが持つ未保存の演出。設定されている間はディスクの .sequence より優先する。
    ///
    /// WHY 要るか: 演出の良し悪しは尺で決まるので、0.2 秒ずらすたびに Ctrl+S が要る形だと
    ///     試行錯誤が止まる。SequencePanel が編集のたびにここへ差し替え、
    ///     revision を進めて System へ「作り直せ」と伝える。
    std::shared_ptr<const asset::SequenceAsset> authoringSequence;
    std::uint64_t authoringRevision = 0;

    /// 現在のスナップショットがどのアセットのものか。**識別にしか使わない**。
    /// WHY デリファレンスしないか: 差し替え後は無効なアドレスになりうる。
    ///     「変わったか」を知るためだけに持ち、中身は必ず今回解決したポインタから読む。
    const void*   appliedSequence = nullptr;
    std::uint64_t appliedAuthoringRevision = 0;
    int           appliedAssetGeneration   = -1;

    std::vector<SequenceTrackRuntime> trackRuntime;
    bool awakeHandled = false;

    void Play()
    {
        pendingPlay = true;
        pendingStop = false;
        paused      = false;
    }

    void Stop()
    {
        pendingStop = true;
        pendingPlay = false;
    }

    void Pause()  { paused = true; }
    void Resume() { paused = false; }

    void SetTime(double seconds) { pendingSeek = seconds < 0.0 ? 0.0 : seconds; }

    /// binding キーへ実体を割り当てる。既存キーは差し替える。
    void Bind(std::string_view key, EntityRef target)
    {
        for (auto& binding : bindings) {
            if (binding.key == key) { binding.target = target; return; }
        }
        bindings.push_back(SequenceBinding{ std::string(key), target });
    }

    const char* GetTypeName() const { return "Sequence Player"; }

    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.FileField("sequencePath", sequencePath, ".sequence",
                    "再生する .sequence アセット");
        r.Field("playOnAwake", playOnAwake);
        r.FloatRange("speed", speed, -4.0f, 4.0f);

        r.Field("overrideAssetSettings", overrideAssetSettings);
        r.Tooltip("オフのときは .sequence 側の wrapMode / timeMode を使う");

        static constexpr const char* kWrapLabels[] = { "Once", "Loop", "HoldEnd" };
        static constexpr const char* kTimeLabels[] = { "Scaled", "Unscaled" };
        // WHY int を経由するか: 列挙の実体は uint8_t で、int& へ reinterpret_cast すると
        //     1 バイトの変数へ 4 バイト書き込むことになり、隣のフィールドを壊す。
        int wrap = static_cast<int>(wrapMode);
        r.BeginField("wrapMode", "Wrap Mode");
        r.SetFieldVisible(overrideAssetSettings);
        r.Enum("Wrap Mode", wrap, kWrapLabels);
        r.EndField();
        wrapMode = static_cast<asset::SequenceWrapMode>(wrap < 0 || wrap > 2 ? 0 : wrap);

        int mode = static_cast<int>(timeMode);
        r.BeginField("timeMode", "Time Mode");
        r.SetFieldVisible(overrideAssetSettings);
        r.Enum("Time Mode", mode, kTimeLabels);
        r.EndField();
        timeMode = static_cast<asset::SequenceTimeMode>(mode < 0 || mode > 1 ? 0 : mode);

        r.BeginField("bindings", "Bindings");
        const std::size_t count = r.BeginObjectList("Bindings", bindings.size());
        bindings.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            r.BeginObjectElement(i);
            r.BeginField("key", "Key");
            r.Field("Key", bindings[i].key);
            r.BeginField("target", "Target");
            r.RefField("Target", bindings[i].target, "");
            r.EndField();
            r.EndObjectElement();
        }
        const std::size_t removeIndex = r.EndObjectList();
        if (removeIndex < bindings.size())
            bindings.erase(bindings.begin() + static_cast<std::ptrdiff_t>(removeIndex));
    }
};

} // namespace fbzz::scene
