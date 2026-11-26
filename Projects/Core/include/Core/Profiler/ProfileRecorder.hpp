/// @file    ProfileRecorder.hpp
/// @brief   対象に依存しない有界な CPU 区間収集と障害復旧。
/// @author  Hasegawa Jin
/// @date    2026-10-03
#pragma once
#include <Core/Api.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace fbzz::profiler {

enum class ProfileSampleStatus : uint8_t {
    COMPLETE, ///< 正常に閉じた区間、または開始と終了を個別に持たないイベント。
    FAULTED, ///< 障害終了として区間を閉じた。
    ABORTED, ///< 復元やフレーム確定時に、未終了の区間を中断として閉じた。
};
enum class ProfileSampleKind : uint8_t {
    TIMED_SCOPE, ///< 開始・終了時に記録器の単調時計を読んで計測した区間。
    EXTERNAL_DURATION, ///< 外部から所要時間だけを渡されたサンプル。
    INSTANT, ///< フレーム内の時刻だけを記録した瞬間イベント。
};

/// @brief フレーム内で記録した一つの区間または瞬間イベント。
struct ProfileSample {
    uint64_t sampleId = 0;        ///< 収集した個々のサンプルを識別する ID。
    uint64_t parentSampleId = 0;  ///< 記録に残った最も近い親サンプルの ID。最上位は 0。
    uint64_t sampleKey = 0;       ///< 区間名・分類・表示色を引くための記述子キー。
    double startOffsetMs = 0.0;   ///< フレーム開始から区間開始までのミリ秒。startAvailable が true のとき有効。
    double inclusiveMs = 0.0;     ///< 子区間を含む経過時間（ミリ秒）。瞬間イベントは 0。
    double selfMs = 0.0;          ///< 直接の子区間にかかった時間を除いたミリ秒。selfAvailable が true のとき有効。
    uint32_t depth = 0;           ///< 記録に残った祖先の数。最上位は 0。
    uint32_t completionOrder = 0; ///< フレーム内でサンプルが終了した順番（1 始まり）。
    ProfileSampleStatus status = ProfileSampleStatus::COMPLETE; ///< 正常終了、障害終了、中断を区別する終了状態。
    ProfileSampleKind sampleKind = ProfileSampleKind::TIMED_SCOPE; ///< 区間計測、外部から渡された所要時間、瞬間イベントの区別。
    bool startAvailable = true;   ///< フレーム内の開始位置を特定できるとき true。
    bool selfAvailable = true;    ///< 子区間を除く所要時間を求められるとき true。
};

/// @brief 開いた計測区間を対応する終了処理に渡すための識別情報。
/// @note 所有ポインターを持たず、SEH 境界を値として越えられる。
struct ProfileToken {
    uint64_t recorderId = 0;    ///< 発行元の計測器 ID。0 は無効なトークン。
    uint64_t generation = 0;    ///< 発行時の世代。リセット前のトークンを失効させる。
    uint64_t frame = 0;         ///< 発行元の計測フレーム番号。
    uint64_t scopeId = 0;       ///< 開始と終了を対応付ける区間 ID。
    uint64_t parentScopeId = 0; ///< 抑制区間を閉じるときに戻す親の ID。最初の抑制区間では 0。
    uint32_t depth = 0;         ///< 開始時の入れ子の深さ。記録を抑制した区間も数える。
    bool suppressed = false;    ///< スタックに積まず、抑制中の入れ子として終了を追跡するとき true。
};

/// @brief 同じ計測器・世代・フレーム内で入れ子を復元するための位置。
struct ProfileCheckpoint {
    uint64_t recorderId = 0;            ///< 復元先の計測器 ID。0 は無効な復元位置。
    uint64_t generation = 0;            ///< 保存時の世代。リセット後の計測器への復元を防ぐ。
    uint64_t frame = 0;                 ///< 復元を許す計測フレーム番号。
    uint64_t topScopeId = 0;            ///< 保存時のスタック頂上の区間 ID。空なら 0。
    uint64_t suppressionTopScopeId = 0; ///< 保存時に最も内側にあった抑制区間の ID。抑制なしなら 0。
    uint32_t stackSize = 0;             ///< 復元後にスタックへ残す区間数。
    uint32_t suppressionDepth = 0;      ///< 復元後に残す抑制区間の入れ子の数。
};

