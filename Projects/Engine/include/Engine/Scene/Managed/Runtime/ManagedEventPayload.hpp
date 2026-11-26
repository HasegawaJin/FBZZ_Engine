/// @file    ManagedEventPayload.hpp
/// @brief   ScriptEventBus の managed channel で共用する版付き JSON payload。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#pragma once
#include <string_view>

namespace fbzz::scene {
/// @brief Managed の ScriptEventBus が同期配送する版付き JSON。
/// @note チャネル名は `managed:<CLR payload fullname>`。JSON は `{"$version":1,"value":...}`。個体参照は `{"$go":"GUID"}`、実数は有限値。
struct ManagedEventPayload {
    std::string_view json; ///< PublishRaw が戻るまで発行側が所有する JSON バイト列の非所有参照。
};
} // namespace fbzz::scene
