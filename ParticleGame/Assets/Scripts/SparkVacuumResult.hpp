// ParticleGame
// SparkVacuumResult.hpp | particlegame
// Resultシーンへ最終スコアと回収結果を表示する
#pragma once

#include <Engine/Scene/Script.hpp>
#include "SparkVacuumGame.hpp"

namespace particlegame {

// SparkVacuumResult — シーン切り替え後も保持されたセッション結果をUITextへ反映する。
class SparkVacuumResult final : public fbzz::scene::Script {
    FBZZ_SCRIPT(SparkVacuumResult)

public:
    void OnStart() override
    {
        ui.SetText("SCORE  " + std::to_string(SparkVacuumGame::FinalScore())
                   + "\nKILLS  " + std::to_string(SparkVacuumGame::FinalKills())
                   + "\nSPARKS  " + std::to_string(SparkVacuumGame::FinalCollected())
                   + "\nBEST COMBO  x" + std::to_string(SparkVacuumGame::FinalCombo()));
    }
};

FBZZ_REFLECT(SparkVacuumResult)

} // namespace particlegame
