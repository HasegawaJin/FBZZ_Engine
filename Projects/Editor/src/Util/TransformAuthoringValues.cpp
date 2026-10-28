/// @file    TransformAuthoringValues.cpp
/// @brief   Inspector と Gizmo の Transform 編集値比較。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include <Editor/Util/TransformAuthoringValues.hpp>
#include <Engine/Scene/Transform.hpp>

namespace fbzz::editor {

bool TransformAuthoringValuesEqual(const scene::Transform& lhs, const scene::Transform& rhs)
{
    return lhs.position.x == rhs.position.x &&
           lhs.position.y == rhs.position.y &&
           lhs.position.z == rhs.position.z &&
           lhs.rotation.x == rhs.rotation.x &&
           lhs.rotation.y == rhs.rotation.y &&
           lhs.rotation.z == rhs.rotation.z &&
           lhs.rotation.w == rhs.rotation.w &&
           lhs.scale.x == rhs.scale.x &&
           lhs.scale.y == rhs.scale.y &&
           lhs.scale.z == rhs.scale.z;
}

} /// @note namespace fbzz::editor
