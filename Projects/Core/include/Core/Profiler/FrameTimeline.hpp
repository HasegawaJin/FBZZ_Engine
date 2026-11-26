/// @file    FrameTimeline.hpp
/// @brief   フレーム・描画パケット・待機を、コールバックを使わず固定容量で時系列に記録する任意計測機能。
/// @author  Hasegawa Jin
/// @date    2026-10-16
#pragma once
#include <Core/Api.hpp>

#include <cstddef>
#include <cstdint>

namespace fbzz::profiler::frame_timeline {

enum class Event : uint16_t {
    MAIN_FRAME, TIME_TICK, INPUT, WINDOW_EVENTS, INPUT_CALLBACKS, INPUT_ACTIONS,
    ASSET_PUMP, MODULE_UPDATE, MODULE_LATE_UPDATE, AUDIO_UPDATE, MODULE_RENDER,
    FRAME_TAIL, EDITOR_UPDATE, SIMULATION, VIEW, SKINNING, EXTRACT, ENVIRONMENT,
    GRAPH, POST_CLEANUP, WORKER_FRAME, PACKET, PACKET_EXECUTE, PACKET_DESTROY,
    PACKET_SUBMIT, PACKET_ACCEPT, WAIT_SLOT, WAIT_ACCEPTANCE, WAIT_COMPLETION,
    WAIT_DRAIN, WAIT_OTHER, WAIT_PUMP, EXECUTION_MUTEX, THREAD_MUTEX, FENCE_WAIT,
    SYNCHRONOUS_FENCE_WAIT, PRESENT, QUEUE_SUBMIT, COUNT
};

/// @brief イベントを元のフレームや描画要求と照合するための識別情報。
/// @note 発生元の識別子を値としてコピーする。シーン・リソース・レンダラーの寿命には影響しない。
struct Context {
    uint64_t captureFrameOrdinal = 0;    ///< 収集開始後に数えたフレームの順番（1 始まり）。0 は未指定。
    uint64_t applicationFrameSerial = 0; ///< アプリケーションが発行したフレーム番号。
    uint64_t resourceFrameSerial = 0;    ///< リソース管理側のフレーム番号。GPU 計測との照合に使う。
    uint64_t submissionSerial = 0;       ///< 描画パケットの投入と実行を結び付ける通し番号。
    uint64_t targetSerial = 0;           ///< 待機対象の投入番号またはフェンス値。意味はイベント種別で決まる。
    uint64_t objectId = 0;               ///< イベントの発生元を識別するオブジェクト ID。
    uint32_t viewId = 0;                 ///< イベントが属する描画ビューの識別子。
    uint32_t detail = 0;                 ///< イベント種別に応じた補足値。
};

/// @note フレームループの外で初期化する。FBZZ_FRAME_TIMELINE_OUTPUT が未設定なら時計を読まず、収集領域も確保しない。
FBZZ_CORE_API void InitializeFromEnvironment();
[[nodiscard]] FBZZ_CORE_API bool IsEnabled();
/// @brief 環境変数で指定された出力先に向けてタイムライン収集を準備する。
/// @param[in] observerSession 観測セッションの識別子。0 は無効。
/// @param[in] warmupFrames 計測対象に入る前に進めるフレーム数。0 なら直ちに計測する。
/// @param[in] sampleFrames 収集する計測フレームの数。0 は無効。
/// @return 出力先未設定、収集領域の予約済み、無効な指定、または時刻取得に使う周波数が得られない場合は false。
/// @note 収集はプロセスにつき 1 回。既存の計測器が次のフレーム境界を観測する前に記録領域を予約する。
/// @note 既存の計測区間を保ち、ウォームアップの最終フレーム、計測対象、最後の区間を閉じる次のフレーム開始を記録する。
[[nodiscard]] FBZZ_CORE_API bool ArmFromEnvironment(uint64_t observerSession, size_t warmupFrames, size_t sampleFrames);
/// @brief 現在の収集が指定の観測セッションと一致したときに中止を記録する。
/// @param[in] observerSession ArmFromEnvironment で指定した収集の識別子。
FBZZ_CORE_API void CancelCapture(uint64_t observerSession);
/// @brief 現在のアプリケーションフレームの開始を記録する。
/// @param[in] applicationFrameSerial アプリケーションが発行したフレーム番号。
/// @param[in] resourceFrameSerial リソース管理側が別に発行したフレーム番号。GPU 計測との照合に使う。
/// @param[in] profileStartMs 既存の計測器と開始を照合する時刻 [ms]。
FBZZ_CORE_API void BeginApplicationFrame(uint64_t applicationFrameSerial, uint64_t resourceFrameSerial, double profileStartMs);
/// @brief BeginApplicationFrame で開いた CPU 計測区間を閉じる。
/// @param[in] profileEndMs 開始と同じ時計で測った終了時刻 [ms]。
FBZZ_CORE_API void EndApplicationFrame(double profileEndMs);

[[nodiscard]] FBZZ_CORE_API Context CurrentContext();

class ContextScope final {
public:
    FBZZ_CORE_API explicit ContextScope(Context context);
    FBZZ_CORE_API ~ContextScope();
    ContextScope(const ContextScope&) = delete;
    ContextScope& operator=(const ContextScope&) = delete;
private:
    Context m_previous{}; ///< スコープから戻るときに復元する外側の計測コンテキスト。
    bool m_active = false; ///< 現在のスレッドのコンテキストを置き換えた場合に true。
};

/// @return 計測が無効、対象フレームの範囲外、または記録枠を確保できない場合は 0。
/// @note captureFrameOrdinal が 0 なら現在のスレッドの識別情報を使う。個別に指定する場合は CurrentContext() の値を基にする。
/// @note スレッドの CPU カウンターはフレーム境界などの粗い粒度でのみ取得し、生のサイクル数は経過時間に換算しない。
[[nodiscard]] FBZZ_CORE_API uint64_t Begin(Event event, Context context = {}, uint64_t value = 0, bool threadCpu = false);
/// @pre 0 以外のチケットは、開始した OS スレッドで一度だけ終了させること。
FBZZ_CORE_API void End(uint64_t ticket, uint64_t value = 0);
FBZZ_CORE_API void Mark(Event event, Context context = {}, uint64_t value = 0);

class Scope final {
public:
    FBZZ_CORE_API explicit Scope(Event event, Context context = {}, uint64_t value = 0, bool threadCpu = false);
    FBZZ_CORE_API ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    uint64_t m_ticket = 0; ///< デストラクタで閉じる計測区間のチケット。0 は未記録。
};

/// @pre すべての描画ワーカーと計測区間が通常の終了手順を終えていること。
/// @note CSV への変換とファイル出力はここでのみ行う。RHI の待機やコールバックの呼び出しは行わない。
[[nodiscard]] FBZZ_CORE_API bool WriteAfterStop();

} // namespace fbzz::profiler::frame_timeline
