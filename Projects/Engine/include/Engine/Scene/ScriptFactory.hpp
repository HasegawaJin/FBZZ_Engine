/// @file    ScriptFactory.hpp
/// @brief   スクリプト型名から生成関数を引くレジストリ。
/// @author  Hasegawa Jin
/// @date    2026-05-21
/// @note SceneSerializer が保存名から Script を復元する。未登録型は生成失敗として扱う。
#pragma once
#include <Engine/Api.hpp>

#include <Engine/Scene/Script.hpp>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::scene {

class ScriptFactory {
public:
    using IdentityFactory = std::function<std::unique_ptr<Script>(const std::string& savedScriptId)>;

    /// @note ManagedScriptRuntime の catalog だけを登録する。
    FBZZ_ENGINE_API static bool RegisterManaged(const std::string& typeName, IdentityFactory factory);
    /// @note managed binding より前に保存 ID を渡す。新規追加以外は既存 ID を明示する。
    FBZZ_ENGINE_API static std::unique_ptr<Script> Create(const std::string& typeName, const std::string& savedScriptId);
    /// @brief 型名または alias の生成関数が登録済みか調べる。
    /// @note factory は呼ばず、Clone の typed-field schema preflight に使う。
    [[nodiscard]] FBZZ_ENGINE_API static bool CanCreate(std::string_view typeName);
    [[nodiscard]] FBZZ_ENGINE_API static bool IsValidSavedScriptId(std::string_view savedScriptId);
    FBZZ_ENGINE_API static std::vector<std::string> RegisteredTypeNames();
    FBZZ_ENGINE_API static bool SetDisplayName(const std::string& typeName, std::string displayName);
    [[nodiscard]] FBZZ_ENGINE_API static std::string DisplayName(const std::string& typeName);
    /// @return 完全名または単純名の一致が一件だけなら保存キーを返す。曖昧/未登録は false。
    FBZZ_ENGINE_API static bool ResolveTypeName(std::string_view name, std::string& typeKey);
    /// @note 移行 alias は catalog へ列挙せず、登録済み型だけを指す。
    FBZZ_ENGINE_API static bool RegisterAlias(const std::string& alias, const std::string& typeKey);

    FBZZ_ENGINE_API static void UnregisterManaged();
};

} /// @note namespace fbzz::scene
