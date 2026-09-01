/// @file    StageProgressState.hpp
/// @brief   ステージ選択が読む、ステージごとの解放状態と自己ベスト。
/// @author  Hasegawa Jin
/// @date    2026-08-29
///
/// WHY GameResultState と分けるか:
///   GameResultState は «直前の 1 周» を運ぶ器で、リザルトを出したら役目が終わる。
///   こちらは «これまで» を持つ。寿命が違うものを同じ器に入れると、リトライで
///   直前の記録が消えるたびに自己ベストまで巻き込まれる。
///
/// WHY 保存しないか:
///   保存は GameSettings の担当。ここはセッション中だけ持ち、Commit を呼ばれた
///   ときに «今回の方が良ければ» 上書きするだけにしておく。
#pragma once

#include <Scripts/Game/GameResultState.hpp>
#include <Scripts/UI/StageCatalog.hpp>
#include <algorithm>

namespace sandbox {

struct StageRecord {
    bool  cleared     = false;
    bool  unlocked    = false;
    int   bestScore   = 0;      ///< 0〜9。ランクはここから引く
    float bestSeconds = 0.0f;
    int   bestChain   = 0;
    int   bestPush    = 0;
};

struct StageProgressState {
    /// ステージ数。増やすときはここと `kStageCount`、シーンの Row の数を合わせる。
    static constexpr int kCount = kStageCount;
    static inline StageRecord stages[kCount] = {};
    static inline int cursor = 0;      ///< 選択画面を出し直したとき、同じ行に戻す

    /// 最初の 1 回だけ。STAGE 01 は最初から遊べる。
    static void EnsureInit()
    {
        if (stages[0].unlocked) return;
        stages[0].unlocked = true;
    }

    [[nodiscard]] static const char* RankLabel(int score)
    {
        if (score >= 8) return "S";
        if (score >= 6) return "A";
        if (score >= 4) return "B";
        return "C";
    }

    /// リザルトが «勝ちで» 閉じるときに呼ぶ。次のステージを開ける。
    static void Commit(int index)
    {
        if (index < 0 || index >= kCount) return;
        StageRecord& r = stages[index];
        r.cleared  = true;
        r.unlocked = true;
        const int score = GameResultState::Score();
        r.bestScore = (std::max)(r.bestScore, score);
        if (r.bestSeconds <= 0.0f || GameResultState::clearSeconds < r.bestSeconds)
            r.bestSeconds = GameResultState::clearSeconds;
        r.bestChain = (std::max)(r.bestChain, GameResultState::bestChain);
        r.bestPush  = (std::max)(r.bestPush,  GameResultState::pushKills);

        // WHY 実体のある枠だけ開けるか: 解放してしまうと選択画面が «押せる行» として
        //     見せ、押した先で読み込みに失敗する。作っていないステージは
        //     «前のステージをクリアすると解放される» のまま伏せておく方が嘘が少ない。
        if (index + 1 < kCount && StageExists(index + 1)) stages[index + 1].unlocked = true;
    }
};

} // namespace sandbox
