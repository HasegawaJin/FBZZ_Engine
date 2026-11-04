/// @file    SaveStore.cpp
/// @brief   ランタイム永続化ストアの TOML 入出力。
/// @author  Hasegawa Jin
/// @date    2026-08-23
#include <Engine/Util/SaveStore.hpp>

#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Scene/TomlReflector.hpp>
#include "PortableStoreValidation.hpp"

#include <toml++/toml.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <filesystem>
#include <sstream>
#include <utility>
#include <limits>

namespace fbzz::util {
namespace {
uint64_t AllocateStoreRevision()
{
    static std::atomic<uint64_t> next{1};
    uint64_t value = next.load(std::memory_order_relaxed);
    while (value != std::numeric_limits<uint64_t>::max()
        && !next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed)) {}
    assert(value != std::numeric_limits<uint64_t>::max() && "SaveStore revision space exhausted");
    return value != std::numeric_limits<uint64_t>::max() ? value : 0;
}
} /// @note namespace

/// @note メモリ上の正本を toml::table で持つ。型ごとの variant を自前で持たずに済み、
/// @note 保存も oss << table で完結する。
struct SaveStore::Impl {
    toml::table table;
    std::string path;
    bool        dirty = false;
    uint64_t    revision = AllocateStoreRevision();

    [[nodiscard]] std::string ResolveAbsolutePath() const
    {
        const std::filesystem::path slot = FileSystem::PathFromUtf8(path);
        if (slot.is_absolute()) return FileSystem::PathToUtf8(slot);
        /// @note カレントディレクトリはエディタ/GameHub/スタンドアロンで異なるため、実行ファイルの
        /// @note ディレクトリを基準にし、保存先が起動元依存で別ファイルになるのを避ける。
        return FileSystem::PathToUtf8(FileSystem::GetExecutableDirectory() / slot);
    }
};

namespace {

template<std::size_t N>
void WriteFloats(toml::table& table, std::string_view key, const float (&values)[N])
{
    toml::array arr;
    for (std::size_t i = 0; i < N; ++i) arr.push_back(static_cast<double>(values[i]));
    table.insert_or_assign(std::string(key), std::move(arr));
}

[[nodiscard]] const toml::array* FindArray(const toml::table& table, std::string_view key)
{
    const auto it = table.find(std::string(key));
    return it == table.end() ? nullptr : it->second.as_array();
}

} /// @note namespace

SaveStore::SaveStore(std::string defaultPath)
    : m_impl(std::make_unique<Impl>())
{
    m_impl->path = std::move(defaultPath);
}

SaveStore::~SaveStore() = default;
SaveStore::SaveStore(SaveStore&&) noexcept = default;
SaveStore& SaveStore::operator=(SaveStore&&) noexcept = default;

void SaveStore::SetPath(const std::string& path) { m_impl->path = path; m_impl->revision = AllocateStoreRevision(); }
const std::string& SaveStore::GetPath() const { return m_impl->path; }

/// @name ユーザー定義型

bool SaveStore::Write(std::string_view key, scene::IScriptSerializable& object)
{
    /// @note Reflect callback が呼出し側のキー文字列を変更しても、開始時の保存先を使う。
    std::string ownedKey(key);
    /// @note 完成するまで独立した heap table を所有し、nested pointer は writer の生存中に移さない。
    auto candidate = std::make_unique<toml::table>();
    { TomlWriteReflector writer(*candidate); object.Reflect(writer); }
    m_impl->table.insert_or_assign(std::move(ownedKey), std::move(*candidate));
    m_impl->dirty = true;
    m_impl->revision = AllocateStoreRevision();
    return true;
}

bool SaveStore::Read(std::string_view key, scene::IScriptSerializable& object)
{
    const auto it = m_impl->table.find(std::string(key));
    if (it == m_impl->table.end()) return false;
    const toml::table* source = it->second.as_table();
    if (!source) return false;

    /// @note Reflect の再入で同じキーの置換・Clear・Load が起きても、入れ子の参照を失効させない。
    const toml::table snapshot = *source;
    TomlReadReflector reader(snapshot);
    object.Reflect(reader);
    return true;
}

/// @name スカラー書き込み

