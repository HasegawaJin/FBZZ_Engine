/// @file    ObjectReference.hpp
/// @brief   GUID を正本とする軽量な Scene GameObject 参照。
/// @author  Hasegawa Jin
/// @date    2026-10-08
#pragma once

#include <Engine/Scene/Entity.hpp>
#include <cstdint>
#include <string>

namespace fbzz::scene {

/// @brief GUID と Scene 寿命付き EntityID を保持する非所有参照。
/// @note 空 GUID は null。解決できない GUID は保持し、id は同じ Scene 世代の一時キャッシュとして扱う。
struct ObjectReference {
    std::string guid;
    mutable EntityID id = EntityID::INVALID;
    mutable uint64_t sceneGeneration = 0;
};

/// @brief 既存 Script API 名との互換 alias。
using ScriptObjectReference = ObjectReference;

} /// @note namespace fbzz::scene