/// @brief 一つの計測フレームに属するサンプルと収集状態。
struct ProfileSnapshot {
    bool recording = false;                    ///< 計測器で記録が有効なとき true。
    bool available = false;                    ///< 参照できるフレームの計測結果があるとき true。
    bool complete = true;                      ///< 取りこぼし・異常終了による欠落がないとき true。フィルターによる除外は許容する。
    bool wallFrameIntervalAvailable = false;   ///< 同じ収集セッションの直前フレームと開始時刻を比較できるとき true。
    uint64_t captureSessionId = 1;             ///< 収集セッションの識別子。記録状態の切り替えやクリアで更新する。
    uint64_t applicationFrameSerial = 0;       ///< 呼び出し側が指定したフレーム番号。ほかの計測との照合に使う。
    uint64_t frameIndex = 0;                   ///< 計測器が記録対象のフレームに割り当てる通し番号。
    uint64_t unframedInvocationCount = 0;      ///< フレームが開いていない間の計測要求数。次の記録フレームに載せる。
    uint32_t recordedSampleCount = 0;          ///< このフレームで保存したサンプルの件数。
    uint32_t droppedSampleCount = 0;           ///< 容量・深さ・記述子の制約で記録できなかった件数。
    uint32_t filteredSampleCount = 0;          ///< 収集フィルターの条件に合わず除外された件数。
    uint32_t sampleCapacity = 8192;            ///< このフレームに保存できるサンプルの上限。
    uint32_t capacityDroppedSampleCount = 0;   ///< サンプルの保存枠を使い切ったため取りこぼした件数。
    uint32_t depthDroppedSampleCount = 0;      ///< 入れ子の深さや親区間の抑制により取りこぼした件数。
    uint32_t descriptorDroppedSampleCount = 0; ///< 計測対象の記述子キーを取得できず取りこぼした件数。
    uint32_t faultedSampleCount = 0;           ///< 障害終了として閉じた区間の数。
    uint32_t abortedSampleCount = 0;           ///< 復旧またはフレーム終了時に中断として閉じた区間の数。
    double wallFrameIntervalMs = 0.0;          ///< 直前と今回のフレーム開始時刻の差（ミリ秒）。
    double cpuFrameElapsedMs = 0.0;            ///< フレーム開始から計測終了までの単調時計の経過時間（ミリ秒）。
    double scopeRootSumMs = 0.0;               ///< 記録された最上位サンプルの経過時間の合計（ミリ秒）。
    std::vector<ProfileSample> samples;        ///< このフレームが所有するサンプル列。終了順は completionOrder で参照する。
};

