/// @file    RenderCpuTrace.hpp
/// @brief   Opt-in bounded CSV diagnostics for producer CPU phases and render-thread synchronization.
/// @author  Hasegawa Jin
/// @date    2026-10-13
#pragma once
#include <Graphics/Api.hpp>

#include <cstdint>

namespace fbzz::renderer::cpu_trace {

enum class Phase : uint8_t { OUTSIDE, SETUP, SKIN, EXTRACT, ENVIRONMENT_PIPELINE, GRAPH, POST_CLEANUP, VIEW };
enum class Source : uint8_t { UNKNOWN, BUFFER_GET, BATCH_STATIC, BATCH_DEFORMED, BATCH_FLUSH };
enum class WaitKind : uint8_t { UNSPECIFIED, SLOT, ACCEPTANCE, COMPLETION, DRAIN };
enum class Counter : uint8_t { SYNC, CPU_FRAME_WAIT, BUFFER_PRODUCER, BUFFER_WORKER, BUFFER_INVALID, BUFFER_DETACHED, EMPTY_SUBMIT, PACKETS, COMMANDS, SOURCE_OPERATIONS, COUNT };
enum class AcquireKind : uint8_t { EXECUTION_MUTEX, THREAD_MUTEX };
enum class TestCpuMode : uint8_t { NATIVE, UNAVAILABLE, REGRESSING };

/// @note Fixed upper bounds; exceeding any bound marks the capture incomplete rather than growing storage.
inline constexpr uint32_t MAX_SELECTED_FRAMES = 32, MAX_VIEWS = 256, MAX_PHASES = 2048;
inline constexpr uint32_t MAX_DOMAINS = 8192, MAX_PACKETS = 65536, MAX_THREADS = 16;
inline constexpr uint32_t MAX_API_ORIGINS = 64, MAX_API_ORIGIN_BYTES = 512, MAX_REENTRY = 8;

namespace detail {
struct Context {
    uint64_t threadId = 0; ///< 登録した描画スレッドを再利用前後で区別する寿命 ID。
    uint64_t applicationFrame = 0; ///< 記録元のアプリケーションフレーム番号。
    uint64_t resourceFrame = 0; ///< ビューが参照する ResourceManager の描画フレーム番号。
    uint64_t resourceInstance = 0; ///< ビューが参照する資源管理器の識別番号。
    uint64_t resourceEpoch = 0; ///< 資源管理器をリセットした前後を区別する世代。
    uint64_t sceneGeneration = 0; ///< 同一フレーム番号を持つ別シーンを区別する世代。
    uint32_t token = 0; ///< 収集対象フレームに割り当てた計測トークン。
    uint32_t viewOrdinal = 0; ///< 収集内のビュー呼び出しを識別する通し番号。
    uint32_t parentViewOrdinal = 0; ///< 呼び出しをネストしたときの親ビューの通し番号。
    uint32_t viewId = 0; ///< ホストが設定したビューの識別番号。
    uint32_t outputId = 0; ///< ビュー出力のレンダーターゲットのスロット添字。
    uint32_t outputGeneration = 0; ///< ビュー出力のレンダーターゲットのスロット世代。
    uint32_t depth = 0; ///< 親子関係をたどるためのビュー呼び出しの深さ。
    Phase phase = Phase::OUTSIDE; ///< この文脈で計測している描画段階。
    Source source = Source::UNKNOWN; ///< バッファ取得やバッチ等の呼び出し元分類。
    WaitKind waitKind = WaitKind::UNSPECIFIED; ///< この文脈で測る同期待ちの分類。
    uint16_t api = 0; ///< 呼び出し元 API 名を引く計測内の識別番号。
    bool worker = false; ///< ワーカー側の実行を測っている文脈かどうか。
};
/// @note CPU cycles remain raw execution counters; never convert them to elapsed time using an assumed frequency.
struct Stamp {
    uint64_t wall = 0; ///< 単調時計から読み取った経過時間の基準値 [ns]。
    uint64_t kernel = 0; ///< 当該スレッドのカーネルモード累積 CPU 時間 [100 ns]。
    uint64_t user = 0; ///< 当該スレッドのユーザーモード累積 CPU 時間 [100 ns]。
    uint64_t thread = 0; ///< 計測を読み取った OS スレッドの識別値。
    uint64_t cycles = 0; ///< 当該スレッドで実行された CPU サイクルの累積値。
    bool cpuAvailable = false; ///< スレッドの CPU 時間を取得できたかどうか。
    bool cyclesAvailable = false; ///< スレッドの CPU サイクル数を取得できたかどうか。
};
} // namespace detail

/// @note Initialize outside the hot loop. Missing FBZZ_RENDER_CPU_TRACE_OUTPUT leaves all capture branches disabled.
FBZZ_GRAPHICS_API void InitializeFromEnvironment();
[[nodiscard]] FBZZ_GRAPHICS_API bool IsEnabled();
[[nodiscard]] FBZZ_GRAPHICS_API bool HasSelectedFrame();
/// @note Register identity includes a monotonically issued lifetime ID, not only the object's address.
FBZZ_GRAPHICS_API void RegisterThread(const void* thread);
FBZZ_GRAPHICS_API void UnregisterThread(const void* thread);
FBZZ_GRAPHICS_API void ThreadStopped(const void* thread);
FBZZ_GRAPHICS_API void BeginFrame(const void* thread, uint64_t applicationFrame);
FBZZ_GRAPHICS_API void Count(Counter counter, uint64_t amount = 1);
/// @note The packet's immutable origin describes submission, not the exclusive origin of all queued commands.
FBZZ_GRAPHICS_API void BindPacket(const void* thread, uint64_t serial, uint64_t commands);
FBZZ_GRAPHICS_API void PacketAccepted(const void* thread, uint64_t serial, bool accepted);
/// @pre All registered worker threads have stopped through the existing drain/Stop path.
/// @note No additional RHI wait is introduced; serialization and file I/O occur only here.
[[nodiscard]] FBZZ_GRAPHICS_API bool WriteCsvAfterStop();

class SourceScope final {
public:
    FBZZ_GRAPHICS_API SourceScope(Source source, const char* api = nullptr, bool preserveSource = false);
    FBZZ_GRAPHICS_API ~SourceScope();
    SourceScope(const SourceScope&) = delete;
    SourceScope& operator=(const SourceScope&) = delete;
private:
    Source m_previousSource = Source::UNKNOWN; ///< スコープを抜けるときに復元する呼び出し元分類。
    uint16_t m_previousApi = 0; ///< スコープを抜けるときに復元する API 識別番号。
    bool m_active = false; ///< このスコープが計測文脈を変更したかどうか。
};

class WaitKindScope final {
public:
    FBZZ_GRAPHICS_API explicit WaitKindScope(WaitKind kind);
    FBZZ_GRAPHICS_API ~WaitKindScope();
    WaitKindScope(const WaitKindScope&) = delete;
    WaitKindScope& operator=(const WaitKindScope&) = delete;
private:
    WaitKind m_previous = WaitKind::UNSPECIFIED; ///< スコープを抜けるときに復元する待ち分類。
    bool m_active = false; ///< このスコープが待ち分類を変更したかどうか。
};

class ViewScope final {
public:
    FBZZ_GRAPHICS_API ViewScope(uint64_t resourceInstance, uint64_t resourceEpoch, uint64_t resourceFrame,
        uint64_t sceneGeneration, uint32_t outputId, uint32_t outputGeneration);
    FBZZ_GRAPHICS_API ~ViewScope();
    FBZZ_GRAPHICS_API void SetViewId(uint32_t viewId);
    FBZZ_GRAPHICS_API void SetPhase(Phase phase);
    ViewScope(const ViewScope&) = delete;
    ViewScope& operator=(const ViewScope&) = delete;
private:
    detail::Context m_previous{}; ///< ビュー記録を閉じた後に復元する親の文脈。
    detail::Stamp m_viewStart{}; ///< ビュー全体の時間・CPU 差分の基準となる開始時計値。
    detail::Stamp m_phaseStart{}; ///< 現在の段階の時間・CPU 差分の基準となる開始時計値。
    uint32_t m_viewRow = UINT32_MAX; ///< ビュー計測行の添字。UINT32_MAX は未記録。
    uint32_t m_phaseRow = UINT32_MAX; ///< 現在の段階を記録する行の添字。UINT32_MAX は未記録。
    bool m_active = false; ///< このビューで計測文脈を有効にしたかどうか。
};

/// @note Fast-ready waits count without reading a wall or OS CPU clock.
/// @note Leaf elapsed subtracts only directly nested waits' inclusive elapsed; overflow makes leaf unavailable.
class WaitScope final {
public:
    FBZZ_GRAPHICS_API explicit WaitScope(bool initiallyNotReady);
    FBZZ_GRAPHICS_API ~WaitScope();
    WaitScope(const WaitScope&) = delete;
    WaitScope& operator=(const WaitScope&) = delete;
private:
    friend class PumpScope;
    WaitScope* m_parent = nullptr; ///< 直接親の待ちスコープ。ネストがなければ null。
    uint64_t m_start = 0; ///< 待ち時間計測を始めた単調時計の値 [ns]。
    uint64_t m_children = 0; ///< 直接ネストした待ち時間の合計 [ns]。
    uint32_t m_cell = UINT32_MAX; ///< 待ちの集計先となる文脈ごとの行の添字。UINT32_MAX は未記録。
    uint32_t m_depth = 0; ///< 待ちのネスト深さ。再入上限の判定に使う。
    bool m_timed = false; ///< 初回の準備確認を通過できず、待ち時間を計測するかどうか。
    bool m_leafAvailable = true; ///< 直接ネストした待ちを除いた時間を計算できるかどうか。
};

/// @note Pump elapsed overlaps nested waits and must not be added to wait elapsed.
class PumpScope final {
public:
    FBZZ_GRAPHICS_API PumpScope();
    FBZZ_GRAPHICS_API ~PumpScope();
    PumpScope(const PumpScope&) = delete;
    PumpScope& operator=(const PumpScope&) = delete;
private:
    uint64_t m_start = 0; ///< キュー処理の開始時計値 [ns]。
    uint32_t m_cell = UINT32_MAX; ///< キュー処理を集計する待ち文脈の行の添字。UINT32_MAX は未記録。
};

/// @note Lock acquisition elapsed is not proof that the mutex was contended.
class AcquireScope final {
public:
    FBZZ_GRAPHICS_API explicit AcquireScope(AcquireKind kind);
    FBZZ_GRAPHICS_API ~AcquireScope();
    FBZZ_GRAPHICS_API void Acquired();
    AcquireScope(const AcquireScope&) = delete;
    AcquireScope& operator=(const AcquireScope&) = delete;
private:
    uint64_t m_start = 0; ///< ロックを取得する前の開始時計値 [ns]。
    uint32_t m_cell = UINT32_MAX; ///< ロック取得時間を集計する文脈行の添字。UINT32_MAX は未記録。
    AcquireKind m_kind = AcquireKind::EXECUTION_MUTEX; ///< 実行またはスレッド管理のどちらのロックを計測するか。
};

class PacketScope final {
public:
    FBZZ_GRAPHICS_API PacketScope(const void* thread, uint64_t serial);
    FBZZ_GRAPHICS_API ~PacketScope();
    FBZZ_GRAPHICS_API void BeginDestruction();
    FBZZ_GRAPHICS_API void FinishedDestruction();
    PacketScope(const PacketScope&) = delete;
    PacketScope& operator=(const PacketScope&) = delete;
private:
    detail::Context m_previous{}; ///< パケットの記録後に復元する元の文脈。
    detail::Stamp m_start{}; ///< 実行または破棄の時間・CPU 差分の基準となる時計値。
    uint32_t m_row = UINT32_MAX; ///< 対象パケットの記録行の添字。UINT32_MAX は未記録。
    bool m_destroying = false; ///< コマンド記録の終了後に破棄の計測段階へ入ったかどうか。
    bool m_finished = false; ///< パケットの計測終了を登録済みかどうか。
};

/// @note Cold test/snapshot API; never invoked by the render loop.
struct Summary {
    uint64_t selectedFrames = 0; ///< 記録対象になったアプリケーションフレーム数。
    uint64_t views = 0; ///< 記録したビュー呼び出し数。
    uint64_t phases = 0; ///< 記録した描画段階の行数。
    uint64_t waitCalls = 0; ///< 文脈別に集計した同期待ち API の呼び出し数。
    uint64_t notReady = 0; ///< 最初の準備確認で未完了だった待ちの件数。
    uint64_t waitInclusiveNs = 0; ///< ネストした時間を含む待ち時間の合計 [ns]。
    uint64_t waitLeafNs = 0; ///< 直接ネストした待ちを差し引いた待ち時間の合計 [ns]。
    uint64_t pumpCalls = 0; ///< 待ち中に呼び出したキュー処理の件数。
    uint64_t cpuQueries = 0; ///< スレッド CPU 時間の取得を試みた回数。
    uint64_t wallQueries = 0; ///< 単調時計を読み取った回数。
    uint64_t cycleQueries = 0; ///< スレッドの CPU サイクル数の取得を試みた回数。
    uint64_t packets = 0; ///< 記録したパケット行数。
    uint64_t completedPackets = 0; ///< 受け付けられ、実行と破棄の記録が完了したパケットの件数。
    uint64_t dropped = 0; ///< 収集上限などにより捨てた計測イベント数。
    uint64_t bufferProducer = 0; ///< 描画生成側でのバッファ参照の記録数。
    uint64_t bufferWorker = 0; ///< 描画ワーカー側でのバッファ参照の記録数。
    uint64_t emptySubmits = 0; ///< 処理対象が無い状態で提出した回数。
    uint64_t sourceStaticCalls = 0; ///< 静的ジオメトリのバッチ元操作として集計した回数。
    uint64_t sourceDeformedCalls = 0; ///< 変形済みジオメトリのバッチ元操作として集計した回数。
    bool complete = true; ///< 予定フレームと全記録を欠落なく閉じられたかどうか。
    bool leafAvailable = true; ///< すべての待ち文脈で直接ネストを除いた時間を取得できたかどうか。
    bool cpuAvailable = true; ///< すべての対象区間でスレッド CPU 時間を取得できたかどうか。
    bool cyclesAvailable = true; ///< すべての対象区間で CPU サイクル数を取得できたかどうか。
};
[[nodiscard]] FBZZ_GRAPHICS_API Summary SnapshotSummary();
struct PacketInfo {
    uint64_t threadLifetimeId = 0; ///< パケットを提出した登録スレッドの寿命 ID。
    uint64_t applicationFrame = 0; ///< 提出元のアプリケーションフレーム番号。
    uint64_t resourceFrame = 0; ///< 提出元の ResourceManager フレーム番号。
    uint64_t serial = 0; ///< 提出元のスレッド内でパケットを識別する通し番号。
    uint32_t token = 0; ///< 提出元の収集対象フレームを識別するトークン。
    uint32_t viewOrdinal = 0; ///< 提出元のビュー呼び出しを識別する通し番号。
    uint32_t viewId = 0; ///< 提出元にホストが設定したビューの識別番号。
    bool accepted = false; ///< 提出先がパケットを受理したかどうか。
    bool complete = false; ///< パケットの計測行を閉じられたかどうか。
};
[[nodiscard]] FBZZ_GRAPHICS_API PacketInfo SnapshotPacket(uint32_t index);
/// @pre No trace scope or packet is active. Existing idle workers may be registered again at the next BeginFrame.
[[nodiscard]] FBZZ_GRAPHICS_API bool ConfigureForTesting(uint32_t warmup = 0, uint32_t stride = 1,
    uint32_t samples = 1, uint32_t packetLimit = MAX_PACKETS, TestCpuMode cpuMode = TestCpuMode::NATIVE,
    const char* outputPath = nullptr);
/// @pre All trace scopes/packets have finished; fixture shutdown precedes disabling/resetting capture state.
[[nodiscard]] FBZZ_GRAPHICS_API bool DisableForTesting();

} // namespace fbzz::renderer::cpu_trace
