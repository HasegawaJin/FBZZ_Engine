/// @file    AnomalyLog.hpp
/// @brief   ベンチの場面が «人より先に気づけること» を積む先。
/// @author  Hasegawa Jin
/// @date    2026-09-09
///
/// 目視のベンチは «見ていない間に壊れたもの» を取り逃す。数十秒回して 1 度だけ NaN が出る、
/// あるスライダー位置でだけ法線が裏返る ── どれも画面を凝視し続けないと気づけない。
///
/// そこで各場面に «これが破れたら異常» という不変条件を自分で書かせ、ここへ積ませる。
/// 合否を機械判定できる契約は自動テスト (CTest) の担当なので、ここに書くのは
/// **設定を動かして初めて破れるもの** ── つまり自動テストでは配置を固定できないものに限る。
#pragma once

#include <Math/Vector3.hpp>

#include <cstdarg>
#include <string>
#include <vector>

namespace fbzz::bench {

enum class Severity {
    Warning, ///< 設定次第では起こりうる。文脈を見て人が判断する
    Error,   ///< 不変条件が破れている。設定に関わらず不具合
};

struct Anomaly {
    Severity    severity = Severity::Error;
    std::string message;
};

/// 1 フレームぶんの検出結果と、消えた異常を拾うための履歴を持つ。
class AnomalyLog {
public:
    /// 検出の直前と直後に対で呼ぶ。EndFrame が現フレームの結果を履歴へ畳む。
    void BeginFrame();
    void EndFrame();

    /// 不変条件が破れたときにだけ呼ぶ。format は printf 形式。
    void Report(Severity severity, const char* format, ...);
    /// broken が真のときだけ Report する。
    void ReportIf(bool broken, Severity severity, const char* format, ...);

    /// NaN / Inf と発散をまとめて見る。物理の壊れ方はほぼこの 2 つに行き着く。
    /// 壊れていれば false を返すので、それ以上の検査を続けずに降りられる。
    bool CheckFinite(const char* label, float value, float limit);
    bool CheckFinite(const char* label, const math::Vector3& value, float limit);

    [[nodiscard]] const std::vector<Anomaly>& Current() const { return m_current; }
    [[nodiscard]] bool Clean() const { return m_current.empty(); }
    /// 上限を超えて捨てた件数。ここが 0 でないときだけ «ほか N 件» を出す。
    [[nodiscard]] int  Suppressed() const { return m_suppressed; }

    /// 一瞬だけ出て消えた異常を落とさないための履歴。
    [[nodiscard]] int                Frames()    const { return m_frames; }
    [[nodiscard]] const std::string& FirstSeen() const { return m_firstSeen; }
    void ClearHistory();

private:
    /// 1 フレームに積む上限。取りこぼしより «画面が異常で埋まって読めない» ほうが困る。
    static constexpr int kMaxEntries = 12;

    void Push(Severity severity, const char* format, va_list args);

    std::vector<Anomaly> m_current;
    int                  m_suppressed = 0;
    int                  m_frames     = 0;
    std::string          m_firstSeen;
};

} // namespace fbzz::bench
