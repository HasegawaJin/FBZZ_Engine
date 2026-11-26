/// @file    FrameTimelineCapture.hpp
/// @brief   収集とファイル出力が共有するタイムラインのデータ。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#pragma once

#include <Core/Profiler/FrameTimeline.hpp>
#include "FrameTimelineStorage.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace fbzz::profiler::frame_timeline::detail {

inline constexpr uint64_t INVALID_INDEX = UINT64_MAX;

inline constexpr size_t CAPTURE_ANCHOR_COUNT = 2;

inline constexpr unsigned DEFERRED_SLOT_BITS = 6;
inline constexpr size_t MAX_PENDING_FINE_SPANS = size_t{1} << DEFERRED_SLOT_BITS;
inline constexpr uint64_t FINE_SPANS_PER_SECOND = 200000;

enum class Edge : uint8_t { BEGIN, END, INSTANT };

/// @brief 一つのイベント境界で取得したスレッドの累積 CPU カウンター。
struct CpuCounters {
    uint64_t kernel = 0; ///< このスレッドがカーネルモードで実行された累積時間（100 ナノ秒単位）。
    uint64_t user = 0; ///< このスレッドがユーザーモードで実行された累積時間（100 ナノ秒単位）。
    uint64_t cycles = 0; ///< このスレッドの累積 CPU サイクル数。経過時間には換算しない。
    bool cpuAvailable = false; ///< カーネル時間とユーザー時間の取得に成功したとき true。
    bool cyclesAvailable = false; ///< サイクル数の取得に成功したとき true。
};
/// @brief イベントの発生時刻と対応する計測区間を表す記録。
struct EventRow {
    Context context{};               ///< このイベントに関連付けたフレーム・投入元の識別情報。
    uint64_t qpc = 0; ///< イベントが発生した時刻（QPC ティック）。
    uint64_t scope = 0; ///< 開始行と終了行を結び付けるチケット。瞬間イベントは 0。
    uint64_t value = 0; ///< イベントと記録時点に応じて呼び出し側が渡した値。
    uint64_t cpu = INVALID_INDEX; ///< 対応する CPU カウンターの配列添字。記録がなければ INVALID_INDEX。
    uint32_t osThreadId = 0;         ///< イベントを記録した OS スレッドの識別子。
    Event event = Event::MAIN_FRAME; ///< フレーム処理、パケット処理、待機などのイベント種別。
    Edge edge = Edge::INSTANT;       ///< 区間の開始・終了、または瞬間イベントの区別。
    bool threadCpu = false;          ///< この区間の開始・終了でスレッドの CPU カウンターを取得する指定。
    std::atomic<bool> ended{false};  ///< 開始行に対する終了を受理済みなら true。二重終了の検出に使う。
};
/// @brief 一つのフレームについて二つの時計の境界時刻を対応付ける記録。
struct FrameRow {
    Context context{};           ///< フレームの開始時に確定した識別情報。
    detail::FrameTimes times;    ///< このフレームの処理時間と次の開始までの間隔を求める QPC 時刻。
    double profileBeginMs = 0.0; ///< 既存プロファイラの時計でのフレーム開始時刻（ミリ秒）。
    double profileEndMs = 0.0; ///< 既存プロファイラの時計でのフレーム終了時刻（ミリ秒）。
    uint32_t osThreadId = 0;     ///< フレーム開始を記録した OS スレッドの識別子。
    detail::FrameRole role = detail::FrameRole::UNSELECTED; ///< ウォームアップの最終フレーム、計測対象、終端の時刻記録などの役割。
};
/// @brief 一回の収集で使う固定容量の記録領域と、記録の完全性を判定するための統計。
struct Capture {
    Capture(uint64_t session, size_t warmupCount, size_t requestedSamples, size_t maxEvents)
        : observerSession(session),
          warmup(warmupCount),
          samples(requestedSamples),
          eventCapacity(maxEvents),
          cpuCapacity((requestedSamples + CAPTURE_ANCHOR_COUNT) * 4 + 64),
          window(warmupCount, requestedSamples),
          events(std::make_unique<EventRow[]>(maxEvents)),
          cpu(std::make_unique<CpuCounters[]>(cpuCapacity)),
          frames(std::make_unique<FrameRow[]>(requestedSamples + CAPTURE_ANCHOR_COUNT))
    {
    }
    uint64_t observerSession = 0; ///< この収集を開始した観測セッションの ID。中止要求の照合にも使う。
    uint64_t warmup = 0; ///< 計測前に進めるウォームアップのフレーム数。
    uint64_t samples = 0; ///< 収集開始時に指定した計測対象のフレーム数。
    uint64_t frequency = 0; ///< QPC の周波数（1 秒あたりのティック数）。
    size_t eventCapacity = 0; ///< 開始・終了・瞬間イベントを合計して保存できる行数。
    size_t cpuCapacity = 0; ///< スレッドの CPU カウンターを保存できる件数。
    detail::FrameWindow window;                      ///< フレームの通過順からウォームアップ、計測対象、終端を判定する。
    std::unique_ptr<EventRow[]> events;              ///< 開始時に確保し、各スレッドが予約した位置へ書き込むイベント行。
    std::unique_ptr<CpuCounters[]> cpu;              ///< イベント行の添字から参照する、予約済みの CPU カウンター配列。
    std::unique_ptr<FrameRow[]> frames;              ///< 計測フレームと前後の境界時刻を保持するフレーム行。
    std::atomic<uint64_t> eventCount{0}; ///< イベント行の予約試行数。容量超過による失敗も数える。
    std::atomic<uint64_t> cpuCount{0}; ///< CPU カウンターの予約試行数。容量超過による失敗も数える。
    std::atomic<uint64_t> droppedEvents{0}; ///< 容量不足で保存できなかったイベント行数。
    std::atomic<uint64_t> droppedCpu{0}; ///< 容量不足で保存できなかった CPU カウンターの件数。
    std::atomic<uint64_t> activeWriters{0}; ///< イベントの書き込み処理に入っている数。停止後の出力可否の判定に使う。
    std::atomic<uint64_t> activeScopes{0}; ///< 終了を待っている計測区間の数。スレッド内で保留中の区間も含む。
    std::atomic<uint64_t> wallQueries{0}; ///< イベントやフレーム境界で QPC の読み取りを試みた回数。
    std::atomic<uint64_t> cpuQueries{0}; ///< 予約した記録枠へスレッドの CPU カウンターを取得しようとした回数。
    std::atomic<uint64_t> invalidBoundaries{0}; ///< 時刻やフレーム番号の不整合、不正な区間終了などを検出した回数。
    std::array<std::atomic<uint64_t>, static_cast<size_t>(Event::COUNT)> filteredFineScopes{}; ///< 短すぎてイベント行を省略した区間の数（イベント種別ごと）。
    std::array<std::atomic<uint64_t>, static_cast<size_t>(Event::COUNT)> filteredFineTicks{}; ///< 省略した区間の長さの合計（種別ごとの QPC ティック、区間の重複を含む）。
    std::atomic<uint64_t> fineSpanFallbacks{0}; ///< スレッドの保留枠が埋まり、短い区間も通常どおり記録した回数。
    std::atomic<uint64_t> fineTicketSequence{0}; ///< 再利用した保留枠を以前のチケットと区別するための連番。
    std::atomic<bool> closing{false}; ///< 出力に備えて新規のイベント書き込みを止めている間は true。
    std::atomic<bool> cancelled{false}; ///< この収集の中止要求を受理したとき true。
    size_t frameCount = 0; ///< 境界時刻の記録用フレームを含め、配列に追加したフレーム行数。
    size_t sampleCount = 0; ///< 実際に計測対象として数えたフレーム数。
    bool closingAnchor = false; ///< 最後の計測フレームに続くフレームの開始を記録できたとき true。
    bool exported = false; ///< CSV とメタデータをともに書き出せたとき true。
};

/// @note スレッドの CPU カウンターを要求しない場合、この高頻度イベント群に限り 5 マイクロ秒未満の区間を省略する。
inline bool IsFineSpan(Event event)
{
    return event == Event::EXECUTION_MUTEX || event == Event::THREAD_MUTEX
        || event == Event::WAIT_SLOT || event == Event::PACKET_DESTROY;
}

inline uint64_t FineThresholdTicks(const Capture& capture)
{
    return capture.frequency / FINE_SPANS_PER_SECOND + (capture.frequency % FINE_SPANS_PER_SECOND != 0);
}

/// @brief タイムラインを CSV とメタデータへ保存する。
/// @param[in] capture 書き込みが完了した収集結果。
/// @param[in] outputFile UTF-8 の CSV パス。同じパスに `.meta.json` を付けた先へメタデータを保存する。
/// @pre 新規イベントの受付を止め、残っている書き込みと計測区間を完了していること。
/// @return 同名ファイルがあるか保存に失敗した場合は false。失敗段階に応じて一時ファイルや出力済みファイルが残る。
bool PublishCaptureFiles(const Capture& capture, const std::string& outputFile);

} // namespace fbzz::profiler::frame_timeline::detail
