/// @file    ScriptStoreProxies.cpp
/// @brief   Store 領域の Script 転送と関連する内部ヘルパー。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#include <Engine/Scene/Script.hpp>
#include <Engine/Asset/AssetStreaming.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Input/Input.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Util/SaveStore.hpp>
#include <limits>
#include <type_traits>
#include <string_view>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cmath>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fbzz::scene {
namespace {


util::SaveStore& StoreOf(SaveStoreKind kind)
{
    auto& app = core::Application::Get();
    return kind == SaveStoreKind::Config ? app.GetConfigStore() : app.GetSaveStore();
}


} /// @note namespace
void ScriptStoreProxyBase::SetPath(std::string_view path) const
{
    StoreOf(kind).SetPath(std::string(path));
}


std::string ScriptStoreProxyBase::GetPath() const
{
    return StoreOf(kind).GetPath();
}


bool ScriptStoreProxyBase::Write(std::string_view key, IScriptSerializable& object) const
{
    return StoreOf(kind).Write(key, object);
}


bool ScriptStoreProxyBase::Read(std::string_view key, IScriptSerializable& object) const
{
    return StoreOf(kind).Read(key, object);
}


void ScriptStoreProxyBase::SetBool(std::string_view key, bool value) const
{
    StoreOf(kind).SetBool(key, value);
}


void ScriptStoreProxyBase::SetInt(std::string_view key, int value) const
{
    StoreOf(kind).SetInt(key, value);
}


void ScriptStoreProxyBase::SetFloat(std::string_view key, float value) const
{
    StoreOf(kind).SetFloat(key, value);
}


void ScriptStoreProxyBase::SetString(std::string_view key, std::string_view value) const
{
    StoreOf(kind).SetString(key, value);
}


void ScriptStoreProxyBase::SetVector2(std::string_view key, const math::Vector2& value) const
{
    StoreOf(kind).SetVector2(key, value);
}


void ScriptStoreProxyBase::SetVector3(std::string_view key, const math::Vector3& value) const
{
    StoreOf(kind).SetVector3(key, value);
}


void ScriptStoreProxyBase::SetVector4(std::string_view key, const math::Vector4& value) const
{
    StoreOf(kind).SetVector4(key, value);
}


bool ScriptStoreProxyBase::GetBool(std::string_view key, bool defaultValue) const
{
    return StoreOf(kind).GetBool(key, defaultValue);
}


int ScriptStoreProxyBase::GetInt(std::string_view key, int defaultValue) const
{
    return StoreOf(kind).GetInt(key, defaultValue);
}


float ScriptStoreProxyBase::GetFloat(std::string_view key, float defaultValue) const
{
    return StoreOf(kind).GetFloat(key, defaultValue);
}


std::string ScriptStoreProxyBase::GetString(std::string_view key, std::string_view defaultValue) const
{
    return StoreOf(kind).GetString(key, defaultValue);
}


math::Vector2 ScriptStoreProxyBase::GetVector2(std::string_view key, const math::Vector2& defaultValue) const
{
    return StoreOf(kind).GetVector2(key, defaultValue);
}


math::Vector3 ScriptStoreProxyBase::GetVector3(std::string_view key, const math::Vector3& defaultValue) const
{
    return StoreOf(kind).GetVector3(key, defaultValue);
}


math::Vector4 ScriptStoreProxyBase::GetVector4(std::string_view key, const math::Vector4& defaultValue) const
{
    return StoreOf(kind).GetVector4(key, defaultValue);
}


bool ScriptStoreProxyBase::Has(std::string_view key) const
{
    return StoreOf(kind).Has(key);
}


void ScriptStoreProxyBase::Remove(std::string_view key) const
{
    StoreOf(kind).Remove(key);
}


void ScriptStoreProxyBase::Clear() const
{
    StoreOf(kind).Clear();
}


bool ScriptStoreProxyBase::Save() const
{
    return StoreOf(kind).Save();
}


bool ScriptStoreProxyBase::Load() const
{
    return StoreOf(kind).Load();
}


bool ScriptStoreProxyBase::IsDirty() const
{
    return StoreOf(kind).IsDirty();
}

/// @name ScriptDisplayProxy


} /// @note namespace fbzz::scene
