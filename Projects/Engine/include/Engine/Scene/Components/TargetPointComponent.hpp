/// @file    TargetPointComponent.hpp
/// @brief   共通制御点の Editor 表示設定。
/// @author  Hasegawa Jin
/// @date    2026-10-08
#pragma once

#include <Math/Vector4.hpp>

namespace fbzz::scene {

struct IReflector;

/// @brief GameObject の Transform を著者向け制御点として表示する Marker。
/// @note 所有者、index、用途別属性を持たず、非表示でも利用側の参照解決を無効にしない。
struct TargetPointComponent {
    bool visible = true;
    math::Vector4 color{0.25f, 0.85f, 1.0f, 1.0f};
    float screenSize = 9.0f;

    void Reflect(IReflector& reflector);
};

} /// @note namespace fbzz::scene
