/// @file    Profiler.hpp
/// @brief   CPU 計測の互換 API と、計測結果を所有するスナップショット。
/// @author  Hasegawa Jin
/// @date    2026-06-02
#pragma once
#include <Core/Api.hpp>

#include <Core/Profiler/ProfilerMarker.hpp>
#include <Core/Profiler/ProfileRecorder.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::profiler {

/// @brief 旧 API が終了順に公開する計測レコード。
/// @note 旧 ABI のレイアウトを維持する。次のフレーム確定、収集セッションの切り替え、または即時リセットで文字列は失効する。
struct ProfileRecord {
    const char* name = "Unnamed";     ///< 計測区間の表示名。公開済みの計測記述子が文字列を所有する。
    const char* category = "General"; ///< 計測区間の分類名。公開済みの計測記述子が文字列を所有する。
    double elapsedMs = 0.0;           ///< 子区間を含む経過時間（ミリ秒）。
    uint64_t frameIndex = 0;          ///< このレコードを公開した計測器のフレーム番号。
    uint32_t depth = 0;               ///< 記録された祖先区間の数。最上位は 0。
    uint32_t color = 0xFF4FA3FF;      ///< 計測区間の表示色（0xAARRGGBB）。
};

/// @brief 同じマーカーを複数のサンプルから参照するための表示情報。
struct PerformanceDescriptor {
    uint64_t sampleKey = 0;      ///< 同じスレッドでサンプルとこの記述子を結び付けるキー。0 は未登録。
    std::string name;            ///< 記述子が所有する計測区間の表示名。
    std::string category;        ///< 記述子が所有する分類名。収集フィルターとの照合に使う。
    uint32_t color = 0xFF4FA3FF; ///< 表示色（0xAARRGGBB）。名前・分類と合わせて計測対象を識別する。
};

/// @brief フレームに収集する計測区間の選択条件。
struct PerformanceCaptureFilter {
    std::string category; ///< 分類名との完全一致に使う文字列。大文字小文字を区別し、空ならすべての分類が対象。
    std::string scope;    ///< 区間名に含む文字列。大文字小文字を区別し、空ならすべての区間が対象。
    bool operator==(const PerformanceCaptureFilter&) const = default;
};

/// @brief 計測結果に、そのフレームの収集条件と表示情報を加えたスナップショット。
struct PerformanceSnapshot : ProfileSnapshot {
    PerformanceCaptureFilter captureFilter;         ///< このフレームの収集開始時に適用した条件。
    std::vector<PerformanceDescriptor> descriptors; ///< このフレームのサンプルが参照する記述子。表示に使う文字列も所有する。
};

/// @brief 計測器と互換 API の両方を巻き戻すための復元位置。
/// @note 計測器と互換 API の開始位置をまとめて保存する。所有ポインターを持たず、構造化例外処理（SEH）の境界を越えられる。
struct ProfilerCheckpoint {
    ProfileCheckpoint recorder;         ///< 計測器本体のスタックを復元する位置。
    ProfileCheckpoint compatOverflow;   ///< 互換 API が深さの上限を超える直前に保存した復元位置。
    uint64_t compatTopScopeId = 0;      ///< 保存した互換スタックの頂上を識別する ID。空なら 0。
    uint64_t compatOverflowScopeId = 0; ///< 互換 API で最初に抑制した区間の ID。同じ深さの別区間への復元を防ぐ。
    uint32_t compatStackSize = 0;       ///< 保存時に互換スタックが保持していたトークン数。
    uint32_t compatSuppressedDepth = 0; ///< 互換スタックの外で終了を待つ、抑制中の入れ子の数。
};

/// @note 各スレッドが計測器、記録要求、記述子、公開結果を保持する。収集フィルターの要求は全スレッドで共有する。
/// @pre 計測の開始・終了・チェックポイントからの復元は同じスレッドで行うこと。
class Profiler {
public:
    /// @note トークンと公開データを即時に失効させる旧 API。フレーム境界での切り替えには RequestRecording を使う。
    FBZZ_CORE_API static void SetEnabled(bool enabled);
    FBZZ_CORE_API static bool IsEnabled();
    /// @note 次のフレーム境界への適用を待っている要求も含め、記録が要求されているかを返す。
    FBZZ_CORE_API static bool IsRecordingRequested();
    FBZZ_CORE_API static void RequestRecording(bool recording);
    FBZZ_CORE_API static void RequestClear();
    /// @param[in] filter 全スレッドで共有する分類と区間名の条件。
    /// @note 各計測スレッドの次の BeginFrame で適用する。条件が変わると新しい収集セッションを開始する。
    /// @return category が 128 バイト、または scope が 256 バイトを超えた場合は設定を維持して false。
    FBZZ_CORE_API static bool RequestCaptureFilter(PerformanceCaptureFilter filter);
    FBZZ_CORE_API static PerformanceCaptureFilter GetRequestedCaptureFilter();
    FBZZ_CORE_API static void BeginFrame();
    /// @param[in] applicationFrameSerial 呼び出し側が採番したフレーム番号。他の計測結果との照合に使う。
    /// @param[in] frameStartMs ProfileRecorder::NowMs() と同じ単調時計上のフレーム開始時刻（ミリ秒）。
    FBZZ_CORE_API static void BeginFrame(uint64_t applicationFrameSerial, double frameStartMs);
    FBZZ_CORE_API static void EndFrame();
    FBZZ_CORE_API static void BeginSample(const ProfilerMarker& marker);
    FBZZ_CORE_API static void EndSample();
    FBZZ_CORE_API static ProfileToken BeginScope(const ProfilerMarker& marker);
    FBZZ_CORE_API static void EndScope(ProfileToken token);
    FBZZ_CORE_API static void PushMarker(const ProfilerMarker& marker);
    /// @note 外部で計測した所要時間だけを登録するため、開始位置と子区間を除いた所要時間は取得できない。
    /// @param[in] elapsedMs 外部で計測した所要時間（ミリ秒）。負値は 0 として扱う。
    FBZZ_CORE_API static void PushSample(const ProfilerMarker& marker, double elapsedMs);
    FBZZ_CORE_API static ProfilerCheckpoint Checkpoint();
    FBZZ_CORE_API static void Recover(ProfilerCheckpoint checkpoint);
    /// @return 現在のスレッドの確定結果から終了順で組み立てた旧 API 向けのレコード。
    FBZZ_CORE_API static const std::vector<ProfileRecord>& GetLastFrameRecords();
    FBZZ_CORE_API static uint64_t GetLastFrameIndex();
    /// @return 現在のスレッドが保持する公開結果への参照。以後のフレーム境界や記録設定の適用で内容は更新される。
    FBZZ_CORE_API static const PerformanceSnapshot& GetSnapshot();
};

} // namespace fbzz::profiler