/// @pre 各インスタンスの全操作は同じ所有スレッドから呼ぶ。
/// @note 時刻は単調時計のミリ秒。注入時計はテストにも同じ契約を要求する。
class ProfileRecorder {
public:
    using ClockFn = double (*)();
    static constexpr size_t MAX_SAMPLES = 8192; ///< 1 フレームの保存サンプル数と抑制中の復元位置数の上限。
    static constexpr size_t MAX_DEPTH = 128; ///< 通常スタックで追跡する未終了区間数の上限。
    FBZZ_CORE_API explicit ProfileRecorder(bool recording = true, ClockFn clock = nullptr);
    ProfileRecorder(const ProfileRecorder&) = delete;
    ProfileRecorder& operator=(const ProfileRecorder&) = delete;
    FBZZ_CORE_API static double NowMs();
    FBZZ_CORE_API double ReadClock() const;
    bool IsRecording() const { return m_recording; }
    /// @note 次のフレーム境界に適用する記録要求があれば、その値を現在値より優先する。
    bool IsRecordingRequested() const { return m_hasPendingRecording ? m_pendingRecording : m_recording; }
    bool IsFrameOpen() const { return m_frameOpen; }
    FBZZ_CORE_API void RequestRecording(bool recording);
    FBZZ_CORE_API void RequestClear();
    /// @note 旧 API 専用の即時失効。新しい UI は RequestRecording を使う。
    FBZZ_CORE_API void SetRecordingImmediate(bool recording);
    FBZZ_CORE_API void BeginFrame(uint64_t applicationFrameSerial, double frameStartMs);
    FBZZ_CORE_API void EndFrame();
    /// @note collect が false なら時計を読まず、サンプルも保存しない。入れ子は追跡し、子区間は個別に選択できる。
    FBZZ_CORE_API ProfileToken Begin(uint64_t sampleKey, bool collect = true);
    FBZZ_CORE_API void End(ProfileToken token, ProfileSampleStatus status = ProfileSampleStatus::COMPLETE);
    FBZZ_CORE_API void Push(uint64_t sampleKey, double durationMs, ProfileSampleKind kind, bool collect = true);
    /// @note 抑制中に保持できる復元位置は MAX_SAMPLES 件。上限を超えた要求には無効な値を返し、外側の復元位置を維持する。
    FBZZ_CORE_API ProfileCheckpoint Checkpoint() const;
    FBZZ_CORE_API bool Recover(ProfileCheckpoint checkpoint);
    const ProfileSnapshot& GetSnapshot() const { return m_snapshot; }
private:
    friend class Profiler;
    /// @note Profiler が所有するスナップショットへ直接結果を渡し、サンプル配列の二重コピーを避ける。
    /// @param[in,out] destination 確定フレームの受け取り先。保持済みの配列は次のフレーム収集に再利用できる。
    FBZZ_CORE_API void EndFrameTo(ProfileSnapshot& destination);
    /// @brief 区間を閉じるまで保持する計測時刻と親子関係。
    struct ActiveScope {
        ProfileToken token;               ///< 対応する終了要求を識別する開始時のトークン。
        double startMs = 0.0;             ///< 区間開始時に単調時計から読んだ時刻（ミリ秒）。
        double childMs = 0.0;             ///< 終了済みの直接の子区間にかかった時間の合計（ミリ秒）。
        size_t sampleIndex = MAX_SAMPLES; ///< 結果を書き込む位置。MAX_SAMPLES は保存枠なし。
        bool selfAvailable = true;        ///< 子区間の所要時間を差し引いて自身の時間を求められるとき true。
        bool filtered = false;            ///< 収集対象外でも子区間の入れ子を追跡するために保持した区間。
        uint64_t nearestSampleId = 0;     ///< 子が参照する最も近い記録済みサンプルの ID。自身を記録した場合はその ID。
        uint32_t retainedDepth = 0;       ///< 次に記録する子サンプルの深さ。記録から除外した区間は数えない。
    };
    /// @brief 復元時に、保存した抑制区間がまだ開いているかを確かめるための識別情報。
    struct SuppressionAnchor {
        uint64_t scopeId = 0; ///< 復元位置が属する抑制区間の ID。同じ深さの別区間と区別する。
        uint32_t depth = 0;   ///< 復元位置を保存したときの抑制区間の入れ子の数。
    };
    FBZZ_CORE_API bool Matches(uint64_t recorderId, uint64_t generation, uint64_t frame) const;
    FBZZ_CORE_API void MarkIncomplete();
    FBZZ_CORE_API void Reset();
    ClockFn m_clock = nullptr; ///< 単調時計をミリ秒で読む関数。テストは同じ契約の関数を渡す。
    uint64_t m_recorderId = 0; ///< 別の計測器が発行したトークンを排除するための ID。
    uint64_t m_generation = 1; ///< リセット前に発行したトークンを無効にするための世代。
    uint64_t m_frameIndex = 0; ///< この計測器が記録を開始したフレームの通し番号。
    uint64_t m_nextScopeId = 0; ///< 次の区間 ID の採番に用いるカウンター。
    uint64_t m_nextSampleId = 0; ///< 次の保存サンプル ID の採番に用いるカウンター。
    uint64_t m_captureSessionId = 1; ///< 記録切り替えとクリアで進む収集セッション ID。
    uint64_t m_unframedInvocationCount = 0; ///< フレームが開いていない間の計測要求数。
    uint64_t m_suppressionTopScopeId = 0; ///< 最も内側で記録を抑制している区間の ID。
    uint32_t m_suppressionDepth = 0; ///< 開いている記録抑制区間の入れ子の数。
    uint32_t m_completionOrder = 0; ///< 現在のフレームで保存サンプルに付けた最後の終了順。
    double m_frameStartMs = 0.0; ///< 開いているフレームの開始時刻（ミリ秒）。
    double m_previousStartMs = 0.0; ///< 次のフレーム開始間隔の計算用に保持する開始時刻（ミリ秒）。
    bool m_havePreviousStart = false; ///< 保持した開始時刻を次のフレームと比較できる場合に true。
    bool m_recording = true; ///< 現在の記録状態。有効なら true。
    bool m_frameOpen = false; ///< 区間を受け付ける記録フレームが開いている場合に true。
    bool m_pendingRecording = true; ///< 次のフレーム境界で適用する記録状態。
    bool m_hasPendingRecording = false; ///< 次のフレームへ保留した記録状態の要求がある場合に true。
    bool m_pendingClear = false; ///< 次のフレームで計測結果と継続中の区間をクリアする要求。
    std::vector<ActiveScope> m_stack; ///< 記録を抑制していない未終了区間を外側から保持するスタック。
    mutable std::vector<SuppressionAnchor> m_suppressionAnchors; ///< 記録抑制中に保存した復元位置を照合する表。
    ProfileSnapshot m_current; ///< 現在収集中のフレーム結果。
    ProfileSnapshot m_snapshot; ///< 計測器自身が確定して保持する直近のフレーム結果。
};

} // namespace fbzz::profiler
