/// @file    PlaySettingsBirthTable.hpp
/// @brief   Initial Edit/Play correspondence rejects GUID-only resurrection during selected settings adoption.
/// @author  Hasegawa Jin
/// @date    2026-10-05
#pragma once
#include <Editor/Op/OpData.hpp>
#include <Engine/Scene/Entity.hpp>
#include <Engine/Scene/ManagedBridgeAbi.hpp>
#include <array>
#include <cstdint>
#include <string>
#include <vector>
namespace fbzz::scene { class Scene; class ManagedScriptAdapter; }
namespace fbzz::editor {
class PlaySettingsBirthTable {
public:
    bool Capture(scene::Scene& edit, scene::Scene& play, std::string& diagnostic);
    bool ValidateSource(scene::Scene& edit, scene::Scene& play, const std::string& ownerGuid,
        const std::string& savedScriptId, scene::ManagedScriptAdapter*& destination, std::string& diagnostic) const;
    bool ValidateDestination(scene::Scene& edit, const std::string& ownerGuid, const std::string& savedScriptId,
        scene::ManagedScriptAdapter*& destination, std::string& diagnostic) const;
    /// @note Exported exact tokens must still denote initial surviving Play births; returned mappings contain only primitive token data.
    bool MapReferences(scene::Scene& edit, scene::Scene& play, const OpData& references, OpData& mappings, std::string& diagnostic) const;
    uint64_t EditLifetime() const { return m_editLifetime; }
    uint64_t PlayLifetime() const { return m_playLifetime; }
private:
    struct ScriptBirth { std::string savedId, type, editBirth, playBirth; };
    struct Owner {
        std::string guid;
        scene::EntityID editEntity, playEntity;
        uint64_t editObjectId = 0, playObjectId = 0;
        /// @note Native birth slots use the canonical registered component kind count, independently of settings collection lengths.
        std::array<uint64_t, static_cast<std::size_t>(scene::ManagedNativeComponentKind::COUNT)> editComponents{}, playComponents{};
        std::vector<ScriptBirth> scripts;
    };
    const Owner* Find(const std::string& guid) const;
    bool LiveOwners(scene::Scene& edit, scene::Scene& play, const Owner& owner, std::string& diagnostic) const;
    uint64_t m_editLifetime = 0, m_playLifetime = 0;
    std::vector<Owner> m_owners;
};
} /// @note namespace fbzz::editor
