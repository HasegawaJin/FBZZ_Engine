/// @file    PlaySettingsAdoption.hpp
/// @brief   Captured Play settings keep stable birth proofs and primitive handles for explicit post-Stop adoption.
/// @author  Hasegawa Jin
/// @date    2026-10-06
#pragma once
#include <Editor/Op/EditorOperator.hpp>
#include <Engine/Scene/Entity.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
namespace fbzz::editor {
class PlaySettingsBirthTable;
struct PlaySettingsAdoptionTarget {
    uint64_t captureId = 0;
    scene::EntityID originalEntity;
    std::string ownerGuid, savedScriptId, originalBirth, type;
    OpData summary;
};
struct PlaySettingsAdoptionDraft {
    ~PlaySettingsAdoptionDraft();
    uint64_t id = 0, editLifetime = 0;
    std::shared_ptr<const PlaySettingsBirthTable> births;
    std::vector<PlaySettingsAdoptionTarget> targets;
    std::string diagnostic, plan;
    bool consumed = false;
    bool referenceProofReady = true;
    uint64_t preparedHostToken = 0, preparedGeneration = 0;
    void Consume();
};
void RegisterPlaySettingsAdoptionOperators(OperatorRegistry& registry);
void DrawPlaySettingsAdoptionReview(EditorContext& context);
} /// @note namespace fbzz::editor
