/// @file    ManagedInputBridge.hpp
/// @brief   C# 入力名の存在確認と値・名前一覧を実行中の InputActionMap から取得する。
/// @author  Hasegawa Jin
/// @date    2026-10-05
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Scene/ManagedBridgeAbi.hpp>
#include <string>
#include <string_view>

namespace fbzz::scene::managed_detail {

class ManagedInputBridge final {
public:
    /// @note Runtime が callback と thread を検証する。未登録名は NOT_FOUND、登録済み無入力は OK を返す。
    FBZZ_ENGINE_API static ManagedBridgeStatus Read(std::string_view name, uint32_t state, ManagedBridgeReply& reply);
    /// @note 名前比較は InputActionMap と同じ大文字小文字を区別する比較。一覧は現在の登録順の snapshot。
    /// @return 失敗時は reply と names を変更しない。EXISTS は未登録名にも OK / false を返す。
    FBZZ_ENGINE_API static ManagedBridgeStatus Query(std::string_view name, uint32_t query, ManagedBridgeReply& reply, std::string& names);
};

} /// @note namespace fbzz::scene::managed_detail
