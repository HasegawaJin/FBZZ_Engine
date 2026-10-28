/// @file    PortableStoreValidation.hpp
/// @brief   Portable 保存 envelope の streaming 構文・metadata 照合。
/// @author  Hasegawa Jin
/// @date    2026-10-06
#pragma once
#include <string_view>
namespace fbzz::util {
/// @note 値 schema は Host の共通 codec が検証する。ここでは publication 前に構文・有限 number・version/type/schema を独立照合する。
bool ValidatePortableStoreDocument(std::string_view json, std::string_view type, std::string_view schema);
} /// @note namespace fbzz::util
