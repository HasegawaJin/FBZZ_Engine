/// @file    ResultPresenterComponent.hpp
/// @brief   Result.scene の UIText に直前の勝敗と戦績を表示する。
/// @author  Hasegawa Jin
/// @date    2026-08-19
#pragma once

#include <Engine/Scene/Components/UIText.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Game/GameResultState.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <cmath>
#include <string>

using namespace fbzz::scene;

namespace sandbox {

class ResultPresenterComponent : public Script {
    FBZZ_SCRIPT(ResultPresenterComponent)
    FBZZ_REQUIRE_COMPONENT(UIText)

public:
    void OnStart() override
    {
        const int totalSeconds = static_cast<int>(std::round(GameResultState::clearSeconds));
        const int minutes = totalSeconds / 60;
        const int seconds = totalSeconds % 60;
        std::string value = GameResultState::victory ? "AREA CLEAR" : "MISSION FAILED";
        value += "\nTIME " + std::to_string(minutes) + ":" + (seconds < 10 ? "0" : "") +
                 std::to_string(seconds);
        value += "\nDESTROYED " + std::to_string(GameResultState::defeatedEnemies);
        // 引きと押しを別々に出す。ランクの 3 軸のうち 2 つがこれなので、
        // «どちらが足りなかったか» が数字のまま読めないと次の 1 周へ繋がらない。
        value += "\nBEST CHAIN " + std::to_string(GameResultState::bestChain);
        value += "\nPUSH KILLS " + std::to_string(GameResultState::pushKills);

        // WHY ランクを勝ったときだけ出すか: 負けたプレイに «C» と付けても、
        //     何を直せばよいかは伝わらない。リトライした周も同じ理由で出さない
        //     (Docs/game-flow.md「リトライ」)。
        const bool showRank = GameResultState::RankAvailable();
        if (showRank) value += "\n\nRANK " + std::string(GameResultState::RankLabel());

        if (GameResultState::bestSeconds > 0.0f) {
            const int best = static_cast<int>(std::round(GameResultState::bestSeconds));
            value += "\nBEST " + std::to_string(best / 60) + ":" +
                     (best % 60 < 10 ? "0" : "") + std::to_string(best % 60);
        }
        ui.SetText(value);

        se::EnsureSource(scene, "UI");
        se::Play(audio, se::kUiResult);
        // ランク音は «評価が出た» ことの合図なので、出していないときは鳴らさない。
        if (showRank) se::Play(audio, se::RankBank(GameResultState::RankLabel()[0]));

        // 自己ベストは表示した «後» に更新する。先に更新すると、今回の記録が
        // そのままベストとして併記され、更新できたかどうかが読めない。
        GameResultState::CommitBest();
    }
};

FBZZ_REFLECT(ResultPresenterComponent)

} // namespace sandbox
