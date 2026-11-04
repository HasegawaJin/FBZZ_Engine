/// @file    ManagedRagdollControls.hpp
/// @brief   ラグドール操作APIの型付き入力検証。
/// @author  Hasegawa Jin
/// @date    2026-11-03
#pragma once
#include "ManagedComponentServices.hpp"
namespace fbzz::scene {struct RagdollComponent;}
namespace fbzz::scene::managed_detail {
ManagedBridgeStatus RunRagdollControls(RagdollComponent& row,GameObject& target,GameObject* related,
    uint32_t selector,int64_t integer,std::span<const float> values,std::span<const std::string> text,ManagedServiceResult& output);
}
