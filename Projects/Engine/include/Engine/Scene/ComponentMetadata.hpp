/// @file    ComponentMetadata.hpp
/// @brief   Component の表示・保存方針の軽量な宣言。
/// @author  Hasegawa Jin
/// @date    2026-10-30
#pragma once

namespace fbzz::scene {
/// @brief Editor 上の分類。依存方向を増やさず Engine 側の登録情報だけでメニューを構築する。
enum class ComponentCategory {
    Rendering,
    Lighting,
    Physics,
    Animation,
    Audio,
    Effects,
    Environment,
    Navigation,
    Terrain,
    UI,
    Misc,
    Internal
};

/// @brief Automatic は Reflect() から描画し、Custom は専用 Editor、Hidden は内部型として扱う。
enum class ComponentInspectorMode { Automatic, Custom, Hidden };

/// @brief Automatic は Reflect() で TOML 化し、Custom は既存の複合データ専用処理を使う。
enum class ComponentSerializationMode { Automatic, Custom };

} /// @note namespace fbzz::scene
