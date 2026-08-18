// FBZZ Engine
// ResultPresenterComponent.hpp | sandbox
// Result.scene の UIText に直前の勝敗と戦績を表示する
#pragma once

#include <Engine/Scene/Components/UIText.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Game/GameResultState.hpp>
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
        value += "\nENEMY COLLISIONS " + std::to_string(GameResultState::enemyImpacts);
        value += "\nANCHOR SLAMS " + std::to_string(GameResultState::anchorImpacts);
        ui.SetText(value);
    }
};

FBZZ_REFLECT(ResultPresenterComponent)

} // namespace sandbox
