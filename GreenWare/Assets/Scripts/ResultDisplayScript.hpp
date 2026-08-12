// GreenWare
// ResultDisplayScript.hpp | sandbox
// Result シーンの Canvas に付け、MagnetGameManager の勝敗結果を表示へ反映する
#pragma once

#include <Engine/Scene/Components/UIText.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include "Scripts/MagnetPrototypeComponent.hpp"
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class ResultDisplayScript : public Script {
    FBZZ_SCRIPT(ResultDisplayScript)

public:
    FBZZ_FIELD(std::string, resultTextObjectName, "ResultText", "Result Text Object")
    FBZZ_FIELD(std::string, scoreTextObjectName,  "ScoreText",  "Score Text Object")

    void OnStart() override;
};

FBZZ_REFLECT(ResultDisplayScript)

inline void ResultDisplayScript::OnStart()
{
    if (auto* resultObject = scene.Find(resultTextObjectName)) {
        if (auto* text = resultObject->GetComponent<UIText>()) {
            text->text  = MagnetGameManager::s_won ? "CLEAR!" : "TIME UP";
            text->color = MagnetGameManager::s_won
                ? Vector4{ 0.35f, 0.85f, 0.4f, 1.0f }
                : Vector4{ 0.9f, 0.3f, 0.3f, 1.0f };
        }
    }

    if (auto* scoreObject = scene.Find(scoreTextObjectName)) {
        if (auto* text = scoreObject->GetComponent<UIText>()) {
            text->text = "SCORE " + std::to_string(MagnetGameManager::s_score)
                       + "   (" + std::to_string(MagnetGameManager::s_defeated)
                       + "/" + std::to_string(MagnetGameManager::s_total) + ")";
        }
    }
}

} // namespace sandbox
