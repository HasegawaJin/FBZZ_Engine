/// @file ManagerWatch.hpp
/// @brief マネージャーが見つからないことを、開始順に依存せず 1 度だけ報告する小さな状態
/// @author Hasegawa Jin
/// @date 2026-08-23
///
/// WHY 型にするか:
///   ScriptSystem は GameObject ごとに OnAwake → OnStart → OnUpdate をまとめて回す。
///   全スクリプトの OnStart が済んでから OnUpdate が始まるわけではない。つまり
///   最初のフレームでは「自分より後ろに並んでいるスクリプトの OnStart はまだ」で、
///   Instance() は空を返す。
///
///   ここで警告をラッチすると、マネージャーが正しく置いてあるシーンでも
///   「見つからない」と毎回エラーが出る。ヒエラルキーの並べ替えだけで嘘になる警告は、
///   本当に落ちているときの警告まで信用できなくする。かといって猶予を入れずに
///   毎フレーム出すとログが埋まって他が読めない。
///
///   「数フレーム待ってから、1 度だけ」という規則を各マネージャーが書き写すと
///   必ず食い違うので、1 箇所に閉じる。
///
/// NOTE: 探し当てるのを待つのはあくまで報告のタイミングだけ。参照そのものは
///       毎回 Instance() を引き直すこと (掴んで持ち続けると、相手が作り直された
///       ときに解放済みのポインタを読む)。
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
