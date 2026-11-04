/// @file    RagdollDefinition.hpp
/// @brief   モデル別の剛体・関節・駆動設定 (.ragdoll) の保存契約。
/// @author  Hasegawa Jin
/// @date    2026-11-03
#pragma once
#include <Engine/Scene/Ragdoll/RagdollProfile.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::scene {
struct RagdollDefinition {
    std::string name;
    std::string skeletonSource;
    bool definitionsOnly = true;
    std::vector<RagdollBodyDefinition> bodies;
    std::vector<RagdollCollisionExclusion> collisionExclusions;
    /// @note 読み込んだ内容の署名。他アセットの再読込で同一の物理を組み直さないために使う。
    std::uint64_t signature = 0;
};

/// @note version=1 の TOML。角度は degrees、寸法は m、質量は kg、慣性は kg·m²、駆動周波数は Hz。
/// @note 失敗時は out を変更しない。GUID 参照と相対パスの解決順は他アセットと共通。
[[nodiscard]] FBZZ_ENGINE_API bool LoadRagdollDefinition(
    const std::string& reference, RagdollDefinition& out, std::string* error = nullptr);
[[nodiscard]] FBZZ_ENGINE_API bool SaveRagdollDefinition(
    const std::string& path, const RagdollDefinition& value, std::string* error = nullptr);
[[nodiscard]] FBZZ_ENGINE_API bool ValidateRagdollDefinition(
    const RagdollDefinition& value, std::string* error = nullptr);
FBZZ_ENGINE_API void ApplyRagdollDefinition(const RagdollDefinition& value, RagdollProfile& profile);
}