void SaveStore::SetBool(std::string_view key, bool value)
{
    m_impl->table.insert_or_assign(std::string(key), value);
    m_impl->dirty = true;
    m_impl->revision = AllocateStoreRevision();
}

void SaveStore::SetInt(std::string_view key, int value)
{
    m_impl->table.insert_or_assign(std::string(key), static_cast<int64_t>(value));
    m_impl->dirty = true;
    m_impl->revision = AllocateStoreRevision();
}

void SaveStore::SetFloat(std::string_view key, float value)
{
    m_impl->table.insert_or_assign(std::string(key), static_cast<double>(value));
    m_impl->dirty = true;
    m_impl->revision = AllocateStoreRevision();
}

void SaveStore::SetString(std::string_view key, std::string_view value)
{
    m_impl->table.insert_or_assign(std::string(key), std::string(value));
    m_impl->dirty = true;
    m_impl->revision = AllocateStoreRevision();
}

void SaveStore::SetVector2(std::string_view key, const math::Vector2& value)
{
    const float values[2] = { value.x, value.y };
    WriteFloats(m_impl->table, key, values);
    m_impl->dirty = true;
    m_impl->revision = AllocateStoreRevision();
}

void SaveStore::SetVector3(std::string_view key, const math::Vector3& value)
{
    const float values[3] = { value.x, value.y, value.z };
    WriteFloats(m_impl->table, key, values);
    m_impl->dirty = true;
    m_impl->revision = AllocateStoreRevision();
}

void SaveStore::SetVector4(std::string_view key, const math::Vector4& value)
{
    const float values[4] = { value.x, value.y, value.z, value.w };
    WriteFloats(m_impl->table, key, values);
    m_impl->dirty = true;
    m_impl->revision = AllocateStoreRevision();
}

/// @name スカラー読み出し

bool SaveStore::GetBool(std::string_view key, bool defaultValue) const
{
    return m_impl->table[std::string(key)].value_or(defaultValue);
}

int SaveStore::GetInt(std::string_view key, int defaultValue) const
{
    return static_cast<int>(
        m_impl->table[std::string(key)].value_or(static_cast<int64_t>(defaultValue)));
}

float SaveStore::GetFloat(std::string_view key, float defaultValue) const
{
    /// @note TOML は 1.0 を書いても整数として読み戻る場合があるため int64 も見る。
    /// @note SetFloat(1.0f) → GetFloat() が既定値に落ちる事故を防ぐ。
    const auto node = m_impl->table[std::string(key)];
    if (const auto asDouble = node.value<double>()) return static_cast<float>(*asDouble);
    if (const auto asInt = node.value<int64_t>())   return static_cast<float>(*asInt);
    return defaultValue;
}

std::string SaveStore::GetString(std::string_view key, std::string_view defaultValue) const
{
    return m_impl->table[std::string(key)].value_or(std::string(defaultValue));
}

bool SaveStore::TryGetStringView(std::string_view key, std::string_view& out) const
{
    const auto it = m_impl->table.find(std::string(key));
    if (it == m_impl->table.end() || !it->second.is_string()) return false;
    out = it->second.as_string()->get();
    return true;
}

math::Vector2 SaveStore::GetVector2(std::string_view key, const math::Vector2& defaultValue) const
{
    return ArrToVec2(FindArray(m_impl->table, key), defaultValue);
}

math::Vector3 SaveStore::GetVector3(std::string_view key, const math::Vector3& defaultValue) const
{
    return ArrToVec3(FindArray(m_impl->table, key), defaultValue);
}

math::Vector4 SaveStore::GetVector4(std::string_view key, const math::Vector4& defaultValue) const
{
    return ArrToVec4(FindArray(m_impl->table, key), defaultValue);
}

/// @name 管理

bool SaveStore::Has(std::string_view key) const
{
    return m_impl->table.contains(std::string(key));
}

void SaveStore::Remove(std::string_view key)
{
    if (m_impl->table.erase(std::string(key)) > 0) m_impl->dirty = true;
    m_impl->revision = AllocateStoreRevision();
}

void SaveStore::Clear()
{
    if (m_impl->table.empty()) return;
    m_impl->table.clear();
    m_impl->dirty = true;
    m_impl->revision = AllocateStoreRevision();
}

