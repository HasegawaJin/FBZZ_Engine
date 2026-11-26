/// @file    SequencePlayerComponent.hpp
/// @brief   .sequence を 1 本再生し、binding キーをシーンの GameObject へ結ぶ
/// @author  Hasegawa Jin
/// @date    2026-08-26
#pragma once

#include <Engine/Asset/Animation/Sequences/SequenceAsset.hpp>
#include <Engine/Scene/References/EntityRef.hpp>
#include <Engine/Reflection/Reflection.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fbzz::scene {

/// @note .sequence 側の binding 名と、このシーンでの実体の対応。
/// @note アセットへ焼かないのは、EntityRef が GameObject の instanceId で解決されるため。
/// @note アセットへ入れると「その .scene 専用の演出」になり、別シーンで再生できなくなる。
struct SequenceBinding {
    std::string key; ///< シーケンスアセット内で対象を識別するバインディング名。
    EntityRef   target; ///< 対象キーに紐づける、このシーン内のエンティティ参照。
};

/// @note 停止時に元へ戻すための、トラック 1 本ぶんの開始時スナップショット。
/// @note 呼び出し側の後始末ではなくトラックの性質として持つのは、演出が必ず途中で死ぬため
/// @note (DLL ホットリロード / Play・Stop 往復 / シーン遷移)。「ボスが空中に浮いたまま」が
/// @note 残らないことを、書いた人の注意力ではなく仕組みで保証する。
struct SequenceTrackRuntime {
    EntityID target; ///< 解決済み binding。無効なら未解決
    bool     warnedUnresolved = false; ///< 未解決のバインディングについて警告を発行済みかどうか。
    bool     warnedContract   = false; ///< 契約違反 (ボーンへの Transform 書き込み等)
    bool     hasSnapshot      = false; ///< 停止時の復元に使える、トラック開始前の状態を保存済みかどうか。

    /// @note 復帰に必要なトラック情報。スナップショットと同じ瞬間にここへ写す。
    /// @note アセットを引き直さないのは、復帰しなければならない場面に「アセットが差し替わった」
    /// @note (ディスクの再読込 / エディタの編集) が含まれるため。元の値と一緒に「どこへ戻すか」も
    /// @note 撮っておけば、差し替わった後でも復帰は常に成立する。
    asset::SequenceTrackType type = asset::SequenceTrackType::Event; ///< 停止時に初期値の復元先を識別するため保存したトラックの種類。
    bool                     restoreOnStop = true; ///< 停止時にトラックが変更した値を保存した状態へ戻すかどうか。
    std::string              layerName; ///< アニメーション
    asset::PropertyAnimationTrack propertyBinding; ///< Property の当たり先 (キーは持たない。復帰には識別だけあればよい)。

    math::Vector3    position = math::Vector3::ZERO; ///< 位置・姿勢・スケール
    math::Quaternion rotation = math::Quaternion::Identity(); ///< 停止時の復元に使う対象のローカル回転。
    math::Vector3    scale    = math::Vector3::ONE; ///< 停止時の復元に使う対象のローカル軸別スケール。
    math::Vector3    basePosition = math::Vector3::ZERO; ///< RelativeToStart の基準となる、再生開始時のローカル姿勢。
    math::Quaternion baseRotation = math::Quaternion::Identity(); ///< 相対 Transform キーを適用する基準となる、再生開始時のローカル回転。

    bool active = true; ///< 有効状態

    asset::AnimValueType propertyType  = asset::AnimValueType::Float; ///< プロパティ
    float                propertyValue[4] = { 0.0f, 0.0f, 0.0f, 0.0f }; ///< 停止時の復元に使う数値プロパティの初期値。成分数は propertyType に従う。
    int32_t              propertyInt   = 0; ///< 停止時の復元に使う整数プロパティの初期値。
    bool                 propertyBool  = false; ///< 停止時の復元に使う真偽値プロパティの初期値。
    bool                 propertyFound = false; ///< 保存時に対象プロパティを解決できたかどうか。
    bool                 hadMaterialOverride = false; ///< 再生開始時に対象のマテリアルパラメーターに上書きがあったかどうか。
    std::vector<float>   materialValues; ///< 停止時に復元するマテリアルパラメーターの上書き値。

    /// @note Animation — この Track が握っている Slot の状態
    int  activeClip = -1; ///< animationClips の添字。-1 なら何も差し込んでいない
    bool slotActive = false; ///< このトラックが現在アニメーションスロットの再生を制御しているかどうか。

    int  loopingClip = -1; ///< Audio — 止める責任があるループ再生
};

/// @note 演出 1 本の再生窓口。1 GameObject につき 1 本。
struct SequencePlayerComponent {
    bool        enabled = true; ///< シーケンスの実行と再生要求の処理を行うかどうか。
    std::string sequencePath; ///< 再生するシーケンスアセットの参照パス。
    std::vector<SequenceBinding> bindings; ///< アセットのバインディング名とこのシーンの対象の対応表。
    bool  playOnAwake = false; ///< コンポーネント初回評価時にシーケンスの再生を要求するかどうか。
    float speed       = 1.0f; ///< シーケンスの時間進行に掛ける再生速度倍率。

