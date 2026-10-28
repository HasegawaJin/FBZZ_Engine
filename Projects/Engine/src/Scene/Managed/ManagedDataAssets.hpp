/// @file    ManagedDataAssets.hpp
/// @brief   managed 設定アセットを既存 .fzdata TOML から純値 JSON へ読み取る。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#pragma once
#include <Engine/Scene/ManagedBridgeAbi.hpp>
#include <string>
#include <string_view>

namespace fbzz::scene::managed_detail {
/// @note AssetManager と同じ GUID/path 解決を使い、型の生成・ユーザー callback・ディスク変更を行わない。
ManagedBridgeStatus ReadDataAsset(std::string_view reference, std::string& json);
}
