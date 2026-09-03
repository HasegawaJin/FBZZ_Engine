/// @file    MemoryLeakDiff.hpp
/// @brief   Renderer リソース台帳を「基準時点」と突き合わせ、増えた発生位置だけを取り出す差分器。
/// @author  Hasegawa Jin
/// @date    2026-09-02
///
/// Analysis パネルの Memory タブと Play/Stop の後始末から使う。
#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace fbzz::renderer { class ResourceManager; }

namespace fbzz::editor {

/// 発生位置 1 つぶんの «今» の本数。
struct MemoryLeakGroup {
    std::string origin;              ///< "file:line"
    std::string allocatorName;
    std::size_t count = 0;
    std::size_t bytes = 0;
};

/// 発生位置 1 つぶんの増減。
struct MemoryLeakRow {
    std::string    origin;              ///< "file:line"
    std::string    allocatorName;
    std::size_t    baseCount  = 0;
    std::size_t    nowCount   = 0;
    std::ptrdiff_t countDelta = 0;
    std::ptrdiff_t bytesDelta = 0;
};

/// 基準時点から今までの増減。
struct MemoryLeakReport {
    bool                      valid           = false;
    std::string               label;          ///< 基準を取った文脈 ("Play" / "Manual")
    std::vector<MemoryLeakRow> rows;          ///< 増えた発生位置のみ。bytesDelta 降順
    std::ptrdiff_t            totalBytesDelta = 0;
    std::ptrdiff_t            totalCountDelta = 0;
    std::size_t               liveCount       = 0;

    /// リークを疑うべきか。
    ///
    /// rows が空でないことを条件にしてはいけない。rows は増えた origin だけを載せるので、
    /// 「A が 3 増えて B が 3 減った」= 本数が元に戻った往復でも空にならない。
    /// バイト数だけの増減はバッファの張り直しで起きるため、本数で判断する。
    [[nodiscard]] bool Grew() const { return totalCountDelta > 0; }
};

/// 生存中の Renderer リソースを「発生位置ごとの本数」に畳んで基準と比べる。
///
/// WHY 個体 (allocationId) で比べないか: Play/Stop はシーンを丸ごと作り直すため、
///     同じ役目のリソースでも別の id になる。個体単位の差分では「作り直しただけ」の
///     ものが全部リークに見えてしまう。同じ file:line の本数が増えたときだけ残す。
class MemoryLeakDiff {
public:
    void CaptureBaseline(const renderer::ResourceManager& resources, std::string label);
    void ClearBaseline();
    [[nodiscard]] bool HasBaseline() const { return m_hasBaseline; }
    [[nodiscard]] const std::string& GetBaselineLabel() const { return m_baselineLabel; }
    [[nodiscard]] std::size_t GetBaselineCount() const { return m_baselineCount; }

    /// 基準と今を比べる。基準が無ければ valid == false の結果を返す。
    [[nodiscard]] MemoryLeakReport Compare(const renderer::ResourceManager& resources) const;

    /// Play→Stop のように「後から読み返したい」結果を保持する。
    void SetPinnedReport(MemoryLeakReport report);
    [[nodiscard]] const MemoryLeakReport& GetPinnedReport() const { return m_pinned; }

    /// @name セッション基準 (最初の Play からの累計)
    ///
    /// WHY 1 往復の差分だけでは足りないか: キャッシュ (LoadTexture / LoadShader /
    ///     .mat 解決) は «初回の Play で 1 度だけ» 増える。1 往復ぶんの差分では、
    ///     それとリークが同じ «+N» に見えてしまう。最初の Play を基準に据えて累計を並べれば、
    ///     往復のたびに比例して伸びるものだけがリークだと分かる。
    ///@{
    void CaptureSessionBaselineIfAbsent(const renderer::ResourceManager& resources);
    [[nodiscard]] bool HasSessionBaseline() const { return m_hasSession; }
    [[nodiscard]] int  GetCycleCount() const { return m_cycleCount; }
    void NoteCycleCompleted() { ++m_cycleCount; }
    void ClearSessionBaseline();
    /// セッション基準との差分。基準が無ければ valid == false。
    [[nodiscard]] MemoryLeakReport CompareSession(const renderer::ResourceManager& resources) const;
    ///@}

    /// frames フレーム後に比較し、結果を保持してログへ出す。
    /// WHY 即座に比べないか: 解放はフレーム境界をまたぐ (復元直後のシーンはまだ描画中の
    ///     リソースを掴んでいるし、DX12 の実解放はフェンス待ちの後)。落ち着いてから数える。
    void ScheduleCompare(int frames, std::string label);
    /// 毎フレーム呼ぶ。予約が満了したフレームで比較する。@ret 比較したら true。
    bool Tick(const renderer::ResourceManager& resources);

    /// Console へ 1 行サマリ + 上位行を出す。
    static void LogReport(const MemoryLeakReport& report, std::size_t maxRows = 8);

private:
    struct Bucket {
        std::size_t count = 0;
        std::size_t bytes = 0;
        std::string origin;
        std::string allocatorName;
    };
    using Buckets = std::map<std::string, Bucket>;

    static Buckets Collect(const renderer::ResourceManager& resources);
    [[nodiscard]] static MemoryLeakReport CompareTo(const Buckets& baseline,
                                                    const renderer::ResourceManager& resources);

    Buckets     m_session;
    bool        m_hasSession = false;
    int         m_cycleCount = 0;

    Buckets     m_baseline;
    std::string m_baselineLabel;
    std::size_t m_baselineCount = 0;
    bool        m_hasBaseline   = false;
    MemoryLeakReport m_pinned;
    int         m_pendingFrames = 0;
    std::string m_pendingLabel;
};

/// 生存中の Renderer リソースを発生位置ごとに畳んだ一覧 (本数の多い順)。
[[nodiscard]] std::vector<MemoryLeakGroup> SummarizeLiveResources(
    const renderer::ResourceManager& resources);

/// 貼り付け用のテキスト一式 (合計・発生位置ごとの一覧・基準からの差分)。
///
/// WHY 要るか: リークの相談は «どの発生位置が何本» が揃って初めて始まる。
///     パネルの表を人が読んで書き写すと、いちばん要る行が落ちる。
[[nodiscard]] std::string FormatMemoryReport(const renderer::ResourceManager& resources,
                                             const MemoryLeakDiff& diff);

} // namespace fbzz::editor
