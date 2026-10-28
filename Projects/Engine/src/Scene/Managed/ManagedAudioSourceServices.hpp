/// @file    ManagedAudioSourceServices.hpp
/// @brief   AudioSource の追加設定を検証する managed 実行境界。
/// @author  Hasegawa Jin
/// @date    2026-10-11
#pragma once
#include <Engine/Scene/ManagedBridgeAbi.hpp>
#include <span>
#include <string>
namespace fbzz::scene { class GameObject; }
namespace fbzz::scene::managed_detail {
struct ManagedServiceResult;
/// @pre caller・Scene・row lifetime の権限は Runtime が検証済み。
/// @return 不正 packet・scope・不在では対象と result を変更しない。
ManagedBridgeStatus RunAudioSourceSettings(GameObject& target, const ManagedBridgeRequest& request,
    int64_t integer, std::span<const float> values, std::span<const std::string> text,
    uint64_t relatedId, ManagedServiceResult& result);
} /// @note namespace fbzz::scene::managed_detail
