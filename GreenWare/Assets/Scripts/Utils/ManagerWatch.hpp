/// @file    ManagerWatch.hpp
/// @brief   マネージャーが見つからないことを、開始順に依存せず 1 度だけ報告する小さな状態
/// @author  Hasegawa Jin
/// @date    2026-08-23
///
/// @note シーン配置済み Script は最初の Update 前に全て Start 済みになる。
/// @note GameFlow は Start 内で動的に生成されるボスも扱うため、次フレームから参加する対象に猶予を残す。
/// @note 待つのは警告だけ。参照自体は相手の作り直しに備え毎回取り直す。
#pragma once

namespace sandbox {

struct ManagerWatch {
    /// @note 動的生成が複数フレームに連なる参照先を待つためのゲーム側の猶予。
    static constexpr int kGraceFrames = 3;

    /// @note  毎フレーム呼ぶ。報告すべき最初のフレームだけ true を返す。
    /// @note  一度でも見つかっていれば、あとから消えても報告しない
    /// @note  (シーン遷移中の破棄を「設定漏れ」として出さないため)。
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

} /// @note namespace sandbox