    /// @note アセットが持つ wrapMode / timeMode をこのコンポーネントの値で上書きするか。
    /// @note 既定 false: 尺の終わり方 (HoldEnd) と時間軸 (Unscaled) は演出そのものの性質で
    /// @note .sequence 側が正本。既定で上書きすると「アセットは HoldEnd なのに Once で戻る」が
    /// @note 黙って起きる。
    bool overrideAssetSettings = false; ///< シーケンスアセットの再生末尾と時計の設定をこのコンポーネントの値で上書きするかどうか。
    asset::SequenceWrapMode wrapMode = asset::SequenceWrapMode::Once; ///< アセット設定を上書きする場合の、末尾での再生や復元の方式。
    asset::SequenceTimeMode timeMode = asset::SequenceTimeMode::Scaled; ///< アセット設定を上書きする場合に使う、スケール時間または実時間の指定。

    /// @name ランタイム状態 (Reflect しない)
    /// @{
    /// @note 保存しないのは、再生位置が Play/Stop 往復で既定値へ戻るのが正しいため。
    /// @note シーンに焼くと「保存した瞬間の演出の途中」から始まる。
    bool   playing = false; ///< シーケンスを再生中かどうか。停止や初期化時に戻すランタイム状態。
    bool   paused  = false; ///< 再生位置を保持したまま時間を進めないかどうか。
    double time    = 0.0; ///< 再生中のシーケンス内の現在時刻 [秒]。
    /// @note 離散トラックを発火した半開区間の下端。「前フレームの時刻」とほぼ同じだが、
    /// @note 再生開始直後だけ負になる (時刻 0 に置いたキーを取りこぼさないため)。
    double timePrev = 0.0; ///< 離散イベントを発火する再生時刻の区間の下端 [秒]。

    float editorScrubTime = -1.0f; ///< エディタのスクラブ位置 [s]。負なら未指定。離散トラックは発火しない。
    bool  editorSnapshot  = false; ///< スクラブに入る直前の姿勢を撮ったか。スクラブを抜けるときに 1 回だけ戻す。

    /// @note Script / Editor が積み、SequenceSystem が消費する要求。
    /// @note 直接状態を書かないのは、再生開始がスナップショット取得を伴い停止が復帰を伴うため。
    /// @note これらは binding の解決が済んでいる System 側でしか正しく行えない。
    bool   pendingPlay = false; ///< 次回の System 更新で再生と初期状態の保存を開始する要求。
    bool   pendingStop = false; ///< 次回の System 更新でシーケンスの停止と復元を行う要求。
    double pendingSeek = -1.0; ///< 次回の System 更新でシークするシーケンス内時刻 [秒]。負値は要求なし。

    /// @note エディタが持つ未保存の演出。設定されている間はディスクの .sequence より優先する。
    /// @note 演出の良し悪しは尺で決まるので、0.2 秒ずらすたびに Ctrl+S が要る形だと試行錯誤が
    /// @note 止まる。SequencePanel が編集のたびにここへ差し替え、revision を進めて
    /// @note System へ「作り直せ」と伝える。
    std::shared_ptr<const asset::SequenceAsset> authoringSequence; ///< エディタが編集した未保存の演出。存在する間はディスク上のアセットより優先する。
    std::uint64_t authoringRevision = 0; ///< 編集中のシーケンスデータを更新するたびに進める版番号。

    /// @note 現在のスナップショットがどのアセットのものか。識別にしか使わない。
    /// @note デリファレンスしないのは、差し替え後は無効なアドレスになりうるため。
    /// @note 「変わったか」を知るためだけに持ち、中身は必ず今回解決したポインタから読む。
    const void*   appliedSequence = nullptr; ///< 最後に適用したアセットを比較するための識別アドレス。参照先は読み取らない。
    std::uint64_t appliedAuthoringRevision = 0; ///< 現在のスナップショットを取得したときの編集データの版番号。
    int           appliedAssetGeneration   = -1; ///< 現在のスナップショットを取得したときのアセット世代。

    std::vector<SequenceTrackRuntime> trackRuntime; ///< シーケンスのトラック添字ごとの対象解決、再生と復元の実行時状態。
    bool awakeHandled = false; ///< 初回評価時の自動再生を処理済みかどうか。

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

    /// @note binding キーへ実体を割り当てる。既存キーは差し替える。
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
        // int を経由するのは、列挙の実体が uint8_t で int& へ reinterpret_cast すると
        // 1 バイトの変数へ 4 バイト書き込み、隣のフィールドを壊すため。
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
        if (removeIndex < bindings.size()) {
            bindings.erase(bindings.begin() + static_cast<std::ptrdiff_t>(removeIndex));
        }
    }
    /// @}
};

}
