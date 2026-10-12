/// @file    ManagedServicesBridge.hpp
/// @brief   型付き managed facade から既存 native ScriptProxy を呼ぶ検証境界。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#pragma once
#include <Engine/Scene/ManagedBridgeAbi.hpp>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

namespace fbzz::scene {
class Scene;
class Script;
class GameObject;
namespace managed_detail {

/// @note object は呼出中だけ有効な非所有参照。Runtime が opaque ID と GUID へ変換する。
struct ManagedServiceResult {
    std::vector<float> values;
    std::vector<std::string> text;
    /// @note SaveStore の既存 string だけを同期 reply encoding まで借用する。fallback/一時文字列は text が所有する。
    std::string_view borrowedText;
    bool hasBorrowedText = false;
    /// @note 同期 read 応答だけが Animator の既存配列を借用する。Runtime が復号を終えるまで callback を配送しない。
    std::span<const std::pair<std::string, float>> blendWeights;
    int64_t integer = 0;
    GameObject* object = nullptr;
};

} /// @note namespace managed_detail
} /// @note namespace fbzz::scene