std::vector<std::string> SaveStore::Keys() const
{
    std::vector<std::string> keys;
    keys.reserve(m_impl->table.size());
    for (const auto& [key, value] : m_impl->table) keys.emplace_back(key.str());
    std::sort(keys.begin(), keys.end());
    return keys;
}

bool SaveStore::Save()
{
    const std::string absPath = m_impl->ResolveAbsolutePath();
    if (!FileSystem::EnsureParentDirectory(FileSystem::PathFromUtf8(absPath))) {
        FBZZ_LOG_ERROR("SaveStore: could not create directory -> %s", absPath.c_str());
        return false;
    }

    std::ostringstream oss;
    oss << m_impl->table;
    if (!FileSystem::WriteTextAtomic(absPath, oss.str())) {
        FBZZ_LOG_ERROR("SaveStore: write failed -> %s", absPath.c_str());
        return false;
    }

    m_impl->dirty = false;
    m_impl->revision = AllocateStoreRevision();
    return true;
}

bool SaveStore::Load()
{
    const std::string absPath = m_impl->ResolveAbsolutePath();
    std::error_code error;
    const bool exists = std::filesystem::exists(FileSystem::PathFromUtf8(absPath), error);
    if (error) {
        FBZZ_LOG_ERROR("SaveStore: could not inspect save file -> %s: %s", absPath.c_str(), error.message().c_str());
        return false;
    }
    if (!exists) {
        m_impl->table.clear();
        m_impl->dirty = false;
        m_impl->revision = AllocateStoreRevision();
        return true;
    }
    std::string text;
    if (!FileSystem::ReadText(absPath, text)) {
        FBZZ_LOG_ERROR("SaveStore: read failed; previous values retained -> %s", absPath.c_str());
        return false;
    }

    auto result = toml::parse(text);
    if (!result) {
        FBZZ_LOG_ERROR("SaveStore: TOML parse failed (corrupted?) -> %s", absPath.c_str());
        return false;
    }

    m_impl->table = std::move(result.table());
    m_impl->dirty = false;
    m_impl->revision = AllocateStoreRevision();
    return true;
}

bool SaveStore::IsDirty() const { return m_impl->dirty; }
uint64_t SaveStore::GetRevision() const { return m_impl->revision; }

bool SaveStore::CommitString(std::string_view key, std::string&& value, uint64_t expectedRevision, std::string_view expectedPath)
{
    if (!expectedRevision || m_impl->revision != expectedRevision || m_impl->path != expectedPath) return false;
    m_impl->table.insert_or_assign(std::string(key), std::move(value));
    m_impl->dirty = true;
    m_impl->revision = AllocateStoreRevision();
    return true;
}

bool SaveStore::CommitDocument(std::string_view key, std::string type, std::string schema, std::string&& json,
    uint64_t expectedRevision, std::string_view expectedPath)
{
    if (!expectedRevision || m_impl->revision != expectedRevision || m_impl->path != expectedPath || type.empty() || schema.empty()) return false;
    if (!ValidatePortableStoreDocument(json, type, schema)) return false;
    auto candidate = std::make_unique<toml::table>();
    candidate->insert("formatVersion", 1); candidate->insert("type", std::move(type));
    candidate->insert("schema", std::move(schema)); candidate->insert("json", std::move(json));
    m_impl->table.insert_or_assign(std::string(key), std::move(*candidate));
    m_impl->dirty = true; m_impl->revision = AllocateStoreRevision(); return true;
}

bool SaveStore::TryGetDocumentView(std::string_view key, std::string_view& type, std::string_view& schema, std::string_view& json) const
{
    const auto found = m_impl->table.find(std::string(key));
    const auto* table = found == m_impl->table.end() ? nullptr : found->second.as_table();
    if (!table || (*table)["formatVersion"].value_or(0) != 1) return false;
    const auto* storedType = (*table)["type"].as_string(); const auto* storedSchema = (*table)["schema"].as_string();
    const auto* storedJson = (*table)["json"].as_string();
    if (!storedType || !storedSchema || !storedJson) return false;
    type = storedType->get(); schema = storedSchema->get(); json = storedJson->get(); return true;
}

} /// @note namespace fbzz::util
