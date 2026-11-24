/// @file    ManagedEventPayload.hpp
/// @brief   ScriptEventBus の managed channel で共用する版付き JSON payload。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#pragma once
#include <string_view>

namespace fbzz::scene {
/// @note channel は managed:<CLR payload fullname>。PublishRaw の同期呼び出しが戻るまで JSON バイトを保持する。
/// @note JSON は {"$version":1,"value":...}。GameObject は {"$go":"GUID"}、float は有限値のみ。
struct ManagedEventPayload { std::string_view json; };
}
