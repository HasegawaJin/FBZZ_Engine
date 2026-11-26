/// @file    ObjectReference.hpp
/// @brief   GUID を正本とする軽量な Scene GameObject 参照。
/// @author  Hasegawa Jin
/// @date    2026-10-08
#pragma once

#include <Engine/Scene/References/Entity.hpp>
#include <cstdint>
#include <string>

namespace fbzz::scene {

/// @brief GUID と Scene 寿命付き EntityID を保持する非所有参照。
/// @note 空 GUID は null。解決できない GUID は保持し、id は同じ Scene 世代の一時キャッシュとして扱う。
struct ObjectReference {
    std::string guid; ///< 保存と参照先の再解決に使うオブジェクト GUID。
    mutable EntityID id = EntityID::INVALID; ///< 現在のシーンで解決した参照先の Entity ID。
    mutable uint64_t sceneGeneration = 0; ///< キャッシュした Entity ID が属するシーンの世代。
};

/// @brief 既存 Script API 名との互換 alias。
using ScriptObjectReference = ObjectReference;

} // namespace fbzz::scene
