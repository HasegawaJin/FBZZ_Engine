/// @file    ManagerWatch.hpp
/// @brief   マネージャーが見つからないことを、開始順に依存せず 1 度だけ報告する小さな状態
/// @author  Hasegawa Jin
/// @date    2026-08-23
///
/// @note ScriptSystem は GameObject ごとに OnAwake→OnStart→OnUpdate をまとめて回すため、
///       最初のフレームは自分より後ろのスクリプトの OnStart がまだで Instance() は空を
///       返しうる。ここで即警告を出すとヒエラルキーの並びだけで嘘の「見つからない」が出て
///       本物の警告まで信用できなくなるため、数フレーム待って 1 度だけ報告する規則を
///       1 箇所に閉じる。待つのは報告のタイミングだけで、参照自体は相手の作り直しに備え
///       毎回 Instance() を引き直すこと (掴んで持ち続けると解放済みポインタを読む)。
#pragma once

namespace sandbox {

struct ManagerWatch {
    /// 全スクリプトの OnStart が済むまでの猶予フレーム数。
    /// 1 フレームあれば足りるが、別のスクリプトが OnStart の中で生成する
    /// マネージャーまで拾えるよう少し多めに取る。
    static constexpr int kGraceFrames = 3;

    /// 毎フレーム呼ぶ。報告すべき最初のフレームだけ true を返す。
    /// 一度でも見つかっていれば、あとから消えても報告しない
    /// (シーン遷移中の破棄を「設定漏れ」として出さないため)。
    [[nodiscard]] bool ShouldReport(bool found)
    {
        if (found) {
            m_found = true;
            return false;
        }
        if (m_found || m_reported) return false;
        if (++m_frames <= kGraceFrames) return false;

        m_reported = true;
        return true;
    }

    void Reset() { *this = ManagerWatch{}; }

private:
    int  m_frames   = 0;
    bool m_found    = false;
    bool m_reported = false;
};

} // namespace sandbox
