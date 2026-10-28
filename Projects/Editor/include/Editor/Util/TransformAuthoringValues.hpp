/// @file    TransformAuthoringValues.hpp
/// @brief   Transform の編集値の比較契約。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#pragma once

namespace fbzz::scene { struct Transform; }

namespace fbzz::editor {

/// @note ローカル position / rotation / scale のみを厳密比較し、派生した world 値は比較しない。
[[nodiscard]] bool TransformAuthoringValuesEqual(const scene::Transform& lhs, const scene::Transform& rhs);

} /// @note namespace fbzz::editor
